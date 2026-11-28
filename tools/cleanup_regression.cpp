#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "addon/stage_math.hpp"
#include "bridge/private_device_bridge.hpp"
#include "client/timeout.hpp"
#include "d3d11_texture.hpp"
#include "ipc/settings_text.hpp"
#include "nvfp4/Nvfp4Support.h"
#include "sources/auto_exposure.hpp"
#include "sources/stage_sets.hpp"
#include "sources/pipeline_rules.hpp"
#include "ui/localization.hpp"
#include "ui/settings_schema.hpp"

using namespace uplift;
void Check(bool condition, const char* why) { if (!condition) throw std::runtime_error(why); }

void Configuration() {
  constexpr std::array<std::string_view, 2> retired = {"DLSSQualityMode", "DLSSAutoExposure"};
  auto store = ipc::TextConfigStore::Parse(
      "DLSSQualityMode=obsolete\nDLSSAutoExposure=obsolete\nDynamicMFGTargetFps=97\nReflexFpsCap=117\n");
  std::vector<std::string> warnings;
  auto settings = ui::LoadSettings(store, &warnings);
  Check(warnings.empty(), "retired DLSS settings were parsed instead of ignored");
  for (const auto key : {"DynamicMFGTargetFps", "ReflexFpsCap"}) {
    const auto* descriptor = ui::FindSetting(key);
    Check(descriptor && descriptor->step == 1 && descriptor->fast_step == 32, "FPS increments are incorrect");
    Check(descriptor->get(settings) == (std::string_view(key) == "ReflexFpsCap" ? 117 : 97), "old config lost a live setting");
  }
  ipc::TextConfigStore saved;
  ui::SaveSettings(settings, &saved);
  for (const auto key : retired) Check(!saved.Get(key), "a retired DLSS setting was saved");
  const auto loaded = ui::LoadSettings(saved, &warnings);
  Check(ui::FindSetting("ReflexFpsCap")->get(loaded) == 117, "settings round trip lost the FPS cap");
  const auto zh = ui::Language::SIMPLIFIED_CHINESE;
  for (const auto line : {
       "Vulkan (64-bit): NR runs in Uplift's 64-bit helper (shared textures and fences, 12.5 MiB)",
       "Vulkan (64-bit): NR runs in Uplift's 64-bit helper (shared textures, CPU-ordered, 12.5 MiB): test"}) {
    const auto translated = ui::Localize(zh, line);
    Check(translated.find("共享纹理") != std::string::npos && translated.find("12.5 MiB") != std::string::npos,
          "current nested Vulkan status stopped translating");
  }
  std::cout << "PASS settings, FPS steps and live Vulkan translations\n";
}

void ExposureConfiguration() {
  auto store = ipc::TextConfigStore::Parse("AutoExposureMode=1\nAutoExposureBlend=0.8\n");
  auto settings = ui::LoadSettings(store, nullptr);
  Check(settings.auto_exposure_blend == 0.f && !ui::FindSetting("AutoExposureMode"), "Switch migration retained a runtime mode or old blend");
  ui::SaveSettings(settings, &store);
  settings.auto_exposure_blend = .35f;
  ui::SaveSettings(settings, &store);
  Check(ui::LoadSettings(store, nullptr).auto_exposure_blend == .35f, "legacy Switch key overrode a later blend edit");
  ipc::TextConfigStore fresh;
  ui::SaveSettings(settings, &fresh);
  Check(!fresh.Get("AutoExposureMode"), "new settings emitted the retired mode");
  for (const auto mode : {"0", "invalid", "2"}) {
    auto legacy = ipc::TextConfigStore::Parse(std::string("AutoExposureMode=") + mode + "\nAutoExposureBlend=0.4\n");
    Check(ui::LoadSettings(legacy, nullptr).auto_exposure_blend == .4f, "non-Switch legacy mode changed the blend");
  }
  const sources::ColorFixes defaults;
  const ui::Settings ui_defaults;
  Check(defaults.input_exposure == ui_defaults.input_exposure && defaults.auto_blend == ui_defaults.auto_exposure_blend
        && defaults.adapt_brighter == ui_defaults.adapt_brighter_stops && defaults.adapt_darker == ui_defaults.adapt_darker_stops,
        "pipeline and settings exposure defaults differ");
  settings.exposure_adapt = ui::ExposureAdapt::OFF;
  for (const auto key : {"ExposureAdapt", "AdaptBrighterStops", "AdaptDarkerStops", "AutoExposureBlend"}) {
    const auto& descriptor = *ui::FindSetting(key);
    const bool hidden = std::string_view(key).starts_with("Adapt");
    Check(descriptor.hidden(settings) == hidden && ui::ExposureRowHidden(descriptor, settings) == hidden,
          "exposure UI paths disagree about adaptation visibility");
  }
  std::cout << "PASS exposure migration, round trip, defaults and UI visibility\n";
}

