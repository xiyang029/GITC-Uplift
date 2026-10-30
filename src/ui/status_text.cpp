#include "ui/status_text.hpp"
#include "nr/types.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

namespace uplift::ui {

SetupView ResolveSetup(const SetupFacts& facts) {
  SetupView view;
  const auto at = [](auto& options, auto value) -> OptionState& { return options[static_cast<size_t>(value)]; };
  const auto selectable = [](const OptionState& option) { return option.why.empty(); };
  const auto grey_for_dlss = [&facts](OptionState* option) {
    if (!facts.dlss_unavailable.empty()) {
      *option = {.why = facts.dlss_unavailable};
    } else if (!facts.dlss_seen) {
      *option = {.why = DLSS_NOT_SEEN_REASON, .temporary = true};
    } else if (facts.dlss_off) {
      *option = {.why = DLSS_OFF_REASON, .temporary = true};  // Plan 18 Task 12
    }
  };

  // NR stage.
  grey_for_dlss(&at(view.stage_options, SourcePick::AFTER_DLSS));
  OptionState& before = at(view.stage_options, SourcePick::BEFORE_UPSCALING);
  grey_for_dlss(&before);
  if (selectable(before) && facts.ray_reconstruction && facts.ray_reconstruction_after_dlss) {
    before = {.why = "The game's DLSS is Ray Reconstruction, which keeps NR after DLSS", .temporary = true};
  }
  view.before_upscaling_on_rr = (facts.ray_reconstruction && !facts.ray_reconstruction_after_dlss);
  // Plan 18: a stage whose image the Direct3D 11 bridge cannot share is a fixed cause for that stage alone; it beats the temporary ones.
  const auto grey_fixed = [](OptionState* option, std::string_view fixed) {
    if (!fixed.empty() && (option->why.empty() || option->temporary)) {
      *option = {.why = fixed};
    }
  };
  grey_fixed(&at(view.stage_options, SourcePick::AFTER_DLSS), facts.after_dlss_fixed);
  grey_fixed(&before, facts.before_upscaling_fixed);
  if (!facts.present_fixed.empty()) {
    at(view.stage_options, SourcePick::PRESENT) = {.why = facts.present_fixed};  // a fixed cause beats the temporary one: it would not clear
  } else if (facts.frame_generation_blocks_present) {
    at(view.stage_options, SourcePick::PRESENT) = {.why = "Frame generation is on: turn on Present with frame generation (Advanced)", .temporary = true};
  }
  // Plan 15: held after the game's NGX shutdown on Direct3D 12, no stage can run until the game's DLSS starts again. That beats the other temporary causes
  // (it is why nothing runs now); a fixed cause keeps its own text, since it would not clear with the hold.
  const auto hold = [&facts](OptionState* option) {
    if (!facts.held.empty() && (option->why.empty() || option->temporary)) {
      *option = {.why = facts.held, .temporary = true};
    }
  };
  for (OptionState& option : view.stage_options) {
    hold(&option);
  }
  const OptionState stage_wish = at(view.stage_options, facts.preferred_stage);  // the preference's option, before the running one is un-greyed
  if (facts.stage) {
    view.stage = *facts.stage;
  } else if (selectable(stage_wish)) {
    view.stage = facts.preferred_stage;
  } else if (facts.preferred_stage == SourcePick::BEFORE_UPSCALING && selectable(at(view.stage_options, SourcePick::AFTER_DLSS))) {
    view.stage = SourcePick::AFTER_DLSS;  // Ray Reconstruction keeps NR after DLSS
  } else {
    view.stage = SourcePick::PRESENT;
  }

  // Motion vectors (Launchpad depends on the stage just decided).
  OptionState& dlss = at(view.motion_options, MotionPick::DLSS);
  grey_for_dlss(&dlss);
  if (dlss.why.empty() || dlss.temporary) {
    // A fixed reason beats "not seen yet": it would not clear when DLSS starts.
    if (!facts.dlss_motion_fixed.empty()) {
      dlss = {.why = facts.dlss_motion_fixed};
    } else if (selectable(dlss) && !facts.game_passes_motion) {
      dlss = {.why = "The game passes DLSS no motion vectors", .temporary = true};
    }
  }
  OptionState& launchpad = at(view.motion_options, MotionPick::LAUNCHPAD);
  if (!facts.launchpad_fixed.empty()) {
    launchpad = {.why = facts.launchpad_fixed};  // a fixed cause beats the temporary ones: it would not clear
  } else if (view.stage != SourcePick::PRESENT && !facts.present_fixed.empty()) {
    launchpad = {.why = facts.present_fixed};  // Launchpad runs at Present only, which cannot run again
  } else if (view.stage != SourcePick::PRESENT) {
    launchpad = {.why = "Launchpad feeds NR on the presented image only", .temporary = true};
  } else if (!facts.launchpad_ready) {
    launchpad = {.why = "Turn iMMERSE Launchpad on and enable Uplift.fx's Uplift technique below it", .temporary = true};
  }
  // 2026-10-08: Lumenite, as Launchpad.
  OptionState& lumenite = at(view.motion_options, MotionPick::LUMENITE);
  if (!facts.lumenite_fixed.empty()) {
    lumenite = {.why = facts.lumenite_fixed};
  } else if (view.stage != SourcePick::PRESENT && !facts.present_fixed.empty()) {
    lumenite = {.why = facts.present_fixed};
  } else if (view.stage != SourcePick::PRESENT) {
    lumenite = {.why = "Lumenite feeds NR on the presented image only", .temporary = true};
  } else if (!facts.lumenite_ready) {
    lumenite = {.why = "Turn LumeniteFX's Kernel on and enable Uplift.fx's Uplift technique below it", .temporary = true};
  }
  hold(&dlss);
  hold(&launchpad);
  hold(&lumenite);
  const OptionState motion_wish = at(view.motion_options, facts.preferred_motion);
  MotionPick resolved_motion = MotionPick::OFF;
  if (selectable(motion_wish)) {
    resolved_motion = facts.preferred_motion;
  } else if (facts.motion_auto && selectable(launchpad)) {
    resolved_motion = MotionPick::LAUNCHPAD;
  } else if (facts.motion_auto && selectable(lumenite)) {
    resolved_motion = MotionPick::LUMENITE;  // Auto's order: DLSS, Launchpad, Lumenite
  } else if ((facts.preferred_motion == MotionPick::LAUNCHPAD || facts.preferred_motion == MotionPick::LUMENITE) && view.stage != SourcePick::PRESENT
             && selectable(dlss)) {
    resolved_motion = MotionPick::DLSS;  // inside the game's frame a Launchpad or Lumenite preference runs on the game's own vectors (the engine's rule)
  }
  view.motion = facts.motion.value_or(resolved_motion);

  // Resolution.
  OptionState& match_game = at(view.resolution_options, ResolutionMode::MATCH_GAME);
  if (!facts.match_game_readable && facts.match_game_at_dlss_stages) {
    match_game = {.why = VULKAN_MATCH_GAME_REASON, .temporary = true};  // final review, minor 3: a DLSS stage here reads it
  } else if (!facts.match_game_readable) {
    match_game = {.why = "Match game needs the game's DLSS render size, which Uplift cannot read here"};
  } else if (!facts.dlss_seen) {
    match_game = {.why = DLSS_NOT_SEEN_REASON, .temporary = true};
  } else if (facts.dlss_off) {
    match_game = {.why = DLSS_OFF_REASON, .temporary = true};  // Plan 18 Task 12 (fix round 1, M-1): it reads DLSS's render size, which is off too
  }
  if (!facts.below_full_fixed.empty()) {
    for (size_t index = 1u; index < view.resolution_options.size(); ++index) {
      view.resolution_options[index] = {.why = facts.below_full_fixed};
    }
  }
  const OptionState resolution_wish = at(view.resolution_options, facts.preferred_resolution);
  view.resolution = facts.resolution.value_or(selectable(resolution_wish) ? facts.preferred_resolution : ResolutionMode::FULL);

  // What runs is possible by definition: its option is never greyed.
  at(view.stage_options, view.stage) = {};
  at(view.motion_options, view.motion) = {};
  at(view.resolution_options, view.resolution) = {};

  // Split chain (Task 3): passes 2..N's "Runs at" rows. Where each pass runs is the context's chain; without one, the stored picks from pass 1's highlight,
  // raised to the pass before's stage (DecideChain's never-backwards rule; its fallbacks need the engine's facts).
  const uint32_t passes = std::clamp(facts.pass_count, 1u, MAX_PASSES);
  std::array<SourcePick, MAX_PASSES> runs = {};
  runs.fill(view.stage);
  if (facts.chain) {
    runs = *facts.chain;
  } else if (facts.split_capable) {
    for (uint32_t pass = 1u; pass < passes; ++pass) {
      const PassStage stored = facts.stored_stages[pass - 1u];
      runs[pass] = runs[pass - 1u];
      if (stored != PassStage::SAME) {
        runs[pass] = std::max(runs[pass - 1u], static_cast<SourcePick>(static_cast<uint32_t>(stored) - 1u));
      }
    }
  }
  view.split_capable = facts.split_capable;
  const auto runs_at = [](SourcePick stage) -> std::string_view {
    switch (stage) {
      case SourcePick::BEFORE_UPSCALING: return "before upscaling";
      case SourcePick::AFTER_DLSS:       return "after DLSS";
      case SourcePick::PRESENT:          break;
    }
    return "at Present";
  };
  for (uint32_t pass = 1u; pass < passes; ++pass) {
    PassStageRow& row = view.pass_stages[pass - 1u];
    const SourcePick pass_before = runs[pass - 1u];
    row.runs = runs[pass];
    row.starts_group = (facts.split_capable && runs[pass] != pass_before);
    view.chain_split = view.chain_split || (runs[pass] != runs[0]);
    const PassStage stored = facts.stored_stages[pass - 1u];
    const auto runs_pick = static_cast<PassStage>(static_cast<uint32_t>(runs[pass]) + 1u);
    const bool runs_as_stored = (stored == runs_pick || (stored == PassStage::SAME && runs[pass] == pass_before));
    row.shown = (runs_as_stored ? stored : runs_pick);
    if (!facts.split_capable) {
      for (PassStageOption& option : row.options) {
        option = {.why = std::string(ONE_STAGE_REASON)};  // the highlight too: nothing here is a choice
      }
      continue;
    }
    for (uint32_t stage = 0u; stage < static_cast<uint32_t>(view.stage_options.size()); ++stage) {
      PassStageOption& option = row.options[stage + 1u];
      const auto pick = static_cast<SourcePick>(stage);
      const OptionState& setup_option = view.stage_options[stage];
      if (pick < pass_before) {
        option = {.why = std::format("Runs the chain backwards: pass {} runs {}", pass, runs_at(pass_before))};
      } else if (pick != pass_before && !setup_option.why.empty()) {
        option = {.why = std::string(setup_option.why), .temporary = setup_option.temporary};  // never the pass before's stage, which always takes it
      }
    }
    row.options[static_cast<size_t>(row.shown)] = {};  // what runs is possible by definition
  }

  // Plan 15 fix round (minor 6): stopped for the session, nothing can run, the highlights included: every option of the three rows is greyed with the fixed
  // reason (the card says it too), and there is no Why line. Split chain fix round 1 (M5): the Runs at rows' too.
  if (!facts.stopped.empty()) {
    view.stopped = true;
    const auto stop = [&facts](auto& options) {
      for (OptionState& option : options) {
        option = {.why = facts.stopped};
      }
    };
    stop(view.stage_options);
    stop(view.motion_options);
    stop(view.resolution_options);
    for (PassStageRow& row : view.pass_stages) {  // fix round 1 (M5): the Runs at rows too
      for (PassStageOption& option : row.options) {
        option = {.why = std::string(facts.stopped)};
      }
    }
    return view;
  }
  // Plan 15: while held, the card says why NR waits (CardFacts::held), and a held context's card is never the working one, which alone draws the Why line.
  if (!facts.held.empty()) return view;

  // The Why line: temporary fallbacks from the preference only (a fixed cause has its tooltip). The first sentence as written (it starts with a name),
  // later ones with a capital.
  const auto add = [&view](std::string sentence) {
    if (!view.why.empty()) {
      sentence[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(sentence[0])));
      view.why += ' ';
    }
    view.why += sentence;
  };
  // A DLSS stage waits for the game's DLSS: not seen yet, or seen (a create) but not evaluated yet, or Vulkan's hand-off from Present.
  const bool stage_waits = (view.stage != facts.preferred_stage && facts.preferred_stage != SourcePick::PRESENT
                            && (selectable(stage_wish) || stage_wish.why == DLSS_NOT_SEEN_REASON));
  const bool motion_falls_back = (facts.preferred_motion == MotionPick::DLSS && view.motion != MotionPick::DLSS);
  const bool motion_waits = (motion_falls_back
                             && (motion_wish.why == DLSS_NOT_SEEN_REASON || (selectable(motion_wish) && facts.dlss_motion_gap == MotionGap::NONE)));
  const std::string_view stage_name = (facts.preferred_stage == SourcePick::BEFORE_UPSCALING ? "Before upscaling" : "After DLSS");
  // Plan 18 Task 12: the game switched its DLSS off: what fell back returns when it is on again.
  const bool stage_off = (view.stage != facts.preferred_stage && facts.preferred_stage != SourcePick::PRESENT && stage_wish.why == DLSS_OFF_REASON);
  const bool motion_off = (motion_falls_back && motion_wish.why == DLSS_OFF_REASON);
  if (stage_off) {
    add(motion_off ? std::format("{} and DLSS's motion vectors return when the game's DLSS is on again.", stage_name)
                   : std::format("{} returns when the game's DLSS is on again.", stage_name));
  } else if (stage_waits) {
    add(motion_waits ? std::format("{} and DLSS's motion vectors start when the game renders with DLSS.", stage_name)
                     : std::format("{} starts when the game renders with DLSS.", stage_name));
  } else if (view.stage != facts.preferred_stage && stage_wish.temporary && facts.preferred_stage == SourcePick::BEFORE_UPSCALING) {
    add("Before upscaling returns when the game's DLSS is not Ray Reconstruction.");
  }
  if (motion_off) {
    if (!stage_off) {
      add("DLSS's motion vectors return when the game's DLSS is on again.");  // else said with the stage above
    }
  } else if (motion_waits && !stage_waits) {
    add("DLSS's motion vectors start when the game renders with DLSS.");
  } else if (motion_falls_back && (facts.dlss_motion_gap == MotionGap::GAME_PASSED_NONE || (motion_wish.temporary && !motion_waits))) {
    add("the game passes DLSS no motion vectors.");
  } else if (motion_falls_back && selectable(motion_wish) && facts.dlss_motion_gap == MotionGap::NO_COPY) {
    add("no copy of DLSS's motion vectors reaches the Present path (the GPU runs too far behind).");
  } else if (motion_falls_back && selectable(motion_wish) && facts.dlss_motion_gap == MotionGap::DLSS_PAUSED) {
    add("DLSS's motion vectors return when the game's DLSS runs again (it is paused, as in a menu or a loading screen).");
  }
  if (facts.preferred_motion == MotionPick::LAUNCHPAD && view.motion != MotionPick::LAUNCHPAD) {
    if (motion_wish.temporary && view.stage != SourcePick::PRESENT) {
      add("Launchpad's vectors return when NR runs at Present; inside the game's frame NR uses the game's own.");
    } else if (motion_wish.temporary) {
      add("Launchpad's vectors return when Launchpad and Uplift.fx's Uplift technique are on.");
    } else if (selectable(motion_wish) && facts.launchpad_gap == MotionGap::NO_UPLIFT_MV) {
      add("no UPLIFT_MV arrives: put Uplift.fx's Uplift technique below Launchpad.");
    }
  }
  if (facts.preferred_motion == MotionPick::LUMENITE && view.motion != MotionPick::LUMENITE) {
    if (motion_wish.temporary && view.stage != SourcePick::PRESENT) {
      add("Lumenite's vectors return when NR runs at Present; inside the game's frame NR uses the game's own.");
    } else if (motion_wish.temporary) {
      add("Lumenite's vectors return when Lumenite's Kernel and Uplift.fx's Uplift technique are on.");
    } else if (selectable(motion_wish) && facts.launchpad_gap == MotionGap::NO_UPLIFT_MV) {
      add("no UPLIFT_MV arrives: put Uplift.fx's Uplift technique below Lumenite's Kernel.");
    }
  }
  if (view.resolution != facts.preferred_resolution && facts.preferred_resolution == ResolutionMode::MATCH_GAME
      && (selectable(resolution_wish) || resolution_wish.why == DLSS_NOT_SEEN_REASON)) {
    add("Match game applies when the game renders with DLSS.");
  } else if (view.resolution != facts.preferred_resolution && facts.preferred_resolution == ResolutionMode::MATCH_GAME
             && resolution_wish.why == VULKAN_MATCH_GAME_REASON) {
    add("Match game applies when NR runs at After DLSS or Before upscaling.");
  } else if (view.resolution != facts.preferred_resolution && facts.preferred_resolution == ResolutionMode::MATCH_GAME
             && resolution_wish.why == DLSS_OFF_REASON) {
    add("Match game applies when the game's DLSS is on again.");  // Plan 18 Task 12 (fix round 1, M-1)
  }
  return view;
}

