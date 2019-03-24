/**
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "log.hpp"
#include "hook_manager.hpp"
#include "runtime_d3d12.hpp"
#include "runtime_objects.hpp"
#include "resource_loading.hpp"
#include "dxgi/format_utils.hpp"
#include <imgui.h>
#include <d3dcompiler.h>

namespace reshade::d3d12
{
	struct d3d12_tex_data : base_object
	{
		com_ptr<ID3D12Resource> resource;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu_handle[2];
		D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle[2];
		D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_handle[2];
	};
	struct d3d12_pass_data : base_object
	{
		com_ptr<ID3D12PipelineState> pipeline;
		D3D12_VIEWPORT viewport;
		UINT num_render_targets;
		D3D12_CPU_DESCRIPTOR_HANDLE render_targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
	};
	struct d3d12_technique_data : base_object
	{
	};
	struct d3d12_effect_data
	{
		com_ptr<ID3D12Resource> cb;
		com_ptr<ID3D12RootSignature> signature;
		com_ptr<ID3D12DescriptorHeap> srv_heap;
		com_ptr<ID3D12DescriptorHeap> sampler_heap;

		UINT64 storage_offset, storage_size;
		D3D12_GPU_VIRTUAL_ADDRESS cbv_gpu_address;
		D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_base;
		D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_base;
		D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_base;
		D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu_base;
	};

	static void transition_state(
		const com_ptr<ID3D12GraphicsCommandList> &list,
		const com_ptr<ID3D12Resource> &res,
		D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to,
		UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
	{
		D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
		transition.Transition.pResource = res.get();
		transition.Transition.Subresource = subresource;
		transition.Transition.StateBefore = from;
		transition.Transition.StateAfter = to;
		list->ResourceBarrier(1, &transition);
	}
}

reshade::d3d12::runtime_d3d12::runtime_d3d12(ID3D12Device *device, ID3D12CommandQueue *queue, IDXGISwapChain3 *swapchain) :
	_device(device), _commandqueue(queue), _swapchain(swapchain)
{
	assert(queue != nullptr);
	assert(device != nullptr);
	assert(swapchain != nullptr);

	_renderer_id = D3D_FEATURE_LEVEL_12_0;

	if (com_ptr<IDXGIFactory4> factory;
		SUCCEEDED(swapchain->GetParent(IID_PPV_ARGS(&factory))))
	{
		const LUID luid = device->GetAdapterLuid();

		if (com_ptr<IDXGIAdapter> adapter;
			factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))
		{
			DXGI_ADAPTER_DESC desc;
			adapter->GetDesc(&desc);
			_vendor_id = desc.VendorId;
			_device_id = desc.DeviceId;
		}
	}
}
reshade::d3d12::runtime_d3d12::~runtime_d3d12()
{
	if (_d3d_compiler != nullptr)
		FreeLibrary(_d3d_compiler);
}

bool reshade::d3d12::runtime_d3d12::init_backbuffer_textures(unsigned int num_buffers)
{
	{   D3D12_DESCRIPTOR_HEAP_DESC desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV };
		desc.NumDescriptors = num_buffers;
		if (HRESULT hr = _device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_backbuffer_rtvs)); FAILED(hr))
			return false;
	}

	_rtv_handle_size = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE handle = _backbuffer_rtvs->GetCPUDescriptorHandleForHeapStart();

	_backbuffers.resize(num_buffers);

	for (unsigned int i = 0; i < num_buffers; ++i, handle.ptr += _rtv_handle_size)
	{
		if (FAILED(_swapchain->GetBuffer(i, IID_PPV_ARGS(&_backbuffers[i]))))
			return false;
#ifdef _DEBUG
		_backbuffers[i]->SetName(L"Backbuffer");
#endif

		_device->CreateRenderTargetView(_backbuffers[i].get(), nullptr, handle);
	}

	{   D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D };
		desc.Width = _width;
		desc.Height = _height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = make_dxgi_format_typeless(_backbuffer_format);
		desc.SampleDesc = { 1, 0 };
		D3D12_HEAP_PROPERTIES props = { D3D12_HEAP_TYPE_DEFAULT };

		if (FAILED(_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&_backbuffer_texture))))
			return false;
#ifdef _DEBUG
		_backbuffer_texture->SetName(L"ReShade Backbuffer Texture");
#endif
	}


	{   D3D12_DESCRIPTOR_HEAP_DESC desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV };
		desc.NumDescriptors = 100;
		if (FAILED(_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_rtvs))))
			return false;
	}
	{   D3D12_DESCRIPTOR_HEAP_DESC desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV };
		desc.NumDescriptors = 100;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_srvs))))
			return false;
	}

	_rtv_cpu_handle = _rtvs->GetCPUDescriptorHandleForHeapStart();
	_srv_cpu_handle = _srvs->GetCPUDescriptorHandleForHeapStart();
	_srv_gpu_handle = _srvs->GetGPUDescriptorHandleForHeapStart();
	_srv_handle_size = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	_sampler_handle_size = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);


	_screenshot_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (_screenshot_event == nullptr)
		return false;
	if (HRESULT hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_screenshot_fence)); FAILED(hr))
		return false;

	return true;
}

bool reshade::d3d12::runtime_d3d12::on_init(const DXGI_SWAP_CHAIN_DESC &desc)
{
	RECT window_rect = {};
	GetClientRect(desc.OutputWindow, &window_rect);

	_width = desc.BufferDesc.Width;
	_height = desc.BufferDesc.Height;
	_window_width = window_rect.right - window_rect.left;
	_window_height = window_rect.bottom - window_rect.top;
	_backbuffer_format = desc.BufferDesc.Format;

	if (FAILED(_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_cmd_alloc))))
		return false;

	if (!init_backbuffer_textures(desc.BufferCount)
#if RESHADE_GUI
		|| !init_imgui_resources()
#endif
		)
		return false;

	return runtime::on_init(desc.OutputWindow);
}
void reshade::d3d12::runtime_d3d12::on_reset()
{
	runtime::on_reset();

	_cmd_alloc.reset();

	_backbuffers.clear();
	_backbuffer_rtvs.reset();
	_backbuffer_texture.reset();

	_screenshot_fence.reset();
	CloseHandle(_screenshot_event);

#if RESHADE_GUI
	for (unsigned int resource_index = 0; resource_index < 3; ++resource_index)
	{
		_imgui_index_buffer_size[resource_index] = 0;
		_imgui_index_buffer[resource_index].reset();
		_imgui_vertex_buffer_size[resource_index] = 0;
		_imgui_vertex_buffer[resource_index].reset();
	}

	_imgui_cmd_list.reset();
	_imgui_pipeline.reset();
	_imgui_signature.reset();
#endif
}

void reshade::d3d12::runtime_d3d12::on_present()
{
	if (!_is_initialized)
		return;

	_swap_index = _swapchain->GetCurrentBackBufferIndex();

	_cmd_alloc->Reset();

	update_and_render_effects();
	runtime::on_present();
}

void reshade::d3d12::runtime_d3d12::capture_screenshot(uint8_t *buffer) const
{
	if (_backbuffer_format != DXGI_FORMAT_R8G8B8A8_UNORM &&
		_backbuffer_format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
		_backbuffer_format != DXGI_FORMAT_B8G8R8A8_UNORM &&
		_backbuffer_format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
	{
		LOG(WARN) << "Screenshots are not supported for back buffer format " << _backbuffer_format << '.';
		return;
	}

	const uint32_t data_pitch = _width * 4;
	const uint32_t download_pitch = (data_pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);

	D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_BUFFER };
	desc.Width = _height * download_pitch;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc = { 1, 0 };
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	D3D12_HEAP_PROPERTIES props = { D3D12_HEAP_TYPE_READBACK };

	com_ptr<ID3D12Resource> intermediate;
	if (FAILED(_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&intermediate))))
	{
		LOG(ERROR) << "Failed to create system memory texture for screenshot capture!";
		return;
	}

#ifdef _DEBUG
	intermediate->SetName(L"ReShade screenshot texture");
#endif

	const UINT swap_index = _swapchain->GetCurrentBackBufferIndex();

	const com_ptr<ID3D12GraphicsCommandList> cmd_list = create_command_list();
	transition_state(cmd_list, _backbuffers[swap_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
	{ // Copy data from upload buffer into target texture
		D3D12_TEXTURE_COPY_LOCATION src_location = { _backbuffers[swap_index].get() };
		src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src_location.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION dst_location = { intermediate.get() };
		dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst_location.PlacedFootprint.Footprint.Width = _width;
		dst_location.PlacedFootprint.Footprint.Height = _height;
		dst_location.PlacedFootprint.Footprint.Depth = 1;
		dst_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		dst_location.PlacedFootprint.Footprint.RowPitch = download_pitch;

		cmd_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
	}
	transition_state(cmd_list, _backbuffers[swap_index], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT, 0);

	if (FAILED(cmd_list->Close()))
		return;

	execute_command_list(cmd_list); // Execute and wait for completion

	// Copy data from system memory texture into output buffer
	uint8_t *mapped_data;
	if (FAILED(intermediate->Map(0, nullptr, reinterpret_cast<void **>(&mapped_data))))
		return;

	for (uint32_t y = 0; y < _height; y++, buffer += data_pitch, mapped_data += download_pitch)
	{
		memcpy(buffer, mapped_data, data_pitch);
		for (uint32_t x = 0; x < data_pitch; x += 4)
			buffer[x + 3] = 0xFF; // Clear alpha channel
	}

	intermediate->Unmap(0, nullptr);
}

bool reshade::d3d12::runtime_d3d12::init_texture(texture &info)
{
	info.impl = std::make_unique<d3d12_tex_data>();

	if (info.impl_reference != texture_reference::none)
		return update_texture_reference(info);

	D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D };
	desc.Width = info.width;
	desc.Height = info.height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = static_cast<UINT16>(info.levels);
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc = { 1, 0 };
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; // Textures may be bound as render target

	switch (info.format)
	{
	case reshadefx::texture_format::r8:
		desc.Format = DXGI_FORMAT_R8_UNORM;
		break;
	case reshadefx::texture_format::r16f:
		desc.Format = DXGI_FORMAT_R16_FLOAT;
		break;
	case reshadefx::texture_format::r32f:
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		break;
	case reshadefx::texture_format::rg8:
		desc.Format = DXGI_FORMAT_R8G8_UNORM;
		break;
	case reshadefx::texture_format::rg16:
		desc.Format = DXGI_FORMAT_R16G16_UNORM;
		break;
	case reshadefx::texture_format::rg16f:
		desc.Format = DXGI_FORMAT_R16G16_FLOAT;
		break;
	case reshadefx::texture_format::rg32f:
		desc.Format = DXGI_FORMAT_R32G32_FLOAT;
		break;
	case reshadefx::texture_format::rgba8:
		desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		break;
	case reshadefx::texture_format::rgba16:
		desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
		break;
	case reshadefx::texture_format::rgba16f:
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		break;
	case reshadefx::texture_format::rgba32f:
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		break;
	case reshadefx::texture_format::dxt1:
		desc.Format = DXGI_FORMAT_BC1_TYPELESS;
		break;
	case reshadefx::texture_format::dxt3:
		desc.Format = DXGI_FORMAT_BC2_TYPELESS;
		break;
	case reshadefx::texture_format::dxt5:
		desc.Format = DXGI_FORMAT_BC3_TYPELESS;
		break;
	case reshadefx::texture_format::latc1:
		desc.Format = DXGI_FORMAT_BC4_UNORM;
		break;
	case reshadefx::texture_format::latc2:
		desc.Format = DXGI_FORMAT_BC5_UNORM;
		break;
	}

	D3D12_HEAP_PROPERTIES props = { D3D12_HEAP_TYPE_DEFAULT };

	// Render targets are always either cleared to zero or not cleared at all (see 'ClearRenderTargets' pass state), so can set the optimized clear value here to zero
	D3D12_CLEAR_VALUE clear_value = {};
	clear_value.Format = make_dxgi_format_normal(desc.Format);

	const auto texture_data = info.impl->as<d3d12_tex_data>();

	if (HRESULT hr = _device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear_value, IID_PPV_ARGS(&texture_data->resource)); FAILED(hr))
	{
		LOG(ERROR) << "Failed to create texture '" << info.unique_name << "' ("
			"Width = " << desc.Width << ", "
			"Height = " << desc.Height << ", "
			"Format = " << desc.Format << ")! "
			"HRESULT is '" << std::hex << hr << std::dec << "'.";
		return false;
	}

#ifdef _DEBUG
	std::wstring debug_name;
	debug_name.reserve(info.unique_name.size());
	utf8::unchecked::utf8to16(info.unique_name.begin(), info.unique_name.end(), std::back_inserter(debug_name));
	texture_data->resource->SetName(debug_name.c_str());
#endif

	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = make_dxgi_format_normal(desc.Format);
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv_desc.Texture2D.MipLevels = desc.MipLevels;

	assert((_srv_cpu_handle.ptr - _srvs->GetCPUDescriptorHandleForHeapStart().ptr) / _srv_handle_size < 100);
	texture_data->srv_cpu_handle[0] = _srv_cpu_handle; _srv_cpu_handle.ptr += _srv_handle_size;
	texture_data->srv_gpu_handle[0] = _srv_gpu_handle; _srv_gpu_handle.ptr += _srv_handle_size;
	_device->CreateShaderResourceView(texture_data->resource.get(), &srv_desc, texture_data->srv_cpu_handle[0]);
		
	srv_desc.Format = make_dxgi_format_srgb(desc.Format);
	if (srv_desc.Format != desc.Format)
	{
		texture_data->srv_cpu_handle[1] = _srv_cpu_handle; _srv_cpu_handle.ptr += _srv_handle_size;
		texture_data->srv_gpu_handle[1] = _srv_gpu_handle; _srv_gpu_handle.ptr += _srv_handle_size;
		_device->CreateShaderResourceView(texture_data->resource.get(), &srv_desc, texture_data->srv_cpu_handle[1]);
	}
	else
	{
		texture_data->srv_cpu_handle[1] = texture_data->srv_cpu_handle[0];
		texture_data->srv_gpu_handle[1] = texture_data->srv_gpu_handle[0];
	}

	D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
	rtv_desc.Format = make_dxgi_format_normal(desc.Format);
	rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

	assert((_rtv_cpu_handle.ptr - _rtvs->GetCPUDescriptorHandleForHeapStart().ptr) / _srv_handle_size < 100);
	texture_data->rtv_cpu_handle[0] = _rtv_cpu_handle; _rtv_cpu_handle.ptr += _rtv_handle_size;
	_device->CreateRenderTargetView(texture_data->resource.get(), &rtv_desc, texture_data->rtv_cpu_handle[0]);

	rtv_desc.Format = make_dxgi_format_srgb(desc.Format);
	if (rtv_desc.Format != desc.Format)
	{
		texture_data->rtv_cpu_handle[1] = _rtv_cpu_handle; _rtv_cpu_handle.ptr += _rtv_handle_size;
		_device->CreateRenderTargetView(texture_data->resource.get(), &rtv_desc, texture_data->rtv_cpu_handle[1]);
	}
	else
	{
		texture_data->rtv_cpu_handle[1] = texture_data->rtv_cpu_handle[0];
	}

	return true;
}
void reshade::d3d12::runtime_d3d12::upload_texture(texture &texture, const uint8_t *pixels)
{
	assert(texture.impl_reference == texture_reference::none);

	const uint32_t data_pitch = texture.width * 4;
	const uint32_t upload_pitch = (data_pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);

	D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_BUFFER };
	desc.Width = texture.height * upload_pitch;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc = { 1, 0 };
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	D3D12_HEAP_PROPERTIES props = { D3D12_HEAP_TYPE_UPLOAD };

	com_ptr<ID3D12Resource> intermediate;
	if (FAILED(_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&intermediate))))
	{
		LOG(ERROR) << "Failed to create system memory texture for texture updating!";
		return;
	}

#ifdef _DEBUG
	intermediate->SetName(L"ReShade upload texture");
#endif

	// Fill upload buffer with pixel data
	uint8_t *mapped_data;
	if (FAILED(intermediate->Map(0, nullptr, reinterpret_cast<void **>(&mapped_data))))
		return;

	switch (texture.format)
	{
	case reshadefx::texture_format::r8:
		for (uint32_t y = 0; y < texture.height; ++y, mapped_data += upload_pitch, pixels += data_pitch)
			for (uint32_t x = 0; x < texture.width; ++x)
				mapped_data[x] = pixels[x * 4];
		break;
	case reshadefx::texture_format::rg8:
		for (uint32_t y = 0; y < texture.height; ++y, mapped_data += upload_pitch, pixels += data_pitch)
			for (uint32_t x = 0; x < texture.width; ++x)
				mapped_data[x * 2 + 0] = pixels[x * 4 + 0],
				mapped_data[x * 2 + 1] = pixels[x * 4 + 1];
		break;
	case reshadefx::texture_format::rgba8:
		for (uint32_t y = 0; y < texture.height; ++y, mapped_data += upload_pitch, pixels += data_pitch)
			memcpy(mapped_data, pixels, data_pitch);
		break;
	default:
		LOG(ERROR) << "Texture upload is not supported for format " << static_cast<unsigned int>(texture.format) << '!';
		break;
	}

	intermediate->Unmap(0, nullptr);

	const auto texture_impl = texture.impl->as<d3d12_tex_data>();

	assert(pixels != nullptr);
	assert(texture_impl != nullptr);

	const com_ptr<ID3D12GraphicsCommandList> cmd_list = create_command_list();
	transition_state(cmd_list, texture_impl->resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST, 0);
	{ // Copy data from upload buffer into target texture
		D3D12_TEXTURE_COPY_LOCATION src_location = { intermediate.get() };
		src_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src_location.PlacedFootprint.Footprint.Width = texture.width;
		src_location.PlacedFootprint.Footprint.Height = texture.height;
		src_location.PlacedFootprint.Footprint.Depth = 1;
		src_location.PlacedFootprint.Footprint.Format = texture_impl->resource->GetDesc().Format;
		src_location.PlacedFootprint.Footprint.RowPitch = upload_pitch;

		D3D12_TEXTURE_COPY_LOCATION dst_location = { texture_impl->resource.get() };
		dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst_location.SubresourceIndex = 0;

		cmd_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
	}
	transition_state(cmd_list, texture_impl->resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 0);

	// TODO: Generate mipmaps

	if (FAILED(cmd_list->Close()))
		return;

	execute_command_list(cmd_list);
}
bool reshade::d3d12::runtime_d3d12::update_texture_reference(texture &texture)
{
	// TODO
	return true;
}

bool reshade::d3d12::runtime_d3d12::compile_effect(effect_data &effect)
{
	if (_d3d_compiler == nullptr)
		_d3d_compiler = LoadLibraryW(L"d3dcompiler_47.dll");

	if (_d3d_compiler == nullptr)
	{
		LOG(ERROR) << "Unable to load D3DCompiler library.";
		return false;
	}

	const auto D3DCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(_d3d_compiler, "D3DCompile"));

	const std::string hlsl = effect.preamble + effect.module.hlsl;

	std::unordered_map<std::string, com_ptr<ID3DBlob>> entry_points;

	// Compile the generated HLSL source code to DX byte code
	for (const auto &entry_point : effect.module.entry_points)
	{
		std::string profile = entry_point.second ? "ps_5_0" : "vs_5_0";

		com_ptr<ID3DBlob> d3d_errors;

		HRESULT hr = D3DCompile(hlsl.c_str(), hlsl.size(), nullptr, nullptr, nullptr, entry_point.first.c_str(), profile.c_str(), D3DCOMPILE_ENABLE_STRICTNESS, 0, &entry_points[entry_point.first], &d3d_errors);

		if (d3d_errors != nullptr) // Append warnings to the output error string as well
			effect.errors.append(static_cast<const char *>(d3d_errors->GetBufferPointer()), d3d_errors->GetBufferSize() - 1); // Subtracting one to not append the null-terminator as well

		// No need to setup resources if any of the shaders failed to compile
		if (FAILED(hr))
			return false;
	}

	if (_effect_data.size() <= effect.index)
		_effect_data.resize(effect.index + 1);

	d3d12_effect_data &effect_data = _effect_data[effect.index];
	effect_data.storage_size = effect.storage_size;
	effect_data.storage_offset = effect.storage_offset;

	{   D3D12_DESCRIPTOR_RANGE srv_range = {};
		srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srv_range.NumDescriptors = effect.module.num_texture_bindings;
		srv_range.BaseShaderRegister = 0;
		D3D12_DESCRIPTOR_RANGE sampler_range = {};
		sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		sampler_range.NumDescriptors = effect.module.num_sampler_bindings;
		sampler_range.BaseShaderRegister = 0;

		D3D12_ROOT_PARAMETER params[3] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[0].Descriptor.ShaderRegister = 0; // b0 (global constant buffer)
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &srv_range;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].DescriptorTable.NumDescriptorRanges = 1;
		params[2].DescriptorTable.pDescriptorRanges = &sampler_range;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.NumParameters = ARRAYSIZE(params);
		desc.pParameters = params;
		desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		if (com_ptr<ID3DBlob> blob; SUCCEEDED(hooks::call(D3D12SerializeRootSignature)(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)))
			_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&effect_data.signature));
		else
			return false;
	}


	if (effect.storage_size != 0)
	{
		D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_BUFFER };
		desc.Width = effect.storage_size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc = { 1, 0 };
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		D3D12_HEAP_PROPERTIES props = { D3D12_HEAP_TYPE_UPLOAD };

		if (FAILED(_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&effect_data.cb))))
			return false;
#ifdef _DEBUG
		effect_data.cb->SetName(L"ReShade Global CB");
#endif

		effect_data.cbv_gpu_address = effect_data.cb->GetGPUVirtualAddress();
	}

	{   D3D12_DESCRIPTOR_HEAP_DESC desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV };
		desc.NumDescriptors = effect.module.num_texture_bindings;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

		if (FAILED(_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&effect_data.srv_heap))))
			return false;

		effect_data.srv_cpu_base = effect_data.srv_heap->GetCPUDescriptorHandleForHeapStart();
		effect_data.srv_gpu_base = effect_data.srv_heap->GetGPUDescriptorHandleForHeapStart();
	}

	{   D3D12_DESCRIPTOR_HEAP_DESC desc = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER };
		desc.NumDescriptors = effect.module.num_sampler_bindings;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

		if (FAILED(_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&effect_data.sampler_heap))))
			return false;

		effect_data.sampler_cpu_base = effect_data.sampler_heap->GetCPUDescriptorHandleForHeapStart();
		effect_data.sampler_gpu_base = effect_data.sampler_heap->GetGPUDescriptorHandleForHeapStart();
	}

	bool success = true;

	for (const reshadefx::sampler_info &info : effect.module.samplers)
	{
		if (info.binding >= D3D12_COMMONSHADER_SAMPLER_SLOT_COUNT)
		{
			LOG(ERROR) << "Cannot bind sampler '" << info.unique_name << "' since it exceeds the maximum number of allowed sampler slots in D3D12 (" << info.binding << ", allowed are up to " << D3D12_COMMONSHADER_SAMPLER_SLOT_COUNT << ").";
			return false;
		}
		if (info.texture_binding >= D3D12_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
		{
			LOG(ERROR) << "Cannot bind texture '" << info.texture_name << "' since it exceeds the maximum number of allowed resource slots in D3D12 (" << info.texture_binding << ", allowed are up to " << D3D12_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT << ").";
			return false;
		}

		const auto existing_texture = std::find_if(_textures.begin(), _textures.end(),
			[&texture_name = info.texture_name](const auto &item) {
			return item.unique_name == texture_name && item.impl != nullptr;
		});
		if (existing_texture == _textures.end())
			return false;

		com_ptr<ID3D12Resource> resource;
		switch (existing_texture->impl_reference)
		{
		case texture_reference::back_buffer:
			resource = _backbuffer_texture;
			break;
		case texture_reference::depth_buffer:
			break; // TODO
		default:
			resource = existing_texture->impl->as<d3d12_tex_data>()->resource;
			break;
		}

		if (resource == nullptr)
			continue;

		{   D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
			desc.Format = info.srgb ?
				make_dxgi_format_srgb(resource->GetDesc().Format) :
				make_dxgi_format_normal(resource->GetDesc().Format);
			desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			desc.Texture2D.MipLevels = existing_texture->levels;

			D3D12_CPU_DESCRIPTOR_HANDLE srv_handle = effect_data.srv_cpu_base;
			srv_handle.ptr += info.texture_binding * _srv_handle_size;

			_device->CreateShaderResourceView(resource.get(), &desc, srv_handle);
		}

		{   D3D12_SAMPLER_DESC desc = {};
			desc.Filter = static_cast<D3D12_FILTER>(info.filter);
			desc.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(info.address_u);
			desc.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(info.address_v);
			desc.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(info.address_w);
			desc.MipLODBias = info.lod_bias;
			desc.MaxAnisotropy = 1;
			desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			desc.MinLOD = info.min_lod;
			desc.MaxLOD = info.max_lod;

			D3D12_CPU_DESCRIPTOR_HANDLE sampler_handle = effect_data.sampler_cpu_base;
			sampler_handle.ptr += info.binding * _sampler_handle_size;

			// TODO: Only intialize if not yet initialized
			_device->CreateSampler(&desc, sampler_handle);
		}
	}

	for (technique &technique : _techniques)
		if (technique.impl == nullptr && technique.effect_index == effect.index)
			success &= init_technique(technique, entry_points);

	return success;
}
void reshade::d3d12::runtime_d3d12::unload_effects()
{
	runtime::unload_effects();

	_effect_data.clear();
}

bool reshade::d3d12::runtime_d3d12::init_technique(technique &technique, const std::unordered_map<std::string, com_ptr<ID3DBlob>> &entry_points)
{
	technique.impl = std::make_unique<d3d12_technique_data>();

	for (size_t pass_index = 0; pass_index < technique.passes.size(); ++pass_index)
	{
		technique.passes_data.push_back(std::make_unique<d3d12_pass_data>());

		auto &pass_data = *technique.passes_data.back()->as<d3d12_pass_data>();
		const auto &pass_info = technique.passes[pass_index];

		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
		pso_desc.pRootSignature = _effect_data[technique.effect_index].signature.get();

		const auto &VS = entry_points.at(pass_info.vs_entry_point);
		pso_desc.VS = { VS->GetBufferPointer(), VS->GetBufferSize() };
		const auto &PS = entry_points.at(pass_info.ps_entry_point);
		pso_desc.PS = { PS->GetBufferPointer(), PS->GetBufferSize() };

		pass_data.viewport.Width = static_cast<FLOAT>(pass_info.viewport_width);
		pass_data.viewport.Height = static_cast<FLOAT>(pass_info.viewport_height);
		pass_data.viewport.MaxDepth = 1.0f;

		pso_desc.NumRenderTargets = 1;
		pso_desc.RTVFormats[0] = _backbuffer_format;
		pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

		for (unsigned int k = 0; k < 8; k++)
		{
			if (pass_info.render_target_names[k].empty())
				continue; // Skip unbound render targets

			const auto render_target_texture = std::find_if(_textures.begin(), _textures.end(),
				[&render_target = pass_info.render_target_names[k]](const auto &item) {
				return item.unique_name == render_target;
			});
			if (render_target_texture == _textures.end())
				return assert(false), false;

			const auto texture_impl = render_target_texture->impl->as<d3d12_tex_data>();
			assert(texture_impl != nullptr);

			pso_desc.NumRenderTargets = k + 1;
			const DXGI_FORMAT format = texture_impl->resource->GetDesc().Format;
			pso_desc.RTVFormats[k] = pass_info.srgb_write_enable ? make_dxgi_format_srgb(format) : make_dxgi_format_normal(format);

			pass_data.render_targets[k] = texture_impl->rtv_cpu_handle[pass_info.srgb_write_enable ? 1 : 0];
		}

		pass_data.num_render_targets = pso_desc.NumRenderTargets;

		if (pass_data.viewport.Width == 0)
		{
			pass_data.viewport.Width = FLOAT(frame_width());
			pass_data.viewport.Height = FLOAT(frame_height());
		}

		pso_desc.SampleMask = UINT_MAX;
		pso_desc.SampleDesc = { 1, 0 };
		pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso_desc.NodeMask = 1;

		{   D3D12_BLEND_DESC &desc = pso_desc.BlendState;
			desc.RenderTarget[0].BlendEnable = pass_info.blend_enable;

			const auto literal_to_blend_func = [](unsigned int value) {
				switch (value) {
				default:
				case 1: return D3D12_BLEND_ONE;
				case 0: return D3D12_BLEND_ZERO;
				case 2: return D3D12_BLEND_SRC_COLOR;
				case 4: return D3D12_BLEND_INV_SRC_COLOR;
				case 3: return D3D12_BLEND_SRC_ALPHA;
				case 5: return D3D12_BLEND_INV_SRC_ALPHA;
				case 6: return D3D12_BLEND_DEST_ALPHA;
				case 7: return D3D12_BLEND_INV_DEST_ALPHA;
				case 8: return D3D12_BLEND_DEST_COLOR;
				case 9: return D3D12_BLEND_INV_DEST_COLOR;
				}
			};

			desc.RenderTarget[0].SrcBlend = literal_to_blend_func(pass_info.src_blend);
			desc.RenderTarget[0].DestBlend = literal_to_blend_func(pass_info.dest_blend);
			desc.RenderTarget[0].BlendOp = static_cast<D3D12_BLEND_OP>(pass_info.blend_op);
			desc.RenderTarget[0].SrcBlendAlpha = literal_to_blend_func(pass_info.src_blend_alpha);
			desc.RenderTarget[0].DestBlendAlpha = literal_to_blend_func(pass_info.dest_blend_alpha);
			desc.RenderTarget[0].BlendOpAlpha = static_cast<D3D12_BLEND_OP>(pass_info.blend_op_alpha);
			desc.RenderTarget[0].RenderTargetWriteMask = pass_info.color_write_mask;
		}

		{   D3D12_RASTERIZER_DESC &desc = pso_desc.RasterizerState;
			desc.FillMode = D3D12_FILL_MODE_SOLID;
			desc.CullMode = D3D12_CULL_MODE_NONE;
			desc.DepthClipEnable = true;
		}

		{   D3D12_DEPTH_STENCIL_DESC &desc = pso_desc.DepthStencilState;
			desc.DepthEnable = FALSE;
			desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

			const auto literal_to_stencil_op = [](unsigned int value) {
				switch (value) {
				default:
				case 1: return D3D12_STENCIL_OP_KEEP;
				case 0: return D3D12_STENCIL_OP_ZERO;
				case 3: return D3D12_STENCIL_OP_REPLACE;
				case 4: return D3D12_STENCIL_OP_INCR_SAT;
				case 5: return D3D12_STENCIL_OP_DECR_SAT;
				case 6: return D3D12_STENCIL_OP_INVERT;
				case 7: return D3D12_STENCIL_OP_INCR;
				case 8: return D3D12_STENCIL_OP_DECR;
				}
			};

			desc.StencilEnable = pass_info.stencil_enable;
			desc.StencilReadMask = pass_info.stencil_read_mask;
			desc.StencilWriteMask = pass_info.stencil_write_mask;
			desc.FrontFace.StencilFailOp = literal_to_stencil_op(pass_info.stencil_op_fail);
			desc.FrontFace.StencilDepthFailOp = literal_to_stencil_op(pass_info.stencil_op_depth_fail);
			desc.FrontFace.StencilPassOp = literal_to_stencil_op(pass_info.stencil_op_pass);
			desc.FrontFace.StencilFunc = static_cast<D3D12_COMPARISON_FUNC>(pass_info.stencil_comparison_func);
			desc.BackFace = desc.FrontFace;
		}

		if (HRESULT hr = _device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pass_data.pipeline)); FAILED(hr))
		{
			LOG(ERROR) << "Failed to create pipeline for pass " << pass_index << " in technique '" << technique.name << "'! "
				"HRESULT is '" << std::hex << hr << std::dec << "'.";
			return false;
		}
	}

	return true;
}

void reshade::d3d12::runtime_d3d12::render_technique(technique &technique)
{
	bool is_default_depthstencil_cleared = false;
	d3d12_effect_data &effect_data = _effect_data[technique.effect_index];

	const com_ptr<ID3D12GraphicsCommandList> cmd_list = create_command_list();

	ID3D12DescriptorHeap *const descriptor_heaps[] = { effect_data.srv_heap.get(), effect_data.sampler_heap.get() };
	cmd_list->SetDescriptorHeaps(ARRAYSIZE(descriptor_heaps), descriptor_heaps);
	cmd_list->SetGraphicsRootSignature(effect_data.signature.get());

	// Setup vertex input
	cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Setup shader constants
	if (effect_data.storage_size != 0)
	{
		void *mapped;
		effect_data.cb->Map(0, nullptr, &mapped);
		memcpy(mapped, _uniform_data_storage.data() + effect_data.storage_offset, effect_data.storage_size);
		effect_data.cb->Unmap(0, nullptr);

		cmd_list->SetGraphicsRootConstantBufferView(0, effect_data.cbv_gpu_address);
	}

	// Setup shader resources
	cmd_list->SetGraphicsRootDescriptorTable(1, effect_data.srv_gpu_base);

	// Setup samplers
	cmd_list->SetGraphicsRootDescriptorTable(2, effect_data.sampler_gpu_base);

	transition_state(cmd_list, _backbuffers[_swap_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

	for (size_t i = 0; i < technique.passes.size(); ++i)
	{
		const auto &pass_info = technique.passes[i];
		const auto &pass_data = *technique.passes_data[i]->as<d3d12_pass_data>();

		// Save back buffer of previous pass
		transition_state(cmd_list, _backbuffer_texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		transition_state(cmd_list, _backbuffers[_swap_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		cmd_list->CopyResource(_backbuffer_texture.get(), _backbuffers[_swap_index].get());
		transition_state(cmd_list, _backbuffer_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		transition_state(cmd_list, _backbuffers[_swap_index], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

		// Setup states
		cmd_list->SetPipelineState(pass_data.pipeline.get());
		cmd_list->OMSetStencilRef(pass_info.stencil_reference_value);

		// Setup render targets
		const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		if (pass_data.render_targets[0].ptr == 0)
		{
			assert(pass_data.num_render_targets == 1);

			D3D12_CPU_DESCRIPTOR_HANDLE render_target = { _backbuffer_rtvs->GetCPUDescriptorHandleForHeapStart().ptr + _swap_index * _rtv_handle_size };
			cmd_list->OMSetRenderTargets(1, &render_target, false, nullptr/*&_default_depthstencil*/);

			if (pass_info.clear_render_targets)
				cmd_list->ClearRenderTargetView(render_target, clear_color, 0, nullptr);
		}
		else if (_width == UINT(pass_data.viewport.Width) && _height == UINT(pass_data.viewport.Height))
		{
			cmd_list->OMSetRenderTargets(pass_data.num_render_targets, pass_data.render_targets, false, nullptr/*&_default_depthstencil*/);

			if (pass_info.clear_render_targets)
				for (UINT k = 0; k < pass_data.num_render_targets; ++k)
					cmd_list->ClearRenderTargetView(pass_data.render_targets[k], clear_color, 0, nullptr);

			/*if (!is_default_depthstencil_cleared)
			{
				is_default_depthstencil_cleared = true;

				cmd_list->ClearDepthStencilView(_default_depthstencil, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			}*/
		}
		else
		{
			cmd_list->OMSetRenderTargets(pass_data.num_render_targets, pass_data.render_targets, false, nullptr);

			if (pass_info.clear_render_targets)
				for (UINT k = 0; k < pass_data.num_render_targets; ++k)
					cmd_list->ClearRenderTargetView(pass_data.render_targets[k], clear_color, 0, nullptr);
		}

		cmd_list->RSSetViewports(1, &pass_data.viewport);

		D3D12_RECT scissor_rect = { 0, 0, LONG(pass_data.viewport.Width), LONG(pass_data.viewport.Height) };
		cmd_list->RSSetScissorRects(1, &scissor_rect);

		// Draw triangle
		cmd_list->DrawInstanced(3, 1, 0, 0);

		_vertices += 3;
		_drawcalls += 1;

		// TODO: Generate mipmaps
	}

	transition_state(cmd_list, _backbuffers[_swap_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

	if (FAILED(cmd_list->Close()))
		return;

	execute_command_list_async(cmd_list);
}

#if RESHADE_GUI
bool reshade::d3d12::runtime_d3d12::init_imgui_resources()
{
	if (FAILED(_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _cmd_alloc.get(), NULL, IID_PPV_ARGS(&_imgui_cmd_list))) ||
		FAILED(_imgui_cmd_list->Close())) // Close command list immediately since it is reset every frame in 'render_imgui_draw_data'
		return false;

	{   D3D12_DESCRIPTOR_RANGE range = {};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		range.BaseShaderRegister = 0;

		D3D12_ROOT_PARAMETER params[2] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.ShaderRegister = 0;
		params[0].Constants.Num32BitValues = 16;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &range;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_STATIC_SAMPLER_DESC samplers[1] = {};
		samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.NumParameters = ARRAYSIZE(params);
		desc.pParameters = params;
		desc.NumStaticSamplers = ARRAYSIZE(samplers);
		desc.pStaticSamplers = samplers;
		desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		if (com_ptr<ID3DBlob> blob; SUCCEEDED(hooks::call(D3D12SerializeRootSignature)(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)))
			_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_imgui_signature));
		else
			return false;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
	pso_desc.pRootSignature = _imgui_signature.get();
	pso_desc.SampleMask = UINT_MAX;
	pso_desc.NumRenderTargets = 1;
	pso_desc.RTVFormats[0] = _backbuffer_format;
	pso_desc.SampleDesc = { 1, 0 };
	pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso_desc.NodeMask = 1;

	{   const resources::data_resource vs = resources::load_data_resource(IDR_RCDATA3);
		pso_desc.VS = { vs.data, vs.data_size };

		static const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, offsetof(ImDrawVert, pos), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, offsetof(ImDrawVert, uv ), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(ImDrawVert, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
		pso_desc.InputLayout = { input_layout, ARRAYSIZE(input_layout) };
	}
	{   const resources::data_resource ps = resources::load_data_resource(IDR_RCDATA4);
		pso_desc.PS = { ps.data, ps.data_size };
	}

	{   D3D12_BLEND_DESC &desc = pso_desc.BlendState;
		desc.RenderTarget[0].BlendEnable = true;
		desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		desc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
		desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}

	{   D3D12_RASTERIZER_DESC &desc = pso_desc.RasterizerState;
		desc.FillMode = D3D12_FILL_MODE_SOLID;
		desc.CullMode = D3D12_CULL_MODE_NONE;
		desc.DepthClipEnable = true;
	}

	{   D3D12_DEPTH_STENCIL_DESC &desc = pso_desc.DepthStencilState;
		desc.DepthEnable = false;
		desc.StencilEnable = false;
	}

	return SUCCEEDED(_device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&_imgui_pipeline)));
}