void ExposureDecisions() {
  using namespace sources;
  for (const auto input : {color::InputExposure::AUTO, color::InputExposure::GAME, color::InputExposure::METERED, color::InputExposure::MANUAL}) {
    for (float blend : {0.f, .8f, 1.f}) for (bool game : {false, true}) for (bool meter : {false, true})
    for (bool state : {false, true}) for (bool latched : {false, true}) {
      ColorFixes fixes{.input_exposure = input, .auto_blend = blend};
      AutoExposure judge;
      if (latched) {
        const ExposureSample invalid{.texture_value = 64.f, .game = 64.f};
        for (unsigned n = 0; n < AutoExposure::SUSTAIN_SAMPLES; ++n) judge.Observe(invalid, n * .2);
        Check(judge.Latched(), "test failed to produce an invalid-game latch");
      }
      const auto decision = DecideExposure(fixes, game, meter, judge.Latched());
      const bool allocated = state && decision.WantsState();
      const auto outcome = PickExposure(decision, fixes, game, allocated, judge);
      ExposureSource expected = ExposureSource::NONE;
      bool probe = false;
      if (input == color::InputExposure::GAME) expected = ExposureSource::GAME;
      if (input == color::InputExposure::METERED && meter && allocated) expected = ExposureSource::STATE;
      if (input == color::InputExposure::AUTO) {
        if (!meter) expected = game ? ExposureSource::GAME : ExposureSource::NONE;
        else if (!game || latched) expected = allocated ? ExposureSource::STATE : ExposureSource::NONE;
        else {
          expected = blend == 0.f || !allocated ? ExposureSource::GAME : ExposureSource::STATE;
          probe = allocated && blend < 1.f;
        }
      }
      Check(outcome.source == expected && outcome.probe == probe, "exposure endpoint/capability/allocation matrix changed");
      Check(outcome.metered == (expected == ExposureSource::STATE), "exposure source and dispatch disagree");
      if (probe) Check(std::abs(outcome.game_weight - (1.f - blend)) < 1e-6f, "blend/direct-game history weight changed");
      if (input == color::InputExposure::AUTO && blend == 1.f && meter && allocated && !latched) {
        Check(!outcome.with_game && outcome.report.use == ExposureUse::METERED, "100% still binds/probes game exposure");
      }
    }
  }
  Check(AutoStops(0.f, false, 2.f, -2.f) == 2.f && AutoStops(1.f, false, 2.f, -2.f) == -2.f
        && AutoStops(.5f, false, 2.f, -2.f) == 0.f && AutoStops(0.f, true, 2.f, -2.f) == -2.f,
        "exposure blend/latch arithmetic changed");
  std::cout << "PASS exposure decision matrix and allocation fallbacks\n";
}