LiveCard BuildLiveCard(const SetupFacts& facts, const SetupView& shown) {
  LiveCard card;
  switch (shown.stage) {
    case SourcePick::BEFORE_UPSCALING: card.stage = "Before upscaling"; break;
    case SourcePick::AFTER_DLSS:       card.stage = "After DLSS"; break;
    case SourcePick::PRESENT:          card.stage = "Present"; break;
  }
  switch (shown.motion) {
    case MotionPick::OFF:       card.motion = "none"; break;
    case MotionPick::DLSS:      card.motion = (facts.motion_copied ? "DLSS (the game's, copied for Present)" : "DLSS (the game's)"); break;
    case MotionPick::LAUNCHPAD: card.motion = "Launchpad"; break;
    case MotionPick::LUMENITE:  card.motion = "Lumenite"; break;
  }
  std::string mode;
  switch (shown.resolution) {
    case ResolutionMode::FULL:        mode = "Full"; break;
    case ResolutionMode::QUALITY:     mode = "Quality"; break;
    case ResolutionMode::BALANCED:    mode = "Balanced"; break;
    case ResolutionMode::PERFORMANCE: mode = "Performance"; break;
    case ResolutionMode::MATCH_GAME:  mode = "Match game"; break;
    case ResolutionMode::CUSTOM:
      mode = (facts.resolution_asymmetric ? std::format("Custom {:.0f} x {:.0f} %", facts.custom_scale, facts.custom_scale_y)
                                          : std::format("Custom {:.0f} %", facts.custom_scale));
      break;
  }
  const std::string_view upsampling = (facts.upsampling == color::Upsampling::CLASSIC ? "Classic" : "Edge-aware");
  const nr::Size work = facts.work;
  const bool full_size = (facts.frame.Empty() || work == facts.frame);
  if (work.Empty()) {
    card.resolution = mode;
  } else if (shown.stage == SourcePick::BEFORE_UPSCALING) {
    if (!facts.canvas.Empty() && facts.canvas != work) {
      card.resolution = std::format("{}x{} ({}, a {}x{} render padded)", facts.canvas.width, facts.canvas.height, mode, work.width, work.height);
    } else if (full_size) {
      card.resolution = std::format("{}x{} ({}, the render size)", work.width, work.height, mode);
    } else {
      card.resolution = std::format("{}x{} of the {}x{} render ({}, {})", work.width, work.height, facts.frame.width, facts.frame.height, mode, upsampling);
    }
  } else if (full_size) {
    card.resolution = std::format("{}x{} ({})", work.width, work.height, mode);
  } else {
    card.resolution = std::format("{}x{} of {}x{} ({}, {})", work.width, work.height, facts.frame.width, facts.frame.height, mode, upsampling);
  }
  for (const std::string_view note : {facts.latch_note, facts.passes_note, facts.chain_note, facts.frame_generation_line}) {
    if (!note.empty()) {
      card.note = std::string(note);
      break;
    }
  }
  card.footer = std::format("{} · {}/{} {}", facts.api, facts.passes_run, facts.passes_requested, (facts.passes_requested == 1u ? "pass" : "passes"));
  if (facts.vram_bytes > 0u) {
    card.footer += std::format(" · VRAM {:.0f} MiB", static_cast<double>(facts.vram_bytes) / (1024.0 * 1024.0));
  }
  return card;
}