void reshade::d3d12::runtime_d3d12::render_imgui_draw_data(ImDrawData *draw_data)
{
	com_ptr<ID3D12GraphicsCommandList> &cmd_list = _imgui_cmd_list;
	cmd_list->Reset(_cmd_alloc.get(), _imgui_pipeline.get());

	// Create and grow vertex/index buffers if needed
	const unsigned int resource_index = _framecount % 3;
	if (_imgui_index_buffer_size[resource_index] < UINT(draw_data->TotalIdxCount))
	{
		_imgui_index_buffer[resource_index].reset();

		const UINT new_size = draw_data->TotalIdxCount + 10000;
		D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_BUFFER };
		desc.Width = new_size * sizeof(ImDrawIdx);
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc = { 1, 0 };
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		D3D12_HEAP_PROPERTIES props = { D3D12_HEAP_TYPE_UPLOAD };

		if (FAILED(_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&_imgui_index_buffer[resource_index]))))
			return;
#ifdef _DEBUG
		_imgui_index_buffer[resource_index]->SetName(L"ImGui Index Buffer");
#endif

		_imgui_index_buffer_size[resource_index] = new_size;
	}
	if (_imgui_vertex_buffer_size[resource_index] < UINT(draw_data->TotalVtxCount))
	{
		_imgui_vertex_buffer[resource_index].reset();

		const UINT new_size = draw_data->TotalVtxCount + 5000;
		D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_BUFFER };
		desc.Width = new_size * sizeof(ImDrawVert);
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc = { 1, 0 };
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		D3D12_HEAP_PROPERTIES props = { D3D12_HEAP_TYPE_UPLOAD };

		if (FAILED(_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&_imgui_vertex_buffer[resource_index]))))
			return;
