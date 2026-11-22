#pragma once
// Helpers the NVFP4 model loaders share: reading an asset file, creating a committed buffer, and the two error
// checks. Each loader keeps its own state and namespace; only these mechanics are common, so they live here once
// instead of four times over.
#include <windows.h>

#include <d3d12.h>
#include <nvapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nvfp4 {

inline void Check(HRESULT result, const char* what) {
    if (FAILED(result)) throw std::runtime_error(std::string(what) + " failed: D3D12 status " + std::to_string(unsigned(result)));
}

inline void NvCheck(NvAPI_Status result, std::string_view what) {
    if (result != NVAPI_OK) throw std::runtime_error(std::string(what) + " failed: NVAPI status " + std::to_string(result));
}

// The bytes of an asset file; `kind` names the asset in the failure ("NVFP4", "FP8").
inline std::vector<unsigned char> ReadAsset(const std::filesystem::path& path, const char* kind = "NVFP4") {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("missing " + std::string(kind) + " asset " + path.string());
    std::vector<unsigned char> bytes(size_t(file.tellg()));
    file.seekg(0);
    if (bytes.empty() || !file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))
        throw std::runtime_error("cannot read " + std::string(kind) + " asset " + path.string());
    return bytes;
}

inline Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (heap == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)), "NVFP4 buffer allocation");
    return resource;
}

}  // namespace nvfp4