SetupView SetupSettle::Update(SetupView resolved, std::chrono::steady_clock::time_point now, std::chrono::milliseconds settle) {
  if (!last_update_ || now - *last_update_ > 2 * settle) {
    rows_ = {};  // the first draw, or the overlay was closed for a while: what runs shows at once
  }
  last_update_ = now;
  const std::array<uint32_t, 3> wanted = {
      static_cast<uint32_t>(resolved.stage),
      static_cast<uint32_t>(resolved.motion),
      static_cast<uint32_t>(resolved.resolution),
  };
  std::array<uint32_t, 3> shown = {};
  bool settling = false;
  for (size_t index = 0u; index < rows_.size(); ++index) {
    Row& row = rows_[index];
    if (!row.shown || row.shown == wanted[index]) {
      row.shown = wanted[index];
      row.candidate.reset();
    } else {
      if (row.candidate != wanted[index]) {
        row.candidate = wanted[index];
        row.since = now;
      }
      if (now - row.since >= settle) {
        row.shown = wanted[index];
        row.candidate.reset();
      }
    }
    shown[index] = *row.shown;
    settling = settling || (*row.shown != wanted[index]);
  }
  resolved.stage = static_cast<SourcePick>(shown[0]);
  resolved.motion = static_cast<MotionPick>(shown[1]);
  resolved.resolution = static_cast<ResolutionMode>(shown[2]);
  // What is shown is what ran a moment ago: never greyed, and the Why belongs to the resolved highlights. Plan 15 fix round: unless nothing can run again.
  if (!resolved.stopped) {
    resolved.stage_options[shown[0]] = {};
    resolved.motion_options[shown[1]] = {};
    resolved.resolution_options[shown[2]] = {};
  }
  if (settling) {
    resolved.why.clear();
  }
  return resolved;
}

