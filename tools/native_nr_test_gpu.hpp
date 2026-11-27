#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "nr/ngx.hpp"

namespace uplift::native_nr_test {
using Microsoft::WRL::ComPtr;
constexpr auto READ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
void Check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void Hr(HRESULT value) { if (FAILED(value)) throw std::runtime_error("D3D12 failure: " + std::to_string(value)); }
void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* image, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  if (before == after) return;
  D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; list->ResourceBarrier(1u, &barrier);
}
struct Parameters : NVSDK_NGX_Parameter {
  using Value = std::variant<unsigned long long, float, double, unsigned int, int, ID3D11Resource*, ID3D12Resource*, void*>;
  std::unordered_map<std::string, Value> values;
#define PARAMETER(Type) \
  void Set(const char* key, Type value) override { values[key] = value; } \
  NVSDK_NGX_Result Get(const char* key, Type* value) const override { \
    const auto found = values.find(key); \
    if (found != values.end()) if (const auto result = std::get_if<Type>(&found->second)) { *value = *result; return NVSDK_NGX_Result_Success; } \
    return NVSDK_NGX_Result_Fail; \
  }
  PARAMETER(unsigned long long) PARAMETER(float) PARAMETER(double) PARAMETER(unsigned int) PARAMETER(int)
  PARAMETER(ID3D11Resource*) PARAMETER(ID3D12Resource*) PARAMETER(void*)
#undef PARAMETER
  void Reset() override { values.clear(); }
};
struct Gpu {
  ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence; ComPtr<ID3D12InfoQueue> validation;
  std::vector<ComPtr<ID3D12Resource>> uploads;
  uint64_t serial = 0u; HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Gpu() {
    ComPtr<ID3D12Debug> debug; if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0u; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == S_OK; ++i) {
      DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
      if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        std::wcout << L"GPU: " << desc.Description << L'\n'; break;
      }
    }
    if (!device) { Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))); Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))); }
    device.As(&validation);
    D3D12_COMMAND_QUEUE_DESC queue_desc{}; Hr(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    Hr(device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    Hr(device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  }
  ~Gpu() { CloseHandle(event); }
  ComPtr<ID3D12Resource> Buffer(uint64_t bytes, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1u; desc.SampleDesc.Count = 1u; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
        : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result))); return result;
  }
  ComPtr<ID3D12Resource> Texture(UINT width, UINT height, DXGI_FORMAT format, const void* pixels, UINT pixel_bytes,
                                D3D12_RESOURCE_STATES state = READ, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1u; desc.SampleDesc.Count = 1u; desc.Format = format; desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; uint64_t bytes = 0u;
    device->GetCopyableFootprints(&desc, 0u, 1u, 0u, &footprint, nullptr, nullptr, &bytes);
    auto upload = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD); void* mapped = nullptr; Hr(upload->Map(0u, nullptr, &mapped));
    for (UINT y = 0u; y < height; ++y) std::memcpy(static_cast<char*>(mapped) + y * footprint.Footprint.RowPitch,
        static_cast<const char*>(pixels) + size_t(y) * width * pixel_bytes, size_t(width) * pixel_bytes);
    upload->Unmap(0u, nullptr);
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION target{}; target.pResource = result.Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&target, 0u, 0u, 0u, &source, nullptr); Barrier(list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
    uploads.push_back(upload); return result;
  }
  struct Read {
    ComPtr<ID3D12Resource> buffer; UINT pitch;
    template<class T> T At(UINT x, UINT y, UINT channels = 1u, UINT channel = 0u) const {
      void* mapped = nullptr; Hr(buffer->Map(0u, nullptr, &mapped)); T result;
      std::memcpy(&result, static_cast<char*>(mapped) + size_t(y) * pitch + (size_t(x) * channels + channel) * sizeof(T), sizeof(T));
      buffer->Unmap(0u, nullptr); return result;
    }
  };
  Read Readback(ID3D12Resource* image, D3D12_RESOURCE_STATES state) {
    const auto desc = image->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; uint64_t bytes = 0u;
    device->GetCopyableFootprints(&desc, 0u, 1u, 0u, &footprint, nullptr, nullptr, &bytes); auto buffer = Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
    Barrier(list.Get(), image, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = image; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION target{}; target.pResource = buffer.Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; target.PlacedFootprint = footprint;
    list->CopyTextureRegion(&target, 0u, 0u, 0u, &source, nullptr); Barrier(list.Get(), image, D3D12_RESOURCE_STATE_COPY_SOURCE, state); return {buffer, footprint.Footprint.RowPitch};
  }
  void Submit() {
    Hr(list->Close()); ID3D12CommandList* commands = list.Get(); queue->ExecuteCommandLists(1u, &commands);
    Hr(queue->Signal(fence.Get(), ++serial)); Hr(fence->SetEventOnCompletion(serial, event));
    Check(WaitForSingleObject(event, 30000u) == WAIT_OBJECT_0, "GPU test timed out");
    if (validation) {
      for (uint64_t i = 0u; i < validation->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T bytes = 0u; validation->GetMessage(i, nullptr, &bytes); std::vector<char> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); Hr(validation->GetMessage(i, message, &bytes));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::cerr << message->pDescription << '\n'; throw std::runtime_error("D3D12 validation error"); }
      }
      validation->ClearStoredMessages();
    }
    uploads.clear(); Hr(allocator->Reset()); Hr(list->Reset(allocator.Get(), nullptr));
  }
};

}  // namespace uplift::native_nr_test
