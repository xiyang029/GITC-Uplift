#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace uplift::sources {

// DLSS's vectors are copied at the game's evaluate for the present that closes the frame. Vulkan and D3D11 use the frame's indexed slot
// (PresentMotionSlotOf); D3D12 prefers that slot and redirects to another retired slot when needed. A slot is written only once the CPU sees its last
// reader complete. A present stamps the copy it binds, its own or an older one, with its own mark, so an older read moves that slot's last use forward too.
// In the per-second line's terms, present P's mark
// completes when the GPU passes frame value P, which is signalled at the start of present P+1. The evaluate for frame F runs with CurrentFrame F-1 and
// lag L = CurrentFrame - CompletedFrame (at least 1: value F-1 is only signalled at present F), so the GPU has passed F-1-L, and the reader of slot
// F % N, present F-N, is complete when L <= N-1: four slots keep every copy up to a lag of 3, eight up to 7.
// Each ring starts at PRESENT_MOTION_MIN_SLOTS and grows once to PRESENT_MOTION_SLOTS, the first time a copy is skipped because its slot
// was still read (PresentMotionGrows); it shrinks back only when the copies are released. A game whose GPU keeps up pays for PRESENT_MOTION_MIN_SLOTS.
// Shared by the Vulkan copies (VkNrPipeline), Direct3D 12's (NrPipeline) and Direct3D 11's bridge ring.
inline constexpr size_t PRESENT_MOTION_MIN_SLOTS = 4u;
inline constexpr size_t PRESENT_MOTION_SLOTS = 8u;  // the most a ring grows to (its arrays' size)
// A present whose own copy is missing binds the newest copy at most this many frames older, so NR keeps DLSS's vectors (no source switch, no history
// reset, no zero motion); only with none that recent does it fall back (zeros of the copies' shape natively, Launchpad's or none on a bridge).
// Capped at 1 (2026-11-01, restored 2026-11-17): a vector two frames old points the other way on a fast pan and the reproject lands a texel or more off,
// smearing shadows and edges; a one-frame-old one does not. Widened to 2 on 2026-11-14 to match upstream 2.0.0, but that two-frame-old copy brought the
// smear back on the NVFP4 2x/3x presets at Present, so it is back to 1. A present whose own copy is missing falls back only past this age (RecordPresent
// keeps the reset off, so NR's history survives a one-frame gap).
inline constexpr uint64_t PRESENT_MOTION_MAX_AGE = 1u;
// Fix round 1 (M7): at present P the evaluate for P+1 may already have written its slot, (P+1) % N, which held the copy for P+1-N, P's copy of age N-1.
// So a copy of age N-2 is the oldest a present can always still find; the limit holds for the smallest ring, before any growth.
static_assert(PRESENT_MOTION_MAX_AGE + 2u <= PRESENT_MOTION_MIN_SLOTS, "an older copy must still be in a four-slot ring when its present binds it");
static_assert(PRESENT_MOTION_MIN_SLOTS <= PRESENT_MOTION_SLOTS);

// The slot a copy for `frame` is written to, in a ring of `slots`. Pure.
[[nodiscard]] constexpr size_t PresentMotionSlotOf(uint64_t frame, size_t slots) { return static_cast<size_t>(frame % slots); }

// Prefer the frame's natural slot, but use another slot whose writer and reader have both retired before dropping a copy.
// Empty slots are preferred over old copies; otherwise overwrite the oldest available copy to preserve the newest fallback.
[[nodiscard]] inline std::optional<size_t> PickPresentMotionWriteSlot(std::span<const uint64_t> frames,
                                                                       std::span<const uint8_t> writable,
                                                                       size_t preferred) {
  if (frames.empty() || frames.size() != writable.size() || preferred >= frames.size()) return std::nullopt;
  if (writable[preferred] != 0u) return preferred;
  for (size_t index = 0u; index < frames.size(); ++index) {
    if (writable[index] != 0u && frames[index] == 0u) return index;
  }
  std::optional<size_t> oldest;
  for (size_t index = 0u; index < frames.size(); ++index) {
    if (writable[index] != 0u && (!oldest || frames[index] < frames[*oldest])) oldest = index;
  }
  return oldest;
}

struct PresentMotionPick {
  std::optional<size_t> slot;  // none: no copy within the age limit
  uint64_t age = 0u;           // frames between the copy and the present: 0 is the present's own copy
};

// The copy a present for `frame` binds: its own, else the newest one older than it by at most `max_age` frames. `frames` holds each slot's copy frame (0:
// none). A copy for a later frame (an evaluate that ran ahead of this present) is never bound. Pure.
[[nodiscard]] inline PresentMotionPick PickPresentMotion(std::span<const uint64_t> frames, uint64_t frame, uint64_t max_age = PRESENT_MOTION_MAX_AGE) {
  PresentMotionPick pick;
  if (frame == 0u) return pick;
  uint64_t newest = 0u;
  for (size_t index = 0u; index < frames.size(); ++index) {
    const uint64_t copied = frames[index];
    if (copied == 0u || copied > frame || frame - copied > max_age || copied <= newest) continue;
    newest = copied;
    pick.slot = index;
  }
  if (pick.slot) {
    pick.age = frame - newest;
  }
  return pick;
}

// Why an evaluate made no copy (the per-second counters' gates).
enum class PresentMotionWrite : uint8_t {
  WRITTEN,
  NO_INPUT,   // no readable vectors, or the copy's pipeline or images could not be made
  RING_BUSY,  // the copy pipeline's constants ring slot is still read by the GPU
  SLOT_BUSY,  // the copy slot's last reader is not complete yet
};

// A ring of `slots` may grow to PRESENT_MOTION_SLOTS when no slot is safe to write. Pure.
[[nodiscard]] constexpr bool PresentMotionGrows(size_t slots, PresentMotionWrite write) {
  return write == PresentMotionWrite::SLOT_BUSY && slots < PRESENT_MOTION_SLOTS;
}

// D3D12 can establish read order on the consumer queue. Prefer an ordered
// candidate at each age; enqueue a wait only when no such candidate exists.
// The callback validates submission proof and returns true only on safe order.
template<class PrepareRead>
[[nodiscard]] PresentMotionPick PickD3D12PresentMotion(std::span<const uint64_t> recorded,
                                                       std::span<const uint64_t> ordered, uint64_t frame,
                                                       bool foveated, PrepareRead prepare_read) {
  if (frame == 0u || recorded.size() != ordered.size()) return {};
  for (uint64_t age = 0u; age <= PRESENT_MOTION_MAX_AGE && age < frame; ++age) {
    const uint64_t candidate = frame - age;
    if (auto pick = PickPresentMotion(ordered, candidate, 0u); pick.slot) return {*pick.slot, age};
    if (age != 0u && !foveated) continue;
    for (size_t i = 0u; i < recorded.size(); ++i) {
      if (recorded[i] == candidate && prepare_read(i)) return {i, age};
    }
  }
  return {};
}

}  // namespace uplift::sources
