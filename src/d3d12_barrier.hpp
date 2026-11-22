#pragma once

#include <d3d12.h>

namespace uplift::d3d {

// Callers own the state tracking and ordering; this only emits the requested whole-resource barrier.
inline void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE};
  barrier.Transition = {.pResource = resource, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        .StateBefore = before, .StateAfter = after};
  list->ResourceBarrier(1u, &barrier);
}

}  // namespace uplift::d3d
