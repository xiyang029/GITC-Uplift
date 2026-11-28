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

#include <DirectXPackedVector.h>
#include "native_nr_test_gpu.hpp"
#include "native_nr_parameter_view.hpp"
#include "addon/command_list_tracker.hpp"
#include "addon/device_context.hpp"
#include "addon/frame_config.hpp"
#include "addon/sl_nr_viewports.hpp"
#include "ngx_hooks/dlss_capture.hpp"
#include "nr/host.hpp"
#include "nr/keys.hpp"
#include "sources/after_dlss_source.hpp"
#include "sources/work_size.hpp"
#include "ui/settings_schema.hpp"

using Microsoft::WRL::ComPtr;
using namespace uplift;
using namespace uplift::native_nr_test;
namespace {
std::vector<std::string> input_logs;
std::vector<std::string> frame_logs;
std::vector<std::string> gap_logs;
std::vector<std::string> submit_logs;
void NativeModes() {
  constexpr std::array<std::string_view, 3> keys = {"OverrideNativeNrParameters", "UpliftAfterNativeNr", "ReplaceNativeNr"};
  const auto apply = [&](ui::Settings& settings, size_t mode, bool on) {
    const auto* descriptor = ui::FindSetting(keys[mode]);
    Check(descriptor != nullptr, "native NR setting not found");
    descriptor->set(settings, on ? 1.0 : 0.0);
  };
  const auto selected = [&](const ui::Settings& settings) {
    size_t count = 0u;
    for (const auto key : keys) count += ui::FindSetting(key)->get(settings) == 1.0;
    return count;
  };
  for (const bool enabled : {false, true}) for (const bool parameter_only : {false, true}) {
    ui::Settings settings; settings.enabled = enabled; settings.native_nr_mode = parameter_only ? ui::NativeNrMode::PARAMETERS : ui::NativeNrMode::OFF;
    settings.style = 2u; settings.intensity = .4f; settings.local_tone = .3f; settings.local_structure = .2f;
    settings.skin_structure = .1f; settings.auto_mask = false;
    const auto controls = ui::NativeNrParameterOverrides(settings);
    Check(controls.has_value() == (enabled && parameter_only), "native parameter override ignored the master switch");
    if (controls) Check(*controls == nr::Controls{.intensity = .4f, .local_tone = .3f, .local_structure = .2f,
        .auto_mask = false, .skin_structure = .1f, .style = 2u}, "native parameter controls changed");
    Check(ui::ToSessionOptions(settings).enabled == (enabled && !parameter_only),
          "parameter-only mode stacked an Uplift session");
    ui::ControlsCoalescer coalescer;
    const auto frame = addon::BuildFrameSettings({.settings = &settings}, &coalescer, std::chrono::steady_clock::now());
    Check(frame.native_nr_mode == settings.native_nr_mode, "frame settings lost the selected native NR mode");
    Check(frame.session.enabled == (enabled && !parameter_only),
          "frame/helper settings re-enabled extra Uplift NR");
  }
  for (size_t previous = 0u; previous < keys.size(); ++previous) for (size_t next = 0u; next < keys.size(); ++next) {
    ui::Settings settings; settings.enabled = true; apply(settings, previous, true); apply(settings, next, true);
    Check(selected(settings) == 1u && ui::FindSetting(keys[next])->get(settings) == 1.0, "native NR modes were not mutually exclusive");
    Check(ui::ToSessionOptions(settings).enabled == (next == 1u || next == 2u), "exclusive mode selected the wrong Uplift session state");
    Check(ui::NativeNrParameterOverrides(settings).has_value() == (next == 0u), "non-parameter mode retained native parameter overrides");
    apply(settings, (next + 1u) % keys.size(), false);
    Check(ui::FindSetting(keys[next])->get(settings) == 1.0, "clearing an inactive key cleared the selected native NR mode");
    apply(settings, next, false);
    Check(selected(settings) == 0u && ui::ToSessionOptions(settings).enabled && !ui::NativeNrParameterOverrides(settings),
          "leaving a native mode did not restore ordinary Uplift");
  }
  struct Store final : ui::ConfigStore {
    unsigned modes = 0u;
    std::optional<std::string> Get(std::string_view key) const override {
      if (key == "Enabled") return "1";
      if (key == "OverrideNativeNrParameters") return (modes & 1u) ? "1" : "0";
      if (key == "UpliftAfterNativeNr") return (modes & 2u) ? "1" : "0";
      if (key == "ReplaceNativeNr") return (modes & 4u) ? "1" : "0";
      return std::nullopt;
    }
    void Set(std::string_view, std::string_view) override {}
  } store;
  for (store.modes = 1u; store.modes < 8u; ++store.modes) {
    const auto settings = ui::LoadSettings(store, nullptr);
    Check(selected(settings) == 1u, "persisted native NR modes could stack");
    const size_t expected = (store.modes & 4u) ? 2u : (store.modes & 2u) ? 1u : 0u;
    Check(ui::FindSetting(keys[expected])->get(settings) == 1.0, "config mode conflict did not follow schema order");
    Check(ui::ToSessionOptions(settings).enabled == (expected == 1u || expected == 2u),
          "loaded native mode selected the wrong Uplift session state");
    Check(ui::NativeNrParameterOverrides(settings).has_value() == (expected == 0u),
          "loaded native parameter override did not select the merged mode");
  }
  std::cout << "PASS native NR master switch, parameter-only mode, exclusive After/Replace and persisted conflicts\n";
}
void ParameterViews() {
  Parameters original; original.Set("unknown-native-key", 42u); original.Set(nr::keys::RESET, 0);
  original.Set(nr::keys::MVEC_SCALE_X, .5f);
  original.Set(nr::keys::COLOR.resource, static_cast<ID3D12Resource*>(nullptr));
  const auto before = original.values;
  {
    ParameterView view(original); view.Set(nr::keys::RESET, 1u); view.Set(nr::keys::MVEC_SCALE_X, 2.f);
    const auto pointer = reinterpret_cast<ID3D12Resource*>(uintptr_t{0x1234u}); view.Set(nr::keys::COLOR.resource, pointer);
    unsigned value = 0u; int flag = 0; float scale = 0.f; void* read_pointer = nullptr;
    Check(NVSDK_NGX_SUCCEED(view.Get("unknown-native-key", &value)) && value == 42u, "overlay lost unknown native parameter");
    Check(NVSDK_NGX_SUCCEED(view.Get(nr::keys::RESET, &flag)) && flag == 1, "overlay signed/unsigned flag mismatch");
    Check(NVSDK_NGX_SUCCEED(view.Get(nr::keys::MVEC_SCALE_X, &scale)) && scale == 2.f, "overlay scalar override lost");
    Check(NVSDK_NGX_SUCCEED(view.Get(nr::keys::COLOR.resource, &read_pointer)) && read_pointer == pointer, "overlay resource pointer representation mismatch");
    view.Reset();
    Check(NVSDK_NGX_SUCCEED(view.Get(nr::keys::RESET, &flag)) && flag == 0, "overlay Reset changed source");
  }
  Check(original.values == before, "call-local overlay mutated the original native block");
  std::cout << "PASS native parameter overlay: unknown keys, pointer types, flags and unchanged original block\n";
}
struct Runtime final : nr::Host {
  nr::EvaluateInputs last; bool fail = false;
  struct Feature final : nr::FeatureInterface {
    Runtime& host; nr::Size size; bool created = false;
    explicit Feature(Runtime& value) : host(value) {}
    NVSDK_NGX_Result Create(ID3D12GraphicsCommandList*, const nr::CreateInfo& info) override { size = info.capacity; created = true; return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* list, const nr::EvaluateInputs& inputs, const nr::Controls&) override {
      host.last = inputs; if (host.fail) return NVSDK_NGX_Result_Fail;
      Barrier(list, inputs.color, READ, D3D12_RESOURCE_STATE_COPY_SOURCE); Barrier(list, inputs.output, UAV, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(inputs.output, inputs.color); Barrier(list, inputs.color, D3D12_RESOURCE_STATE_COPY_SOURCE, READ);
      Barrier(list, inputs.output, D3D12_RESOURCE_STATE_COPY_DEST, UAV); return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Release() override { created = false; return NVSDK_NGX_Result_Success; }
    void Abandon() override { created = false; }
    bool Created() const override { return created; } nr::Size Capacity() const override { return size; }
  };
  NVSDK_NGX_Result LoadRuntime() override { return NVSDK_NGX_Result_Success; }
  void UnloadRuntime() override {} void AbandonRuntime() override {}
  std::unique_ptr<nr::FeatureInterface> NewFeature() override { return std::make_unique<Feature>(*this); }
  nr::MemoryInfo QueryMemory() override { return {32ull << 30u, 0u}; }
  std::optional<uint64_t> QueryRuntimeBytes() override { return std::nullopt; }
  std::chrono::steady_clock::time_point Now() override { return std::chrono::steady_clock::now(); }
  void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* image, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) override { Barrier(list, image, before, after); }
};
void Trace(addon::ListState& tracked, ID3D12Resource* image, D3D12_RESOURCE_STATES state) {
  tracked.barrier_trace[tracked.barrier_trace_count++] = {.resource = reinterpret_cast<uint64_t>(image), .after = static_cast<uint32_t>(state)};
}
void CaptureAndPolicy(Gpu& gpu) {
  std::vector<uint32_t> pixels(size_t(672u) * 384u, 0xff404040u);
  auto color = gpu.Texture(672u, 384u, DXGI_FORMAT_R8G8B8A8_UNORM, pixels.data(), 4u);
  auto output = gpu.Texture(672u, 384u, DXGI_FORMAT_R8G8B8A8_UNORM, pixels.data(), 4u, UAV);
  auto no_uav = gpu.Texture(672u, 384u, DXGI_FORMAT_R8G8B8A8_UNORM, pixels.data(), 4u, READ, D3D12_RESOURCE_FLAG_NONE);
  auto mask = gpu.Texture(672u, 384u, DXGI_FORMAT_R8G8B8A8_UNORM, pixels.data(), 4u);
  Parameters parameters; parameters.Set(nr::keys::COLOR.resource, color.Get()); parameters.Set(nr::keys::OUTPUT.resource, output.Get());
  parameters.Set(nr::keys::COLOR.subrect.base_x, 32u); parameters.Set(nr::keys::COLOR.subrect.base_y, 24u);
  parameters.Set(nr::keys::OUTPUT.subrect.width, 640u); parameters.Set(nr::keys::OUTPUT.subrect.height, 360u);
  parameters.Set(nr::keys::CONTROL_MASK.resource, mask.Get()); parameters.Set(nr::keys::DEPTH.resource, mask.Get());
  parameters.Set(nr::keys::MVEC.resource, mask.Get()); parameters.Set(nr::keys::BACKBUFFER.resource, color.Get());
  parameters.Set(nr::keys::UI.resource, mask.Get()); parameters.Set(nr::keys::UI_ALPHA.resource, mask.Get());
  parameters.Set(nr::keys::DEPTH_INVERTED, 1u); parameters.Set(nr::keys::UI_CORRECTION, 1); parameters.Set(nr::keys::RESET, 1u);
  auto original = ngx_hooks::CaptureNativeNrFrame(parameters, nullptr, {});
  Check(original.color_region.width == 640u && original.color_region.height == 360u && original.reset, "native region remainder or unsigned reset lost");
  Check(original.native_nr && original.native_nr->depth_inverted.value_or(false) && original.native_nr->ui_correction, "native flags lost");
  std::string reason; auto ready = original;
  addon::ListState unobserved;
  Check(addon::NativeNrInputsReady(&unobserved, &ready, &reason), "valid NGX inputs required a ReShade-observed barrier");
  Check(ready.native_nr->color_state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
      && ready.depth == mask.Get() && ready.motion_vectors == mask.Get() && ready.native_nr->control_mask.resource == mask.Get(),
      "NGX input-state contract did not cover an unobserved command list");
  auto alias_without_trace = original; alias_without_trace.color = output.Get(); alias_without_trace.color_region = alias_without_trace.output_region;
  Check(addon::NativeNrInputsReady(&unobserved, &alias_without_trace, &reason)
      && alias_without_trace.native_nr->color_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      "aliased Color/Output did not use the output UAV contract");
  ready = original; ready.color = output.Get(); ready.color_region = ready.output_region;
  Check(addon::NativeNrInputsReady(&unobserved, &ready, &reason)
      && ready.native_nr->color_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      "game-owned aliased Color/Output was rejected from the handle resize path");
  ready = original;
  Check(addon::NativeNrInputsReady(&unobserved, &ready, &reason),
      "separate Color/Output inputs were blocked from the handle resize path");
  addon::ListState tracked; Trace(tracked, color.Get(), READ); Trace(tracked, output.Get(), UAV); Trace(tracked, mask.Get(), READ);
  ready = original; Check(addon::NativeNrInputsReady(&tracked, &ready, &reason), reason.c_str());
  Check(ready.native_nr->control_mask.resource == mask.Get() && ready.native_nr->ui_alpha.resource == mask.Get(), "readable native auxiliary inputs lost");
  sources::AfterDlssConfig config; config.replace_native_nr = true;
  const auto game = sources::ReadGameFrame(ready, output.Get(), ready.output_region, ready.reset, config);
  Check(game.target.source.resource == color.Get() && game.target.resource == output.Get() && game.inputs.depth_inverted == 1
      && game.inputs.control_mask.resource == mask.Get() && game.inputs.ui_correction, "native inputs did not reach the replacement frame");
  config.depth_inverted = false;
  Check(sources::ReadGameFrame(ready, output.Get(), ready.output_region, false, config).inputs.depth_inverted == 0, "explicit panel depth direction was overridden");
  tracked.barrier_trace_count = 2u; ready = original;
  Check(addon::NativeNrInputsReady(&tracked, &ready, &reason), "partial barrier trace blocked valid NGX inputs");
  Check(ready.depth == mask.Get() && ready.motion_vectors == mask.Get() && ready.native_nr->control_mask.resource == mask.Get()
      && ready.native_nr->ui.resource == mask.Get() && ready.native_nr->ui_alpha.resource == mask.Get()
      && ready.native_nr->ui_correction, "valid optional inputs were dropped because the trace was incomplete");
  ready = original; ready.color_region.width = 639u; Check(!addon::NativeNrInputsReady(&tracked, &ready, &reason), "mismatched active extents admitted");
  ready = original; ready.native_nr->enabled = false;
  Check(!addon::NativeNrInputsReady(&tracked, &ready, &reason), "disabled native NR admitted by ordinary replacement");
  constexpr auto fallback = addon::NativeNrInputPolicy::DISABLED_FALLBACK;
  Check(addon::NativeNrInputsReady(&tracked, &ready, &reason, fallback), "valid disabled fallback rejected");
  auto wrong_extent = ready; --wrong_extent.color_region.width;
  Check(!addon::NativeNrInputsReady(&tracked, &wrong_extent, &reason, fallback), "disabled fallback accepted mismatched extents");
  auto enabled = original;
  Check(!addon::NativeNrInputsReady(&tracked, &enabled, &reason, fallback), "disabled fallback admitted an enabled call");
  ready = original; ready.output = no_uav.Get(); ready.output_region = ready.color_region;
  Check(!addon::NativeNrInputsReady(&tracked, &ready, &reason), "Output without UAV capability admitted");
  ready = original; ready.color = output.Get(); ready.color_region = ready.output_region;
  Check(addon::NativeNrInputsReady(&tracked, &ready, &reason), "aliased UAV Color/Output regressed");
  ready = original; Trace(tracked, color.Get(), UAV);
  Check(addon::NativeNrInputsReady(&tracked, &ready, &reason)
      && ready.native_nr->color_state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      "diagnostic barriers overrode the NGX Color entry-state contract");
  ready = original; ready.native_nr->control_mask.rect = {670u, 380u, 8u, 8u}; Trace(tracked, mask.Get(), READ);
  Check(addon::NativeNrInputsReady(&tracked, &ready, &reason) && !ready.native_nr->control_mask.resource, "invalid optional region did not fall back to null");
  Check(ready.native_nr->input_status[2] == nr::NativeInputStatus::INVALID_REGION, "invalid-region diagnosis lost");
  ready = original; ready.depth = nullptr;
  Check(addon::NativeNrInputsReady(&tracked, &ready, &reason) && ready.native_nr->input_status[0] == nr::NativeInputStatus::MISSING,
        "missing-input diagnosis lost");
  ready = original; ready.depth = output.Get(); ready.depth_region = ready.output_region;
  Check(addon::NativeNrInputsReady(&tracked, &ready, &reason) && ready.native_nr->input_status[0] == nr::NativeInputStatus::OUTPUT_ALIAS,
        "output-alias diagnosis lost");
  ready = original; tracked.barrier_trace_count = 2u;
  Check(addon::NativeNrInputsReady(&tracked, &ready, &reason) && ready.native_nr->input_status[0] == nr::NativeInputStatus::READY,
        "incomplete diagnostic trace was treated as authoritative state");
  config.motion_vectors = false;
  Check(sources::ReadGameFrame(ready, output.Get(), ready.output_region, false, config).inputs.native_input_status[1] == nr::NativeInputStatus::DISABLED,
        "panel motion-disable diagnosis lost");
  Check(!addon::NativeNrInputsReady(nullptr, &original, &reason), "untracked command list admitted");
  gpu.Submit(); std::cout << "PASS native capture, subrects, signed/unsigned flags, auxiliary forwarding and safe fallback\n";
}
// Independent double-precision oracle: invert the density integral by bisection,
// rather than repeating the shader's four-step Newton inverse.
double WarpAxis(double n, const nr::foveation::Axis& axis) {
  if (axis.packed == 0u) return n;
  const double t = 2.0 * n - 1.0;
  return .5 + .5 * (t < 0.0 ? -1.0 : 1.0) * nr::foveation::G(std::abs(t), nr::foveation::Decode(axis.packed)) / axis.gmax;
}
double UnwarpAxis(double v, const nr::foveation::Axis& axis) {
  if (axis.packed == 0u) return v;
  const double target = std::abs(2.0 * v - 1.0) * axis.gmax;
  const auto decoded = nr::foveation::Decode(axis.packed);
  double lo = 0.0, hi = 8.0;
  for (unsigned i = 0u; i < 64u; ++i) {
    const double mid = (lo + hi) * .5;
    if (nr::foveation::G(mid, decoded) < target) lo = mid; else hi = mid;
  }
  return .5 + .5 * (v < .5 ? -1.0 : 1.0) * ((lo + hi) * .5);
}
void CheckFoveatedPixels(const nr::foveation::Plan& plan, nr::Rect region,
                        const Gpu::Read& depth, const Gpu::Read& mask, const Gpu::Read& backbuffer,
                        const Gpu::Read& ui, const Gpu::Read& alpha, const Gpu::Read& motion,
                        const std::vector<float>& depth_pixels, const std::vector<uint8_t>& mask_pixels,
                        const std::vector<uint32_t>& backbuffer_pixels, const std::vector<uint32_t>& ui_pixels,
                        const std::vector<uint8_t>& alpha_pixels, const std::vector<float>& motion_pixels, UINT source_width) {
  unsigned checked = 0u, offscreen = 0u, moving = 0u;
  for (UINT y = 0u; y < plan.height; y += 29u) for (UINT x = 0u; x < plan.width; x += 37u) {
    const double vx = (x + .5) / plan.width, vy = (y + .5) / plan.height;
    const double nx = UnwarpAxis(vx, plan.grid.x), ny = UnwarpAxis(vy, plan.grid.y);
    const double px = nx * region.width, py = ny * region.height;
    // Nearest-sample ties can fall on opposite sides after fp32 rounding.
    if (std::abs(px - std::round(px)) < .02 || std::abs(py - std::round(py)) < .02) continue;
    const UINT sx = region.x + std::min<UINT>(region.width - 1u, static_cast<UINT>(px));
    const UINT sy = region.y + std::min<UINT>(region.height - 1u, static_cast<UINT>(py));
    const size_t index = size_t(sy) * source_width + sx;
    Check(std::abs(depth.At<float>(x, y) - depth_pixels[index]) < 1e-6f, "patterned depth sampled wrong FOV coordinate");
    Check(mask.At<uint8_t>(x, y) == mask_pixels[index], "patterned control mask sampled wrong FOV coordinate");
    Check(backbuffer.At<uint32_t>(x, y) == backbuffer_pixels[index], "backbuffer mapping/table mismatch");
    Check(ui.At<uint32_t>(x, y) == ui_pixels[index], "patterned UI lost raw colour/alpha or sampled wrong coordinate");
    Check(alpha.At<uint8_t>(x, y) == alpha_pixels[index], "UI alpha mapping/table mismatch");
    const double prev_x = nx + motion_pixels[2u * index] * .75 / region.width;
    const double prev_y = ny + motion_pixels[2u * index + 1u] * -1.25 / region.height;
    const std::array expected = {(WarpAxis(prev_x, plan.grid.x) - vx) * plan.width,
                                 (WarpAxis(prev_y, plan.grid.y) - vy) * plan.height};
    for (UINT channel = 0u; channel < 2u; ++channel) {
      const float actual = DirectX::PackedVector::XMConvertHalfToFloat(motion.At<uint16_t>(x, y, 2u, channel));
      Check(std::abs(actual - expected[channel]) <= .03 + std::abs(expected[channel]) * .001,
            "nonzero FOV motion endpoint/scale/origin mismatch");
    }
    ++checked;
    if (prev_x < 0.0 || prev_x > 1.0 || prev_y < 0.0 || prev_y > 1.0) ++offscreen;
    if (std::abs(expected[0]) + std::abs(expected[1]) > 1.0) ++moving;
  }
  Check(checked > 150u && offscreen > 0u && moving > 100u, "FOV oracle did not cover enough motion/offscreen samples");
  std::cout << "PASS patterned FOV inputs and nonzero motion: " << checked << " coordinates, " << offscreen << " offscreen endpoints\n";
}
void ViewportLifetime() {
  addon::slnr::ViewportOptions<uint32_t> options;
  for (uint32_t id = 0u; id < 8u; ++id) {
    const auto entry = options.GetOrInsert(id);
    Check(entry != nullptr, "viewport slot rejected before cap");
    *entry = id + 1u;
  }
  Check(!options.GetOrInsert(8u), "viewport cap not enforced");
  sl::ViewportHandle freed(3u);
  sl::ViewportHandle kept(4u);
  sl::BaseStructure chain({}, sl::kStructVersion1);
  chain.next = &freed;
  const sl::BaseStructure* inputs[] = {nullptr, &chain};
  const auto current = [&] { return addon::slnr::FindEvaluateViewport(inputs, 2u); };
  Check(current() == &freed && *options.Find(*current()) == 4u, "NR evaluate did not find its own viewport's options");
  const auto on_free = [&](sl::Feature feature, sl::Result result) {
    if (addon::slnr::FreedNrViewport(feature, freed, result)) options.Erase(freed);
  };
  on_free(sl::kFeatureDLSS_NR, sl::Result::eErrorInvalidParameter);
  on_free(sl::kFeatureDLSS, sl::Result::eOk);
  Check(options.Find(*current()) && !options.GetOrInsert(8u), "failed/other-feature release invalidated NR options");
  Check(!addon::slnr::FreedNrViewport(sl::kFeatureDLSS_NR, chain, sl::Result::eOk), "non-viewport free was accepted");
  on_free(sl::kFeatureDLSS_NR, sl::Result::eOk);
  Check(!options.Find(*current()) && options.GetOrInsert(8u), "freed options still replayable or viewport slot not reclaimed");
  chain.next = &kept;
  Check(current() == &kept && *options.Find(*current()) == 5u, "free evicted another viewport's options");
  inputs[1] = nullptr;
  Check(!current(), "missing NR viewport selected an unrelated cache entry");
  std::cout << "PASS Streamline NR viewport selection, free success/failure isolation and cache reuse\n";
}

void PresetRestoreRetry() {
  addon::slnr::PresetSubmission state;
  const auto now = addon::slnr::PresetSubmission::Clock::now();
  const auto error = sl::Result::eErrorInvalidParameter;
  using namespace std::chrono_literals;
  Check(state.ShouldReplay(2u, now), "initial panel choice was not pending");
  state.Complete(2u, 0x80000002u, false, sl::Result::eOk, now);
  Check(!state.ShouldReplay(2u, now), "accepted choice remained pending");
  state.Complete(0u, 0x80000001u, false, error, now);
  Check(state.accepted == 2u && state.effective == 0x80000002u && !state.ShouldReplay(0u, now + 999ms),
        "failed restore changed the effective preset or ignored backoff");
  Check(state.ShouldReplay(0u, now + 1s) && state.ShouldReplay(2u, now), "failed restore was abandoned or blocked a new choice");
  state.Complete(0u, 0x80000001u, false, sl::Result::eOk, now + 1s);
  Check(!state.ShouldReplay(0u, now + 2s) && state.effective == 0x80000001u, "successful restore was not committed");
  state.Complete(2u, 0x80000001u, true, error, now + 2s);
  Check(!state.refused && state.ShouldReplay(2u, now + 3s), "failed fallback was treated as an accepted refusal");
  state.Complete(2u, 0x80000001u, true, sl::Result::eOk, now + 3s);
  Check(state.refused == 2u && !state.ShouldReplay(2u, now + 4s), "successful fallback kept retrying a rejected override");
  std::cout << "PASS Streamline NR preset restore failure, backoff and successful retry\n";
}

void FrameReservation() {
  addon::NativeNrFrame frame;
  Check(!frame.DeferOrdinaryDlss(false, true), "parameter mode reserved an ordinary DLSS frame");
  Check(frame.DeferOrdinaryDlss(true, true) && frame.NeedsPresentFallback(true, false),
        "releasing the handle lost the fallback after ordinary DLSS had deferred");
  Check(frame.Close().NeedsPresentFallback(true, false) && !frame.NeedsPresentFallback(true, false),
        "a deferred evaluate leaked past its Present boundary");
  frame.ordinary_dlss_applied = true;
  int first_output = 0, second_output = 0;
  Check(!frame.CanApplyNative(&first_output) && !frame.NeedsPresentFallback(true, true),
        "a late native handle could run NR again after ordinary DLSS had applied it");
  (void)frame.Close();
  for (const bool reverse : {false, true}) {
    const void* other = reverse ? &first_output : &second_output;
    const void* main = reverse ? &second_output : &first_output;
    frame.MarkGameOutput(other);
    Check(frame.CanApplyNative(main) && !frame.CanApplyNative(other), "successful original call claimed another output");
    frame.MarkGameOutput(main);
    Check(!frame.CanApplyNative(main) && frame.Close().GameOwns(main), "same-output duplicate escaped ownership");
  }
}

void DisabledFallbackAndOwnership(Gpu& gpu) {
  auto fake = std::make_unique<Runtime>();
  Runtime* const runtime = fake.get();
  std::string error;
  auto context = addon::DeviceContext::Create(gpu.device.Get(), {}, &error, {.host = std::move(fake)});
  Check(context != nullptr, error.c_str());
  constexpr UINT width = 672u, height = 384u;
  std::vector<uint32_t> source(size_t(width) * height, 0xff303030u), sentinel(size_t(width) * height, 0xffb0b0b0u);
  for (UINT y = 12u; y < 372u; ++y) for (UINT x = 16u; x < 656u; ++x) source[size_t(y) * width + x] = 0xff606060u;
  auto color = gpu.Texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, source.data(), 4u);
  auto output = gpu.Texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, sentinel.data(), 4u, UAV);
  auto alias = gpu.Texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, source.data(), 4u, UAV);
  auto backbuffer = gpu.Texture(640u, 360u, DXGI_FORMAT_R8G8B8A8_UNORM, sentinel.data(), 4u, D3D12_RESOURCE_STATE_PRESENT,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  struct Host final : sources::DlssFrameHost, addon::FrameHost {
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* target = nullptr;
    std::vector<uint64_t> tokens;
    std::vector<std::shared_ptr<sources::NrTimingSlot>> timings;
    ID3D12GraphicsCommandList* NativeList() override { return list; }
    bool StateKnown() override { return true; }
    void ReplayState(state::RestoreMode) override {}
    void AttachToken(uint64_t token) override { tokens.push_back(token); }
    void HoldTiming(const std::shared_ptr<sources::NrTimingSlot>& t) override { timings.push_back(t); }
    void FlushPending() override {}
    void TargetBarrier(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) override { Barrier(list, target, before, after); }
  } host;
  host.list = gpu.list.Get();
  host.target = backbuffer.Get();
  addon::FrameSettings settings;
  settings.session.enabled = true;
  settings.session.pass_count = 2u;
  settings.source = ui::PlacementSource::DLSS;
  settings.native_nr_mode = ui::NativeNrMode::AFTER;
  settings.encoding = color::Encoding::SRGB;
  const addon::FrameFacts facts = {.game_facing_present = true, .effects_on = true, .split_capable = true};
  const auto now = [] { return std::chrono::steady_clock::now(); };
  const auto begin = [&] { return context->BeginFrame(gpu.queue.Get(), settings, facts, {.resource = backbuffer.Get()}, true, now()); };
  const auto submit = [&] {
    gpu.Submit();
    context->SubmitTokens(host.tokens, gpu.queue.Get(), GetCurrentThreadId(), now());
    context->StampThread(GetCurrentThreadId());
    host.tokens.clear();
    host.timings.clear();
  };
  const auto check_present = [&] {
    const auto point = begin();
    Check(point == addon::TriggerPoint::PRESENT && context->FrameReady(), "live-but-idle/failed native NR did not arm Present");
    const auto status = context->Status();
    Check(status.group_count == 1u && status.groups[0].stage == addon::Stage::PRESENT && status.groups[0].count == 2u,
          "Present fallback did not configure the full Session chain");
    Check(context->OnBeginEffects(true) == addon::TriggerPoint::NONE && context->OnTechnique(true) == addon::TriggerPoint::NONE,
          "native fallback also armed an effects trigger");
    return point;
  };
  context->SetNativeNrFeatureLive(true);
  for (unsigned i = 0u; i < 1000u && context->NrState() != nr::SessionState::ACTIVE; ++i) { begin(); Sleep(1u); }
  Check(context->NrState() == nr::SessionState::ACTIVE, "native routing runtime did not load");
  const auto idle = context->Run(&host, D3D12_RESOURCE_STATE_PRESENT, check_present());
  Check(idle.nr_applied && idle.passes_run == 2u, "live idle handle blocked ordinary Present NR");
  submit();
  addon::ListState tracked;
  ngx_hooks::DlssFrame frame = {.color = color.Get(), .output = output.Get(), .color_region = {16u, 12u, 640u, 360u},
                                .output_region = {8u, 4u, 640u, 360u}};
  frame.native_nr.emplace().enabled = false;
  const auto fallback = [&](ngx_hooks::DlssFrame input) {
    Check(addon::NativeNrInputsReady(&tracked, &input, nullptr, addon::NativeNrInputPolicy::DISABLED_FALLBACK), "disabled inputs rejected");
    return context->OnNativeNrEvaluate(input, host, now(), addon::NativeNrPath::DISABLED_FALLBACK);
  };
  runtime->fail = true;
  Check(!fallback(frame).nr_applied, "failed fallback would skip the original call");
  auto read = gpu.Readback(output.Get(), UAV); submit();
  Check(read.At<uint32_t>(8u, 4u) == 0xffb0b0b0u, "failed disabled fallback modified Output");
  runtime->fail = false;
  // The original failed (or remained disabled) and the handle was released before Present.
  context->NoteNativeNrSite();
  context->SetNativeNrFeatureLive(false);
  check_present();
  context->SetNativeNrFeatureLive(true);
  const auto ran = fallback(frame);
  Check(ran.nr_applied && ran.passes_run == 2u, "disabled distinct Color→Output fallback did not apply the full native slice");
  read = gpu.Readback(output.Get(), UAV); submit();
  Check(read.At<uint32_t>(8u, 4u) == 0xff606060u && read.At<uint32_t>(0u, 0u) == 0xffb0b0b0u,
        "disabled fallback read stale Output or wrote outside the active region");
  Check(!fallback(frame).nr_applied, "second evaluate replaced an already claimed frame");
  Check(begin() == addon::TriggerPoint::NONE && !context->FrameReady(), "Present reprocessed a replaced native frame");
  check_present();  // ownership did not leak through a Present which did not call Run
  frame.color = frame.output = alias.Get();
  Check(fallback(frame).nr_applied, "disabled aliased Color/Output fallback did not apply");
  read = gpu.Readback(alias.Get(), UAV); submit();
  Check(read.At<uint32_t>(8u, 4u) == 0xff606060u, "disabled aliased fallback ignored the input origin");
  Check(begin() == addon::TriggerPoint::NONE, "aliased fallback did not claim the frame");
  context->MarkNativeNrPassthrough(frame);  // The enabled original succeeded; no Uplift host was needed.
  Check(!fallback(frame).nr_applied, "disabled call overwrote an earlier native success");
  context->SetNativeNrFeatureLive(false);  // the game may release its handle before this Present
  Check(begin() == addon::TriggerPoint::NONE && !context->FrameReady(), "release lost successful native frame ownership");
  context->SetNativeNrFeatureLive(true);
  check_present();
  context->MarkNativeNrPassthrough(frame);
  context->CloseNativeNrFrame();  // outer Present rejected before BeginFrame
  check_present();
  auto auxiliary = frame;
  auxiliary.output = output.Get();
  auxiliary.color_region.width = auxiliary.output_region.width = 320u;
  auxiliary.color_region.height = auxiliary.output_region.height = 240u;
  for (const bool auxiliary_first : {false, true}) {
    const auto offer_auxiliary = [&] {
      Check(!context->IsNativeNrPresentCandidate(auxiliary) && !fallback(auxiliary).nr_applied,
            "auxiliary DLSS-NR output entered the primary temporal chain");
      context->MarkNativeNrPassthrough(auxiliary);
    };
    if (auxiliary_first) offer_auxiliary();
    Check(fallback(frame).nr_applied, "auxiliary evaluation prevented the primary DLSS-NR fallback");
    if (!auxiliary_first) offer_auxiliary();
    submit();
    Check(begin() == addon::TriggerPoint::NONE, "primary recording did not own Present");
  }
  context->MarkNativeNrPassthrough(auxiliary);
  check_present();  // a successful auxiliary evaluate does not own the primary image
  context->MarkNativeNrPassthrough(frame);
  Check(begin() == addon::TriggerPoint::NONE, "full-frame passthrough without a tracked host armed Present fallback");
  context->SetNativeNrFeatureLive(false);
  settings.source = ui::PlacementSource::PRESENT;
  Check(begin() == addon::TriggerPoint::NONE && context->FrameReady(), "ordinary Present did not arm its marker");
  context->NoteNativeNrSite();  // a new handle appeared after the ordinary effects trigger was armed
  context->MarkNativeNrPassthrough(frame);
  const auto marker = context->OnTechnique(true);
  Check(marker == addon::TriggerPoint::MARKER && !context->Run(&host, D3D12_RESOURCE_STATE_PRESENT, marker).nr_applied,
        "a delayed effects trigger applied NR after a successful native evaluate");
  Check(begin() == addon::TriggerPoint::NONE && !context->FrameReady(), "effects trigger discarded ownership before Present");

  settings.native_nr_mode = ui::NativeNrMode::REPLACE;
  context->SetNativeNrFeatureLive(true);
  auto trace_now = now();
  const auto trace_begin = [&] {
    trace_now += std::chrono::milliseconds(2);
    return context->BeginFrame(gpu.queue.Get(), settings, facts, {.resource = backbuffer.Get()}, true, trace_now);
  };
  trace_begin();
  const auto passthrough = [&](std::string_view reason) {
    context->NoteNativeNrSite();
    context->NoteNativeNrReplacement(frame, addon::NativeNrPath::REPLACE, {}, reason);
    context->MarkNativeNrPassthrough(frame);
    Check(trace_begin() == addon::TriggerPoint::NONE, "game passthrough armed Present NR");
  };
  frame.serial = 17u;
  frame.native_nr->enabled = true;
  passthrough("test input bypass");
  Check(frame_logs.back().find("serial=17 calls=1 replace=not-applied site=game-enabled reason=[test input bypass] game_nr_succeeded=true present=not-armed")
            != std::string::npos, "native bypass and game ownership lost in the frame diagnostic");
  const size_t stable = frame_logs.size();
  for (int i = 0; i < 12; ++i) passthrough("test input bypass");
  Check(frame_logs.size() == stable, "stable passthrough logged every frame");

  context->NoteNativeNrSite();
  context->NoteNativeNrReplacement(frame, addon::NativeNrPath::REPLACE, {.reason = nr::SkipReason::DESCRIPTORS_BUSY});
  const auto present = trace_begin();
  Check(present == addon::TriggerPoint::PRESENT, "failed Replace did not arm Present");
  Check(context->Run(&host, D3D12_RESOURCE_STATE_PRESENT, present).nr_applied, "failed Replace did not run Present fallback");
  Check(frame_logs.size() == stable + 1u
      && frame_logs.back().find("reason=[descriptors busy] game_nr_succeeded=false present=applied trigger=[before effects]") != std::string::npos,
      "Present outcome was not logged alongside the same frame's replacement failure");
  submit();

  frame.reset = true;
  context->NoteNativeNrSite();
  const auto replaced = context->OnNativeNrEvaluate(frame, host, now(), addon::NativeNrPath::REPLACE);
  Check(replaced.nr_applied && replaced.first_pass_reset, "native replacement did not report the actual first-pass reset");
  context->NoteNativeNrReplacement(frame, addon::NativeNrPath::REPLACE, replaced, {}, host.list);
  context->NoteNativeNrReplacement(frame, addon::NativeNrPath::REPLACE, {.reason = nr::SkipReason::INACTIVE});
  submit();
  Check(trace_begin() == addon::TriggerPoint::NONE, "recovered replacement armed Present");
  Check(frame_logs.back().find("calls=2 replace=applied site=game-enabled reason=[-] game_nr_succeeded=false present=not-armed") != std::string::npos
      && frame_logs.back().find("reset(game_hint=true, uplift_first_pass=true, present_first_pass=false)") != std::string::npos,
      "replacement recovery/reset was lost or a duplicate overwrote its result");

  const size_t before_gap = gap_logs.size();
  const size_t before_submit_log = submit_logs.size();
  frame.reset = false;
  context->NoteNativeNrHookBypass(frame.serial, host.list, "untracked command list");
  const auto gap = trace_begin();
  Check(gap == addon::TriggerPoint::PRESENT && context->Run(&host, D3D12_RESOURCE_STATE_PRESENT, gap).nr_applied,
        "a hook bypass changed the Present fallback policy");
  Check(gap_logs.size() == before_gap + 1u && gap_logs.back().find("state=present-fallback") != std::string::npos
      && gap_logs.back().find("hook_bypasses=1 hook_serial=17") != std::string::npos
      && gap_logs.back().find("hook_reason=[untracked command list]") != std::string::npos,
      "a hook-level bypass was indistinguishable from no NGX call");
  Check(submit_logs.size() == before_submit_log + 1u && submit_logs.back().find("latest token=") != std::string::npos
      && submit_logs.back().find("proof=true") != std::string::npos && submit_logs.back().find("ordered=true") != std::string::npos,
      "the fallback lost the previous native recording's submission proof");
  submit();

  const size_t before_burst = frame_logs.size();
  for (int i = 0; i < 20; ++i) passthrough(i % 2 ? "bypass A" : "bypass B");
  Check(frame_logs.size() <= before_burst + 8u, "flapping diagnostics were not rate limited");
  trace_now += std::chrono::seconds(2);
  passthrough("bypass A");
  Check(frame_logs.back().find("; suppressed=0") == std::string::npos, "quiet window lost the suppressed transition count");
  context->Teardown();
  std::cout << "PASS native NR frame diagnostics: bypass, Present fallback, recovery/reset, submit proof, dedup and rate limit\n";
  std::cout << "PASS disabled fallback pixels/failure/alias, full-chain idle Present and one-frame ownership\n";
}

