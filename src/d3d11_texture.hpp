#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <optional>

namespace uplift::d3d {

// Nullopt for a null resource or anything other than a 2D texture.
[[nodiscard]] inline std::optional<D3D11_TEXTURE2D_DESC> TextureDesc11(ID3D11Resource* resource) {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  if (resource == nullptr || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return std::nullopt;
  D3D11_TEXTURE2D_DESC description = {};
  texture->GetDesc(&description);
  return description;
}

}  // namespace uplift::d3d