void SetupSettle::Clicked(SetupRow row, uint32_t index) {
  Row& clicked = rows_[static_cast<size_t>(row)];
  clicked.shown = index;
  clicked.candidate.reset();
}

void SetupSettle::ClickedBest(SetupRow row, uint32_t index, const SetupView& options, bool motion_auto) {
  const auto runs = [](const auto& states, auto pick) { return states[static_cast<size_t>(pick)].why.empty(); };
  uint32_t best = index;
  switch (row) {
    case SetupRow::STAGE:
      if (!runs(options.stage_options, index)) {
        const bool after_dlss = (index == static_cast<uint32_t>(SourcePick::BEFORE_UPSCALING) && runs(options.stage_options, SourcePick::AFTER_DLSS));
        best = static_cast<uint32_t>(after_dlss ? SourcePick::AFTER_DLSS : SourcePick::PRESENT);
      }
      break;
    case SetupRow::MOTION:
      if (!runs(options.motion_options, index)) {
        const std::optional<uint32_t>& stage = rows_[static_cast<size_t>(SetupRow::STAGE)].shown;
        const bool present = (!stage || *stage == static_cast<uint32_t>(SourcePick::PRESENT));
        const bool launchpad = (motion_auto && present && runs(options.motion_options, MotionPick::LAUNCHPAD));
        const bool lumenite = (motion_auto && present && runs(options.motion_options, MotionPick::LUMENITE));  // Auto's order
        best = static_cast<uint32_t>(launchpad ? MotionPick::LAUNCHPAD : lumenite ? MotionPick::LUMENITE : MotionPick::OFF);
      }
      break;
    case SetupRow::RESOLUTION:
      if (!runs(options.resolution_options, index)) {
        best = static_cast<uint32_t>(ResolutionMode::FULL);
      }
      break;
  }
  Clicked(row, best);
}