#ifdef _DEBUG
		_imgui_index_buffer[resource_index]->SetName(L"ImGui Vertex Buffer");
#endif

		_imgui_vertex_buffer_size[resource_index] = new_size;
	}

	ImDrawIdx *idx_dst; ImDrawVert *vtx_dst;
	if (FAILED(_imgui_index_buffer[resource_index]->Map(0, nullptr, reinterpret_cast<void **>(&idx_dst))) ||
		FAILED(_imgui_vertex_buffer[resource_index]->Map(0, nullptr, reinterpret_cast<void **>(&vtx_dst))))
		return;

	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList *draw_list = draw_data->CmdLists[n];
		CopyMemory(idx_dst, draw_list->IdxBuffer.Data, draw_list->IdxBuffer.Size * sizeof(ImDrawIdx));
		CopyMemory(vtx_dst, draw_list->VtxBuffer.Data, draw_list->VtxBuffer.Size * sizeof(ImDrawVert));
		idx_dst += draw_list->IdxBuffer.Size;
		vtx_dst += draw_list->VtxBuffer.Size;
	}

	_imgui_index_buffer[resource_index]->Unmap(0, nullptr);
	_imgui_vertex_buffer[resource_index]->Unmap(0, nullptr);

	// Transition render target
	transition_state(cmd_list, _backbuffers[_swap_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

	// Setup orthographic projection matrix
	const float ortho_projection[16] = {
		2.0f / draw_data->DisplaySize.x, 0.0f,  0.0f, 0.0f,
		0.0f, -2.0f / draw_data->DisplaySize.y, 0.0f, 0.0f,
		0.0f,                            0.0f,  0.5f, 0.0f,
		-(2 * draw_data->DisplayPos.x + draw_data->DisplaySize.x) / draw_data->DisplaySize.x,
		+(2 * draw_data->DisplayPos.y + draw_data->DisplaySize.y) / draw_data->DisplaySize.y, 0.5f, 1.0f,
	};

	// Setup render state and render draw lists
	const D3D12_INDEX_BUFFER_VIEW index_buffer_view = {
		_imgui_index_buffer[resource_index]->GetGPUVirtualAddress(), _imgui_index_buffer_size[resource_index] * sizeof(ImDrawIdx), sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
	cmd_list->IASetIndexBuffer(&index_buffer_view);
	const D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view = {
		_imgui_vertex_buffer[resource_index]->GetGPUVirtualAddress(), _imgui_vertex_buffer_size[resource_index] * sizeof(ImDrawVert),  sizeof(ImDrawVert) };
	cmd_list->IASetVertexBuffers(0, 1, &vertex_buffer_view);
	cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D12DescriptorHeap *const descriptor_heaps[] = { _srvs.get() };
	cmd_list->SetDescriptorHeaps(ARRAYSIZE(descriptor_heaps), descriptor_heaps);
	cmd_list->SetGraphicsRootSignature(_imgui_signature.get());
	cmd_list->SetGraphicsRoot32BitConstants(0, sizeof(ortho_projection) / 4, ortho_projection, 0);
	const D3D12_VIEWPORT viewport = { 0, 0, draw_data->DisplaySize.x, draw_data->DisplaySize.y, 0.0f, 1.0f };
	cmd_list->RSSetViewports(1, &viewport);
	const FLOAT blend_factor[4] = { 0.f, 0.f, 0.f, 0.f };
	cmd_list->OMSetBlendFactor(blend_factor);
	D3D12_CPU_DESCRIPTOR_HANDLE render_target = { _backbuffer_rtvs->GetCPUDescriptorHandleForHeapStart().ptr + _swap_index * _rtv_handle_size };
	cmd_list->OMSetRenderTargets(1, &render_target, false, nullptr);

	UINT vtx_offset = 0, idx_offset = 0;
	for (int n = 0; n < draw_data->CmdListsCount; ++n)
	{
		const ImDrawList *const draw_list = draw_data->CmdLists[n];

		for (const ImDrawCmd &cmd : draw_list->CmdBuffer)
		{
			assert(cmd.UserCallback == nullptr);

			const D3D12_RECT scissor_rect = {
				static_cast<LONG>(cmd.ClipRect.x - draw_data->DisplayPos.x),
				static_cast<LONG>(cmd.ClipRect.y - draw_data->DisplayPos.y),
				static_cast<LONG>(cmd.ClipRect.z - draw_data->DisplayPos.x),
				static_cast<LONG>(cmd.ClipRect.w - draw_data->DisplayPos.y)
			};
			cmd_list->RSSetScissorRects(1, &scissor_rect);

			const D3D12_GPU_DESCRIPTOR_HANDLE descriptor_handle =
				static_cast<const d3d12_tex_data *>(cmd.TextureId)->srv_gpu_handle[0];
			cmd_list->SetGraphicsRootDescriptorTable(1, descriptor_handle);

			cmd_list->DrawIndexedInstanced(cmd.ElemCount, 1, idx_offset, vtx_offset, 0);

			idx_offset += cmd.ElemCount;
		}

		vtx_offset += draw_list->VtxBuffer.Size;
	}

	// Transition render target back to previous state
	transition_state(cmd_list, _backbuffers[_swap_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

	if (FAILED(cmd_list->Close()))
		return;

	execute_command_list_async(cmd_list);
}
#endif