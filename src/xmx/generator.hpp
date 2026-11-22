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
// 1.8.3 (upstream 9f0a24e): the states a compute list cannot name. A resource that arrives in one
// of them has to be copied from the non-pixel state instead, or the barrier is a device removal.
constexpr auto kGraphicsOnlyStates = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

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
// GPU timing of one pair: two timestamps, recorded around the pair's copies and Execute calls and read
// back once the commands are certainly done. EvaluateFeature has no queue to wait on, so age stands in
// for a fence here exactly as it does for the retire queue; the margin is the difference between the
// oldest slot read and the slot being rewritten.
constexpr uint32_t kStampSlots = 16;
constexpr uint64_t kStampAgePairs = 12;
constexpr uint64_t kStampMaxUs = 100000;  // longer is a pause, or a slot read while it was being written

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
  // The pair whose frames one Execute call generated, and how many of them are in outputs: a later
  // evaluate of that pair only copies its own frame out of them (see Generate's batch).
  uint64_t batchPair{};
  uint32_t batchFrames{};
  uint32_t contexts{};
  // What the ported model can produce at all (model::kGeneratedFrames once Publish succeeded, 1 until
  // then): the ceiling Prepare chooses a context's frames from, not the context's own count.
  uint32_t modelFrames{1};
  // How many generated frames this context's model was built for: 1 at 2x, modelFrames otherwise.
  uint32_t maxFrames{1};
  // The list Prepare last made a context on: the same list and shape answers without a driver call
  // (Prepare runs once per evaluate, six times per pair at 6x).
  ID3D12GraphicsCommandList* preparedList{};
  D3D12_RESOURCE_DESC outputDesc{};
  bool published{};
  bool failed{};
  // 1.8.3 (upstream 6de60fc): the last ContextShape XeFG's Init refused. Retrying it every frame
  // spends one of the sixteen contexts per frame and gives up on the mode; another shape (a
  // resolution, format or HDR change) is tried again.
  bool refused{};
  policy::ContextShape refusedShape{};
  // Intel's 2x model refused this game's inputs at 2x: every multiplier then runs the five-frame
  // model, at 2x serving its t=1/2 frame exactly as it did before (see policy::ModelFrames).
  bool twoFrameRefused{};
  uint32_t logs{};
  // The pair's GPU time on the game's queue: a timestamp around everything this generator makes the
  // GPU do for the pair, resolved on its last evaluate and read a dozen pairs later. The one number
  // Execute's CPU timer (counters.executeTotalUs) cannot give: what the copies cost on the GPU.
  ComPtr<ID3D12QueryHeap> stamps;
  ComPtr<ID3D12Resource> stampsReadback;
  // The frequency is a property of a queue, not of the device: this one only answers it.
  ComPtr<ID3D12CommandQueue> clock;
  uint64_t stampFrequency{};
  uint32_t stampSlot{};
  uint32_t stampCurrent{};  // the slot this pair writes
  uint64_t stampPair[kStampSlots]{};
  bool stampReady{};
  bool stampFailed{};
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
// The refusal above is no longer the reason once a shape works: the panel reads g_failure, and a
// stale line there says the mode is broken while it generates (upstream 6de60fc's ClearFailure).
inline void ClearFailure() {
  std::lock_guard guard(g_failure_lock);
  g_failure.clear();
}
inline std::atomic_bool g_preflight_passed{false};
// What the mode actually delivered, per interval. A frame rate alone cannot tell "XeFG generates every
// frame, just slower than NVIDIA's generator" (the wide-budget settings) apart from "XeFG does not
// finish in time and the real frame is presented again" (the same number, and a stutter), and the two
// need opposite fixes. Rates, not totals: only an interval's delta says which one is happening now.
struct Stats {
  std::atomic<uint32_t> asked{0}, generated{0}, repeated{0}, refused{0}, errored{0}, pairs{0};
  // Execute calls, and how many of them asked for more than one frame: with the batch the second
  // number is the pairs and the first is one per pair, not one per frame.
  std::atomic<uint32_t> executes{0}, batched{0};
  std::atomic<uint64_t> gapTotalMs{0}, executeTotalUs{0};
  std::atomic<uint32_t> gapMaxMs{0}, executeMaxUs{0};
  // What the pair's GPU work costs (timestamps), the CPU timer's counterpart: a pair at a time, so
  // this is the number that says whether 2x leaving the model to one frame bought anything.
  std::atomic<uint64_t> gpuTotalUs{0};
  std::atomic<uint32_t> gpuMaxUs{0}, gpuPairs{0};
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
  const auto executes = counters.executes.exchange(0, std::memory_order_relaxed);
  const auto batched = counters.batched.exchange(0, std::memory_order_relaxed);
  const auto gpuTotal = counters.gpuTotalUs.exchange(0, std::memory_order_relaxed);
  const auto gpuMax = counters.gpuMaxUs.exchange(0, std::memory_order_relaxed);
  const auto gpuPairs = counters.gpuPairs.exchange(0, std::memory_order_relaxed);
  const auto span = now - start;
  const auto perSecond = [span](uint64_t count) { return span == 0 ? 0ull : count * 1000ull / span; };
  auto& state = Get();
  std::string text = "over " + std::to_string(span) + "ms asked " + std::to_string(asked) + " (" +
                     std::to_string(perSecond(asked)) + "/s) generated " + std::to_string(generated) + " (" +
                     std::to_string(perSecond(generated)) + "/s) repeated " + std::to_string(repeated) + " refused " +
                     std::to_string(refused) + " errored " + std::to_string(errored);
  text += " | pairs " + std::to_string(pairs) + " (" + std::to_string(perSecond(pairs)) + "/s) every " +
          std::to_string(pairs == 0 ? 0 : gapTotal / pairs) + "ms (max " + std::to_string(gapMax) + "ms)";
  // Per call, not per asked frame: with the batch one call covers a pair, and dividing by the
  // presented frames would report a fifth of what the library actually spent.
  text += " | execute " + std::to_string(executes == 0 ? 0 : executeTotal / executes) + "us avg " +
          std::to_string(executeMax) + "us max over " + std::to_string(executes) + " calls (" +
          std::to_string(batched) + " batched)";
  // The GPU half of the same question, and the only field the CPU timer cannot answer: what the
  // pair's copies and Execute calls cost the GPU. Absent when the device refused the query heap.
  if (gpuPairs != 0)
    text += " | gpu pair " + std::to_string(gpuTotal / gpuPairs) + "us avg " + std::to_string(gpuMax) +
            "us max over " + std::to_string(gpuPairs) + " pairs";
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
// The file a runtime was actually loaded from (upstream f9ced61): a copy the game already loaded
// wins over ours for libxell.dll, so the log has to name what ran, not what was asked for.
inline std::string RuntimePath(HMODULE module) {
  wchar_t text[MAX_PATH]{};
  return module != nullptr && GetModuleFileNameW(module, text, MAX_PATH) != 0
             ? std::filesystem::path(text).string()
             : std::string("not loaded");
}
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
    if (ll != nullptr && model::Publish(fg, ll, error)) state.modelFrames = kMaxFrames;
    else Log("multi-frame model refused, 2x only: " + (ll == nullptr ? std::string("libxell.dll not found") : error));
    // 1.8.3 (upstream f9ced61): name the files that actually ran -- a libxell the unlock cannot read
    // is what leaves 2x only, and the paths are the only way to tell which copy was loaded.
    Log("runtime " + RuntimePath(fg) + " and " + RuntimePath(ll));
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
// 1.8.3 (upstream 9f0a24e, the compute-list half): a compute list cannot name a graphics state, so a
// source in one is copied from the non-pixel state instead. Upstream also reads the state the game
// tagged the resource with for direct lists; Uplift does not hook slSetTag and has no such table, so
// a direct list keeps the state its caller declared.
inline D3D12_RESOURCE_STATES EffectiveSourceState(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES declared) {
  if (list->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE) return declared;
  if ((declared & kGraphicsOnlyStates) == 0) return declared;
  return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
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
// The frame's copies as one batch. One at a time, every copy costs four ResourceBarrier calls, each
// with its own validation and submission overhead, and the backbuffer is both the real frame's source
// and the HUD-less capture's, so a frame of six copies used to fall back to that path. Every transition
// is known before the first copy is recorded, so a frame goes out as two barrier calls around the
// copies: one per destination, one per distinct source. A shape this cannot express -- a resource
// written twice, or a resource needed in two states -- keeps the one-at-a-time path.
class CopyBatch {
 public:
  explicit CopyBatch(ID3D12GraphicsCommandList* list) : list_(list) {}
  CopyBatch(const CopyBatch&) = delete;
  CopyBatch& operator=(const CopyBatch&) = delete;
  ~CopyBatch() { Flush(); }

  ID3D12GraphicsCommandList* List() const { return list_; }

  void Add(ID3D12Resource* target, D3D12_RESOURCE_STATES targetState, ID3D12Resource* source,
           D3D12_RESOURCE_STATES sourceState) {
    if (target == nullptr || source == nullptr) return;
    // A resource cannot be two destinations (two writes to one texture would race), and it cannot be
    // both a destination and a source here (one resource cannot sit in two barrier states). A source
    // shared by two copies is fine: the real frame's copy and the HUD-less capture both read the
    // backbuffer, which is what kept every pair on the one-at-a-time path. A pair CopyResource cannot
    // express, one past the fixed storage, or a second copy that disagrees about the source's state
    // falls back to that path, which is correct either way and makes the same check the batch makes.
    if (count_ >= kMaxBatch || !SameShape(target, source) || IsTarget(target) || IsSource(target) ||
        IsTarget(source) || SourceStateConflicts(source, sourceState)) {
      Copy(list_, target, targetState, source, sourceState);
      return;
    }
    targets_[count_] = {target, targetState};
    sources_[count_] = {source, sourceState};
    copies_[count_] = {target, source};
    ++count_;
  }

  void Flush() {
    if (count_ == 0) return;
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
    // A source more than once in the batch is transitioned once: the second copy reads the same
    // state, and a second transition of a resource already in it would be a barrier with equal
    // before and after states, which the driver rejects.
    const auto first = [this](size_t i) {
      for (size_t j = 0; j < i; ++j)
        if (sources_[j].resource == sources_[i].resource) return false;
      return true;
    };
    for (size_t i = 0; i < count_; ++i) push(targets_[i].resource, targets_[i].state, D3D12_RESOURCE_STATE_COPY_DEST);
    for (size_t i = 0; i < count_; ++i)
      if (first(i)) push(sources_[i].resource, sources_[i].state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(count, barriers);
    for (size_t i = 0; i < count_; ++i) list_->CopyResource(copies_[i].target, copies_[i].source);
    count = 0;
    for (size_t i = 0; i < count_; ++i)
      if (first(i)) push(sources_[i].resource, D3D12_RESOURCE_STATE_COPY_SOURCE, sources_[i].state);
    for (size_t i = 0; i < count_; ++i) push(targets_[i].resource, D3D12_RESOURCE_STATE_COPY_DEST, targets_[i].state);
    list_->ResourceBarrier(count, barriers);
    count_ = 0;
  }

 private:
  static constexpr size_t kMaxBatch = 8;
  struct Entry { ID3D12Resource* resource; D3D12_RESOURCE_STATES state; };
  struct Pair { ID3D12Resource* target; ID3D12Resource* source; };
  static bool SameShape(ID3D12Resource* target, ID3D12Resource* source) {
    const auto to = target->GetDesc(), from = source->GetDesc();
    return policy::CanCopy(to.Width, to.Height, to.Format, from.Width, from.Height, from.Format);
  }
  // True when the resource is already a destination of this batch.
  bool IsTarget(ID3D12Resource* resource) const {
    for (size_t i = 0; i < count_; ++i) {
      if (targets_[i].resource == resource) return true;
    }
    return false;
  }
  // True when the resource is already a source of this batch.
  bool IsSource(ID3D12Resource* resource) const {
    for (size_t i = 0; i < count_; ++i) {
      if (sources_[i].resource == resource) return true;
    }
    return false;
  }
  // A source already in the batch keeps the state it was added with (the state the caller declared
  // is the same resource, so a disagreement means the caller sees it differently this time).
  bool SourceStateConflicts(ID3D12Resource* source, D3D12_RESOURCE_STATES state) const {
    for (size_t i = 0; i < count_; ++i) {
      if (sources_[i].resource == source && sources_[i].state != state) return true;
    }
    return false;
  }
  ID3D12GraphicsCommandList* list_;
  // Fixed storage, not three vectors: this object is built once per evaluate, and a heap allocation
  // per copy costs more than the barriers it batches.
  std::array<Entry, kMaxBatch> targets_{}, sources_{};
  std::array<Pair, kMaxBatch> copies_{};
  size_t count_{};
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
  batch.Add(copy.Get(), kXefgInputState, source, EffectiveSourceState(batch.List(), sourceState));
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
// A resource's shape as numbers: what changes when the game switches inputs, and what a rotation
// among its own buffers does not. One GetDesc fills an entry, and the last eight are remembered by
// pointer, so the per-evaluate check below compares numbers and a rotating swapchain costs no driver
// call after the first frames.
struct ShapeNumbers {
  uint32_t width{};
  uint32_t height{};
  uint32_t format{};
};
inline ShapeNumbers ShapeOf(ID3D12Resource* resource) {
  struct Entry {
    const void* resource{};
    ShapeNumbers shape{};
  };
  static Entry cache[8];
  if (resource == nullptr) return {};
  Entry& entry = cache[(reinterpret_cast<uintptr_t>(resource) >> 4) & 7];
  if (entry.resource == resource) return entry.shape;
  const auto desc = resource->GetDesc();
  entry.resource = resource;
  entry.shape = {static_cast<uint32_t>(desc.Width), desc.Height, static_cast<uint32_t>(desc.Format)};
  return entry.shape;
}
// Every argument the mode hands XeFG, once per distinct combination. A refused mode has to say what it
// was refused for, and the game's own input sizes are the only arguments that move with an upscaler
// setting (the context's shape is fixed at init, and only a resize changes it). The key holds shapes,
// not resources: DLSS-G and the game rotate the buffers they hand over, so a pointer-keyed line would
// print the same numbers once per evaluate. The frame index and DLSSG.OutputReal stay out for the
// same reason: the index walks 1..5 within every pair, and only the first evaluate carries OutputReal.
inline void NoteArguments(const NVSDK_NGX_Parameter* params, ID3D12Resource* backbuffer, ID3D12Resource* depth,
                          ID3D12Resource* motion, ID3D12Resource* hudless, ID3D12Resource* interpolated,
                          uint32_t requested, uint32_t frames) {
  auto& state = Get();
  // Only a change in what the mode is handed is worth a line, and the check comes first as plain
  // numbers: this runs once per evaluate -- six times per pair at 6x -- and building the line to
  // compare it against the last one spent nine GetDesc calls and thirty string allocations on the
  // render thread for every one of them, to be thrown away.
  struct Key {
    ShapeNumbers shapes[4];  // backbuffer, depth, motion, hudless
    uint32_t areas[8];
    uint32_t requested, frames, width, height, format, flags;
  };
  Key key{};
  const ShapeNumbers inputs[4] = {ShapeOf(backbuffer), ShapeOf(depth), ShapeOf(motion), ShapeOf(hudless)};
  std::memcpy(key.shapes, inputs, sizeof(inputs));
  const policy::Area depthArea = RawArea(params, "DLSSG.Depth");
  const policy::Area motionArea = RawArea(params, "DLSSG.MVecs");
  const uint32_t declared[8] = {depthArea.x, depthArea.y, depthArea.width, depthArea.height,
                                motionArea.x, motionArea.y, motionArea.width, motionArea.height};
  std::memcpy(key.areas, declared, sizeof(declared));
  key.requested = requested;
  key.frames = frames;
  key.width = state.width;
  key.height = state.height;
  key.format = static_cast<uint32_t>(state.format);
  key.flags = state.flags;
  static Key lastKey{};
  static bool seen = false;
  static uint32_t emitted = 0, changed = 0;
  if (seen && std::memcmp(&key, &lastKey, sizeof(Key)) == 0) return;
  lastKey = key;
  seen = true;
  // A change on every evaluate is a rotation the shapes did not filter, not a diagnosis: the first
  // thirty-two combinations print, then one line per sixty-four changes, so the log keeps the history
  // without owning the file. No line is ever dropped silently.
  ++changed;
  if (emitted >= 32 && changed < 64) return;
  changed = 0;
  ++emitted;
  const std::string shapes = "backbuffer " + Shape(backbuffer) + " depth " + Shape(depth) + " motion " + Shape(motion) +
                             " hudless " + Shape(hudless) + " interpolated " + Shape(interpolated) +
                             " depth-declared " + AreaText(depthArea) +
                             " mvec-declared " + AreaText(motionArea) + " frames " +
                             std::to_string(frames) + " requested " + std::to_string(requested) + " context " +
                             std::to_string(state.width) + "x" +
                             std::to_string(state.height) + " format " +
                             std::to_string(static_cast<int>(state.format)) + " flags " + std::to_string(state.flags);
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
constexpr uint32_t kFlagDepthInverted = 1u << 0;
constexpr uint32_t kFlagMotionJittered = 1u << 4;
// Upstream 1.8.6 (e4d4aad): the ported multi-frame model refuses the jittered-motion flag (HZD
// Remastered, Death Stranding 2) and the fallback ran Intel's 2x model instead. The model is now
// started without that flag; the motion vectors still carry the camera jitter (about a render
// pixel at most), with no visible artifact in Death Stranding 2. XmxJitter=0 under [Uplift] in
// ReShade.ini restores the 2x fallback (upstream spells it [DLSSG] XmxJitter=0).
inline bool JitterFlagStripped() { return uplift::xmx::JitterStrip(); }

// The pair's GPU time. Everything here is optional: a device that refuses one of the three objects
// leaves the rates line without its gpu field and changes nothing else.
inline bool StampsReady(ID3D12Device* device) {
  auto& state = Get();
  if (state.stampReady) return true;
  if (state.stampFailed || device == nullptr) return false;
  const auto giveUp = [&] {
    state.stampFailed = true;
    return false;
  };
  D3D12_QUERY_HEAP_DESC heap{};
  heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  heap.Count = kStampSlots * 2;
  if (FAILED(device->CreateQueryHeap(&heap, IID_PPV_ARGS(&state.stamps)))) return giveUp();
  D3D12_HEAP_PROPERTIES readback{};
  readback.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = kStampSlots * 2 * sizeof(uint64_t);
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc = {1, 0};
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&state.stampsReadback))))
    return giveUp();
  D3D12_COMMAND_QUEUE_DESC queue{};
  queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(device->CreateCommandQueue(&queue, IID_PPV_ARGS(&state.clock)))) return giveUp();
  if (FAILED(state.clock->GetTimestampFrequency(&state.stampFrequency)) || state.stampFrequency == 0) return giveUp();
  state.stampReady = true;
  return true;
}
// Reads the slots whose commands are certainly done. A slot is written once per pair and there are
// kStampSlots of them, so a slot older than kStampAgePairs cannot be in flight; a torn or nonsensical
// pair (a pause, a debugging stop) is dropped rather than folded in.
inline void ReadStamps() {
  auto& state = Get();
  if (!state.stampReady) return;
  const D3D12_RANGE all{0, kStampSlots * 2 * sizeof(uint64_t)};
  void* mapped = nullptr;
  if (FAILED(state.stampsReadback->Map(0, &all, &mapped)) || mapped == nullptr) return;
  const auto* values = static_cast<const uint64_t*>(mapped);
  auto& counters = Counters();
  for (uint32_t slot = 0; slot < kStampSlots; ++slot) {
    const auto pair = state.stampPair[slot];
    if (pair == 0 || state.pairs < pair + kStampAgePairs) continue;
    state.stampPair[slot] = 0;
    const auto begin = values[slot * 2], end = values[slot * 2 + 1];
    if (end <= begin) continue;
    const auto spent = static_cast<uint64_t>((end - begin) * 1000000ull / state.stampFrequency);
    if (spent == 0 || spent > kStampMaxUs) continue;
    counters.gpuTotalUs.fetch_add(spent, std::memory_order_relaxed);
    counters.gpuPairs.fetch_add(1, std::memory_order_relaxed);
    RaisePeak(counters.gpuMaxUs, static_cast<uint32_t>(spent));
  }
  const D3D12_RANGE none{0, 0};
  state.stampsReadback->Unmap(0, &none);
}

