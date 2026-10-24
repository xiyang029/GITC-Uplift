#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ngx_hooks/dlss_overrides.hpp"
#include "ngx_hooks/feature_registry.hpp"
#include "ngx_hooks/ngx_api.hpp"

namespace uplift::addon {

class NgxRouting;

// The live DLSS-SR preset switch (2026-10-25): the panel's DLSSPreset<Mode> choices applied by
// evaluating Uplift's own SuperSampling feature instead of the game's. A DLSS feature reads its render
// preset (J-M) once, at creation, so a choice cannot reach the game's existing feature: the swap creates
// one through the game's NGX core -- the game's device, the game's command list, the game's own runtime
// DLL -- with the chosen preset keys and the game feature's create snapshot (dimensions, flags), and
// BeforeEvaluate hands the evaluate the swap's handle. The game's handle stays valid and untouched
// throughout, so every change rolls back by evaluating the game's handle again; the swap feature goes
// when the game releases its own, or when the panel returns to Game.
//
// Threading: every entry point runs on a game thread inside an outermost hooked NGX call. `mutex_` is a
// leaf lock -- it is never held while taking the add-on's lock (the routing's settings read happens
// first), and NGX calls may run under it (they take no lock of this module). The NGX create and release
// go through ngx_hooks::OwnCreate/OwnRelease, so no detour and no observer sees them.
class DlssSwap {
 public:
  explicit DlssSwap(NgxRouting& routing) : routing_(routing) {}

  // The panel's DLSSPreset<Mode> choices, pushed by the add-on whenever the settings change (the gate
  // BeforeEvaluate reads before it costs anything: no push, no per-frame work).
  void SetDesired(bool any) { wanted_.store(any, std::memory_order_relaxed); }
  // One relaxed load each; true when a BeforeEvaluate must run at all (a choice is set, or a swap
  // feature or a pending retirement exists).
  [[nodiscard]] bool Busy() const {
    return wanted_.load(std::memory_order_relaxed) || active_.load(std::memory_order_relaxed);
  }

  // Before the original evaluate of the game's main upscaler handle `game_handle` (`record` is its
  // registry record). Returns the handle the original must evaluate -- the swap feature's, or
  // `game_handle` to keep the game's own -- creating, recreating or retiring the swap feature as the
  // panel's choices demand. The NGX create runs here, on the game's evaluate thread, with the evaluate's
  // command list; a swap created this frame evaluates this frame.
  [[nodiscard]] const NVSDK_NGX_Handle* BeforeEvaluate(ngx_hooks::NgxApi api, const void* device,
                                                       ID3D12GraphicsCommandList* list,
                                                       const NVSDK_NGX_Handle* game_handle,
                                                       const ngx_hooks::FeatureRecord& record);
  // The game released `game_handle`: its swap feature goes with it, through the same core.
  void BeforeRelease(const NVSDK_NGX_Handle* game_handle);
  // The game shut the NGX core down for `device` (null: every device of `api`): the swap features of
  // that core died with it -- drop their bookkeeping without touching the core.
  void OnCoreShutdown(ngx_hooks::NgxApi api, const void* device);

 private:
  // One game feature's swap: the feature Uplift created for it and the choices it was created with.
  struct SwapState {
    ngx_hooks::NgxApi api = ngx_hooks::NgxApi::D3D12;
    const void* device = nullptr;
    NVSDK_NGX_Handle* handle = nullptr;        // null while the last create failed (or none yet)
    ngx_hooks::SrOverrides applied;            // the choices `handle` was created with
    bool failed = false;                       // the last create failed: backed off until `retry_at`
    uint64_t retry_at = 0u;
  };
  // A swap feature waiting to be released: retired a few frames after it stopped being evaluated, so it
  // is never released under an evaluate still in flight (a plain NGX evaluate is synchronous, but the
  // delay costs nothing and does not bet on it).
  struct Retired {
    NVSDK_NGX_Handle* handle = nullptr;
    ngx_hooks::NgxApi api = ngx_hooks::NgxApi::D3D12;
    uint64_t due = 0u;  // the frame count from `frames_` at which to release
  };

  // Creates the swap feature: a SuperSampling (or Ray Reconstruction, `record.feature`'s kind) create
  // through the game's core with the record's snapshot and the chosen preset keys. Null, with a
  // warn-once log, when the core's own parameters allocator or the create itself fails.
  [[nodiscard]] NVSDK_NGX_Handle* CreateSwap(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list,
                                             const ngx_hooks::FeatureRecord& record, const ngx_hooks::SrOverrides& desired);
  // Releases `handle` through `api`'s original (best effort; logged once when the core refuses it).
  static void ReleaseSwap(ngx_hooks::NgxApi api, NVSDK_NGX_Handle* handle);
  // Releases every retiree whose frame has come, under `mutex_`.
  void RetireDue();
  // Refreshes `active_` from the maps' emptiness; called with `mutex_` held.
  void RefreshActive() {
    active_.store(!states_.empty() || !retired_.empty(), std::memory_order_relaxed);
  }

  NgxRouting& routing_;
  mutable std::mutex mutex_;  // leaf: never held while taking the add-on's lock
  std::unordered_map<const NVSDK_NGX_Handle*, SwapState> states_;  // by the game's handle
  std::vector<Retired> retired_;
  std::atomic<bool> wanted_{false};  // the panel has a non-Game choice (SetDesired)
  std::atomic<bool> active_{false};  // a swap feature or a pending retirement exists
  uint64_t frames_ = 0u;             // advanced by every BeforeEvaluate, for the retire and retry delays
};

}  // namespace uplift::addon
