// Compile the real wrapper in this translation unit so the backend can be replaced without
// installing detours into a process. The static library then supplies only its other objects.
#include "addon/sl_fg_hooks.cpp"

#include <iostream>
#include <stdexcept>
#include <vector>

#include "addon/streamline_present.hpp"
#include "ipc/settings_text.hpp"
#include "ui/localization.hpp"
#include "ui/settings_schema.hpp"
#include "ui/status_text.hpp"

namespace uplift::addon::slfg {
namespace {
std::vector<sl::DLSSGOptions> submitted;
sl::Result backend_result = sl::Result::eOk;
uint32_t presented = 1u;

void Check(bool condition, const char* why) { if (!condition) throw std::runtime_error(why); }

sl::Result Backend(const sl::ViewportHandle&, const sl::DLSSGOptions& options) {
  submitted.push_back(options);
  return backend_result;
}
sl::Result BackendState(const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions*) {
  state.numFramesActuallyPresented = presented;
  state.numFramesToGenerateMax = 5u;
  state.bIsDynamicMFGSupported = sl::Boolean::eTrue;
  return sl::Result::eOk;
}

void Wrappers() {
  g_real_set_options.store(&Backend);
  g_real_get_state.store(&BackendState);
  const sl::ViewportHandle viewport{};
  sl::DLSSGOptions game{};
  game.mode = sl::DLSSGMode::eOn;
  game.numFramesToGenerate = 2u;
  game.flags = sl::DLSSGFlags::eRetainResourcesWhenOff | sl::DLSSGFlags::eDynamicResolutionEnabled;
  game.hudLessBufferFormat = 10u;
  game.uiBufferFormat = 28u;
  game.colorWidth = 2560u;
  game.colorHeight = 1440u;

  SetHudInputsOnly(true);
  for (size_t version = 1u; version <= 5u; ++version) {
    game.structVersion = version;
    Check(HookedSetOptions(viewport, game) == sl::Result::eOk, "supported Off request failed");
    const auto& sent = submitted.back();
    Check(sent.mode == sl::DLSSGMode::eOff && sent.numFramesToGenerate == 2u, "Off was emulated with a zero generation count");
    Check(!(sent.flags & sl::DLSSGFlags::eRetainResourcesWhenOff), "Off retained FG resources");
    Check(sent.flags & sl::DLSSGFlags::eDynamicResolutionEnabled, "unrelated flags changed");
    Check(sent.hudLessBufferFormat == 10u && sent.uiBufferFormat == 28u && sent.colorWidth == 2560u, "game input metadata was lost");
    Check(game.mode == sl::DLSSGMode::eOn && game.numFramesToGenerate == 2u
          && (game.flags & sl::DLSSGFlags::eRetainResourcesWhenOff), "game-owned options were changed");
  }
  Check(ReadLiveState().hud_inputs_active && ReadLiveState().multiplier == 1u, "accepted Off was not reflected in the live state");
  Check(streamline::EffectiveMode(streamline::FgMode::ON, HudInputsOnlyActive()) == streamline::FgMode::OFF,
        "effective mode still reports FG running");
  std::cout << "PASS supported ABI versions, real backend Off and original request preservation\n";

  SetForceMultiplier(4u);
  SetDynamicMfg(true, 144u);
  g_dynamic_d3d12.store(true);
  g_streamline_owner_seen.store(true);
  g_dynamic_support_seen.store(true);
  g_dynamic_supported.store(true);
  g_drain.store(1u);
  for (unsigned i = 0u; i < 4u; ++i) {
    Check(HookedSetOptions(viewport, game) == sl::Result::eOk, "repeated game request was refused");
    Check(submitted.back().mode == sl::DLSSGMode::eOff, "a repeated game request enabled generation");
  }
  Check(!Draining() && !DynamicReady(true) && !ReadLiveState().dynamic, "preset drain or Dynamic MFG overrode HUD-only mode");
  const size_t before_tick = submitted.size();
  FgPresetTick();
  Check(submitted.size() == before_tick, "preset tick restarted frame generation");
  SetForceMultiplier(0u);
  SetDynamicMfg(false, 0u);
  SetHudInputsOnly(false);
  RefreshFromPanel();
  Check(submitted.back().mode == sl::DLSSGMode::eOn && submitted.back().numFramesToGenerate == 2u
        && !HudInputsOnlyActive(), "disabling did not restore the game's request");
  SetHudInputsOnly(true);
  RefreshFromPanel();
  Check(submitted.back().mode == sl::DLSSGMode::eOff && HudInputsOnlyActive(), "live enable waited for a new game call");
  game.mode = sl::DLSSGMode::eOff;
  Check(HookedSetOptions(viewport, game) == sl::Result::eOk, "game Off was refused");
  SetHudInputsOnly(false);
  RefreshFromPanel();
  Check(submitted.back().mode == sl::DLSSGMode::eOff && ReadLiveState().multiplier == 1u, "a stale enabled request revived game FG");
  std::cout << "PASS priority, repeated submissions, live restore and game-side Off\n";

  game.mode = sl::DLSSGMode::eOn;
  SetHudInputsOnly(true);
  backend_result = sl::Result::eErrorInvalidState;
  const size_t before_refusal = submitted.size();
  Check(HookedSetOptions(viewport, game) == backend_result, "backend refusal was hidden");
  Check(submitted.size() == before_refusal + 1u && submitted.back().mode == sl::DLSSGMode::eOff, "refusal fell back to enabled generation");
  Check(ReadLiveState().hud_inputs_refused && !HudInputsOnlyActive(), "refused Off was reported as active");
  backend_result = sl::Result::eOk;
  Check(HookedSetOptions(viewport, game) == sl::Result::eOk && HudInputsOnlyActive(), "a game-side retry did not recover");
  SetHudInputsOnly(false);
  backend_result = sl::Result::eErrorInvalidState;
  RefreshFromPanel();
  Check(HudInputsOnlyActive() && ReadLiveState().hud_inputs_refused, "failed restore forgot that the backend is still Off");
  backend_result = sl::Result::eOk;
  Check(HookedSetOptions(viewport, game) == sl::Result::eOk && !HudInputsOnlyActive(), "game-side restore retry did not recover");

  SetHudInputsOnly(true);
  game.structVersion = 99u;
  const size_t before_unknown = submitted.size();
  Check(HookedSetOptions(viewport, game) == sl::Result::eErrorUnsupportedInterface, "unknown ABI was guessed");
  Check(submitted.size() == before_unknown && ReadLiveState().hud_inputs_unsupported, "unknown ABI reached the backend");
  SetHudInputsOnly(false);
  RefreshFromPanel();
  Check(submitted.size() == before_unknown, "unknown request replayed older cached options");
  game.structVersion = 5u;
  sl::DLSSGOptions extension{};
  game.next = &extension;
  SetHudInputsOnly(true);
  Check(HookedSetOptions(viewport, game) == sl::Result::eOk && submitted.back().next == &extension,
        "immediate request lost its extension chain");
  const size_t before_extension_replay = submitted.size();
  SetHudInputsOnly(false);
  RefreshFromPanel();
  Check(submitted.size() == before_extension_replay, "cached a caller-owned extension chain");
  game.next = nullptr;
  Check(HookedSetOptions(viewport, game) == sl::Result::eOk && !HudInputsOnlyActive(), "extension-chain game could not restore through its next call");
  std::cout << "PASS rejection, restore failure, safe retry and unknown ABI\n";

  game.structVersion = 5u;
  SetHudInputsOnly(true);
  Check(HookedSetOptions(viewport, game) == sl::Result::eOk, "Off setup failed");
  NoteFgEvaluate();
  NoteFgEvaluate();
  presented = 2u;  // Never falsify this even if a transition still presents a generated frame.
  sl::DLSSGState state{};
  Check(HookedGetState(viewport, state, &game) == sl::Result::eOk, "GetState failed");
  Check(state.numFramesActuallyPresented == 2u && ReadLiveState().presented_batch == 2u
        && ReadLiveState().fg_evaluates == 2u, "diagnostics hid real evaluate/presentation counts");
  SetHudInputsOnly(false);
  RefreshFromPanel();
  NoteFgEvaluate();
  Check(ReadLiveState().fg_evaluates == 2u, "disabled mode kept collecting evaluate counts");
  std::cout << "PASS observed evaluate counts and unmodified real presentation answers\n";
}

void SettingsAndText() {
  auto store = ipc::TextConfigStore::Parse("FGHudInputsOnly=1\nUICorrection=On\n");
  std::vector<std::string> warnings;
  auto settings = ui::LoadSettings(store, &warnings);
  Check(settings.fg_hud_inputs_only, "HUD-only config was not loaded");
  ipc::TextConfigStore saved;
  ui::SaveSettings(settings, &saved);
  Check(ui::LoadSettings(saved, &warnings).fg_hud_inputs_only, "HUD-only setting did not round-trip");
  const auto* row = ui::FindSetting("FGHudInputsOnly");
  Check(row && row->section == ui::SettingSection::DLSS_REFLEX, "HUD-only row is missing");
  ui::HudInputsFacts facts = {.wanted = true, .streamline = true, .off_applied = true,
      .hudless_tags = 100u, .opacity_tags = 90u, .fg_evaluates = 2u, .presented_batch = 1u};
  const auto zh = ui::Language::SIMPLIFIED_CHINESE;
  Check(ui::Localize(zh, ui::FormatHudInputsLive(facts)).find("后端已接受关闭") != std::string::npos, "active state is not translated");
  Check(ui::Localize(zh, ui::FormatHudInputsTags(facts)).find("100") != std::string::npos
        && ui::Localize(zh, ui::FormatHudInputsTags(facts)).find("近期标签 0/0") != std::string::npos, "missing tags were reported fresh or not translated");
  facts.refused = true;
  Check(ui::FormatHudInputsLive(facts).find("refused") != std::string::npos, "failure hidden by an earlier accepted Off");
  facts.wanted = false;
  Check(ui::FormatHudInputsLive(facts).find("restore") != std::string::npos || ui::FormatHudInputsLive(facts).find("restoring") != std::string::npos, "failed restore state is missing");
  facts.off_applied = false;
  Check(ui::FormatHudInputsLive(facts).empty() && ui::FormatHudInputsTags(facts).empty(), "disabled experiment still adds UI lines");
  std::cout << "PASS settings persistence, translated state and honest tag diagnostics\n";
}
}  // namespace
}  // namespace uplift::addon::slfg

int main() {
  try {
    uplift::addon::slfg::Wrappers();
    uplift::addon::slfg::SettingsAndText();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
