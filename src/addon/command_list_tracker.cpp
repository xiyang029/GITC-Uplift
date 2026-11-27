#include "addon/command_list_tracker.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <string>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

#include "addon/uplift_catch.hpp"
#include "nr/log.hpp"

namespace uplift::addon {
namespace {

namespace api = reshade::api;

// {6c1a4f2e-93b1-4d8e-a50c-712e449bd318}: the native list's private data holds its ListState*.
constexpr GUID LIST_STATE_GUID = {0x6c1a4f2e, 0x93b1, 0x4d8e, {0xa5, 0x0c, 0x71, 0x2e, 0x44, 0x9b, 0xd3, 0x18}};

std::atomic<bool> g_tracking{false};
std::atomic<uint64_t> g_epoch{0u};

// Every Vulkan list's state, by handle. Never freed: the events and the NGX detour can run until the process ends.
struct VkLists {
  std::shared_mutex mutex;
  std::unordered_map<VkCommandBuffer, std::unique_ptr<VkListState>> states;
};
VkLists& SharedVkLists() {
  static VkLists* const lists = new VkLists();
  return *lists;
}

VkCommandBuffer VkBufferOf(api::command_list* list) {
  return reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(list->get_native()));
}

ID3D12GraphicsCommandList* NativeOf(api::command_list* list) {
  return reinterpret_cast<ID3D12GraphicsCommandList*>(list->get_native());
}

// The tracking handlers: one relaxed load while tracking is off, no lock ever.
void OnBindPipeline(api::command_list* cmd_list, api::pipeline_stage stages, api::pipeline pipeline) {
  if (!g_tracking.load(std::memory_order_relaxed)) return;
  if ((static_cast<uint32_t>(stages) & state::PIPELINE_STAGE_COMPUTE) == 0u) return;
  try {
    if (ListState* const state = FindListState(cmd_list)) {
      state->shadow.OnBindPipeline(static_cast<uint32_t>(stages), pipeline.handle);
    }
  }
  UPLIFT_CATCH("bind_pipeline", )
}

void OnBindDescriptorTables(api::command_list* cmd_list, api::shader_stage stages, api::pipeline_layout layout,
                            uint32_t first, uint32_t count, const api::descriptor_table* tables, uint32_t /*dynamic_offset_count*/,
                            const uint32_t* /*dynamic_offsets*/) {
  if (!g_tracking.load(std::memory_order_relaxed)) return;
  if ((static_cast<uint32_t>(stages) & state::SHADER_STAGE_COMPUTE) == 0u) return;
  try {
    if (ListState* const state = FindListState(cmd_list)) {
      state->shadow.OnBindDescriptorTables(static_cast<uint32_t>(stages), layout.handle, first, count,
                                           reinterpret_cast<const uint64_t*>(tables));
    }
  }
  UPLIFT_CATCH("bind_descriptor_tables", )
}

void OnPushConstants(api::command_list* cmd_list, api::shader_stage stages, api::pipeline_layout /*layout*/,
                     uint32_t layout_param, uint32_t first, uint32_t count, const void* values) {
  if (!g_tracking.load(std::memory_order_relaxed)) return;
  if ((static_cast<uint32_t>(stages) & state::SHADER_STAGE_COMPUTE) == 0u) return;
  try {
    if (ListState* const state = FindListState(cmd_list)) {
      state->shadow.OnPushConstants(static_cast<uint32_t>(stages), layout_param, first, count,
                                    static_cast<const uint32_t*>(values));
    }
  }
  UPLIFT_CATCH("push_constants", )
}

void OnPushDescriptors(api::command_list* cmd_list, api::shader_stage stages, api::pipeline_layout /*layout*/,
                       uint32_t layout_param, const api::descriptor_table_update& update) {
  if (!g_tracking.load(std::memory_order_relaxed)) return;
  if ((static_cast<uint32_t>(stages) & state::SHADER_STAGE_COMPUTE) == 0u) return;
  try {
    ListState* const state = FindListState(cmd_list);
    const std::optional<state::RootViewType> type = state::RootViewTypeOf(static_cast<uint32_t>(update.type));
    if (state == nullptr || !type || update.count != 1u || update.descriptors == nullptr) return;
    uint64_t address = 0u;
    if (*type == state::RootViewType::CBV) {
      // ReShade resolves a root CBV into a buffer range; the replay needs its address back.
      const auto& range = *static_cast<const api::buffer_range*>(update.descriptors);
      auto* const buffer = reinterpret_cast<ID3D12Resource*>(range.buffer.handle);
      if (buffer == nullptr) return;
      address = buffer->GetGPUVirtualAddress() + range.offset;
    } else {
      address = *static_cast<const uint64_t*>(update.descriptors);  // 6.1+: the raw GPU address
    }
    state->shadow.OnPushRootView(static_cast<uint32_t>(stages), layout_param, *type, address);
  }
  UPLIFT_CATCH("push_descriptors", )
}

void OnExecuteSecondaryCommandList(api::command_list* cmd_list, api::command_list* /*secondary*/) {
  if (!g_tracking.load(std::memory_order_relaxed)) return;
  try {
    if (ListState* const state = FindListState(cmd_list)) {
      state->shadow.OnExecuteBundle();
    }
  }
  UPLIFT_CATCH("execute_secondary_command_list", )
}

// Records explicit barriers observed by ReShade, so the native DLSS-NR probe can correlate them
// with the exact resource pointers it received. This is evidence, not a claim about current state.
void OnBarrier(api::command_list* cmd_list, uint32_t count, const api::resource* resources,
               const api::resource_usage* old_states, const api::resource_usage* new_states) {
  if (cmd_list == nullptr || count == 0u || resources == nullptr) return;
  try {
    ListState* const state = FindListState(cmd_list);
    if (state == nullptr || state->device == nullptr || state->device->get_api() != api::device_api::d3d12) return;
    for (uint32_t i = 0u; i < count; ++i) {
      const uint64_t resource = resources[i].handle;
      if (resource == 0u) continue;
      BarrierTraceRecord& record = state->barrier_trace[state->barrier_trace_next];
      record.resource = resource;
      record.ordinal = ++state->barrier_trace_total;
      record.before = (old_states != nullptr ? static_cast<uint32_t>(old_states[i]) : UINT32_MAX);
      record.after = (new_states != nullptr ? static_cast<uint32_t>(new_states[i]) : UINT32_MAX);
      state->barrier_trace_next = (state->barrier_trace_next + 1u) % static_cast<uint32_t>(state->barrier_trace.size());
      state->barrier_trace_count = std::min<uint32_t>(state->barrier_trace_count + 1u,
                                                       static_cast<uint32_t>(state->barrier_trace.size()));
    }
  }
  UPLIFT_CATCH("d3d12_barrier_trace", )
}

// Replay heaps, root signature, pipeline, tables and constants through ReShade; restore root descriptors natively.
class ReshadeReplayTarget final : public state::ReplayTarget {
 public:
  explicit ReshadeReplayTarget(api::command_list* list) : list_(list), native_(NativeOf(list)) {}
  void RebindHeapsAndRootSignature(uint64_t layout) override {
    // A count of 0 re-issues ReShade's cached heaps for any stage mask; the compute bit also re-sets the root
    // signature, so pass it only when the list had one (never a null compute root signature).
    list_->bind_descriptor_tables(layout != 0u ? api::shader_stage::all_compute : static_cast<api::shader_stage>(0),
                                  api::pipeline_layout{layout}, 0u, 0u, nullptr);
  }
  void BindPipeline(uint64_t pipeline) override { list_->bind_pipeline(api::pipeline_stage::all, api::pipeline{pipeline}); }
  void BindTables(uint64_t layout, uint32_t first, uint32_t count, const uint64_t* tables) override {
    list_->bind_descriptor_tables(api::shader_stage::all_compute, api::pipeline_layout{layout}, first, count,
                                  reinterpret_cast<const api::descriptor_table*>(tables));
  }
  void PushConstants(uint64_t layout, uint32_t param, uint32_t count, const uint32_t* values) override {
    list_->push_constants(api::shader_stage::all_compute, api::pipeline_layout{layout}, param, 0u, count, values);
  }
  void SetRootView(uint32_t param, state::RootViewType type, uint64_t gpu_address) override {
    switch (type) {
      case state::RootViewType::SRV: native_->SetComputeRootShaderResourceView(param, gpu_address); break;
      case state::RootViewType::UAV: native_->SetComputeRootUnorderedAccessView(param, gpu_address); break;
      case state::RootViewType::CBV: native_->SetComputeRootConstantBufferView(param, gpu_address); break;
    }
  }