void Pixels(Gpu& gpu) {
  Runtime runtime; nr::Timeline timeline([] { return UINT64_MAX; }, [](auto*, auto) {}, [](auto, auto) { return true; });
  nr::Session session(runtime, timeline); session.SetEnabled(true); session.NoteFrameSize({640u, 360u});
  for (unsigned i = 0u; i < 1000u && session.State() != nr::SessionState::ACTIVE; ++i) { session.Tick(); Sleep(1u); }
  Check(session.State() == nr::SessionState::ACTIVE, "identity NR did not load");
  {
    sources::NrPipeline pipeline(gpu.device.Get(), session, timeline); std::string error; Check(pipeline.Initialize(&error), error.c_str());
    constexpr UINT width = 672u, height = 384u;
    std::vector<uint32_t> color_pixels(size_t(width) * height, 0x7f303030u), output_pixels(size_t(width) * height, 0xffb0b0b0u);
    for (UINT y = 12u; y < 372u; ++y) for (UINT x = 16u; x < 656u; ++x) color_pixels[size_t(y) * width + x] = 0x7f606060u;
    auto color = gpu.Texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, color_pixels.data(), 4u);
    auto output = gpu.Texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, output_pixels.data(), 4u, UAV);
    sources::UavTarget target{.resource = output.Get(), .region = {8u, 4u, 640u, 360u}, .encoding = color::Encoding::SRGB,
        .source = {color.Get(), {16u, 12u, 640u, 360u}}};
    Check(pipeline.RecordUav(gpu.list.Get(), target, {}, {}).nr_applied, "separate Color/Output did not run");
    auto input_read = gpu.Readback(color.Get(), READ), output_read = gpu.Readback(output.Get(), UAV); gpu.Submit();
    Check(output_read.At<uint32_t>(8u, 4u) == 0x7f606060u && output_read.At<uint32_t>(647u, 363u) == 0x7f606060u, "replacement read stale Output or lost source alpha/origin");
    Check(output_read.At<uint32_t>(0u, 0u) == 0xffb0b0b0u && input_read.At<uint32_t>(16u, 12u) == 0x7f606060u, "replacement modified source or pixels outside Output region");
    Barrier(gpu.list.Get(), color.Get(), READ, UAV); target.source_state = UAV;
    Check(pipeline.RecordUav(gpu.list.Get(), target, {}, {}).nr_applied, "separate UAV Color did not run");
    input_read = gpu.Readback(color.Get(), UAV); gpu.Submit(); // validation verifies Color returned to UAV
    runtime.fail = true; Check(!pipeline.RecordUav(gpu.list.Get(), target, {}, {}).nr_applied, "failed NR reported replacement success");
    output_read = gpu.Readback(output.Get(), UAV); input_read = gpu.Readback(color.Get(), UAV); gpu.Submit();
    Check(output_read.At<uint32_t>(8u, 4u) == 0x7f606060u, "failed replacement wrote Output"); runtime.fail = false;
    // Same texture, different regions: copying the source first is required for overlapping areas.
    sources::UavTarget alias{.resource = color.Get(), .region = {0u, 0u, 640u, 360u}, .encoding = color::Encoding::SRGB,
        .source = {color.Get(), {16u, 12u, 640u, 360u}}, .source_state = UAV};
    Check(pipeline.RecordUav(gpu.list.Get(), alias, {}, {}).nr_applied, "aliased atlas replacement failed");
    input_read = gpu.Readback(color.Get(), UAV); gpu.Submit(); Check(input_read.At<uint32_t>(0u, 0u) == 0x7f606060u, "aliased source origin lost");
    // Original RGBA32F UAV support must survive the split source/target refactor.
    std::vector<float> float_pixels(size_t(640u) * 360u * 4u, .25f);
    auto float_image = gpu.Texture(640u, 360u, DXGI_FORMAT_R32G32B32A32_FLOAT, float_pixels.data(), 16u, UAV);
    Check(pipeline.RecordUav(gpu.list.Get(), {.resource = float_image.Get(), .encoding = color::Encoding::SRGB}, {}, {}).nr_applied, "RGBA32F UAV support regressed");
    gpu.Submit();
    // All five optional inputs use independent descriptor tables and follow the foveated work grid.
    std::vector<float> depth_pixels(size_t(width) * height, 1.f), motion_pixels(size_t(width) * height * 2u, 0.f);
    std::vector<uint8_t> mask_pixels(size_t(width) * height, 0u), alpha_pixels(size_t(width) * height, 0u);
    std::vector<uint32_t> ui_pixels(size_t(width) * height, 0u), backbuffer_pixels(size_t(width) * height, 0u);
    for (UINT y = 12u; y < 372u; ++y) for (UINT x = 16u; x < 656u; ++x) {
      const size_t i = size_t(y) * width + x;
      depth_pixels[i] = .1f + .007f * ((7u * x + 13u * y) % 101u);
      mask_pixels[i] = static_cast<uint8_t>((x / 11u + 3u * (y / 7u)) % 256u);
      alpha_pixels[i] = static_cast<uint8_t>((5u * x + y / 3u) % 256u);
      ui_pixels[i] = 0x31000000u | ((x % 256u) << 16u) | ((y % 256u) << 8u) | ((x + y) % 256u);
      backbuffer_pixels[i] = 0xa5000000u | ((3u * x + y) % 0x1000000u);
      motion_pixels[2u * i] = float(int((x / 73u) % 3u) - 1) * 140.f + float(y % 7u) * .2f;
      motion_pixels[2u * i + 1u] = float(int((y / 47u) % 3u) - 1) * 90.f + float(x % 5u) * .3f;
    }
    auto motion = gpu.Texture(width, height, DXGI_FORMAT_R32G32_FLOAT, motion_pixels.data(), 8u);
    auto depth = gpu.Texture(width, height, DXGI_FORMAT_R32_FLOAT, depth_pixels.data(), 4u);
    auto mask = gpu.Texture(width, height, DXGI_FORMAT_R8_UNORM, mask_pixels.data(), 1u);
    auto ui = gpu.Texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, ui_pixels.data(), 4u);
    auto backbuffer = gpu.Texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, backbuffer_pixels.data(), 4u);
    auto alpha = gpu.Texture(width, height, DXGI_FORMAT_R8_UNORM, alpha_pixels.data(), 1u);
    const auto plan = sources::FoveatedWorkPlan(true, {55u, 55u, 28u, 15u, 50u, 50u}, {1280u, 720u}, {1024u, 576u});
    Check(plan.grid.active(), "test did not exercise foveation"); session.NoteFrameSize({plan.width, plan.height});
    const nr::Rect aux_region{16u, 12u, 640u, 360u};
    nr::FrameInputs inputs; inputs.depth = {depth.Get(), aux_region}; inputs.control_mask = {mask.Get(), aux_region};
    inputs.motion = {motion.Get(), aux_region};
    inputs.backbuffer = {backbuffer.Get(), aux_region}; inputs.ui = {ui.Get(), aux_region}; inputs.ui_alpha = {alpha.Get(), aux_region};
    inputs.motion_scale_x = .75f; inputs.motion_scale_y = -1.25f;
    inputs.native_nr = true; inputs.ui_correction = true;
    std::vector<uint32_t> large_pixels(size_t(1280u) * 720u, 0x7f606060u);
    auto large_color = gpu.Texture(1280u, 720u, DXGI_FORMAT_R8G8B8A8_UNORM, large_pixels.data(), 4u);
    auto large_output = gpu.Texture(1280u, 720u, DXGI_FORMAT_R8G8B8A8_UNORM, large_pixels.data(), 4u, UAV);
    sources::UavTarget large_target{.resource = large_output.Get(), .encoding = color::Encoding::SRGB, .source = {large_color.Get(), {0u, 0u, 1280u, 720u}}};
    sources::PipelineResult foveated;
    for (unsigned retry = 0u; retry < 8u; ++retry) {
      foveated = pipeline.RecordUav(gpu.list.Get(), large_target, inputs, {}, {.image = {plan.width, plan.height}, .foveation = plan.grid});
      if (foveated.reason != nr::SkipReason::RESIZING) break;
      gpu.Submit(); session.Tick();
    }
    if (!foveated.nr_applied) throw std::runtime_error("foveated native NR did not run: reason " + std::to_string(static_cast<uint32_t>(foveated.reason))
        + ", work " + std::to_string(plan.width) + "x" + std::to_string(plan.height) + ", session " + session.Status().message);
    const auto forwarded = runtime.last;
    Check(forwarded.depth.resource && forwarded.control_mask.resource && forwarded.backbuffer.resource && forwarded.ui.resource && forwarded.ui_alpha.resource
        && forwarded.ui_correction, "foveated optional inputs were lost");
    Check(forwarded.depth.resource != depth.Get() && forwarded.depth.rect.width == plan.width && forwarded.ui.rect.height == plan.height, "optional inputs kept the unwarped grid");
    Check(forwarded.motion.resource && forwarded.motion.resource != motion.Get() && forwarded.motion.resource->GetDesc().Width == plan.width
        && forwarded.motion_scale_x == 1.f && forwarded.motion_scale_y == 1.f, "native motion did not follow the warped colour");
    auto depth_read = gpu.Readback(forwarded.depth.resource, READ); auto mask_read = gpu.Readback(forwarded.control_mask.resource, READ);
    auto backbuffer_read = gpu.Readback(forwarded.backbuffer.resource, READ); auto ui_read = gpu.Readback(forwarded.ui.resource, READ);
    auto alpha_read = gpu.Readback(forwarded.ui_alpha.resource, READ); auto motion_read = gpu.Readback(forwarded.motion.resource, READ); gpu.Submit();
    CheckFoveatedPixels(plan, aux_region, depth_read, mask_read, backbuffer_read, ui_read, alpha_read, motion_read,
                        depth_pixels, mask_pixels, backbuffer_pixels, ui_pixels, alpha_pixels, motion_pixels, width);
    auto unsupported_mask = gpu.Texture(width, height, DXGI_FORMAT_R8_UINT, mask_pixels.data(), 1u);
    inputs.control_mask = {unsupported_mask.Get(), aux_region};
    Check(pipeline.RecordUav(gpu.list.Get(), large_target, inputs, {}, {.image = {plan.width, plan.height}, .foveation = plan.grid}).nr_applied
        && !runtime.last.control_mask.resource && runtime.last.depth.resource && runtime.last.ui.resource, "unsupported warped mask blocked NR or left a stale binding");
    gpu.Submit();
    Check(!input_logs.empty() && input_logs.back().find("ControlMask=unsupported-format") != std::string::npos,
          "unsupported FOV mask reason missing from input report");
    // A subsequent frame with missing resources clears every binding instead of reusing last frame's game inputs.
    inputs = {}; inputs.native_nr = true;
    Check(pipeline.RecordUav(gpu.list.Get(), large_target, inputs, {}, {.image = {plan.width, plan.height}, .foveation = plan.grid}).nr_applied, "missing optional resources blocked NR");
    Check(!runtime.last.depth.resource && !runtime.last.control_mask.resource && !runtime.last.backbuffer.resource
        && !runtime.last.ui.resource && !runtime.last.ui_alpha.resource && !runtime.last.ui_correction, "missing native resources left stale bindings"); gpu.Submit();
    Check(input_logs.back().find("Motion=missing -> zero-motion") != std::string::npos, "zero-motion fallback missing from input report");
    const auto log_count = input_logs.size();
    Check(pipeline.RecordUav(gpu.list.Get(), large_target, inputs, {}, {.image = {plan.width, plan.height}, .foveation = plan.grid}).nr_applied,
          "repeated native frame failed"); gpu.Submit();
    Check(input_logs.size() == log_count, "unchanged native inputs logged every frame");
    pipeline.ReleaseIntermediates(); timeline.Flush(std::chrono::milliseconds(0));
  }
  session.OnDeviceDestroyed();
  std::cout << "PASS separate/aliased Color-Output pixels, atlas regions, UAV restoration, failed evaluate, RGBA32F and foveated auxiliary inputs\n";
}
}
int main() { try {
  nr::SetLogSink([](nr::LogLevel, std::string_view message, void*) {
    if (message.starts_with("Native NR inputs:")) input_logs.emplace_back(message);
    if (message.starts_with("Native NR frame:")) frame_logs.emplace_back(message);
    if (message.starts_with("Native NR gap:")) gap_logs.emplace_back(message);
    if (message.starts_with("Native NR submit:")) submit_logs.emplace_back(message);
  }, nullptr, nr::LogLevel::TRACE);
  NativeModes(); ParameterViews(); ViewportLifetime(); PresetRestoreRetry(); FrameReservation(); Gpu gpu; CaptureAndPolicy(gpu); DisabledFallbackAndOwnership(gpu); Pixels(gpu); std::cout << "PASS native NR replacement regression\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; } }