std::string FormatStatusLine(const StatusView& view) {
  std::string line(nr::SessionStateName(view.state));
  if (view.suspended) {
    line += " (suspended: game needs VRAM)";
  }
  if (view.state == nr::SessionState::GRACE) {
    // Round up to whole seconds: a caller must never see "released in 0 s" while still counting down.
    const auto seconds_left = (view.grace_remaining.count() + 999) / 1000;
    line += std::format(": NR memory is released in {} s", seconds_left);
    return line;
  }
  if (view.state != nr::SessionState::ACTIVE) return line;
  line += std::format(" | {}/{} passes", view.passes_run, view.passes_requested);
  if (!view.frame.Empty()) {
    line += std::format(" | {}x{}", view.frame.width, view.frame.height);
  }
  if (!view.trigger.empty()) {
    line += std::format(" | {}", view.trigger);
  }
  if (view.encoding) {
    line += std::format(" | {}", color::EncodingName(*view.encoding));
    if (*view.encoding != color::Encoding::SRGB && view.diffuse_white_nits > 0.f) {
      line += std::format(", diffuse white {} nits", view.diffuse_white_nits);
    }
  }
  return line;
}

std::string FormatFrameGenerationLine(bool active, uint32_t multiplier, bool from_ngx) {
  if (!active) return "Frame generation: off";
  return std::format("Frame generation: on ({}x{})", multiplier, (from_ngx ? ", NGX" : ""));
}