 private:
  api::command_list* list_;
  ID3D12GraphicsCommandList* native_;
};

}  // namespace

void CreateListState(api::command_list* list) {
  ListState* const state = list->create_private_data<ListState>();
  state->list = list;
  state->device = list->get_device();
  state->shadow.OnReset(TrackingEpoch());  // a new list is open and empty
  NativeOf(list)->SetPrivateData(LIST_STATE_GUID, sizeof(state), &state);
}

std::vector<uint64_t> DestroyListState(api::command_list* list) {
  ListState* const state = FindListState(list);
  if (state == nullptr) return {};
  std::vector<uint64_t> tokens = std::move(state->tokens);
  NativeOf(list)->SetPrivateData(LIST_STATE_GUID, 0u, nullptr);
  list->destroy_private_data<ListState>();
  return tokens;
}

ListState* FindListState(api::command_list* list) {
  uint64_t value = 0u;
  list->get_private_data(reinterpret_cast<const uint8_t*>(&__uuidof(ListState)), &value);
  return reinterpret_cast<ListState*>(static_cast<uintptr_t>(value));
}

ListState* FindListState(ID3D12GraphicsCommandList* list) {
  if (list == nullptr) return nullptr;
  ListState* state = nullptr;
  UINT size = sizeof(state);
  if (FAILED(list->GetPrivateData(LIST_STATE_GUID, &size, &state)) || size != sizeof(state)) return nullptr;
  return state;
}

