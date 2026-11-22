#pragma once

#include <array>
#include <bit>
#include <cstdint>

#include "look/look_math.hpp"
#include "nr/types.hpp"

namespace uplift::color {

// faces_cs.hlsl's identical constant layout in the D3D12 and Vulkan pipelines.
[[nodiscard]] inline std::array<uint32_t, 16> FacesConstants(nr::Size size, uint32_t levels, uint32_t mode, uint32_t level,
                                                          uint32_t flags, const look::FacesParameters& parameters) {
  const auto bits = [](float value) { return std::bit_cast<uint32_t>(value); };
  return {size.width, size.height, levels, mode, level, flags, bits(parameters.low_level), bits(parameters.mask_level),
          bits(parameters.edge_level), bits(parameters.dead_zone), bits(parameters.full_weight), bits(parameters.coverage_gain), bits(parameters.density),
          parameters.speck_radius, bits(parameters.fill_level), bits(parameters.fill_tolerance)};
}

}  // namespace uplift::color
