#pragma once

#include <cstdint>
#include <sl_core_api.h>

namespace uplift::addon::slnr {

// The existing Network selector: 1 Legacy, 2 Small FP8, everything else restores the game.
void SetDesiredPreset(uint32_t preset);
// Called by both slGetFeatureFunction observers; only the verified local NR provider is wrapped.
void ObserveFunction(const char* name, void*& function);
// Replay once before Streamline consumes options, on its evaluate thread.
void BeforeEvaluate(const sl::BaseStructure* const* inputs, uint32_t count);
void AfterFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport, sl::Result result);
// Discard borrowed provider addresses before Streamline unloads its plugins.
void BeforeShutdown();

}  // namespace uplift::addon::slnr