std::optional<StaleVkList> CreateVkListState(api::command_list* list) {
  auto state = std::make_unique<VkListState>();
  state->device = list->get_device();
  std::unique_ptr<VkListState> previous;
  {
    VkLists& lists = SharedVkLists();
    const std::unique_lock lock(lists.mutex);
    previous = std::exchange(lists.states[VkBufferOf(list)], std::move(state));
  }
  if (previous == nullptr || previous->tokens.empty()) return std::nullopt;
  return StaleVkList{.device = static_cast<api::device*>(const_cast<void*>(previous->device)), .tokens = std::move(previous->tokens)};
}

std::optional<VkListState> DestroyVkListState(api::command_list* list) {
  std::unique_ptr<VkListState> state;
  {
    VkLists& lists = SharedVkLists();
    const std::unique_lock lock(lists.mutex);
    const auto found = lists.states.find(VkBufferOf(list));
    if (found == lists.states.end()) return std::nullopt;
    state = std::move(found->second);
    lists.states.erase(found);
  }
  return std::move(*state);
}

VkListState* FindVkListState(VkCommandBuffer buffer) {
  VkLists& lists = SharedVkLists();
  const std::shared_lock lock(lists.mutex);
  const auto found = lists.states.find(buffer);
  return (found == lists.states.end() ? nullptr : found->second.get());
}

void ForgetVkListStates(const void* device) {
  VkLists& lists = SharedVkLists();
  const std::unique_lock lock(lists.mutex);
  std::erase_if(lists.states, [device](const auto& entry) { return entry.second->device == device; });
}

void SetTracking(bool on) {
  if (!on) {
    g_tracking.store(false, std::memory_order_release);
    return;
  }
  if (!g_tracking.load(std::memory_order_relaxed)) {
    g_epoch.fetch_add(1u, std::memory_order_relaxed);
    g_tracking.store(true, std::memory_order_release);
  }
}