void ExposureValidity() {
  using namespace sources;
  ExposureSample unit;
  AutoExposure held;
  held.Observe(unit, 0.);
  unit.anchor_stops = 3.f;
  Check(held.Observe(unit, .5) && held.Reason() == AutoLatch::HELD, "unit exposure no longer detects scene movement");
  held.DiscardPending();
  Check(held.Latched() && held.HeldThroughMove(), "discard lost a permanent latch's diagnostics");
  AutoExposure candidate;
  unit.anchor_stops = 0.f;
  candidate.Observe(unit, 0.);
  // Product remains one and game EV barely changes; only the raw sentinel stops being unit.
  ExposureSample ordinary{.texture_value = 2.f, .factor = .5f, .game = 1.f, .anchor_stops = 3.f};
  Check(!candidate.Observe(ordinary, .1) && !candidate.HeldThroughMove(), "non-unit exposure retained Held tracking");
  unit.anchor_stops = 3.f;
  Check(!candidate.Observe(unit, .2), "ordinary exposure history contaminated a new unit candidate");
  unit.anchor_stops = 6.f;
  Check(candidate.Observe(unit, .3), "new unit candidate did not collect its own history");
  AutoExposure interrupted;
  unit.anchor_stops = 0.f;
  interrupted.Observe(unit, 0.);
  interrupted.DiscardPending();
  unit.anchor_stops = 3.f;
  Check(!interrupted.Observe(unit, .1), "interrupted probing retained Held history");
  AutoExposure unusable_sample;
  unit.anchor_stops = 0.f;
  unusable_sample.Observe(unit, 0.);
  ExposureSample unusable{.game = std::numeric_limits<float>::quiet_NaN()};
  Check(!unusable_sample.Observe(unusable, .1), "unusable sample latched exposure");
  unit.anchor_stops = 3.f;
  Check(unusable_sample.Observe(unit, .2) && unusable_sample.Reason() == AutoLatch::HELD,
        "unusable sample broke a pending unit exposure history");
  for (bool above : {false, true}) {
    AutoExposure judge;
    ExposureSample invalid{.texture_value = above ? 64.f : 1.f / 64.f, .game = above ? 64.f : 1.f / 64.f};
    // These samples may be polled together, but carry recording times one second apart.
    for (unsigned n = 0; n < AutoExposure::SUSTAIN_SAMPLES - 1; ++n) Check(!judge.Observe(invalid, n * .2), "sustained rule latched too early");
    Check(judge.Observe(invalid, 1.4) && judge.Reason() == (above ? AutoLatch::ABOVE : AutoLatch::BELOW), "sustained recording times lost");
  }
  AutoExposure short_run;
  ExposureSample invalid{.game = 64.f};
  for (unsigned n = 0; n < 20; ++n) Check(!short_run.Observe(invalid, n * .01), "sample count bypassed sustained duration");
  short_run.DiscardPending();
  Check(!short_run.Observe(invalid, 10.), "probe interruption retained invalid duration");
  std::cout << "PASS unit-only Held history, interrupted probes and sustained sample timing\n";
}

struct StageSet { struct { bool history_valid = true, change_valid = true; } look; };
void HistoryDecisions() {
  sources::LookConfig current;
  current.look.stabilize_change = true;
  std::array<StageSet, 3> sets;
  auto next = current;
  next.frame_seconds = .02f;
  sources::ApplyLookConfig(current, next, sets);
  for (const auto& set : sets) Check(set.look.history_valid && set.look.change_valid, "frame time reset histories");
  next.look.detail_radius += 1.f;
  sources::ApplyLookConfig(current, next, sets);
  for (const auto& set : sets) Check(!set.look.history_valid && set.look.change_valid, "radius change failed to reset every stage selectively");
  sets = {};
  next.look.stabilize_change = false;
  sources::ApplyLookConfig(current, next, sets);
  for (const auto& set : sets) Check(set.look.history_valid && !set.look.change_valid, "disabling change stabilization reset the wrong history");
  std::optional<sources::MeterBasis> basis;
  bool snap = false;
  Check(sources::UpdateMeterBasis(basis, snap, 0, color::Encoding::LINEAR_BT709, color::Primaries::AUTO, 1.f) && snap, "first meter basis did not snap");
  snap = false;
  Check(!sources::UpdateMeterBasis(basis, snap, 0, color::Encoding::LINEAR_BT709, color::Primaries::BT709, 1.f) && !snap,
        "equivalent resolved primaries reset the meter");
  Check(sources::UpdateMeterBasis(basis, snap, 1, color::Encoding::LINEAR_BT709, color::Primaries::BT709, 1.f) && snap,
        "moving stages retained a stale meter basis");
  snap = false;
  Check(sources::UpdateMeterBasis(basis, snap, 1, color::Encoding::LINEAR_BT709, color::Primaries::BT709, 2.f) && snap,
        "changing input units retained a stale meter basis");
  struct Owner {
    std::vector<int> order;
    void Main(int* value) { order.push_back(*value * 10); }
    void Look(int* value) { order.push_back(*value * 10 + 1); }
    void Faces(int* value) { order.push_back(*value * 10 + 2); }
    void Exposure(int* value) { order.push_back(*value * 10 + 3); }
  } owner;
  std::array<int, 3> retired = {0, 1, 2};
  sources::RetireStageSets(owner, retired, 1, &Owner::Main, &Owner::Look, &Owner::Faces, &Owner::Exposure);
  Check(owner.order == std::vector<int>{10, 11, 12, 13, 20, 21, 22, 23}, "retirement skipped its start or changed backend operation order");
  std::cout << "PASS selective history resets, meter basis and retirement order\n";
}

