#include <array>
#include <chrono>
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
#include "ui/localization.hpp"
#include "ui/settings_schema.hpp"

using namespace uplift;
void Check(bool condition, const char* why) { if (!condition) throw std::runtime_error(why); }

void Configuration() {
  auto store = ipc::TextConfigStore::Parse("ConfigVersion=3\nPedestalRemoval=obsolete\nDynamicMFGTargetFps=97\nReflexFpsCap=117\n");
  std::vector<std::string> warnings;
  auto settings = ui::LoadSettings(store, &warnings);
  Check(warnings.empty(), "retired setting blocked a valid old config");
  Check(ui::FindSetting("PedestalRemoval") == nullptr, "retired setting remains in the schema");
  for (const auto key : {"DynamicMFGTargetFps", "ReflexFpsCap"}) {
    const auto* descriptor = ui::FindSetting(key);
    Check(descriptor && descriptor->step == 1 && descriptor->fast_step == 32, "FPS increments are incorrect");
    Check(descriptor->get(settings) == (std::string_view(key) == "ReflexFpsCap" ? 117 : 97), "old config lost a live setting");
  }
  ipc::TextConfigStore saved;
  ui::SaveSettings(settings, &saved);
  Check(!saved.Get("PedestalRemoval"), "retired setting was serialized again");
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
  std::cout << "PASS old config compatibility, FPS steps and live Vulkan translations\n";
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
  try { Configuration(); HistoryDecisions(); LayoutAndTimeouts(); Dispatch(); return 0; }
  catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