uint64_t TrackingEpoch() {
  return (g_tracking.load(std::memory_order_acquire) ? g_epoch.load(std::memory_order_relaxed) : 0u);
}

void ResetBarrierTrace(ListState* state) {
  if (state == nullptr) return;
  state->barrier_trace_next = 0u;
  state->barrier_trace_count = 0u;
  state->barrier_trace_total = 0u;
}

void LogNativeNrBarrierTrace(ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* output,
                             ID3D12Resource* depth, ID3D12Resource* motion, std::string_view phase) {
  ListState* const state = FindListState(list);
  const auto ptr = [](ID3D12Resource* resource) { return reinterpret_cast<uint64_t>(resource); };
  if (state == nullptr) {
    nr::Logf(nr::LogLevel::INFO,
             "Native DLSS-NR barrier trace ({}): list {} is untracked; resource states remain unknown",
             phase, static_cast<void*>(list));
    return;
  }
  const std::array<std::pair<std::string_view, uint64_t>, 4> targets = {{
      {"color", ptr(color)}, {"output", ptr(output)}, {"depth", ptr(depth)}, {"motion", ptr(motion)}}};
  std::string matches;
  const uint32_t start = (state->barrier_trace_count == state->barrier_trace.size() ? state->barrier_trace_next : 0u);
  for (uint32_t n = 0u; n < state->barrier_trace_count; ++n) {
    const BarrierTraceRecord& record = state->barrier_trace[(start + n) % static_cast<uint32_t>(state->barrier_trace.size())];
    std::string names;
    for (const auto& [name, resource] : targets) {
      if (resource != 0u && resource == record.resource) {
        if (!names.empty()) names.append("/");
        names.append(name);
      }
    }
    if (names.empty()) continue;
    if (!matches.empty()) matches.append("; ");
    matches.append(std::format("#{} {} resource {:#x} state {:#x}->{:#x}", record.ordinal, names,
                               record.resource, record.before, record.after));
  }
  if (matches.empty()) matches = "no matching explicit barriers in the retained trace";
  nr::Logf(nr::LogLevel::INFO,
           "Native DLSS-NR barrier trace ({}): list {}, Color {} Output {} alias {}, Depth {}, Motion {}; retained {}/{} barrier records (total {}); {}; explicit barriers only—not a complete/current resource-state proof",
           phase, static_cast<void*>(list), static_cast<void*>(color), static_cast<void*>(output),
           (color != nullptr && color == output ? "yes" : "no"), static_cast<void*>(depth), static_cast<void*>(motion),
           state->barrier_trace_count, state->barrier_trace.size(), state->barrier_trace_total, matches);
}

