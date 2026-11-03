// SPDX-License-Identifier: MIT
#pragma once
// The XMX mode's generator: XeFG's own context replaces NVIDIA's generation inside
// NVSDK_NGX_D3D12_EvaluateFeature, while NGX keeps owning presentation. Off unless the add-on's
// own XMX setting is on (host.hpp's SetEnabled). Ported from MFGAdaUnlock-RenoDx 1.8
// (docs/research/xemfg-modo-xmx-2026-11-01.md); offsets are libxess_fg.dll 1.3.1.78 and any
// other build is refused by Preflight before a byte is written.
#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "nr/ngx.hpp"
#include "xmx/host.hpp"
#include "xmx/model.hpp"
#include "xmx/policy.hpp"
#include "xmx/vendor/cuda/cuda_route.h"
#include "xmx/vendor/runtime_layout.h"
#include "embedded_xefg_cuda.generated.h"

namespace uplift::xmx::generator {
using Microsoft::WRL::ComPtr;
constexpr DWORD kKnownTimestamp = 0x69CB0F4D;
constexpr uintptr_t kFactoryRva = 0x1853B0;
constexpr uintptr_t kContextVtableRva = 0xF45D48;
constexpr uintptr_t kInitRva = 0x1871C0;
constexpr uintptr_t kExecuteRva = 0x185800;
constexpr auto kBackbufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kInputState = D3D12_RESOURCE_STATE_COPY_DEST;
constexpr auto kOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto kXefgInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

#pragma pack(push, 8)
struct InitParams {
  uint32_t width, height, format, flags, maxFrames, creationMask, visibleMask, reserved;
  void* tempBufferHeap;
  uint64_t bufferOffset;
  void* tempTextureHeap;
  uint64_t textureOffset;
  void* pipelineLibrary;
  uint32_t uiMode, reserved2;
};
struct Input {
  ID3D12Resource* resource;
  uint32_t baseX, baseY, width, height;
};
struct FrameData {
  Input backbuffer, depth, motion, ui, hudless;
  float view[16], projection[16], resolution[2], jitter[2], motionScale[2];
  uint32_t reset;
  float renderTimeMs;
};
struct ExecuteParams {
  FrameData previous, next;
  ID3D12Resource* const* outputs;
  const uint32_t* outputBases;
  uint8_t outputCount;
  uint8_t reserved[3];
  uint32_t networkScales;  // 0: the default
  void* descriptorHeap;    // null: XeFG's own
  uint32_t heapOffset, uiComposition;
  // The per-pair work runs in the call starting at 0; later calls reuse it.
  uint32_t firstFrame, frameCount;
};
#pragma pack(pop)
static_assert(sizeof(InitParams) == 80 && sizeof(Input) == 24 && sizeof(FrameData) == 0x118);
static_assert(offsetof(FrameData, view) == 0x78 && offsetof(FrameData, reset) == 0x110);
static_assert(offsetof(ExecuteParams, next) == 0x118 && offsetof(ExecuteParams, outputs) == 0x230);
static_assert(offsetof(ExecuteParams, outputCount) == 0x240 && offsetof(ExecuteParams, descriptorHeap) == 0x248);
static_assert(offsetof(ExecuteParams, firstFrame) == 0x258 && sizeof(ExecuteParams) == 0x260);
// One constant for the ceiling: the generator's arrays are sized by it and the model publishes it,
// so the two can never disagree. See xmx/model.hpp.
constexpr uint32_t kMaxFrames = model::kGeneratedFrames;

struct Copies {
  ComPtr<ID3D12Resource> backbuffer, depth, motion, hudless;
  FrameData data{};
  bool valid{};
};
struct State {
  ComPtr<ID3D12Device> device;
  void* context{};  // the C functions' handle: { 'DX12', XefgContext* }
  HMODULE fg{};
  uint32_t width{}, height{}, flags{};
  DXGI_FORMAT format{};
  std::array<Copies, 2> frames;
  uint32_t current{};
  std::array<ComPtr<ID3D12Resource>, kMaxFrames> outputs;
  // Resources replaced while earlier commands may still be using them: a resolution change (a window
  // resize, an Alt-Tab out of fullscreen, a quality change) left the old ones unreleased for the
  // process's life, which leaked ~309 MB each. Age, in pairs of real frames, stands in for the fence
  // EvaluateFeature has no queue to wait on.
  policy::RetireQueue<ComPtr<ID3D12Resource>> retired;
  uint64_t pairs{};
  uint64_t lastCaptureMs{};
  bool lastGenerated{};
  uint32_t contexts{};
  uint32_t maxFrames{1};
  D3D12_RESOURCE_DESC outputDesc{};
  bool published{};
  bool failed{};
  uint32_t logs{};
};
inline State& Get() {
  static auto* state = new State;
  return *state;
}
inline void Log(const std::string& text) {
  auto& state = Get();
  if (state.logs++ < 48) xess_mod::WriteLog("generator", text);
}
// For the panel: nobody may take NVIDIA's generator for XeFG's, or the reverse.
inline std::atomic<uint64_t> g_last_generated_tick{0};
inline std::mutex g_failure_lock;
inline std::string g_failure;
inline void Fail(const std::string& reason) {
  { std::lock_guard guard(g_failure_lock); g_failure = reason; }
  // Not through Log(): its forty-eight line budget is for per-item chatter, and a failure is the one
  // line a later reader needs -- a start that was refused, a crash inside Execute, a game that resized
  // past the model's contexts.
  xess_mod::WriteLog("failed", reason);
}
inline std::atomic_bool g_preflight_passed{false};
// What the mode actually delivered, per interval. A frame rate alone cannot tell "XeFG generates every
// frame, just slower than NVIDIA's generator" (the wide-budget settings) apart from "XeFG does not
// finish in time and the real frame is presented again" (the same number, and a stutter), and the two
// need opposite fixes. Rates, not totals: only an interval's delta says which one is happening now.
struct Stats {
  std::atomic<uint32_t> asked{0}, generated{0}, repeated{0}, refused{0}, errored{0}, pairs{0};
  std::atomic<uint64_t> gapTotalMs{0}, executeTotalUs{0};
  std::atomic<uint32_t> gapMaxMs{0}, executeMaxUs{0};
  std::array<std::atomic<uint32_t>, static_cast<size_t>(policy::Idle::Count)> idle{};
};
inline Stats& Counters() {
  static auto* counters = new Stats;
  return *counters;
}
inline void RaisePeak(std::atomic<uint32_t>& seen, uint32_t value) {
  uint32_t previous = seen.load(std::memory_order_relaxed);
  while (value > previous && !seen.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
  }
}
inline uint64_t Microseconds() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}
inline std::atomic<uint64_t> g_rates_tick{0};
// The multiplier the game asks for, for the rates line: it is the game's setting, and a mode that
// serves fewer frames than it is asked for has to say so beside what it did deliver.
inline std::atomic<uint32_t> g_requested{0};
// Set by the evaluate path (src/xmx/hook.cpp) on every call: DLSS-G calling the generator and the
// game simply not calling it are different answers to "why is there no frame", and only the caller
// knows which one it is.
inline std::atomic<uint64_t> g_last_evaluate_tick{0};
// Why the last evaluate produced no frame; one log line per reason, however often the state flips.
inline std::atomic<int> g_idle{0};
inline std::atomic<uint32_t> g_idle_logged{0};
inline std::atomic<uint32_t> g_frames_limit{0};  // the model's limit, once a game asks for more
inline void NoteIdle(policy::Idle idle) {
  g_idle.store(static_cast<int>(idle), std::memory_order_relaxed);
  if (idle == policy::Idle::None) return;
  Counters().idle[static_cast<size_t>(idle)].fetch_add(1, std::memory_order_relaxed);
  const uint32_t bit = 1u << static_cast<int>(idle);
  if ((g_idle_logged.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
    Log(std::string("not generating: ") + policy::IdleText(idle));
}
// One line every two seconds with the deltas since the last one: fields, not prose, so two runs at
// different upscaler settings can be read side by side. "asked" is the presented frame rate (DLSS-G
// evaluates once per presented frame) and "generated" is how many of those were new; the gap between
// them is the stutter. Everything below the first line is diagnostic detail.
inline void ReportRates() {
  constexpr uint64_t kPeriodMs = 2000;
  const auto now = GetTickCount64();
  const auto start = g_rates_tick.load(std::memory_order_relaxed);
  if (start != 0 && now - start < kPeriodMs) return;
  g_rates_tick.store(now, std::memory_order_relaxed);
  if (start == 0) return;  // the first call only starts the interval
  auto& counters = Counters();
  const auto asked = counters.asked.exchange(0, std::memory_order_relaxed);
  const auto generated = counters.generated.exchange(0, std::memory_order_relaxed);
  const auto repeated = counters.repeated.exchange(0, std::memory_order_relaxed);
  const auto refused = counters.refused.exchange(0, std::memory_order_relaxed);
  const auto errored = counters.errored.exchange(0, std::memory_order_relaxed);
  const auto pairs = counters.pairs.exchange(0, std::memory_order_relaxed);
  const auto gapTotal = counters.gapTotalMs.exchange(0, std::memory_order_relaxed);
  const auto gapMax = counters.gapMaxMs.exchange(0, std::memory_order_relaxed);
  const auto executeTotal = counters.executeTotalUs.exchange(0, std::memory_order_relaxed);
  const auto executeMax = counters.executeMaxUs.exchange(0, std::memory_order_relaxed);
  const auto span = now - start;
  const auto perSecond = [span](uint64_t count) { return span == 0 ? 0ull : count * 1000ull / span; };
  auto& state = Get();
  std::string text = "over " + std::to_string(span) + "ms asked " + std::to_string(asked) + " (" +
                     std::to_string(perSecond(asked)) + "/s) generated " + std::to_string(generated) + " (" +
                     std::to_string(perSecond(generated)) + "/s) repeated " + std::to_string(repeated) + " refused " +
                     std::to_string(refused) + " errored " + std::to_string(errored);
  text += " | pairs " + std::to_string(pairs) + " (" + std::to_string(perSecond(pairs)) + "/s) every " +
          std::to_string(pairs == 0 ? 0 : gapTotal / pairs) + "ms (max " + std::to_string(gapMax) + "ms)";
  text += " | execute " + std::to_string(asked == 0 ? 0 : executeTotal / asked) + "us avg " +
          std::to_string(executeMax) + "us max";
  text += " | " + std::to_string(state.width) + "x" + std::to_string(state.height) + " format " +
          std::to_string(static_cast<int>(state.format)) + " model frames " + std::to_string(state.maxFrames) +
          " asked for " + std::to_string(g_requested.load(std::memory_order_relaxed));
  constexpr const char* kIdleNames[] = {"", "missing-inputs", "no-pair", "reset", "hudless-changed",
                                        "execute-error", "no-output", "copy-mismatch"};
  static_assert(sizeof(kIdleNames) / sizeof(kIdleNames[0]) == static_cast<size_t>(policy::Idle::Count),
                "one name per idle reason");
  for (size_t i = 1; i < sizeof(kIdleNames) / sizeof(kIdleNames[0]); ++i) {
    const auto count = counters.idle[i].exchange(0, std::memory_order_relaxed);
    if (count != 0) text += std::string(" | ") + kIdleNames[i] + " " + std::to_string(count);
  }
  xess_mod::WriteLog("rates", text);
}
// hooked: Preflight passed, so the mode can serve this game's DLSS-G (no hook is installed by this
// route; the name is the panel's, from the module-hook design this replaced).
struct Report {
  bool generating{};
  bool hooked{};
  std::string failure;
  bool evaluating{};  // DLSS-G called the generator within the last two seconds
  policy::Idle idle{policy::Idle::None};
};
inline Report Snapshot() {
  const auto now = GetTickCount64();
  const auto tick = g_last_generated_tick.load(std::memory_order_relaxed);
  const auto evaluated = g_last_evaluate_tick.load(std::memory_order_relaxed);
  std::lock_guard guard(g_failure_lock);
  Report report;
  report.generating = tick != 0 && now - tick < 2000;
  report.hooked = g_preflight_passed.load();
  report.evaluating = evaluated != 0 && now - evaluated < 2000;
  report.idle = static_cast<policy::Idle>(g_idle.load(std::memory_order_relaxed));
  report.failure = g_failure;
  return report;
}
inline DWORD FileTimestamp(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS nt{};
  if (!file.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || !file.seekg(dos.e_lfanew) ||
      !file.read(reinterpret_cast<char*>(&nt), sizeof(nt)))
    return 0;
  return nt.FileHeader.TimeDateStamp;
}
// Before the mode serves a frame: XeFG's offsets are for one exact Intel build, so a mode that cannot
// run must refuse here and let the core generate instead. Reads the files without loading them, which
// also makes it safe under the loader lock. Passed or failed once per process (the files cannot change
// under a running game), so every later call is a load of a const bool.
inline bool Preflight() {
  static const bool passed = [] {
    const auto directory = AddonDirectory();
    std::error_code error;
    if (!std::filesystem::exists(directory / L"libxess_fg.dll", error)) {
      Fail("libxess_fg.dll not found");
      return false;
    }
    if (FileTimestamp(directory / L"libxess_fg.dll") != kKnownTimestamp) {
      Fail("libxess_fg.dll is not the supported 1.3.1.78");
      return false;
    }
    if (!std::filesystem::exists(directory / L"libxell.dll", error)) {
      Fail("libxell.dll not found");
      return false;
    }
    g_preflight_passed = true;
    return true;
  }();
  return passed;
}

inline int Guarded(void* function, void* context, void* a, void* b) noexcept {
  using Call = int(__cdecl*)(void*, void*, void*);
  __try {
    return reinterpret_cast<Call>(function)(context, a, b);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -100000;
  }
}

// The file beside this add-on, or nothing: the RVAs this port carries belong to that exact build,
// and a game that ships its own libxess_fg.dll must not be served instead.
inline HMODULE OpenRuntime(const wchar_t* name) {
  const auto local = AddonDirectory() / name;
  std::error_code error;
  if (!std::filesystem::exists(local, error)) return nullptr;
  return LoadLibraryExW(local.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

inline bool LoadContext(ID3D12Device* device) {
  auto& state = Get();
  state.fg = OpenRuntime(L"libxess_fg.dll");
  if (state.fg == nullptr) {
    Fail("libxess_fg.dll could not be loaded");
    return false;
  }
  auto* base = reinterpret_cast<unsigned char*>(state.fg);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  if (nt->FileHeader.TimeDateStamp != kKnownTimestamp) {
    Fail("unknown libxess_fg.dll build");
    return false;
  }
  if (!state.published) {
    state.published = true;
    const auto ll = OpenRuntime(L"libxell.dll");
    std::string error;
    const HMODULE fg = state.fg;
    if (ll != nullptr && model::Publish(fg, ll, error)) state.maxFrames = kMaxFrames;
    else Log("multi-frame model refused, 2x only: " + (ll == nullptr ? std::string("libxell.dll not found") : error));
    // The tensor-core route (src/xmx/vendor/cuda/): each recognized int8 conv layer program becomes
    // an NvAPI_D3D12_LaunchCubinShader launch in the provider's own command list, with no CUDA
    // context and no second queue. Must be in before the provider creates its pipeline states, i.e.
    // before the Init below. A refusal is not fatal: the DXIL port runs instead, as it did before.
    // Its own diagnostics go through SetLog; without it every cuda_* line the route emits is lost,
    // and "active" would be the only thing this build ever says about the tensor cores.
    xefg_cuda::SetLog([](const char* stage, const std::string& fields) noexcept { xess_mod::WriteLog(stage, fields); });
    if (xefg_cuda::Install(device, fg, xess_payload::Read(xess_mod::Self(), xefg_cuda::embedded::fatbin), error)) {
      const auto route = xefg_cuda::Snapshot();
      Log("tensor-core route active, compute capability " + std::to_string(route.major) + "." + std::to_string(route.minor) +
          "; " + std::to_string(route.layers) + " layer(s) recognized");
    } else {
      Log("tensor-core route refused (" + error + "); the DXIL port runs");
    }
  }
  void** wrapper = nullptr;
  const auto created = Guarded(base + kFactoryRva, device, &wrapper, nullptr);
  if (created != 0 || wrapper == nullptr) {
    Fail("factory returned " + std::to_string(created));
    return false;
  }
  // { 'DX12' magic, XefgContext* }
  if (wrapper[1] == nullptr ||
      reinterpret_cast<uintptr_t>(*static_cast<void***>(wrapper[1])) - reinterpret_cast<uintptr_t>(base) != kContextVtableRva) {
    Fail("unexpected context layout");
    return false;
  }
  state.context = wrapper;
  return true;
}
inline ComPtr<ID3D12Resource> Texture(ID3D12Device* device, D3D12_RESOURCE_DESC desc, D3D12_RESOURCE_STATES initial,
                                      D3D12_RESOURCE_FLAGS flags) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  desc.Flags = flags;
  desc.MipLevels = 1;
  desc.DepthOrArraySize = 1;
  desc.SampleDesc = {1, 0};
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Alignment = 0;
  ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr, IID_PPV_ARGS(&resource))))
    return nullptr;
  return resource;
}
inline void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) {
  if (from == to) return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = from;
  barrier.Transition.StateAfter = to;
  list->ResourceBarrier(1, &barrier);
}
// Copies between resources left in their original states. A mismatch would be undefined on the GPU:
// the target keeps what it had, and the log says why.
inline void Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_RESOURCE_STATES targetState,
                 ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState) {
  const auto to = target->GetDesc(), from = source->GetDesc();
  if (!policy::CanCopy(to.Width, to.Height, to.Format, from.Width, from.Height, from.Format)) {
    NoteIdle(policy::Idle::CopyMismatch);
    return;
  }
  Barrier(list, target, targetState, D3D12_RESOURCE_STATE_COPY_DEST);
  Barrier(list, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(target, source);
  Barrier(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
  Barrier(list, target, D3D12_RESOURCE_STATE_COPY_DEST, targetState);
}
// The frame's copies as one batch. The shapes above cost four ResourceBarrier calls per copy; a
// frame makes six, so twenty-four calls into the driver, each with its own validation and
// submission overhead. Every transition is known before the first copy is recorded, so the batch
// goes out as two barrier calls around the copies. A resource that would appear twice keeps the
// old one-at-a-time path, so a shape this cannot express is never mis-recorded.
class CopyBatch {
 public:
  explicit CopyBatch(ID3D12GraphicsCommandList* list) : list_(list) {}
  CopyBatch(const CopyBatch&) = delete;
  CopyBatch& operator=(const CopyBatch&) = delete;
  ~CopyBatch() { Flush(); }

  void Add(ID3D12Resource* target, D3D12_RESOURCE_STATES targetState, ID3D12Resource* source,
           D3D12_RESOURCE_STATES sourceState) {
    if (target == nullptr || source == nullptr) return;
    // A duplicate would need two destinations for one resource, and the barrier array is sized for
    // kMaxBatch pairs: both fall back to the one-at-a-time path, which is correct either way. So does
    // a pair CopyResource cannot express: the batch records its copies itself, so it makes the same
    // check the one-at-a-time path makes, or the guard only covers the rare path.
    if (Duplicated(target) || Duplicated(source) || targets_.size() >= kMaxBatch || !SameShape(target, source)) {
      Copy(list_, target, targetState, source, sourceState);
      return;
    }
    targets_.push_back({target, targetState});
    sources_.push_back({source, sourceState});
    copies_.push_back({target, source});
  }

  void Flush() {
    if (copies_.empty()) return;
    D3D12_RESOURCE_BARRIER barriers[kMaxBatch * 2]{};
    UINT count = 0;
    const auto push = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
      if (from == to) return;
      auto& barrier = barriers[count++];
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = resource;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = from;
      barrier.Transition.StateAfter = to;
    };
    for (const auto& entry : targets_) push(entry.resource, entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& entry : sources_) push(entry.resource, entry.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(count, barriers);
    for (const auto& copy : copies_) list_->CopyResource(copy.target, copy.source);
    count = 0;
    for (const auto& entry : sources_) push(entry.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, entry.state);
    for (const auto& entry : targets_) push(entry.resource, D3D12_RESOURCE_STATE_COPY_DEST, entry.state);
    list_->ResourceBarrier(count, barriers);
    targets_.clear();
    sources_.clear();
    copies_.clear();
  }

 private:
  static constexpr size_t kMaxBatch = 8;
  struct Entry { ID3D12Resource* resource; D3D12_RESOURCE_STATES state; };
  struct Pair { ID3D12Resource* target; ID3D12Resource* source; };
  static bool SameShape(ID3D12Resource* target, ID3D12Resource* source) {
    const auto to = target->GetDesc(), from = source->GetDesc();
    return policy::CanCopy(to.Width, to.Height, to.Format, from.Width, from.Height, from.Format);
  }
  bool Duplicated(ID3D12Resource* resource) const {
    for (const auto& entry : targets_) if (entry.resource == resource) return true;
    for (const auto& entry : sources_) if (entry.resource == resource) return true;
    return false;
  }
  ID3D12GraphicsCommandList* list_;
  std::vector<Entry> targets_, sources_;
  std::vector<Pair> copies_;
};
// The one rule an output resource must satisfy before XeFG may dispatch into it: this module created
// it, it started life in UNORDERED_ACCESS, and it carries the UAV flag. Nothing that DLSS-G hands over
// can qualify -- its current state is not knowable from here, and a barrier written on a guess is a
// device removal. That is why the frames are generated into textures of ours and copied back, and why
// writing straight into the game's buffer was removed rather than fixed.
constexpr bool XefgOutputEligible(D3D12_RESOURCE_STATES created_state, D3D12_RESOURCE_FLAGS flags) {
  return created_state == kOutputState && (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
}
// Every output texture is created exactly this way; changing either argument breaks the build rather
// than producing frames the GPU cannot see.
static_assert(XefgOutputEligible(kOutputState, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
              "an output texture must start in UNORDERED_ACCESS and carry the UAV flag");

inline ID3D12Resource* Resource(const NVSDK_NGX_Parameter* params, const char* name) {
  ID3D12Resource* value = nullptr;
  return params->Get(name, &value) == NVSDK_NGX_Result_Success ? value : nullptr;
}
template <class T>
inline T Value(const NVSDK_NGX_Parameter* params, const char* name, T fallback) {
  T value{};
  return params->Get(name, &value) == NVSDK_NGX_Result_Success ? value : fallback;
}
// Each copy has a shape to follow. The old one may still be in a command list on the GPU (dynamic
// resolution, a DLSS mode change), so it is retired, not released. The copy is recorded into
// `batch`, which the caller flushes once for the frame.
inline bool Capture(ID3D12Device* device, CopyBatch& batch, ComPtr<ID3D12Resource>& copy, ID3D12Resource* source,
                    D3D12_RESOURCE_STATES sourceState) {
  auto& state = Get();
  if (source == nullptr) {
    if (copy) state.retired.Push(std::move(copy), state.pairs);
    copy.Reset();
    return true;
  }
  const auto shape = source->GetDesc();
  // One GetDesc on the copy, not three: it is a virtual call into the driver, and at 6x this runs
  // four times per displayed frame.
  if (copy) {
    const auto existing = copy->GetDesc();
    if (existing.Width != shape.Width || existing.Height != shape.Height || existing.Format != shape.Format)
      state.retired.Push(std::move(copy), state.pairs);
  }
  if (!copy) copy = Texture(device, shape, kXefgInputState, D3D12_RESOURCE_FLAG_NONE);
  if (!copy) return false;
  batch.Add(copy.Get(), kXefgInputState, source, sourceState);
  return true;
}
inline Input Describe(ID3D12Resource* copy) {
  Input input{};
  input.resource = copy;
  if (copy != nullptr) {
    const auto desc = copy->GetDesc();
    input.width = static_cast<uint32_t>(desc.Width);
    input.height = desc.Height;
  }
  return input;
}
// What the game declares of an input as the part to read; DLSS-G passes the subrect under these names.
inline policy::Area RawArea(const NVSDK_NGX_Parameter* params, const char* name) {
  const std::string prefix(name);
  return {Value<uint32_t>(params, (prefix + "SubrectBaseX").c_str(), 0u),
          Value<uint32_t>(params, (prefix + "SubrectBaseY").c_str(), 0u),
          Value<uint32_t>(params, (prefix + "SubrectWidth").c_str(), 0u),
          Value<uint32_t>(params, (prefix + "SubrectHeight").c_str(), 0u)};
}
// The four numbers as declared, zeros included: DLSS-G passes the keys either way, so a game that
// declares an area has to be told apart from one that declares nothing.
inline std::string AreaText(const policy::Area& area) {
  return std::to_string(area.x) + "," + std::to_string(area.y) + " " + std::to_string(area.width) + "x" +
         std::to_string(area.height);
}
// An absent or out-of-range declaration means the whole texture (policy::InputArea).
inline policy::Area DeclaredArea(const NVSDK_NGX_Parameter* params, const char* name, const Input& input) {
  return policy::InputArea(input.width, input.height, RawArea(params, name));
}
inline void Assign(Input& input, const policy::Area& area) {
  input.baseX = area.x;
  input.baseY = area.y;
  input.width = area.width;
  input.height = area.height;
}
// XeFG takes depth only if it is the motion vectors' own size (developer guide, Inputs: "Buffer size must be the
// same as the motion vectors buffer"), and it compares these fields rather than the textures, so both copies
// keep the shape they were taken at and are handed the area they share. A size further than a percent from the
// other is another picture, not a rounding -- this game's 0.67 setting hands over a 1368x1024 depth with
// 1367x769 motion -- and is left for XeFG to refuse, unless the game declared the areas itself.
inline void MatchDepthAndMotion(const NVSDK_NGX_Parameter* params, FrameData& data) {
  if (data.depth.width == data.motion.width && data.depth.height == data.motion.height) return;
  const policy::Area depth = DeclaredArea(params, "DLSSG.Depth", data.depth);
  const policy::Area motion = DeclaredArea(params, "DLSSG.MVecs", data.motion);
  const bool declared = depth.width != data.depth.width || depth.height != data.depth.height ||
                        motion.width != data.motion.width || motion.height != data.motion.height;
  if (!declared && !(policy::SameShapeWithinOnePercent(data.depth.width, data.motion.width) &&
                     policy::SameShapeWithinOnePercent(data.depth.height, data.motion.height))) {
    return;
  }
  const auto [sharedDepth, sharedMotion] = policy::SharedArea(depth, motion);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    Log("depth " + std::to_string(data.depth.width) + "x" + std::to_string(data.depth.height) + " and motion " +
        std::to_string(data.motion.width) + "x" + std::to_string(data.motion.height) + " differ, using " +
        std::to_string(sharedDepth.width) + "x" + std::to_string(sharedDepth.height) + " at depth " +
        std::to_string(sharedDepth.x) + "," + std::to_string(sharedDepth.y) + " motion " +
        std::to_string(sharedMotion.x) + "," + std::to_string(sharedMotion.y));
  }
  Assign(data.depth, sharedDepth);
  Assign(data.motion, sharedMotion);
}
inline std::string Shape(ID3D12Resource* resource) {
  if (resource == nullptr) return "none";
  const auto desc = resource->GetDesc();
  return std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + "/" +
         std::to_string(static_cast<int>(desc.Format));
}
// Depth and motion carry the area XeFG reads as well as the texture they were copied from: these four
// numbers are what it compares, so a refusal has to show them.
inline std::string InputShape(const Input& input) {
  return Shape(input.resource) + " area " + std::to_string(input.baseX) + "," + std::to_string(input.baseY) + " " +
         std::to_string(input.width) + "x" + std::to_string(input.height);
}
inline std::string Number(float value) {
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%.4g", value);
  return buffer;
}
// Everything about a frame that XeFG can refuse it for. The jitter is not in here: it moves every
// frame and a log line per frame is noise.
inline std::string Layout(const FrameData& data) {
  return "res " + Number(data.resolution[0]) + "x" + Number(data.resolution[1]) + " mvec " +
         Number(data.motionScale[0]) + "," + Number(data.motionScale[1]) + " reset " + std::to_string(data.reset) +
         " backbuffer " + Shape(data.backbuffer.resource) + " depth " + InputShape(data.depth) + " motion " +
         InputShape(data.motion) + " hudless " + Shape(data.hudless.resource);
}
// Every argument the mode hands XeFG, once per distinct combination. A refused mode has to say what it
// was refused for, and the game's own input sizes are the only arguments that move with an upscaler
// setting (the context's shape is fixed at init, and only a resize changes it). The frame index and
// DLSSG.OutputReal stay out of the key: the index walks 1..5 within every pair, so the real-frame
// output, which is there on the pair's first evaluate and absent on the four that follow, would print
// the same inputs twice per pair -- well over a hundred lines a second at 5x.
inline void NoteArguments(const NVSDK_NGX_Parameter* params, ID3D12Resource* backbuffer, ID3D12Resource* depth,
                          ID3D12Resource* motion, ID3D12Resource* hudless, ID3D12Resource* interpolated,
                          uint32_t requested, uint32_t frames) {
  auto& state = Get();
  // Only a change in what the mode is handed is worth a line.
  const std::string shapes = "backbuffer " + Shape(backbuffer) + " depth " + Shape(depth) + " motion " + Shape(motion) +
                             " hudless " + Shape(hudless) + " interpolated " + Shape(interpolated) +
                             " depth-declared " + AreaText(RawArea(params, "DLSSG.Depth")) +
                             " mvec-declared " + AreaText(RawArea(params, "DLSSG.MVecs")) + " frames " +
                             std::to_string(frames) + " requested " + std::to_string(requested) + " context " +
                             std::to_string(state.width) + "x" +
                             std::to_string(state.height) + " format " +
                             std::to_string(static_cast<int>(state.format)) + " flags " + std::to_string(state.flags);
  static std::string last;
  if (shapes == last) return;
  last = shapes;
  xess_mod::WriteLog("arguments", shapes);
}
// What XeFG was handed when it refused: the frame's own numbers, not just the resources.
inline void NoteRefused(const ExecuteParams& execute) {
  const std::string layout = "frameCount " + std::to_string(execute.frameCount) + " outputs " +
                            std::to_string(execute.outputCount) + " | previous " + Layout(execute.previous) +
                            " | next " + Layout(execute.next);
  static std::string last;
  if (layout == last) return;
  last = layout;
  xess_mod::WriteLog("refused", "execute refused: " + layout + " | firstFrame " +
                                    std::to_string(execute.firstFrame) + " jitter " + Number(execute.next.jitter[0]) +
                                    "," + Number(execute.next.jitter[1]));
}
inline bool Prepare(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params, const D3D12_RESOURCE_DESC& output) {
  auto& state = Get();
  uint32_t flags = 0;
  if (Value<int>(params, "DLSSG.DepthInverted", 0) != 0) flags |= 1u << 0;
  if (Value<int>(params, "DLSSG.MvecJittered", 0) != 0) flags |= 1u << 4;
  const auto width = static_cast<uint32_t>(output.Width);
  ComPtr<ID3D12Device> device;
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return false;
  // A new device needs a new context even with the same shape.
  const policy::ContextShape wanted{device.Get(), width, output.Height, static_cast<uint32_t>(output.Format), flags};
  if (state.context != nullptr &&
      wanted == policy::ContextShape{state.device.Get(), state.width, state.height, static_cast<uint32_t>(state.format),
                                     state.flags})
    return true;
  // XeFG's contexts are not destroyed (the function's contract is unverified), so a game that keeps
  // resizing stops generating instead of filling the VRAM.
  if (state.contexts >= policy::kMaxContexts) {
    Fail("the game changed its output size too many times; restart the game");
    return false;
  }
  if (state.context != nullptr) {
    const auto retire = [&](ComPtr<ID3D12Resource>& resource) {
      if (resource) state.retired.Push(std::move(resource), state.pairs);
      resource.Reset();
    };
    for (auto& frame : state.frames) {
      retire(frame.backbuffer);
      retire(frame.depth);
      retire(frame.motion);
      retire(frame.hudless);
      frame = {};
    }
    for (auto& texture : state.outputs) retire(texture);
    state.context = nullptr;  // its commands may be in flight, so it stays alive
    state.lastGenerated = false;
  }
  state.device = device;
  ++state.contexts;
  if (!LoadContext(state.device.Get())) return false;
  InitParams init{};
  init.width = width;
  init.height = output.Height;
  init.format = output.Format;
  init.flags = flags;
  init.maxFrames = state.maxFrames;
  init.creationMask = 1;
  init.visibleMask = 1;
  const auto result = Guarded(reinterpret_cast<unsigned char*>(state.fg) + kInitRva, state.context, &init, nullptr);
  Log("init " + std::to_string(width) + "x" + std::to_string(output.Height) + " format " +
      std::to_string(static_cast<int>(output.Format)) + " flags " + std::to_string(flags) + " result " + std::to_string(result));
  if (result != 0) {
    // Found out only here: the hook into NVIDIA's generation is gone long before this, so a game
    // whose output format XeFG cannot take has to be told what to change.
    Fail(output.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
             ? "XMX cannot use this game's scRGB HDR output (result " + std::to_string(result) + "); turn HDR off or use XeMFG"
             : "XeFG initialization returned " + std::to_string(result));
    return false;
  }
  state.outputDesc = output;
  state.width = width;
  state.height = output.Height;
  state.format = output.Format;
  state.flags = flags;
  return true;
}
// True when this call produced what NGX expects.
inline bool Generate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params, bool& realWritten) {
  auto& state = Get();
  if (state.failed) return false;
  auto* backbuffer = Resource(params, "DLSSG.Backbuffer");
  auto* interpolated = Resource(params, "DLSSG.OutputInterpolated");
  auto* real = Resource(params, "DLSSG.OutputReal");
  auto* depth = Resource(params, "DLSSG.Depth");
  auto* motion = Resource(params, "DLSSG.MVecs");
  auto* hudless = Resource(params, "DLSSG.HUDLess");
  if (backbuffer == nullptr || interpolated == nullptr || depth == nullptr || motion == nullptr) {
    NoteIdle(policy::Idle::MissingInputs);
    return false;
  }
  const auto requested = (std::max)(Value<unsigned int>(params, "DLSSG.MultiFrameCount", 1), 1u);
  const auto requestedIndex = std::clamp(Value<unsigned int>(params, "DLSSG.MultiFrameIndex", 1), 1u, requested);
  if (!Prepare(list, params, interpolated->GetDesc())) {
    state.failed = true;
    return false;
  }
  // Frames past the model's limit repeat the last one it produced rather than failing the mode: the
  // multiplier is the game's setting, and refusing it left the picture on NVIDIA's frames mid-game.
  const auto plan = policy::PlanFrames(requested, requestedIndex, state.maxFrames);
  g_requested.store(requested, std::memory_order_relaxed);
  const auto frameCount = plan.count;
  const auto frameIndex = plan.index;
  NoteArguments(params, backbuffer, depth, motion, hudless, interpolated, requested, frameCount);
  if (plan.limited && g_frames_limit.exchange(state.maxFrames) == 0)
    Log(std::to_string(requested) + " generated frames asked for, this model produces " + std::to_string(state.maxFrames));
  // One batch for every copy this evaluate makes. It is flushed before Execute, which reads these
  // inputs; the output copy below is recorded after that and goes out with the batch's own flush.
  CopyBatch batch(list);
  // The real frame's output only changes once per pair: frameIndex 1 is the pair's first evaluate, and
  // the four that follow (5x/6x) would copy the same backbuffer into it four more times. Copying it
  // on the pair's first evaluate only is what "the real frame is captured once per pair" already
  // means for the inputs below.
  if (real != nullptr && frameIndex == 1) {
    realWritten = true;
    batch.Add(real, kOutputState, backbuffer, kBackbufferState);
  }
  // Past the model's limit: the last generated frame again, no new work.
  if (plan.repeat) {
    Counters().repeated.fetch_add(1, std::memory_order_relaxed);
    ID3D12Resource* source =
        (state.lastGenerated && state.outputs[frameIndex - 1]) ? state.outputs[frameIndex - 1].Get() : backbuffer;
    batch.Add(interpolated, kOutputState, source, source == backbuffer ? kBackbufferState : kOutputState);
    return true;
  }
  auto* device = state.device.Get();
  // The real frame is captured once per pair; each generated frame only reads it.
  if (requestedIndex == 1) {
  state.retired.Collect(++state.pairs);
  const auto now = GetTickCount64();
  // How far apart the real frames are: this is the budget every generated frame of the pair has to fit
  // in, and the one number that separates the settings that work from the ones that stutter.
  if (state.lastCaptureMs != 0) {
    auto& counters = Counters();
    const auto gap = static_cast<uint32_t>(now - state.lastCaptureMs);
    counters.pairs.fetch_add(1, std::memory_order_relaxed);
    counters.gapTotalMs.fetch_add(gap, std::memory_order_relaxed);
    RaisePeak(counters.gapMaxMs, gap);
  }
  // After a pause the two real frames show unrelated moments.
  if (policy::PairIsStale(state.lastCaptureMs, now)) state.frames[state.current].valid = false;
  state.lastCaptureMs = now;
  state.current ^= 1;
  auto& next = state.frames[state.current];
  if (!Capture(device, batch, next.backbuffer, backbuffer, kBackbufferState) ||
      !Capture(device, batch, next.depth, depth, kInputState) ||
      !Capture(device, batch, next.motion, motion, kInputState) ||
      !Capture(device, batch, next.hudless, hudless, kInputState)) {
    Fail("input copies");
    state.failed = true;
    return false;
  }
  auto& data = next.data;
  data = {};
  data.backbuffer = Describe(next.backbuffer.Get());
  data.depth = Describe(next.depth.Get());
  data.motion = Describe(next.motion.Get());
  MatchDepthAndMotion(params, data);
  data.hudless = Describe(next.hudless.Get());
  void* projection = nullptr;
  if (params->Get("DLSSG.CameraViewToClip", &projection) == NVSDK_NGX_Result_Success && projection != nullptr)
    std::memcpy(data.projection, projection, sizeof(data.projection));
  data.resolution[0] = static_cast<float>(state.width);
  data.resolution[1] = static_cast<float>(state.height);
  data.jitter[0] = Value<float>(params, "DLSSG.JitterOffsetX", 0.f);
  data.jitter[1] = Value<float>(params, "DLSSG.JitterOffsetY", 0.f);
  data.motionScale[0] = Value<float>(params, "DLSSG.MvecScaleX", 1.f);
  data.motionScale[1] = Value<float>(params, "DLSSG.MvecScaleY", 1.f);
  const bool reset = Value<unsigned int>(params, "DLSSG.Reset", 0) != 0;
  data.reset = reset ? 1 : 0;
  next.valid = true;
  }
  auto& next = state.frames[state.current];
  auto& previous = state.frames[state.current ^ 1];
  // Without a pair the real frame is shown again.
  bool generated = false;
  // One reason, not a chain of conditions: the panel and the log say which wait this is.
  auto idle = policy::Idle::None;
  if (!previous.valid) idle = policy::Idle::NoPair;
  else if (next.data.reset != 0) idle = policy::Idle::GameReset;
  else if ((next.hudless != nullptr) != (previous.hudless != nullptr)) idle = policy::Idle::HudlessChanged;
  else {
    ExecuteParams execute{};
    execute.previous = previous.data;
    execute.next = next.data;
    ID3D12Resource* outputs[kMaxFrames]{};
    // Every slot is a texture of ours, created in UNORDERED_ACCESS: XeFG dispatches into it by UAV,
    // and a buffer NGX owns cannot be given that state from here (its current state is not knowable,
    // and a wrong barrier is a device removal). Writing straight into DLSS-G's own buffer was tried
    // twice and reverted twice: once the frames arrived with the buffer in the wrong state, and once
    // -- with the flag checked first and no barrier at all -- every frame generated and the picture
    // still stuttered. Handing XeFG that buffer is not where the smoothness is, whatever the panel
    // and the counters say. The copy back below is the price of frames the GPU can actually see.
    for (uint32_t i = 0; i < frameCount; ++i) {
      if (!state.outputs[i])
        state.outputs[i] = Texture(device, state.outputDesc, kOutputState, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      if (!state.outputs[i]) {
        Fail("output texture");
        state.failed = true;
        return false;
      }
      outputs[i] = state.outputs[i].Get();
    }
    const uint32_t bases[kMaxFrames * 2]{};
    execute.outputs = outputs;
    execute.outputBases = bases;
    execute.outputCount = static_cast<uint8_t>(frameCount);
    execute.firstFrame = frameIndex - 1;
    execute.frameCount = 1;
    // Execute reads the captured inputs: they must be barriered and copied before it records.
    batch.Flush();

    const auto executeStarted = Microseconds();
    const auto result = Guarded(reinterpret_cast<unsigned char*>(state.fg) + kExecuteRva, state.context, list, &execute);
    {
      auto& counters = Counters();
      const auto spent = static_cast<uint32_t>(Microseconds() - executeStarted);
      counters.executeTotalUs.fetch_add(spent, std::memory_order_relaxed);
      RaisePeak(counters.executeMaxUs, spent);
    }
    if (result < 0) {
      idle = policy::Idle::ExecuteError;
      // -4 is XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT. A scene change or a mode switch refuses a
      // single evaluate and the next one succeeds, so the transition in and out is not worth a line per
      // hiccup: NoteRefused says what was refused, and the rates line counts them per interval.
      if (result == -4) {
        Counters().refused.fetch_add(1, std::memory_order_relaxed);
        NoteRefused(execute);
      } else {
        Counters().errored.fetch_add(1, std::memory_order_relaxed);
        Log("execute failed: " + std::to_string(result));
      }
      if (result == -100000) {
        Fail("XeFG crashed while generating a frame");
        state.failed = true;
      }
    } else {
      generated = result == 0;
      if (!generated) {
        idle = policy::Idle::NoOutput;
      } else {
        Counters().generated.fetch_add(1, std::memory_order_relaxed);
        g_last_generated_tick.store(GetTickCount64(), std::memory_order_relaxed);
      }
    }
  }
  state.lastGenerated = generated;
  NoteIdle(idle);
  // The real-frame copy recorded above (if any) is still pending; add the output and let the
  // batch's destructor flush them together as one barrier pair.
  const D3D12_RESOURCE_STATES sourceState = generated ? kOutputState : kBackbufferState;
  ID3D12Resource* const source = generated ? state.outputs[frameIndex - 1].Get() : backbuffer;
  batch.Add(interpolated, kOutputState, source, sourceState);
  return true;
}
// Repeats the real frame into every output NGX asked for: what a disabled mode and a failed
// generation both hand back. Less smooth, never black.
inline bool RepeatRealFrame(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params, bool realWritten) {
  auto* backbuffer = Resource(params, "DLSSG.Backbuffer");
  auto* interpolated = Resource(params, "DLSSG.OutputInterpolated");
  if (backbuffer == nullptr || interpolated == nullptr) return false;
  // A generation that failed after recording its real-frame copy already had that copy flushed by the
  // batch, so the same backbuffer is there: copying it again would write the same pixels twice.
  if (!realWritten) {
    if (auto* real = Resource(params, "DLSSG.OutputReal")) Copy(list, real, kOutputState, backbuffer, kBackbufferState);
  }
  Copy(list, interpolated, kOutputState, backbuffer, kBackbufferState);
  return true;
}
// A generation that fails repeats the real frame rather than handing the evaluate back: the frame
// is already half-copied by then, so the core must not also write these buffers (hook.cpp's caller
// treats true as "the outputs are mine"). Less smooth, never black.
inline bool Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params) {
  if (list == nullptr || params == nullptr) return false;
  Counters().asked.fetch_add(1, std::memory_order_relaxed);
  bool realWritten = false;
  const bool handled = Generate(list, params, realWritten) || RepeatRealFrame(list, params, realWritten);
  ReportRates();
  return handled;
}
}  // namespace uplift::xmx::generator