std::string FormatDlssLive(const std::string& mode_name, const std::string& preset_name, const std::string& driver_preset,
                            uint32_t render_width, uint32_t render_height, uint32_t output_width, uint32_t output_height) {
  // One complete line, whichever parts exist: Localize matches whole strings, so a concatenation would
  // never be translated. The sizes come from the game's own create, so they are what DLSS is rendering
  // into -- the one place a render-scale override becomes visible.
  const bool sized = (render_width != 0u && render_height != 0u && output_width != 0u && output_height != 0u);
  std::string line;
  if (driver_preset.empty() && preset_name.empty()) {
    if (mode_name.empty()) return {};
    line = std::format("DLSS: {}", mode_name);
  } else if (driver_preset.empty()) {
    line = std::format("DLSS: {} (preset {})", mode_name, preset_name);
  } else if (preset_name.empty()) {
    line = std::format("DLSS: NVIDIA App preset {} wins", driver_preset);
  } else {
    line = std::format("DLSS: {} (preset {}) · NVIDIA App preset {} wins", mode_name, preset_name, driver_preset);
  }
  if (sized) line += std::format(" {}x{} -> {}x{}", render_width, render_height, output_width, output_height);
  // The sizes are a plain suffix rather than a format field: Localize matches the sentence, and a
  // placeholder at the very end would swallow any trailing text.
  return line;
}

std::string FormatRrLive(const std::string& preset_name) {
  if (preset_name.empty()) return {};
  return std::format("Ray Reconstruction: preset {}", preset_name);
}

std::string FormatFgPresetLive(const std::string& preset_name, bool live_switch, bool pending_drain) {
  if (preset_name.empty()) return {};
  if (pending_drain) return std::format("Frame generation model: switching to {} (one frame off)", preset_name);
  if (live_switch) return std::format("Frame generation model: {} (applied live)", preset_name);
  return std::format("Frame generation model: {} (next start: no Streamline)", preset_name);
}

std::string FormatMultiplierLive(bool dynamic, uint32_t multiplier, bool panel_multiplier, uint32_t panel_choice) {
  if (dynamic) return "Frame generation: Dynamic MFG picks the multiplier";
  if (multiplier < 2u) return {};
  if (panel_multiplier) return std::format("Frame generation: {}x (the panel's choice)", multiplier);
  if (panel_choice >= 2u) return std::format("Frame generation: {}x (the game's own; {}x was refused)", multiplier, panel_choice);
  return std::format("Frame generation: {}x (the game's own)", multiplier);
}

std::string FormatFrameRateLine(uint32_t real_milli, uint32_t output_milli, bool output_estimated, uint64_t age_ms) {
  // A window older than two seconds is not "now" any more (a paused or minimized game): say nothing
  // rather than a stale number.
  if (real_milli == 0u || age_ms > 2000u) return {};
  const double real = static_cast<double>(real_milli) / 1000.0;
  // With frame generation off there is no second rate to report: real and output are the same number,
  // and printing it twice reads as though generation were running.
  if (output_milli <= real_milli) return std::format("Frame rate: {:.1f}", real);
  const double output = static_cast<double>(output_milli) / 1000.0;
  if (output_estimated) return std::format("Frame rate: {:.1f} real, {:.1f} estimated", real, output);
  return std::format("Frame rate: {:.1f} real, {:.1f} generated", real, output);
}

std::string FormatDynamicMfgLive(const DynamicMfgFacts& facts) {
  if (!facts.wanted) return {};
  // The target is what the panel asked Dynamic MFG to hold, not what the game happens to run at: the
  // multiplier follows the target, so a line about the moment-to-moment rate would describe the scene
  // rather than the choice.
  const bool counted = (facts.multiplier >= 2u);
  const bool targeted = (facts.target_fps != 0u);
  if (facts.in_force) {
    if (counted && targeted) return std::format("Dynamic MFG: on (target {} FPS, now {}x)", facts.target_fps, facts.multiplier);
    if (targeted) return std::format("Dynamic MFG: on (target {} FPS)", facts.target_fps);
    if (counted) return std::format("Dynamic MFG: on (now {}x)", facts.multiplier);
    return "Dynamic MFG: on (target: the display's refresh rate)";
  }
  if (!facts.d3d12) return "Dynamic MFG: Direct3D 12 only";
  if (facts.declined) return "Dynamic MFG: refused by the runtime (toggle to retry)";
  if (!facts.support_known) return "Dynamic MFG: waiting for the runtime's support answer";
  if (!facts.supported) return "Dynamic MFG: not supported by this runtime";
  if (!facts.ready) {
    return (counted ? std::format("Dynamic MFG: waiting for frame generation (last {}x)", facts.multiplier)
                    : std::string("Dynamic MFG: waiting for frame generation"));
  }
  return "Dynamic MFG: applies at the next call";
}