bool NativeNrReplaceInputsReady(const ListState* tracked, ngx_hooks::DlssFrame* frame, std::string* reason) {
  const auto refuse = [reason](const char* why) {
    if (reason != nullptr) *reason = why;
    return false;
  };
  if (frame == nullptr || frame->color == nullptr || frame->output == nullptr) return refuse("Color or Output is missing");
  if (frame->native_nr && !frame->native_nr->enabled) return refuse("native NR is disabled");
  const auto valid_region = [](ID3D12Resource* image, nr::Rect* region) {
    if (image == nullptr) return false;
    const auto desc = image->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1u || desc.SampleDesc.Count != 1u
        || region->x >= desc.Width || region->y >= desc.Height) return false;
    if (region->width == 0u) region->width = static_cast<uint32_t>(desc.Width - region->x);
    if (region->height == 0u) region->height = desc.Height - region->y;
    return uint64_t{region->x} + region->width <= desc.Width && uint64_t{region->y} + region->height <= desc.Height;
  };
  if (!valid_region(frame->color, &frame->color_region) || !valid_region(frame->output, &frame->output_region)) {
    return refuse("Color or Output is not a valid single-sample 2D region");
  }
  // NR is same-resolution. Different atlas sizes/origins are fine; different active extents need a separate policy.
  if (frame->color_region.width != frame->output_region.width || frame->color_region.height != frame->output_region.height) {
    return refuse("Color and Output active extents differ");
  }
  const auto desc = frame->output->GetDesc();
  if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0u) return refuse("Output lacks UAV capability");
  if (tracked == nullptr) return refuse("command list is not tracked");
  // The native evaluate call is the state contract boundary: its distinct Color input is
  // shader-readable and Output is UAV. A same-resource Color/Output follows Output's UAV state;
  // the preprocessing pass temporarily transitions that resource to compute-read and restores it.
  // The ReShade barrier event does not see raw ID3D12GraphicsCommandList::ResourceBarrier calls,
  // implicit promotions/decays, or barriers on other queues, so its trace cannot prove current state.
  if ((frame->color->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0u) {
    return refuse("Color denies shader-resource access");
  }
  if (!frame->native_nr) frame->native_nr.emplace();
  auto& native = *frame->native_nr;
  native.color_state = (frame->color == frame->output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                       : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  const auto optional = [&](nr::BoundResource input, size_t index) {
    using Status = nr::NativeInputStatus;
    const auto drop = [&](Status status) { native.input_status[index] = status; return nr::BoundResource{}; };
    if (input.resource == nullptr) return drop(Status::MISSING);
    if (!valid_region(input.resource, &input.rect)) return drop(Status::INVALID_REGION);
    if (input.resource == frame->output && input.resource != frame->color) return drop(Status::OUTPUT_ALIAS);
    if ((input.resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0u) return drop(Status::SRV_DENIED);
    // Inputs supplied to native DLSS-NR follow the NGX shader-read contract. Do not infer a
    // resource state from flags or the incomplete barrier trace; these reads do not transition them.
    native.input_status[index] = Status::READY;
    return input;
  };
  const auto depth = optional({frame->depth, frame->depth_region}, 0u);
  frame->depth = depth.resource; frame->depth_region = depth.rect;
  const auto motion = optional({frame->motion_vectors, frame->motion_region}, 1u);
  frame->motion_vectors = motion.resource; frame->motion_region = motion.rect;
  native.control_mask = optional(native.control_mask, 2u);
  native.backbuffer = optional(native.backbuffer, 3u);
  native.ui = optional(native.ui, 4u);
  native.ui_alpha = optional(native.ui_alpha, 5u);
  if (native.backbuffer.resource == nullptr || (native.ui.resource == nullptr && native.ui_alpha.resource == nullptr)) native.ui_correction = false;
  if (reason != nullptr) reason->clear();
  return true;
}

bool NativeNrHandleResizeInputsReady(const ListState* tracked, ngx_hooks::DlssFrame* frame, std::string* reason) {
  return NativeNrReplaceInputsReady(tracked, frame, reason);
}

void RegisterTrackingEvents() {
  reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
  reshade::register_event<reshade::addon_event::bind_descriptor_tables>(OnBindDescriptorTables);
  reshade::register_event<reshade::addon_event::push_constants>(OnPushConstants);
  reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
  reshade::register_event<reshade::addon_event::execute_secondary_command_list>(OnExecuteSecondaryCommandList);
  // Keep this diagnostic event registered even when compute-state tracking is off: it observes
  // explicit barriers for the native-NR replacement investigation without changing command lists.
  reshade::register_event<reshade::addon_event::barrier>(OnBarrier);
}

ID3D12GraphicsCommandList* ReshadeDlssFrameHost::NativeList() {
  return (state_ == nullptr ? nullptr : NativeOf(state_->list));
}

bool ReshadeDlssFrameHost::StateKnown() {
  return state_ != nullptr && state_->shadow.Known(TrackingEpoch());
}

void ReshadeDlssFrameHost::ReplayState(state::RestoreMode mode) {
  ReshadeReplayTarget target(state_->list);
  state_->shadow.Replay(target, mode);
}

void ReshadeDlssFrameHost::AttachToken(uint64_t token) {
  if (state_->first_token == 0u) {
    state_->first_token = token;
  }
  state_->tokens.push_back(token);
  state_->recording_tokens.push_back(token);
}

}  // namespace uplift::addon