void LayoutAndTimeouts() {
  addon::FrameSettings settings;
  settings.resolution = ui::ResolutionMode::FULL;
  sources::WorkSizeSettle settle;
  const auto now = std::chrono::steady_clock::now();
  const auto full = addon::SettleStagePlan(settings, {1921, 1081}, addon::Stage::PRESENT, true, {}, false, settle, now);
  Check(full.width == 1921 && full.height == 1081, "Full lost odd output dimensions");
  settings.resolution = ui::ResolutionMode::CUSTOM;
  settings.resolution_scale = 50;
  settings.resolution_slider_held = true;
  const auto held = addon::SettleStagePlan(settings, {1280, 720}, addon::Stage::PRESENT, true, {}, false, settle, now);
  Check(held.width <= 1280 && held.height <= 720, "held stale layout escaped current source bounds");
  ngx_hooks::CreateSnapshot snapshot;
  snapshot.render = {1280, 720};
  settings.resolution = ui::ResolutionMode::MATCH_GAME;
  settings.resolution_slider_held = false;
  settle.Reset();
  const auto render = addon::SettleStagePlan(settings, {1920, 1080}, addon::Stage::AFTER_DLSS, true, snapshot, false, settle, now);
  Check(render.width == 1280 && render.height == 720, "Match game lost the create-time render size");
  settle.Reset();
  const auto off = addon::SettleStagePlan(settings, {1920, 1080}, addon::Stage::AFTER_DLSS, true, snapshot, true, settle, now);
  Check(off.width == 1920 && off.height == 1080, "DLSS off reused a stale render size");
  uint32_t consecutive = 0, stops = 0;
  auto stop = [&](const std::string& reason) { ++stops; Check(reason.find("3 times in a row") != std::string::npos, "timeout reason changed"); };
  client::CountTimeout(consecutive, "copy", std::chrono::milliseconds{2000}, stop);
  client::CountTimeout(consecutive, "copy", std::chrono::milliseconds{2000}, stop);
  Check(stops == 0, "transport stopped before its timeout threshold");
  client::CountTimeout(consecutive, "copy", std::chrono::milliseconds{2000}, stop);
  Check(stops == 1, "transport failed to stop at its timeout threshold");
  std::string latch;
  bridge::StopCrossApiWaits(latch, "first", nullptr, 7, nullptr, 9);
  bridge::StopCrossApiWaits(latch, "second", nullptr, 7, nullptr, 9);
  Check(latch == "first" && !d3d::TextureDesc11(nullptr), "CPU transport lost its first latch or accepted a null texture");
  std::cout << "PASS layout bounds, DLSS fallback and transport timeout threshold\n";
}

void Original() {}
void Detour() {}
void Dispatch() {
  struct State {
    std::recursive_mutex mutex;
    void (*createModule)() = nullptr, (*createFunction)() = nullptr, (*launch)() = nullptr,
         (*destroyModule)() = nullptr, (*destroyFunction)() = nullptr;
  } state, other;
  auto* original = static_cast<void*>(&Original);
  auto* detour = static_cast<void*>(&Detour);
  nvfp4::KernelDetours detours{detour, detour, detour, detour, detour};
  Check(nvfp4::WrapKernelNvapi(state, 0x24973538, nullptr, detours) == nullptr && !state.launch, "null original installed a detour");
  Check(nvfp4::WrapKernelNvapi(state, 0, original, detours) == original && !state.launch, "unknown NVAPI entry was intercepted");
  Check(nvfp4::WrapKernelNvapi(state, 0x24973538, original, detours) == detour && state.launch == &Original
        && !state.createModule && !other.launch, "dispatch mutated another entry or model state");
  std::cout << "PASS NVAPI null/unknown handling and per-model state isolation\n";
}

int main() {
  try { Configuration(); ExposureConfiguration(); ExposureDecisions(); ExposureValidity(); HistoryDecisions(); LayoutAndTimeouts(); Dispatch(); return 0; }
  catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