std::string FormatReflexLive(bool known, uint32_t mode, uint32_t fps_cap) {
  // A game submits SetSleepMode once at startup on most engines, so this must not expire: it says what
  // the driver holds, not what happened this second. "None seen" is itself the answer for a game without
  // Reflex, and that is worth showing.
  if (!known) return "Reflex: no call seen (the game does not use it)";
  if (mode == 0u && fps_cap == 0u) return "Reflex: off";
  // Without a cap, "off" is already gone (the line above): only the two modes that limit latency remain.
  if (fps_cap == 0u) return (mode == 2u ? "Reflex: on + boost" : "Reflex: on");
  return std::format("Reflex: {}, cap {} FPS", (mode == 2u ? "on + boost" : mode == 1u ? "on" : "off"), fps_cap);
}

std::optional<std::string> FrameGenerationWarning(bool active, uint32_t passes) {
  if (!active || passes <= 1u) return std::nullopt;
  return std::format("Frame generation is on: {} NR passes can push it past its frame budget; one pass is safest", passes);
}

std::string_view KeepFacesUnavailable(const KeepFacesFacts& facts) {
  if (!facts.nr_runs) return "NR does not run in this game";
  if (!facts.gpu_ready) return "Keep faces cannot run on this GPU (ReShade.log says why)";
  return {};
}

