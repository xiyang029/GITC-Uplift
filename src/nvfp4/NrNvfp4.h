#pragma once
// The 4-bit (NVFP4) kernels and the pass-in-one weight sets of the Display Filter application
// (standalone/NrCapture/nvfp4: NrFfnV2, NrSwin4, NrNet4) for the D3D12 NR path. They sit below pass borrowing in the
// NvAPI hook and replace launches of the NR runtime's Model A with our own kernels; a pass-in-one model is the same kernels
// with a weight set of its own. Self-contained (no precompiled header).
//
// A pass asks for them with a model choice above the runtime's own presets (DlssNr Preset / pass preset):
//   4  Model A with the 4-bit kernels (fused blocks 256 and 128, split blocks, transformer, feed-forward; trained weights)
//   15 NVFP4 3xPass-in-one (weight set 12)
//   16 NVFP4 2xPass-in-one (weight set 13)
//   18 FP8 2xPass-in-one, 19 FP8 3xPass-in-one: NVIDIA's own FP8 kernels with weight records of ours (NrFp8; RTX 40
//       and 50 series; files nvfp4/fp8/fp8_set1.* and fp8_set2.*). No 4-bit kernel runs in such a pass.
//   20 FP8 2xPass-in-one v2, 21 FP8 3xPass-in-one v2: the same with their v2 weights (fp8_set3.*, fp8_set4.*).
// Any other value from 4 up (including retired presets 5..14 and 17) is refused, never run as another model.
// The runtime itself is given preset 1 (Model A) for custom models, whose launches are replaced one by one.
// Assets: <OptiScaler.dll's folder>/OptiScaler/nvfp4, laid out as the Display Filter package's nvfp4 folder.
//
// NO FALLBACK. A pass that asks for one of these and does not get it (files missing, another runtime build, the Vulkan
// path, a layer that did not recognise the model) is never shown as a plain Model A pass: OursRan() says so, the
// caller leaves the picture unprocessed, and Problem() gives the reason for the menu and the log.
#include <filesystem>
#include <string>

struct ID3D12GraphicsCommandList;
struct ID3D12Device;

namespace DlssNr::Nvfp4
{
// The 4-bit numbers are the Display Filter application's (standalone/NrCapture/Common.h).
// Keep the existing local FP8 IDs (18-21) so saved presets select the same weights.
inline constexpr unsigned ModelANvfp4 = 4,
                          ModelTwoTriple = 15, // one pass with the look of three
                          ModelTwoDouble = 16, // one pass with the look of two
                          ModelFp8Double = 18, // FP8: one pass with the look of two
                          ModelFp8Triple = 19, // FP8: one pass with the look of three
                          ModelFp8Double2 = 20, // the same, v2 weights
                          ModelFp8Triple2 = 21;
// Model B with the transformer's feed-forward exchanged for the 4-bit one: the runtime asks for 2, and only that layer
// is ours, so its 4-bit weights live in nvfp4/modelB.
inline constexpr unsigned ModelBFeedForward = 22;
constexpr bool IsPassInOne(unsigned preset) { return preset == ModelTwoTriple || preset == ModelTwoDouble; }
constexpr bool IsFp8(unsigned preset) { return preset >= ModelFp8Double && preset <= ModelFp8Triple2; }
constexpr unsigned Fp8Set(unsigned preset) { return IsFp8(preset) ? preset - ModelFp8Double + 1 : 0; } // nvfp4/fp8/fp8_set<n>
constexpr bool IsModelB(unsigned preset) { return preset == ModelBFeedForward; }
constexpr bool IsKnown(unsigned preset) { return preset == ModelANvfp4 || IsPassInOne(preset) || IsFp8(preset) || IsModelB(preset); }
// Unknown custom presets are refused by BeginEvaluation.
constexpr bool IsNvfp4(unsigned preset) { return preset >= ModelANvfp4; }
constexpr unsigned WeightSet(unsigned preset) { return IsPassInOne(preset) ? preset - 3 : 0; } // 0: the trained 4-bit codes
// What the runtime is asked for: the model our kernels have the weights of. Model B asks for its own preset 2.
constexpr unsigned RuntimePreset(unsigned preset) {
    if (IsModelB(preset)) return 2u;
    return IsNvfp4(preset) ? 1u : preset;
}

void SetLog(void (*log)(const char* line));
// For the NvAPI query hook, inside pass borrowing's wrap: transparent until Configure() switches the layers on, and
// outside the evaluations that ask for them.
void* WrapNvapi(unsigned id, void* original);
// Before a pass's feature is created and before a frame's evaluations: whether any pass wants a 4-bit model. Loads the
// layers the first time one does; they stay loaded (and idle) afterwards.
void Configure(bool wanted, const std::filesystem::path& assets);
// After a drained session reloads its runtime, retry layer setup at the existing asset path.
// Keeps loaded GPU allocations and the feed-forward layer's unrecoverable-failure guard.
void Retry();
// Around every evaluation of the NR model recorded on `list`, whatever the pass's model is.
void BeginEvaluation(unsigned requestedPreset);
void EndEvaluation(ID3D12GraphicsCommandList* list);
// After EndEvaluation() of an evaluation the runtime really made: false when the pass asked for a 4-bit model and our
// kernels did not run it, so its output is the runtime's own model and must not be shown. `whole`: no part of the
// evaluation was borrowed from another pass (a borrowed part runs no kernels of its own). True for other passes.
bool OursRan(bool whole);
// A path that cannot run a 4-bit model at all (Vulkan) says so here instead of running the runtime's own.
void Refuse(const char* reason);
// Why a pass that asks for a 4-bit model is not being applied; empty while it runs or while it is still starting.
std::string Problem();
// The last evaluation that asked for a 4-bit model ran our kernels.
bool Running();
// Only once the device's queue has drained, or the device is lost.
void Release(ID3D12Device* device);
std::string Status();
} // namespace DlssNr::Nvfp4