inline bool Prepare(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params, const D3D12_RESOURCE_DESC& output,
                    uint32_t requested) {
  auto& state = Get();
  uint32_t flags = 0;
  if (Value<int>(params, "DLSSG.DepthInverted", 0) != 0) flags |= kFlagDepthInverted;
  if (Value<int>(params, "DLSSG.MvecJittered", 0) != 0) flags |= kFlagMotionJittered;
  const auto width = static_cast<uint32_t>(output.Width);
  // 2x asks for one generated frame and gets Intel's two-frame model (policy::ModelFrames); every
  // other multiplier gets the ported five-frame model. On the first call modelFrames is still 1 --
  // Publish runs inside LoadContext below -- so this is recomputed after it.
  const bool twoFrameModel = !state.twoFrameRefused;
  uint32_t frames = policy::ModelFrames(requested, state.modelFrames, twoFrameModel);
  // The common call costs no driver work: every evaluate of every pair asks for the same shape on the
  // same list, and list->GetDevice is an AddRef/Release round trip into the runtime -- six of them per
  // pair at 6x. A command list never changes devices, so a different list takes the full path below,
  // and that is what notices a device replaced with one of the same shape.
  if (state.context != nullptr && list == state.preparedList && width == state.width &&
      output.Height == state.height && output.Format == state.format && flags == state.flags &&
      frames <= state.maxFrames)
    return true;
  ComPtr<ID3D12Device> device;
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return false;
  // A new device needs a new context even with the same shape, and so does a capacity that cannot serve
  // the request: a five-frame context serves 2x, a one-frame context does not serve 3x.
  policy::ContextShape wanted{device.Get(), width, output.Height, static_cast<uint32_t>(output.Format), flags, frames};
  if (state.context != nullptr &&
      policy::Serves({state.device.Get(), state.width, state.height, static_cast<uint32_t>(state.format), state.flags,
                      state.maxFrames},
                     wanted))
    return true;
  // A shape XeFG already refused is not retried every frame; one whose shape changed is.
  if (state.refused && wanted == state.refusedShape) return false;
  // XeFG's contexts are not destroyed (the function's contract is unverified), so a game that keeps
  // resizing stops generating instead of filling the VRAM.
  if (state.contexts >= policy::kMaxContexts) {
    Fail("the game changed its output size too many times; restart the game");
    state.failed = true;
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
    // The pair's generated frames went with the old context: no evaluate of it may reuse them.
    state.batchFrames = 0;
    state.context = nullptr;  // its commands may be in flight, so it stays alive
    state.lastGenerated = false;
  }
  state.device = device;
  state.preparedList = list;
  ++state.contexts;
  if (!LoadContext(state.device.Get())) {
    state.failed = true;
    return false;
  }
  // Publish just ran, so the ceiling the model can serve is only known now (and on a 2x-only title it
  // is all this asks for).
  frames = policy::ModelFrames(requested, state.modelFrames, twoFrameModel);
  wanted.frames = frames;
  InitParams init{};
  init.width = width;
  init.height = output.Height;
  init.format = output.Format;
  const bool strip = (flags & kFlagMotionJittered) != 0 && frames > 1 && JitterFlagStripped();
  init.flags = strip ? flags & ~kFlagMotionJittered : flags;
  init.maxFrames = frames;
  init.creationMask = 1;
  init.visibleMask = 1;
  // Upstream 1.8.3 also starts Intel's own 2x model when the ported multi-frame model throws inside
  // Init (-1000) on inputs Intel's model takes (HZD Remastered's jittered motion vectors, flags 17):
  // that retry runs on a second context, with the game's real flags and maxFrames 1. The log names
  // both attempts, so a game left at 2x says whether it was asked for 2x or fell back to it.
  const auto initialize = [&] {
    const auto result = Guarded(reinterpret_cast<unsigned char*>(state.fg) + kInitRva, state.context, &init, nullptr);
    Log("init " + std::to_string(width) + "x" + std::to_string(output.Height) + " format " +
        std::to_string(static_cast<int>(output.Format)) + " flags " + std::to_string(init.flags) + " frames " +
        std::to_string(init.maxFrames) + " result " + std::to_string(result));
    return result;
  };
  auto result = initialize();
  if (strip && result == 0)
    Log("the multi-frame model runs without the jittered-motion flag (XmxJitter=0 under [Uplift] restores Intel's 2x model)");
  // Either model can refuse a game's inputs, and each has the other as its fallback: the ported
  // multi-frame model falls back to Intel's 2x model (upstream 1.8.3), and 2x's own model falls back
  // to the five-frame model's t=1/2 frame, which is what every multiplier ran before ModelFrames.
  uint32_t fallbackFrames = 0;
  if (frames > 1) fallbackFrames = 1;
  else if (twoFrameModel && state.modelFrames > 1) fallbackFrames = state.modelFrames;
  if (result != 0 && fallbackFrames != 0 && state.contexts < policy::kMaxContexts) {
    ++state.contexts;
    if (LoadContext(state.device.Get())) {
      const bool fallbackStrip = (flags & kFlagMotionJittered) != 0 && fallbackFrames > 1 && JitterFlagStripped();
      init.flags = fallbackStrip ? flags & ~kFlagMotionJittered : flags;
      init.maxFrames = fallbackFrames;
      result = initialize();
      if (result == 0) {
        if (fallbackFrames == 1) {
          // The ceiling itself drops: every later call computes frames from it, so leaving it at
          // five would rebuild a context per evaluate instead of settling on what works.
          state.modelFrames = 1;
          Log("the multi-frame model refused these inputs; Intel's 2x model runs instead");
        } else {
          state.twoFrameRefused = true;
          Log("Intel's 2x model refused these inputs; the multi-frame model's t=1/2 frame runs instead");
        }
        frames = fallbackFrames;
        wanted.frames = fallbackFrames;
      }
    }
  }
  if (result != 0) {
    // Found out only here: the hook into NVIDIA's generation is gone long before this, so a game
    // whose output format XeFG cannot take has to be told what to change.
    Fail(output.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
             ? "XMX cannot use this game's scRGB HDR output (result " + std::to_string(result) + "); turn HDR off or use XeMFG"
             : "XeFG initialization returned " + std::to_string(result));
    state.maxFrames = 1;
    if (!state.refused || !(wanted == state.refusedShape)) {
      state.refusedShape = wanted;
      state.refused = true;
    }
    return false;
  }
  if (state.refused) {
    state.refused = false;
    ClearFailure();
  }
  state.outputDesc = output;
  state.width = width;
  state.height = output.Height;
  state.format = output.Format;
  state.flags = flags;
  state.maxFrames = frames;
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
  // Prepare sets `failed` itself when retrying cannot help.
  if (!Prepare(list, params, interpolated->GetDesc(), requested)) return false;
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
  // Set when this evaluate opened a timestamp slot for a pair; the stop is recorded at the end.
  bool stamping = false;
  // The real frame is captured once per pair; each generated frame only reads it.
  if (requestedIndex == 1) {
  state.retired.Collect(++state.pairs);
  ReadStamps();
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
  // The GPU half of the pair's cost starts here: this is the first thing the pair records, and the
  // four input copies land in the list before Execute reads them.
  stamping = StampsReady(device);
  if (stamping) {
    state.stampCurrent = state.stampSlot;
    state.stampSlot = (state.stampSlot + 1) % kStampSlots;
    list->EndQuery(state.stamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, state.stampCurrent * 2);
  }
  // A game that hands the same resource as both backbuffer and HUD-less input has no HUD-less input
  // of its own: the backbuffer copy already is that input, and copying it again would be a second
  // full-resolution copy of the same pixels. The copies alias one texture (refcounted), and the
  // retire queue releases both references.
  const bool sharedHudless = hudless != nullptr && hudless == backbuffer;
  if (!Capture(device, batch, next.backbuffer, backbuffer, kBackbufferState) ||
      !Capture(device, batch, next.depth, depth, kInputState) ||
      !Capture(device, batch, next.motion, motion, kInputState) ||
      (!sharedHudless && !Capture(device, batch, next.hudless, hudless, kInputState))) {
    Fail("input copies");
    state.failed = true;
    return false;
  }
  if (sharedHudless) {
    if (next.hudless && next.hudless.Get() != next.backbuffer.Get())
      state.retired.Push(std::move(next.hudless), state.pairs);
    next.hudless = next.backbuffer;
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
  // This pair's frames were all generated by one call already (see the batch below): this evaluate
  // only copies the frame it was asked for. A refused batch leaves this false, and the frames are
  // generated one call at a time exactly as before.
  const bool reuse = state.batchPair == state.pairs && state.batchFrames >= frameIndex &&
                     state.outputs[frameIndex - 1] != nullptr;
  // One reason, not a chain of conditions: the panel and the log say which wait this is.
  auto idle = policy::Idle::None;
  if (!previous.valid) idle = policy::Idle::NoPair;
  else if (next.data.reset != 0) idle = policy::Idle::GameReset;
  else if ((next.hudless != nullptr) != (previous.hudless != nullptr)) idle = policy::Idle::HudlessChanged;
  else if (reuse) {
    // Already in outputs[frameIndex - 1]; counted like any other generated frame so that the rates
    // line keeps meaning "of the frames the game presented, this many were new".
    generated = true;
    Counters().generated.fetch_add(1, std::memory_order_relaxed);
    g_last_generated_tick.store(GetTickCount64(), std::memory_order_relaxed);
  } else {
    ExecuteParams execute{};
    execute.previous = previous.data;
    execute.next = next.data;
    ID3D12Resource* outputs[kMaxFrames]{};
    // Every slot is a texture of ours, created in UNORDERED_ACCESS: XeFG dispatches into it by UAV,
    // and a buffer NGX owns cannot be given that state from here (its current state is not knowable,
    // and a wrong barrier is a device removal). Writing straight into DLSS-G's own buffer was tried
    // three times and reverted three times: once the frames arrived with the buffer in the wrong
    // state; once, with the flag checked and no barrier at all, every frame generated while the
    // picture still stuttered; and once with a UAV barrier behind the write, which read fine in one
    // game and was not smooth in another. Handing XeFG that buffer is not where the smoothness is,
    // whatever the panel and the counters say. The copy back below is the price of frames the GPU can
    // actually see.
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
    // One call per pair instead of one per frame: the call starting at frame 0 does the pair-level
    // work (optical flow, the network's first half) and then each frame's own pass, so asking for
    // all the pair's frames at once saves the extra call shells, flushes and gaps that 5x/6x
    // otherwise place on the pair's critical path. firstFrame still says where this call's frames
    // go in outputs.
    const bool makeBatch = frameIndex == 1 && frameCount > 1;
    execute.firstFrame = makeBatch ? 0 : frameIndex - 1;
    execute.frameCount = makeBatch ? frameCount : 1;
    // Execute reads the captured inputs: they must be barriered and copied before it records.
    batch.Flush();

    const auto executeStarted = Microseconds();
    const auto result = Guarded(reinterpret_cast<unsigned char*>(state.fg) + kExecuteRva, state.context, list, &execute);
    {
      auto& counters = Counters();
      counters.executes.fetch_add(1, std::memory_order_relaxed);
      if (makeBatch) counters.batched.fetch_add(1, std::memory_order_relaxed);
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
        if (makeBatch) {
          // The pair's remaining evaluates copy out of outputs; this is set only on success, so a
          // refused batch is retried one frame at a time.
          state.batchPair = state.pairs;
          state.batchFrames = frameCount;
        }
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
  if (stamping) {
    // The stop is recorded after the last copy of the pair, so the sample is the pair's whole GPU
    // work: the four input copies, Execute, and the copies back. It is read a dozen pairs later,
    // when the frame that recorded it is long finished; the readback never waits on this frame.
    batch.Flush();
    list->EndQuery(state.stamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, state.stampCurrent * 2 + 1);
    list->ResolveQueryData(state.stamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, state.stampCurrent * 2, 2,
                           state.stampsReadback.Get(), state.stampCurrent * 2 * sizeof(uint64_t));
    state.stampPair[state.stampCurrent] = state.pairs;
  }
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