StatusCard BuildStatusCard(const CardFacts& facts) {
  const auto card = [](CardStage stage, std::string title, std::string_view reason, std::vector<std::string> fixes,
                       CardButton button) {
    return StatusCard{.stage = stage, .working = false, .title = std::move(title), .reason = std::string(reason),
                      .fixes = std::move(fixes), .button = button};
  };
  if (!facts.device_problem.empty()) {
    return card(CardStage::DEVICE, "Uplift is not running on this device", facts.device_problem, {}, CardButton::NONE);
  }
  // Only while NR is on: a user who turned it off sees the normal off state, and the failure (remembered by the add-on) comes
  // back with Retry now when they turn it on again.
  // Plan 17: the 2-strike rule's second stop, for a bridge's private device and the helper alike.
  if (facts.private_stops_final) {
    return card(CardStage::DEVICE, "NR stopped twice", PRIVATE_DEVICE_STOPPED_TWICE_REASON, {"Restart the game to use NR again."}, CardButton::NONE);
  }
  // Test phase (review M-3): a stop on the adapter's shared device is final at once (no new device can be made while the game runs).
  if (facts.private_stop_unretryable && !facts.private_stopped.empty()) {
    return card(CardStage::DEVICE, (facts.device_lost ? "The GPU device was removed" : "NR stopped"), facts.private_stopped,
                {"Restart the game to use NR again."}, CardButton::NONE);
  }
  if (!facts.helper_problem.empty() && facts.enabled) {
    return card(CardStage::DEVICE, "Uplift's 64-bit helper stopped", facts.helper_problem, {"Retry now starts it again."},
                CardButton::RETRY_NOW);
  }
  // Plan 17: a bridge's private device stopped (the first time this session): Retry now builds a new one, as the helper's card restarts the helper.
  if (!facts.private_stopped.empty() && facts.enabled) {
    return card(CardStage::DEVICE, (facts.device_lost ? "The GPU device was removed" : "NR stopped"), facts.private_stopped,
                {"Retry now starts NR again on a new device."}, CardButton::RETRY_NOW);
  }
  if (facts.device_lost) {
    return card(CardStage::DEVICE, "The GPU device was removed", facts.output_problem, {"Restart the game to use NR again."},
                CardButton::NONE);
  }
  // Plan 15 fix round (minor 6): NR stopped on this device for the session (the game shut NVIDIA's NGX down while NR's work was still running).
  if (!facts.stopped.empty()) return card(CardStage::DEVICE, "NR stopped", facts.stopped, {"Restart the game to use NR again."}, CardButton::NONE);
  if (facts.claimed_elsewhere) {
    return card(CardStage::DEVICE, "NR runs on another device", "NR runs on another D3D12 device in this game.",
                {"One device per game runs NR; this one waits."}, CardButton::NONE);
  }
  if (!facts.blocked.empty()) {
    if (facts.runtime_in_game_folder) {
      return card(CardStage::RUNTIME, "Move nvngx_dlssnr.dll", facts.blocked,
                  {std::format("Move it into the GITC-Uplift folder next to {}, then restart the game.", facts.addon_file)}, CardButton::NONE);
    }
    if (facts.blocked_stage == CardStage::RUNTIME) {
      return card(CardStage::RUNTIME, "The NR runtime was not found", facts.blocked,
                  {std::format("Copy nvngx_dlssnr.dll into the GITC-Uplift folder next to {}, or set Runtime path (Advanced).", facts.addon_file)},
                  CardButton::NONE);
    }
    return card(CardStage::CONFLICTS, "Another NR tool is running", facts.blocked,
                {"Remove the other NR add-on, or set \"When another NR producer runs\" to Observe (Advanced)."},
                CardButton::NONE);
  }
  if (!facts.enabled) return card(CardStage::SWITCH, "NR is off", "Neural rendering is switched off.", {}, CardButton::NONE);
  // Plan 15: after the game's NGX shutdown on Direct3D 12 NR waits for the game's DLSS, whatever the stage (a temporary cause; Setup greys with the same text).
  if (!facts.held.empty()) return card(CardStage::PLACEMENT, "Waiting for the game's DLSS", facts.held, {}, CardButton::NONE);
  const nr::SessionStatus& session = facts.session;
  if (session.state == nr::SessionState::FAILED) {
    std::string next = (session.retry_in ? std::format("Retrying automatically in {} s.", (session.retry_in->count() + 999) / 1000)
                        : session.retries_exhausted ? std::string("Automatic retries stopped: change a setting, or retry now.")
                                                    : std::string("Retry now, or change a setting."));
    return card(CardStage::SESSION, "NR failed", session.message, {std::move(next)}, CardButton::RETRY_NOW);
  }
  // 2026-10-09 (owner): the video memory cards also offer the looser Video memory checks there are.
  std::string_view looser_checks;
  switch (facts.vram_check) {
    case nr::VramCheck::CAREFUL: looser_checks = "Relaxed or Off"; break;
    case nr::VramCheck::RELAXED: looser_checks = "Off"; break;
    case nr::VramCheck::OFF:     break;
  }
  if (session.suspended) {
    std::string need_less = (looser_checks.empty() ? std::string("Lower Resolution or Pass count to need less.")
                             : std::format("Lower Resolution or Pass count to need less, or set Video memory check to {} (Advanced).", looser_checks));
    return card(CardStage::SESSION, "Paused: the game needs video memory", session.message,
                {"NR resumes when memory frees up (Resume when VRAM frees up).", std::move(need_less)}, CardButton::NONE);
  }
  if (!facts.output_problem.empty()) {
    return card(CardStage::PLACEMENT, "NR cannot run on this image", facts.output_problem, {}, CardButton::NONE);
  }
  if (!facts.placement_note.empty()) {
    if (facts.dlss_latched) {
      return card(CardStage::PLACEMENT, "NR after DLSS is off", facts.placement_note,
                  {"The device was removed soon after NR ran inside the game's frame.",
                   "Clear the latch to try again from the next start."},
                  CardButton::CLEAR_LATCH);
    }
    return card(CardStage::PLACEMENT, "Waiting", facts.placement_note, {}, CardButton::NONE);
  }
  // FAILED already returned above: only ACTIVE falls through to the placement and skip checks below.
  switch (session.state) {
    case nr::SessionState::OFF:
    case nr::SessionState::LOADING:
      return card(CardStage::SESSION, "Starting NR", "NR loads with the next frame.", {}, CardButton::NONE);
    case nr::SessionState::GRACE:
    case nr::SessionState::DRAINING:
      return card(CardStage::SESSION, "NR is releasing its memory", "NR starts again once the release finishes.", {},
                  CardButton::NONE);
    case nr::SessionState::ACTIVE:
    default: break;
  }
  if (!facts.nr_applied) {
    const std::string_view reason =
        (facts.skip_from_session && !session.message.empty() ? std::string_view(session.message) : facts.skip_reason);
    if (facts.skip_reason == "budget") {
      std::string need_less =
          (looser_checks.empty() ? std::string("Lower Resolution or Pass count, or close other GPU programs.")
           : std::format("Lower Resolution or Pass count, close other GPU programs, or set Video memory check to {} (Advanced).", looser_checks));
      return card(CardStage::BUDGET, "Not enough video memory for NR", reason, {std::move(need_less)}, CardButton::NONE);
    }
    if (facts.skip_reason == "frame too small" || facts.skip_reason == "resizing") {
      return card(CardStage::FRAME, "Waiting for a usable frame", reason, {"NR needs at least " UPLIFT_NR_FLOOR_TEXT "."}, CardButton::NONE);
    }
    if (facts.skip_reason == "evaluate failed" || facts.skip_reason == "create failed") {
      return card(CardStage::EVALUATE, "NR's evaluate failed", reason, {}, CardButton::NONE);
    }
    if (facts.skip_reason == "frame generation is on") {
      return card(CardStage::PLACEMENT, "NR skipped this frame", reason,
                  {"Choose After DLSS, or turn on Present with frame generation (Advanced)."}, CardButton::NONE);
    }
    if (reason.empty()) return card(CardStage::PLACEMENT, "Waiting for a frame", "NR has not run yet.", {}, CardButton::NONE);
    return card(CardStage::PLACEMENT, "NR skipped this frame", reason, {}, CardButton::NONE);
  }
  StatusCard working = {.stage = CardStage::WORKING, .working = true, .title = "NR is working",
                        .reason = std::string(facts.working_line)};
  if (facts.passes_run < session.passes_requested) {
    working.fixes.push_back(session.message.empty()
                                ? std::format("Running {} of {} passes.", facts.passes_run, session.passes_requested)
                                : session.message);
  }
  if (!session.faces_note.empty()) {
    working.fixes.emplace_back(session.faces_note);  // Keep faces fix round 1 (C2): paused or stopped while NR works
  }
  return working;
}

}  // namespace uplift::ui
