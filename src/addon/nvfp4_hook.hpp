#pragma once

#include <filesystem>
#include <string>

namespace uplift::addon {

// Hooks nvapi64.dll's nvapi_QueryInterface so the 4-bit NR kernels can replace the runtime's Model A
// launches. Installed on the nvapi-available poll, like the FG preset and Reflex hooks. The layers stay
// dormant until RequestNvfp4() calls Configure(), which needs the add-on's asset directory.
void EnsureNvfp4Hook();

// Call once after the add-on's directory is known. The loader looks for `<addon_directory>/nvfp4`.
void SetNvfp4AssetsDirectory(const std::filesystem::path& addon_directory);

// Enables (or disables) the 4-bit kernels before a frame's NR evaluations. Uses the directory from
// SetNvfp4AssetsDirectory(). Configure is idempotent for an unchanged path, so a per-frame call is cheap.
void RequestNvfp4(bool wanted);

// True when a D3D12 frame asked for a 4-bit model but the kernels could not run it (missing assets,
// wrong runtime, unsupported path). The caller must skip presenting that frame's NR result.
bool Nvfp4Refused();

// A short reason for the overlay when the last evaluation was refused; empty otherwise.
std::string Nvfp4Problem();

// Vulkan has no CUDA-on-D3D12 path: a 4-bit choice is refused up front, with a reason.
void RefuseNvfp4(const char* reason);

}  // namespace uplift::addon
