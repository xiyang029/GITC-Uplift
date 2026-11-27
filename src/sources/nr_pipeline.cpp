#include "sources/nr_pipeline.hpp"

#include "d3d12_barrier.hpp"

#include "sources/stage_sets.hpp"
#include "sources/after_dlss_source.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <utility>

#include "nr/log.hpp"
#include "sources/look_plan.hpp"
#include "sources/pipeline_rules.hpp"

namespace uplift::sources {
using d3d::Transition;
namespace {

// Superior Scaling's fallback rule (PR #237): a degradation the card only names tersely warns once in the
// log, then runs degraded quietly -- never a per-frame warning, never a fully silent fallback.
template <class... Args>
void WarnOnce(bool* logged, std::format_string<Args...> format, Args&&... args) {
  if (std::exchange(*logged, true)) return;
  nr::Logf(nr::LogLevel::WARN, format, std::forward<Args>(args)...);
}


// The Present decode is a pixel shader; everything else reads through compute-legal states only.
constexpr D3D12_RESOURCE_STATES PIXEL_AND_COMPUTE_READ =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES COMPUTE_READ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t MODEL_BYTES_PER_PIXEL = 8u;   // RGBA16F
constexpr uint32_t MOTION_BYTES_PER_PIXEL = 4u;  // RG16F
constexpr uint64_t METER_STATE_BYTES = 32u;  // the meter's 2x1 RGBA32F state texture (the second texel is Auto's check)
constexpr nr::Size STATE_SIZE = {2u, 1u};
// The look's scene-cut state: one RGBA32F texel.
constexpr nr::Size LOOK_STATE_SIZE = {1u, 1u};
constexpr uint32_t LOOK_STATE_BYTES_PER_PIXEL = 16u;
// One readback slot per placed footprint of the 2x1 state (512 B apart, a 256 B row).
constexpr uint64_t CHECK_STRIDE = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;


void UavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER written = {.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV, .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE};
  written.UAV = {.pResource = resource};
  list->ResourceBarrier(1u, &written);
}

// The view format the game's depth is read with, or nullopt when Uplift cannot read it. The recording samples the depth in a compute shader
// (depth_cs.hlsl reads one plane), so a typed depth-stencil resource is read through the plain twin of its depth plane -- the same bits, stencil
// dropped -- and no copy or reinterpretation of the resource itself is needed. That twin is what the resource's own depth format returns:
// D16_UNORM -> R16_UNORM, D32_FLOAT -> R32_FLOAT, D24_UNORM_S8_UINT -> R24_UNORM_X8_TYPELESS, D32_FLOAT_S8X24_UINT -> R32_FLOAT_X8X24_TYPELESS.
std::optional<DXGI_FORMAT> ReadDepthFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT: return format;  // already-float depth from a bridge
    default: return std::nullopt;
  }
}

struct NativeInputFormat {
  DXGI_FORMAT source = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT target = DXGI_FORMAT_UNKNOWN;
};
std::optional<NativeInputFormat> NativeInputView(nr::BoundResource input, bool depth) {
  if (input.resource == nullptr) return std::nullopt;
  const auto desc = input.resource->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1u || desc.SampleDesc.Count != 1u
      || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0u || input.rect.width == 0u || input.rect.height == 0u
      || uint64_t{input.rect.x} + input.rect.width > desc.Width || uint64_t{input.rect.y} + input.rect.height > desc.Height) return std::nullopt;
  if (depth) {
    if (const auto view = ReadDepthFormat(desc.Format)) return NativeInputFormat{*view, DXGI_FORMAT_R32_FLOAT};
  } else if (const auto view = color::DescribeMaskFormat(desc.Format)) {
    return NativeInputFormat{view->view_format, view->view_format};
  }
  return std::nullopt;
}

// A float SRV format for the game's motion-vector texture, or nullopt when Uplift cannot read it.
std::optional<DXGI_FORMAT> MotionViewFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:          return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:          return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:                                return std::nullopt;
  }
}

}  // namespace

color::StabilizeMotion StabilizeMotionOf(const nr::BoundResource& motion, float scale_x, float scale_y) {
  if (motion.resource == nullptr) return {};
  const D3D12_RESOURCE_DESC description = motion.resource->GetDesc();
  const std::optional<DXGI_FORMAT> view_format = MotionViewFormat(description.Format);
  const std::optional<nr::Rect> rect = MotionRectOf(motion.rect, {static_cast<uint32_t>(description.Width), description.Height});
  if (!view_format || !rect) return {};
  return {.vectors = {.resource = motion.resource, .rect = *rect}, .view_format = *view_format, .scale_x = scale_x, .scale_y = scale_y};
}

NrPipeline::NrPipeline(ID3D12Device* device, nr::Session& session, nr::Timeline& timeline)
    : device_(device), session_(session), timeline_(timeline), timing_(device, timeline) {}

NrPipeline::~NrPipeline() = default;

bool NrPipeline::Initialize(std::string* error) {
  return color_.Initialize(device_.Get(), error);
}

uint64_t NrPipeline::HeldBytes() const {
  const uint64_t copies =
      (motion_copies_.textures[0]
           ? static_cast<uint64_t>(motion_copies_.slots) * motion_copies_.size.Pixels() * MOTION_BYTES_PER_PIXEL
           : 0u);
  const uint64_t launchpad = (launchpad_motion_.texture ? launchpad_motion_.size.Pixels() * MOTION_BYTES_PER_PIXEL : 0u);
  const uint64_t ring = (dlss_present_motion_.texture ? dlss_present_motion_.size.Pixels() * MOTION_BYTES_PER_PIXEL : 0u);  // Plan 18 (fix round 1)
  const uint64_t depth = (depth_copies_.textures[0] ? static_cast<uint64_t>(depth_copies_.slots) * depth_copies_.size.Pixels() * sizeof(float) : 0u);
  // Split chain: every stage set's surfaces count (design 3.5).
  uint64_t sets = 0u;
  for (const StageSet& set : sets_) {
    sets += (set.intermediates.model_a ? set.intermediates.bytes : 0u) + set.look.bytes + (set.exposure.state ? METER_STATE_BYTES : 0u) + set.faces.bytes;
  }
  uint64_t native_handle = 0u;
  for (const auto& surfaces : native_handle_surfaces_) if (surfaces) native_handle += surfaces->bytes;
  return sets + copies + depth + launchpad + ring + mask_.bytes + native_handle;
}

bool NrPipeline::SupportsUavTarget(DXGI_FORMAT format) const {
  const std::optional<color::UavFormatInfo> info = color::DescribeUavFormat(format);
  return info.has_value() && color_.SupportsTypedUavStore(info->uav_format);
}

void NrPipeline::ReleaseIntermediates() {
  RetireSetsFrom(0u);
  RetireMotionCopies();
  RetireDepthCopies();
  RetireLaunchpadMotion();
  RetireDlssPresentMotion();
  RetireMask();
}

void NrPipeline::RetireSetsFrom(uint32_t first) {
  RetireStageSets(*this, sets_, first, &NrPipeline::RetireSet, &NrPipeline::RetireLook,
                  &NrPipeline::RetireFaces, &NrPipeline::RetireExposure);
}

void NrPipeline::RetireSet(StageSet* set) {
  if (!set->intermediates.model_a) return;
  // Every recording takes its mark before its first command, so once the GPU has passed
  // the newest one the set goes at once; otherwise the callback's copy keeps it alive until then.
  if (!timeline_.IsComplete(set->intermediates.last_use)) {
    timeline_.ReleaseAfter(set->intermediates.last_use, [retired = set->intermediates] {});
  }
  set->intermediates = {};
}

bool NrPipeline::MakeMotionCopies(size_t first, size_t end, nr::Size size) {
  bool made = true;
  for (size_t index = first; index < end; ++index) {
    motion_copies_.textures[index] = CreateTexture(size, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, COMPUTE_READ,
                                                   L"Uplift DLSS motion copy");
    made = made && static_cast<bool>(motion_copies_.textures[index]);
  }
  if (!made) {
    for (size_t index = first; index < end; ++index) {
      motion_copies_.textures[index].Reset();  // never used: none was recorded into yet
    }
  }
  return made;
}

void NrPipeline::RetireMotionCopies() {
  for (size_t index = 0u; index < motion_copies_.textures.size(); ++index) {
    if (!motion_copies_.textures[index]) continue;
    const Texture retired = motion_copies_.textures[index];
    if (!timeline_.IsComplete(motion_copies_.last_write[index])) {
      timeline_.ReleaseAfter(motion_copies_.last_write[index], [retired] {});
    }
    if (!timeline_.IsComplete(motion_copies_.last_read[index])) {
      timeline_.ReleaseAfter(motion_copies_.last_read[index], [retired] {});
    }
  }
  motion_copies_ = {};
}

bool NrPipeline::MakeDepthCopies(size_t first, size_t end, nr::Size size) {
  bool made = true;
  for (size_t index = first; index < end; ++index) {
    depth_copies_.textures[index] = CreateTexture(size, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, COMPUTE_READ, L"Uplift DLSS depth copy");
    made = made && static_cast<bool>(depth_copies_.textures[index]);
  }
  if (!made) {
    for (size_t index = first; index < end; ++index) {
      depth_copies_.textures[index].Reset();
    }
  }
  return made;
}

void NrPipeline::RetireDepthCopies() {
  for (size_t index = 0u; index < depth_copies_.textures.size(); ++index) {
    if (depth_copies_.textures[index] && !timeline_.IsComplete(depth_copies_.last_use[index])) {
      timeline_.ReleaseAfter(depth_copies_.last_use[index], [retired = depth_copies_.textures[index]] {});
    }
  }
  depth_copies_ = {};
}

void NrPipeline::RetireLaunchpadMotion() { RetireHeld(launchpad_motion_); }

void NrPipeline::RetireDlssPresentMotion() { RetireHeld(dlss_present_motion_); }

void NrPipeline::RetireLook(StageSet* set) {
  if (set->look.plan == LookPlan{}) return;
  if (!timeline_.IsComplete(set->look.last_use)) {
    timeline_.ReleaseAfter(set->look.last_use, [retired = set->look] {});
  }
  set->look = {};
}

void NrPipeline::RetireFaces(StageSet* set) {
  if (!set->faces.twin) return;
  if (!timeline_.IsComplete(set->faces.last_use)) {
    timeline_.ReleaseAfter(set->faces.last_use, [retired = set->faces] {});
  }
  set->faces = {};
}

bool NrPipeline::EnsureFaces(nr::Size canvas) {
  if (set_->faces.twin && set_->faces.size == canvas) return true;
  RetireFaces(set_);
  const look::Atlas layout = look::MakeAtlas(canvas);
  constexpr D3D12_RESOURCE_FLAGS UAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  FaceSurfaces created = {
      .twin = CreateTexture(canvas, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift Keep faces twin"),
      .detail = CreateTexture(canvas, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift Keep faces detail"),
      .atlas = CreateTexture(layout.size, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift Keep faces pyramid"),
      .fill = CreateTexture(layout.size, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift Keep faces fill pyramid"),
      .layout = layout,
      .size = canvas,
      .bytes = FaceSurfaceBytes(canvas),
  };
  if (!created.twin || !created.detail || !created.atlas || !created.fill) {
    if (!allocation_failure_logged_) {
      nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} Keep faces surfaces", canvas.width, canvas.height);
      allocation_failure_logged_ = true;
    }
    return false;  // `created` was never used by the GPU: freed here
  }
  allocation_failure_logged_ = false;
  set_->faces = std::move(created);
  return true;
}

void NrPipeline::RecordFaces(ID3D12GraphicsCommandList* list, color::FacesPass pass) {
  ID3D12Resource* const atlas = set_->faces.atlas.Get();
  pass.atlas = atlas;
  pass.detail = set_->faces.detail.Get();
  pass.fill = set_->faces.fill.Get();
  pass.layout = set_->faces.layout;
  pass.parameters = look::MakeFacesParameters(config_.keep_faces.lighting_scale, config_.keep_faces.tuning, set_->intermediates.plan.sizes.image.height, set_->faces.layout.levels);
  set_->faces.last_use = slot_marks_[resolve_slot_];
  const bool fill = (pass.mask == nullptr && pass.parameters.fill);  // pass 1's pyramid writes the fill's too
  Transition(list, atlas, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (fill) {
    Transition(list, pass.fill, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  color_.RecordFacesPyramid(list, resolve_slot_, pass);
  Transition(list, atlas, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (fill) {
    Transition(list, pass.fill, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  if (pass.parameters.speck_radius != 0u) {
    // The difference despiked before the combine rewrites `changed` (and pass 1's twin) in place.
    Transition(list, pass.detail, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordFacesDespike(list, resolve_slot_, pass);
    Transition(list, pass.detail, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  Transition(list, pass.changed, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (pass.mask == nullptr) {
    Transition(list, pass.reference, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);  // pass 1: the twin's output becomes the face mask
  }
  color_.RecordFacesCombine(list, resolve_slot_, pass);
  Transition(list, pass.changed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (pass.mask == nullptr) {
    Transition(list, pass.reference, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
}

void NrPipeline::CombineFaces(ID3D12GraphicsCommandList* list, ID3D12Resource* lighting, ID3D12Resource* faces) {
  RecordFaces(list, {.changed = lighting, .reference = faces});
  faces_ran_ = true;
}

void NrPipeline::RetireMask() { RetireHeld(mask_); }

void NrPipeline::RetireExposure(StageSet* set) {
  if (set->exposure.state && !timeline_.IsComplete(set->exposure.last_use)) {
    timeline_.ReleaseAfter(set->exposure.last_use, [retired = set->exposure.state] {});
  }
  set->exposure = {};
  RetireExposureCheck(set);
}

void NrPipeline::RetireExposureCheck(StageSet* set) {
  // The samples still in flight are dropped (Auto's run starts again with the next ones); the buffer goes once the GPU has passed its copies.
  for (uint32_t index = 0u; index < CHECK_SLOTS; ++index) {
    if (set->check.readback && set->check.pending[index] && !timeline_.IsComplete(set->check.marks[index])) {
      timeline_.ReleaseAfter(set->check.marks[index], [retired = set->check.readback] {});
    }
  }
  set->check = {};
}

NrPipeline::Texture NrPipeline::CreateTexture(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                              D3D12_RESOURCE_STATES state, const wchar_t* name) {
  const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC description = {
      .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Alignment = 0u,
      .Width = size.width,
      .Height = size.height,
      .DepthOrArraySize = 1u,
      .MipLevels = 1u,
      .Format = format,
      .SampleDesc = {.Count = 1u, .Quality = 0u},
      .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
      .Flags = flags,
  };
  Texture texture;
  if (SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                                                 IID_PPV_ARGS(&texture)))) {
    texture->SetName(name);
  }
  return texture;
}

nr::SkipReason NrPipeline::EnsureIntermediates(const SetPlan& plan) {
  if (set_->intermediates.model_a) {
    // The foveated grid can move under surfaces that still fit (a shorter fall-off, a denser edge, a smaller centre): NR's
    // history and the stabiliser's then describe the old mapping of the same pixels, so both start afresh. The field is
    // copied first, so the plan comparison below still sees a set that needs no reallocation for a moved grid alone.
    if (set_->intermediates.plan.foveation != plan.foveation) {
      set_->intermediates.plan.foveation = plan.foveation;
      set_->look.change_valid = false;
      set_->intermediates.reset_pending = true;
      set_->look.history_valid = false;
    }
    if (set_->intermediates.plan == plan) return nr::SkipReason::NONE;
  }
  constexpr D3D12_RESOURCE_FLAGS UAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const Intermediates& held = set_->intermediates;
  const bool same_canvas = held.model_a && held.plan.sizes.canvas == plan.sizes.canvas;
  const bool same_copy = held.plan.sizes.output == plan.sizes.output && held.plan.copy_format == plan.copy_format
                         && held.plan.kind == plan.kind;
  const bool private_color = !plan.sizes.private_color.Empty();
  Intermediates created = {
      .source_copy = (plan.copy_format == DXGI_FORMAT_UNKNOWN
                          ? Texture()
                          : same_copy && held.source_copy ? held.source_copy
                          : CreateTexture(plan.sizes.output, plan.copy_format, D3D12_RESOURCE_FLAG_NONE,
                                          (plan.kind == TargetKind::PRESENT ? PIXEL_AND_COMPUTE_READ : COMPUTE_READ),
                                          L"Uplift source copy")),
      .model_a = same_canvas ? held.model_a : CreateTexture(plan.sizes.canvas, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift model A"),
      .model_b = same_canvas ? held.model_b : CreateTexture(plan.sizes.canvas, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift model B"),
      .zero_motion = same_canvas ? held.zero_motion : Texture(),
      .change = held.plan.sizes.image == plan.sizes.image ? held.change : Texture(),
      .canvas_motion = (plan.canvas_motion
                            ? same_canvas && held.canvas_motion ? held.canvas_motion
                            : CreateTexture(plan.sizes.canvas, DXGI_FORMAT_R16G16_FLOAT, UAV, COMPUTE_READ, L"Uplift canvas motion")
                            : Texture()),
      .private_color = (private_color ? held.plan.sizes.private_color == plan.sizes.private_color && held.private_color ? held.private_color
                               : CreateTexture(plan.sizes.private_color, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ,
                                               L"Uplift pre-SR colour")
                               : Texture()),
      .plan = plan,
      .last_use = held.last_use,
  };
  for (size_t i = 0u; i < created.native_inputs.size(); ++i) {
    const auto format = plan.native_input_formats[i];
    if (format == DXGI_FORMAT_UNKNOWN) continue;
    created.native_inputs[i] = same_canvas && held.plan.native_input_formats[i] == format && held.native_inputs[i]
        ? held.native_inputs[i] : CreateTexture(plan.sizes.canvas, format, UAV, COMPUTE_READ, L"Uplift native NR auxiliary input");
  }
  const bool complete = created.model_a && created.model_b
                        && (plan.copy_format == DXGI_FORMAT_UNKNOWN || created.source_copy)
                        && (!plan.canvas_motion || created.canvas_motion) && (!private_color || created.private_color);
  if (!complete) {
    if (!allocation_failure_logged_) {
      nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} NR surfaces", plan.sizes.canvas.width, plan.sizes.canvas.height);
      allocation_failure_logged_ = true;
    }
    return nr::SkipReason::OUT_OF_MEMORY;
  }
  allocation_failure_logged_ = false;
  created.bytes = plan.sizes.output.Pixels() * plan.source_bytes_per_pixel
                  + plan.sizes.canvas.Pixels() * (2u * MODEL_BYTES_PER_PIXEL)
                  + ((created.zero_motion ? 1u : 0u) + (created.canvas_motion ? 1u : 0u)) * plan.sizes.canvas.Pixels() * MOTION_BYTES_PER_PIXEL
                  + (created.change ? plan.sizes.image.Pixels() * MODEL_BYTES_PER_PIXEL : 0u)
                  + plan.sizes.private_color.Pixels() * MODEL_BYTES_PER_PIXEL;
  for (size_t i = 0u; i < created.native_inputs.size(); ++i) {
    if (created.native_inputs[i]) created.bytes += plan.sizes.canvas.Pixels() * color::DescribeMaskFormat(plan.native_input_formats[i])->bytes_per_pixel;
  }
  // Commit only after every new surface exists. Reuse model/motion/history storage independently of
  // source-copy and private-output dimensions, but keep the old mapping's reset semantics.
  if (held.model_a && !timeline_.IsComplete(held.last_use)) {
    timeline_.ReleaseAfter(held.last_use, [retired = held] {});
  }
  set_->look.history_valid = false;
  set_->look.change_valid = false;
  set_->intermediates = std::move(created);
  return nr::SkipReason::NONE;
}

bool NrPipeline::EnsureChange(bool required) {
  auto& set = set_->intermediates;
  const uint64_t bytes = set.plan.sizes.image.Pixels() * MODEL_BYTES_PER_PIXEL;
  if (!required) {
    if (set.change) {
      if (!timeline_.IsComplete(set.last_use)) timeline_.ReleaseAfter(set.last_use, [retired = set.change] {});
      set.change.Reset();
      set.bytes -= bytes;
    }
    return true;
  }
  if (set.change) return true;
  set.change = CreateTexture(set.plan.sizes.image, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, COMPUTE_READ, L"Uplift change field");
  if (!set.change) return false;
  set.bytes += bytes;
  return true;
}

void NrPipeline::SetLookConfig(const LookConfig& config) {
  ApplyLookConfig(config_, config, sets_);
}

LookPlan NrPipeline::LookPlanFor(nr::Size image) const {
  return PlanLook(config_, image, color_.SupportsUavLoads());
}

bool NrPipeline::EnsureLook(const LookPlan& plan) {
  const look::Atlas atlas = plan.shape ? look::MakeAtlas(plan.image) : look::Atlas{};
  const nr::Size history_size = plan.stabilize ? look::LowHistorySize(atlas, config_.look.detail_radius) : nr::Size{};
  if (plan == set_->look.plan && history_size == set_->look.history_size) return true;
  RetireLook(set_);
  if (plan == LookPlan{}) return true;
  LookSurfaces created = {.plan = plan, .history_size = history_size, .atlas = atlas};
  const auto make = [this, &created](bool wanted, nr::Size size, DXGI_FORMAT format, uint32_t bytes_per_pixel, const wchar_t* name) {
    if (!wanted) return Texture();
    created.bytes += size.Pixels() * bytes_per_pixel;
    return CreateTexture(size, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, COMPUTE_READ, name);
  };
  created.basis = make(plan.shape, plan.image, DXGI_FORMAT_R16G16_FLOAT, 4u, L"Uplift look basis");
  created.gauss = make(plan.shape, created.atlas.size, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u, L"Uplift look pyramid");
  created.peaks = make(plan.shape, created.atlas.size, DXGI_FORMAT_R16_FLOAT, 2u, L"Uplift look peaks");
  for (Texture& history : created.history) {
    history = make(plan.stabilize, history_size, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u, L"Uplift look history");
  }
  for (Texture& history : created.detail_history) {
    history = make(plan.detail, plan.image, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u, L"Uplift look detail history");
  }
  created.state = make(plan.stabilize, LOOK_STATE_SIZE, DXGI_FORMAT_R32G32B32A32_FLOAT, LOOK_STATE_BYTES_PER_PIXEL, L"Uplift look state");
  for (Texture& history : created.change_history) {
    history = make(plan.change, plan.image, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u, L"Uplift change history");
  }
  const bool complete = (!plan.shape || (created.basis && created.gauss && created.peaks))
                        && (!plan.stabilize || (created.history[0] && created.history[1] && created.state))
                        && (!plan.detail || (created.detail_history[0] && created.detail_history[1]))
                        && (!plan.change || (created.change_history[0] && created.change_history[1]));
  if (!complete) {
    if (!allocation_failure_logged_) {
      nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} look surfaces", plan.image.width, plan.image.height);
      allocation_failure_logged_ = true;
    }
    return false;  // `created` was never used by the GPU: freed here
  }
  allocation_failure_logged_ = false;
  set_->look = std::move(created);
  return true;
}

void NrPipeline::NoteMeterBasis(uint8_t stage, color::Encoding encoding, float input_scale) {
  if (UpdateMeterBasis(set_->exposure.basis, set_->exposure.snap, stage, encoding, config_.fixes.primaries, input_scale)) {
    RetireExposureCheck(set_);
  }
}

NrPipeline::ExposureChoice NrPipeline::ChooseExposure(color::Encoding encoding, ID3D12Resource* game_texture, float game_factor) {
  PollExposureChecks();
  const bool game_exposure = (game_texture != nullptr || game_factor != 1.f);
  // Only relative scene-linear images are metered, and the meter needs typed UAV loads.
  const bool can_meter = (color::Meterable(encoding) && color_.SupportsUavLoads());
  const ExposureDecision decision = DecideExposure(config_.fixes, game_exposure, can_meter, set_->auto_exposure.Latched());
  if (!decision.WantsState() && set_->exposure.state) {
    RetireExposure(set_);  // InputExposure left Metered/auto-metered; the state is no longer read
  }
  if (decision.WantsState() && !set_->exposure.state) {
    set_->exposure.state = CreateTexture(STATE_SIZE, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, PIXEL_AND_COMPUTE_READ,
                                    L"Uplift exposure state");
    set_->exposure.snap = true;  // undefined until the meter's first snap
  }
  const ExposureOutcome outcome = PickExposure(decision, config_.fixes, game_exposure, static_cast<bool>(set_->exposure.state), set_->auto_exposure);
  set_->exposure_report = outcome.report;
  ExposureChoice choice = {.metered = outcome.metered, .probe = outcome.probe, .game_weight = outcome.game_weight};
  switch (outcome.source) {
    case ExposureSource::STATE: choice.texture = set_->exposure.state.Get(); break;
    case ExposureSource::GAME:
      choice.texture = game_texture;
      choice.factor = game_factor;
      break;
    case ExposureSource::NONE: break;
  }
  if (outcome.with_game) {
    choice.game_texture = game_texture;
    choice.game_factor = game_factor;
  }
  return choice;
}

void NrPipeline::PollExposureChecks() {
  if (!set_->check.readback) return;
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  for (uint32_t step = 0u; step < CHECK_SLOTS; ++step) {
    const uint32_t index = (set_->check.next + step) % CHECK_SLOTS;  // the oldest first
    if (!set_->check.pending[index] || !timeline_.IsComplete(set_->check.marks[index])) continue;
    set_->check.pending[index] = false;
    std::array<float, METER_SAMPLE_FLOATS> texels = {};
    const D3D12_RANGE read = {.Begin = index * CHECK_STRIDE, .End = index * CHECK_STRIDE + sizeof(texels)};
    void* mapped = nullptr;
    if (FAILED(set_->check.readback->Map(0u, &read, &mapped))) continue;
    std::memcpy(texels.data(), static_cast<const std::byte*>(mapped) + read.Begin, sizeof(texels));
    const D3D12_RANGE written = {.Begin = 0u, .End = 0u};
    set_->check.readback->Unmap(0u, &written);
    const ExposureSample sample = ExposureSampleFromTexels(texels);
    if (set_->auto_exposure.Observe(sample, seconds)) {
      nr::Log(nr::LogLevel::INFO, AutoLatchMessage(sample, set_->auto_exposure));
    } else if (!set_->auto_exposure.Latched() && std::isfinite(AutoExposure::OutsideSpan(sample)) && set_->auto_exposure.DebugDue(seconds)) {
      nr::Log(nr::LogLevel::TRACE, AutoDebugMessage(sample, config_.fixes.auto_mode, config_.fixes.auto_blend, set_->auto_exposure.HeldThroughMove()));  // once every AutoExposure::DEBUG_SECONDS
    }
  }
}

void NrPipeline::RecordMeter(ID3D12GraphicsCommandList* list, uint32_t slot, color::MeterPass pass, const ExposureChoice& exposure) {
  ID3D12Resource* const state = set_->exposure.state.Get();
  pass.state = state;
  pass.snap = set_->exposure.snap;
  pass.smooth = config_.fixes.smooth_adapt;
  pass.brighter_rate = config_.fixes.adapt_brighter;
  pass.darker_rate = config_.fixes.adapt_darker;
  pass.frame_seconds = config_.frame_seconds;
  pass.probe = exposure.probe;
  pass.game_exposure = (exposure.probe ? exposure.game_texture : nullptr);
  pass.game_exposure_factor = (exposure.probe ? exposure.game_factor : 1.f);
  pass.game_weight = (exposure.probe ? exposure.game_weight : 0.f);
  Transition(list, state, PIXEL_AND_COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  // Never a failed dispatch here: ColorPipeline::RecordMeter fails only for a slot outside the ring, which next_slot_ never is, and its one meter pipeline
  // exists once Initialize succeeded. Vulkan's meter pipelines are per source format and may be missing, so only there does a Metered recording abort.
  color_.RecordMeter(list, slot, pass);
  Transition(list, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, PIXEL_AND_COMPUTE_READ);
  set_->exposure.snap = false;
  set_->exposure.last_use = slot_marks_[slot];
  if (!exposure.probe) return;
  // Auto's check. The state's two texels into the next readback slot, unless the GPU has not passed that slot's last copy yet (skipped).
  const uint32_t index = set_->check.next;
  if (set_->check.pending[index]) return;
  if (!set_->check.readback) {
    const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC description = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0u,
        .Width = CHECK_SLOTS * CHECK_STRIDE,
        .Height = 1u,
        .DepthOrArraySize = 1u,
        .MipLevels = 1u,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {.Count = 1u, .Quality = 0u},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&set_->check.readback)))) {
      return;
    }
    set_->check.readback->SetName(L"Uplift exposure check readback");
  }
  D3D12_TEXTURE_COPY_LOCATION destination = {.pResource = set_->check.readback.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  destination.PlacedFootprint = {
      .Offset = index * CHECK_STRIDE,
      .Footprint = {.Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .Width = STATE_SIZE.width, .Height = STATE_SIZE.height, .Depth = 1u,
                    .RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT},
  };
  const D3D12_TEXTURE_COPY_LOCATION origin = {.pResource = state, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0u};
  Transition(list, state, PIXEL_AND_COMPUTE_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTextureRegion(&destination, 0u, 0u, 0u, &origin, nullptr);
  Transition(list, state, D3D12_RESOURCE_STATE_COPY_SOURCE, PIXEL_AND_COMPUTE_READ);
  set_->check.marks[index] = slot_marks_[slot];
  set_->check.pending[index] = true;
  set_->check.next = (index + 1u) % CHECK_SLOTS;
}

NrPipeline::MaskBinding NrPipeline::BoundMask(uint32_t slot, ID3D12Resource* shared) {
  if (!config_.mask) return {};
  if (shared != nullptr) {
    const auto format = color::DescribeMaskFormat(shared->GetDesc().Format);
    if (format) return {shared, format->view_format};
  }
  if (!mask_.copied) return {};
  mask_.last_use = slot_marks_[slot];
  return {mask_.texture.Get(), mask_.view_format};
}

ID3D12Resource* NrPipeline::PrepareMaskCopy(const D3D12_RESOURCE_DESC* mask) {
  const std::optional<color::MaskFormatInfo> format = (mask != nullptr ? color::DescribeMaskFormat(mask->Format) : std::nullopt);
  if (!format || mask->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || mask->SampleDesc.Count != 1u) {
    RetireMask();
    return nullptr;
  }
  const nr::Size size = {static_cast<uint32_t>(mask->Width), mask->Height};
  if (!mask_.texture || mask_.size != size || mask_.format != mask->Format) {
    RetireMask();
    Texture texture = CreateTexture(size, mask->Format, D3D12_RESOURCE_FLAG_NONE, PIXEL_AND_COMPUTE_READ, L"Uplift mask copy");
    if (!texture) return nullptr;
    mask_ = {.texture = std::move(texture), .size = size, .format = mask->Format, .view_format = format->view_format,
             .bytes = size.Pixels() * format->bytes_per_pixel};
  }
  mask_.last_use = timeline_.MarkNow();  // the add-on copies into it on this frame's present-queue list
  return mask_.texture.Get();
}

void NrPipeline::ResolvePass(ID3D12GraphicsCommandList* list, uint32_t index, ID3D12Resource* given, ID3D12Resource* returned) {
  if (index == 0u || index > LATER_PASSES || !color_.SupportsUavLoads()) return;
  const bool swapped = (given != set_->intermediates.model_a.Get());
  if (faces_ran_ && index != faces_first_) {
    // Keep faces: inside pass 1's face mask only this pass's broad change is kept. Split chain: a later group's own mask,
    // from its first pass's twin, whose recombination already made that pass's result.
    RecordFaces(list, {.changed = returned, .reference = given, .mask = set_->faces.twin.Get(), .swapped = swapped});
  }
}

NrPipeline::AfterNr NrPipeline::RecordAfterNr(ID3D12GraphicsCommandList* list, uint32_t slot, const color::EncodePass& encode,
                                              ID3D12Resource* nr_output, uint32_t passes_run, bool reduced, bool look_ready,
                                              color::Upsampling upsampling, const color::StabilizeMotion& motion, bool reset,
                                              std::optional<ChangeStabilizeInput> change_input) {
  if (set_->look.plan != LookPlan{}) {
    set_->look.last_use = slot_marks_[slot];
  }
  ID3D12Resource* const change = set_->intermediates.change.Get();
  if (change == nullptr) {
    return {.texture = nr_output, .fused = true,
            .model_input = passes_run == 1u ? set_->intermediates.model_a.Get() : nullptr};
  }
  const bool shape = (look_ready && set_->look.plan.shape);
  ID3D12Resource* const basis = (shape ? set_->look.basis.Get() : nullptr);
  // 1. NR's change field and, for the look, NR's input chroma κ_M.
  Transition(list, change, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (basis != nullptr) {
    Transition(list, basis, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  // A still holds the original FP16 input after one pass; later passes overwrite it through ping-pong.
  const bool changed = color_.RecordChange(list, slot, {.encode = encode, .nr_output = nr_output, .change = change, .basis = basis,
                                                       .model_input = (passes_run == 1u ? set_->intermediates.model_a.Get() : nullptr)});
  Transition(list, change, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (basis != nullptr) {
    Transition(list, basis, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  if (!changed) return {.ok = false};
  AfterNr after = {.texture = change, .upsampling = (reduced ? std::optional{upsampling} : std::nullopt)};
  if (shape) {
    // 2. The pyramids.
    const look::Atlas& atlas = set_->look.atlas;
    const look::Bands bands = look::MakeBands(config_.look.detail_radius, atlas.image.height, atlas.levels);
    ID3D12Resource* const gauss = set_->look.gauss.Get();
    ID3D12Resource* const peaks = set_->look.peaks.Get();
    Transition(list, gauss, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, peaks, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordPyramid(list, slot, {.change = change, .gauss = gauss, .peaks = peaks, .atlas = atlas});
    Transition(list, gauss, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    Transition(list, peaks, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    // 3. The stabiliser: the two levels the low band reads, then the fine band.
    ID3D12Resource* history = nullptr;
    ID3D12Resource* detail = nullptr;
    if (set_->look.plan.stabilize) {
      const uint32_t current = set_->look.current;
      ID3D12Resource* const state = set_->look.state.Get();
      history = set_->look.history[current].Get();
      color::StabilizePass stabilize = {
          .gauss = gauss, .previous = set_->look.history[1u - current].Get(), .current = history, .state = state, .atlas = atlas,
          .bands = bands, .rate = look::StabilizeRate(config_.frame_seconds, config_.look.stabilize_ms),
          .motion_mode = (config_.look.stabilize == look::StabilizeMode::MOTION), .reset = (reset || !set_->look.history_valid),
          .motion = motion, .packed_history = true,
      };
      Transition(list, history, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Transition(list, state, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      color_.RecordStabilize(list, slot, stabilize);
      Transition(list, history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
      if (set_->look.plan.detail) {
        detail = set_->look.detail_history[current].Get();
        stabilize.change = change;
        stabilize.previous = set_->look.detail_history[1u - current].Get();
        stabilize.current = detail;
        Transition(list, detail, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        color_.RecordDetail(list, slot, stabilize);
        Transition(list, detail, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
      }
      Transition(list, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
      set_->look.current = 1u - current;
      set_->look.history_valid = true;
    }
    // 4. The shape pass rewrites C in place.
    Transition(list, change, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordShape(list, slot,
                       {.change = change, .basis = basis, .gauss = gauss, .peaks = peaks, .history = history, .detail = detail,
                        .atlas = atlas, .bands = bands, .settings = config_.look.shape, .sdr = color::IsSdr(encode.encoding), .packed_history = true});
    Transition(list, change, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  // 5. The change field's own stabiliser: the same field, after the shape, averaged along the vectors so NR's
  //    residual stops shimmering where the network is unsure. It reads and writes the field the decode consumes,
  //    so the result is simply the other buffer of the pair, and the decode sees a field that has been through the
  //    same history as the picture it describes.
  if (look_ready && set_->look.plan.change) {
    const uint32_t current = set_->look.change_current;
    ID3D12Resource* const stable = set_->look.change_history[current].Get();
    const ChangeStabilizeInput history_input = change_input.value_or(ChangeStabilizeInput{.reset = reset});
    const bool reset_history = history_input.reset || !set_->look.change_valid;
    Transition(list, stable, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool recorded = color_.RecordChangeStabilize(list, slot,
        {.change = change, .previous = set_->look.change_history[1u - current].Get(), .current = stable,
         .image = set_->look.plan.image,
         .rate = look::StabilizeRate(config_.frame_seconds, static_cast<float>(config_.look.stabilize_change_ms)),
         .reset = reset_history, .motion = history_input.reproject ? motion : color::StabilizeMotion{},
         .flat_denoise = change_input.has_value()});
    Transition(list, stable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    set_->look.change_valid = recorded;
    if (recorded) {
      set_->look.change_current = 1u - current;
      after.texture = stable;
    }
  }
  return after;
}

nr::EvaluateResult NrPipeline::Evaluate(ID3D12GraphicsCommandList* list, uint32_t slot, nr::FrameInputs inputs,
                                        const nr::Controls& controls, const nr::StageSlice& slice,
                                        NrTimingSlot* sample) {
  const bool had_motion = (inputs.motion.resource != nullptr);
  if (!had_motion) {
    inputs.motion = {.resource = set_->intermediates.zero_motion.Get()};
    inputs.motion_scale_x = 1.f;
    inputs.motion_scale_y = 1.f;
  }
  inputs.reset_hint = (inputs.reset_hint || set_->intermediates.reset_pending);
  // Passes 2..10's own controls.
  inputs.later_passes = config_.later_controls;
  inputs.resolver = this;
  resolve_slot_ = slot;
  // Keep faces: pass 1's twin, with its surfaces made before the fit like the set's. Off, unsupported, or paused by the Session,
  // they go; a frame whose surfaces cannot be made only goes without the twin.
  // Split chain: the slice's first pass's twin, at its stage, when the device context's config protects this group.
  faces_ran_ = false;
  faces_first_ = slice.first;
  if (config_.keep_faces.enabled && KeepFacesSupported()) {
    inputs.faces = {.wanted = true, .controls = FaceTwinControls(controls, config_.keep_faces.protection),
                    .surface_bytes = FaceSurfaceBytes(set_->intermediates.plan.sizes.canvas)};
    if (session_.FacesPaused(slice.stage)) {
      RetireFaces(set_);
    } else if (EnsureFaces(set_->intermediates.plan.sizes.canvas)) {
      inputs.faces.output = set_->faces.twin.Get();
      set_->faces.last_use = slot_marks_[slot];  // the Session may evaluate the twin into it even when no recombination follows
    }
  } else {
    RetireFaces(set_);
  }
  const uint64_t held = HeldBytes();  // allocated before the fit: charged as the chain's surfaces and credited as held
  const nr::EvaluateResult evaluated = session_.Evaluate(
      list,
      {.a = set_->intermediates.model_a.Get(), .b = set_->intermediates.model_b.Get(), .size = set_->intermediates.plan.sizes.canvas,
       .surface_bytes = held, .held_intermediate_bytes = held},
      inputs, controls, slice);
  timing_.End(sample, list, evaluated.output ? evaluated.passes_run : 0u);
  if (evaluated.output != nullptr) {
    set_->intermediates.reset_pending = false;  // only a frame that ran NR pays off the owed reset
  }
  if (evaluated.output != nullptr && faces_ran_ && config_.keep_faces.show_mask) {
    // Show the face mask: the mask tinted into NR's result, which the change field and the decode then carry to the image.
    Transition(list, evaluated.output, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordFacesShow(list, slot, evaluated.output, set_->faces.twin.Get(), set_->intermediates.plan.sizes.canvas);
    Transition(list, evaluated.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  return evaluated;
}

void NrPipeline::NoteEncoding(color::Encoding encoding, float input_scale) {
  const color::Primaries primaries = color::ResolvePrimaries(config_.fixes.primaries, encoding);
  // Model and Look histories belong to one colour basis, independently in each stage set.
  if (encoding != set_->intermediates.encoding || primaries != set_->intermediates.primaries || input_scale != set_->intermediates.input_scale) {
    set_->intermediates.encoding = encoding;
    set_->intermediates.primaries = primaries;
    set_->intermediates.input_scale = input_scale;
    set_->intermediates.reset_pending = true;
  }
}

void NrPipeline::CommitSlot(uint32_t slot, bool replayable) {
  // The GPU reads this slot's descriptors, and the intermediates, until this recording completes: the
  // current frame, and the token an After-DLSS caller issued before recording.
  slot_marks_[slot] = timeline_.MarkNow(replayable);
  set_->intermediates.last_use = slot_marks_[slot];
  next_slot_ = (slot + 1u) % color::ColorPipeline::RING_SLOTS;
}

bool NrPipeline::Encode(ID3D12GraphicsCommandList* list, uint32_t slot, color::EncodePass pass, bool needs_zero_motion) {
  Intermediates& intermediates = set_->intermediates;
  if (needs_zero_motion && !intermediates.zero_motion) {
    const nr::Size size = intermediates.plan.sizes.canvas;
    intermediates.zero_motion = CreateTexture(size, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                               COMPUTE_READ, L"Uplift zero motion");
    if (!intermediates.zero_motion) return false;
    intermediates.bytes += size.Pixels() * MOTION_BYTES_PER_PIXEL;
  }
  ID3D12Resource* const model_a = intermediates.model_a.Get();
  // Clear in the same encode dispatch whenever needed. A CPU 'already cleared' bit could refer
  // to a command list the game reset without executing, or to a different GPU queue.
  ID3D12Resource* const zero_motion = needs_zero_motion ? intermediates.zero_motion.Get() : nullptr;
  Transition(list, model_a, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (zero_motion) Transition(list, zero_motion, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  pass.model = model_a;
  pass.motion = zero_motion;
  const bool encoded = color_.RecordEncode(list, slot, pass);
  Transition(list, model_a, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (zero_motion) Transition(list, zero_motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  return encoded;
}

PipelineResult NrPipeline::RecordPresent(ID3D12GraphicsCommandList* list, const PresentTarget& target,
                                         const nr::Controls& controls, bool reset_hint, const WorkLayout& layout, const ChainPart& part,
                                         ID3D12CommandQueue* timing_queue) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = nr::SkipReason::INACTIVE};
  if (list == nullptr || target.resource == nullptr || part.set >= STAGE_SETS) return {.reason = nr::SkipReason::INVALID_INPUT};
  set_ = &sets_[part.set];
  if (!session_.SliceRuns(part.slice)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::BUDGET};
  }
  if (session_.SliceRuns(part.slice) && !config_.keep_faces.show_mask && !session_.SliceHasIntensity(part.slice, controls, config_.later_controls)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::INTENSITY_ZERO};
  }
  const D3D12_RESOURCE_DESC description = target.resource->GetDesc();
  const std::optional<color::FormatInfo> format = color::DescribeFormat(description.Format);
  if (!format) return {.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
  // Never run NR on a frame whose result could not be written back.
  if (!color_.HasDecodePipeline(format->target_view_format)) return {.reason = nr::SkipReason::NO_TARGET_PIPELINE};
  const uint64_t frame = timeline_.CurrentFrame();
  if (frame == last_present_frame_) return {.reason = nr::SkipReason::ALREADY_RAN};
  const uint32_t slot = timeline_.FindReusableSlot(slot_marks_, next_slot_);
  if (slot == slot_marks_.size()) return {.reason = nr::SkipReason::DESCRIPTORS_BUSY};
  const nr::Size size = {static_cast<uint32_t>(description.Width), description.Height};
  if (!nr::MeetsNrFloor(size)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::FRAME_TOO_SMALL};
  }
  const WorkLayout work = ResolveLayout(layout, size);
  const bool foveated = work.foveation.active();
  if (const nr::SkipReason problem = EnsureIntermediates({
          .sizes = {.output = size, .image = work.image, .canvas = work.image},
          .copy_format = format->copy_format,
          .source_bytes_per_pixel = format->bytes_per_pixel,
          .kind = TargetKind::PRESENT,
          .canvas_motion = foveated,
          .foveation = work.foveation,
      });
      problem != nr::SkipReason::NONE) {
    return {.reason = problem};
  }
  last_present_frame_ = frame;
  const bool reduced = (work.image != size);
  const LookPlan look_plan = LookPlanFor(work.image);
  if (!EnsureChange(reduced || foveated || look_plan.shape || look_plan.change)) return {.reason = nr::SkipReason::OUT_OF_MEMORY};
  const bool look_ready = EnsureLook(look_plan);
  CommitSlot(slot, false);
  NrGpuTiming::Recording profile(timing_, list, part.slice.stage, timing_queue, nullptr);
  const float input_scale = color::InputScale(target.encoding, target.diffuse_white_nits, config_.fixes.linear_unit_nits);
  NoteEncoding(target.encoding, input_scale);
  const uint32_t options = ShaderOptions(config_.fixes);
  // The Present path never has the game's own exposure.
  NoteMeterBasis(part.slice.stage, target.encoding, input_scale);
  const ExposureChoice exposure = ChooseExposure(target.encoding, nullptr, 1.f);
  ID3D12Resource* const source_copy = set_->intermediates.source_copy.Get();
  // The copy of the game's motion vectors made for this very frame, when
  // there is one, from the slot the frame it was made for selects (PresentMotionSlotOf).
  nr::FrameInputs inputs = {.ui_correction = config_.present_ui_correction, .reset_hint = reset_hint};
  MotionSource motion_source = MotionSource::NONE;
  bool flip_y = false;
  uint64_t gpu_waits = 0u;
  uint64_t gpu_wait_failures = 0u;
  std::array<uint64_t, MOTION_COPY_SLOTS> copy_frames = {};
  std::array<nr::Timeline::SubmissionStatus, MOTION_COPY_SLOTS> copy_status = {};
  for (size_t index = 0u; index < MOTION_COPY_SLOTS; ++index) {
    copy_status[index] = timeline_.InspectSubmission(motion_copies_.producers[index].get(), timing_queue);
    // Foveated warps cannot use a prior frame's vectors. If this is the exact copy for this Present and its
    // producer fence has been signalled, order the consumer queue after it instead of dropping current-frame MV.
    if (!copy_status[index].ordered && motion_copies_.frames[index] == frame) {
      copy_status[index] = timeline_.PrepareSubmissionRead(motion_copies_.producers[index].get(), timing_queue);
      gpu_waits += (copy_status[index].gpu_wait_enqueued ? 1u : 0u);
      gpu_wait_failures += (copy_status[index].gpu_wait_failed ? 1u : 0u);
    }
    if (copy_status[index].ordered) {
      copy_frames[index] = motion_copies_.frames[index];
    }
  }
  // Foveation requires current-frame vectors; the uniform grid can reuse a recent copy.
  const PresentMotionPick pick = PickPresentMotion(copy_frames, frame, foveated ? 0u : PRESENT_MOTION_MAX_AGE);
  const bool bridged_motion = target.dlss_motion.resource != nullptr && (!foveated || target.dlss_motion_age == 0u);
  if (pick.slot) {
    const size_t copy = *pick.slot;
    inputs.motion = {.resource = motion_copies_.textures[copy].Get()};  // full subrect, scale (1, 1)
    motion_copies_.last_read[copy] = slot_marks_[slot];
    motion_source = MotionSource::PRESENT_COPY;
  } else if (bridged_motion && target.dlss_motion_raw) {
    // Fold the raw D3D11 scale and flip into the warp rather than copying twice.
    if (foveated) {
      inputs.motion = target.dlss_motion;
      inputs.motion_scale_x = target.dlss_motion_scale_x;
      inputs.motion_scale_y = target.dlss_motion_scale_y;
      flip_y = target.dlss_motion_flip;
      motion_source = MotionSource::PRESENT_COPY;
    } else if (const nr::BoundResource converted =
            ConvertDlssMotion(list, slot, target.dlss_motion, target.dlss_motion_scale_x, target.dlss_motion_scale_y, target.dlss_motion_flip);
        converted.resource != nullptr) {
      inputs.motion = converted;  // full subrect, scale (1, 1)
      motion_source = MotionSource::PRESENT_COPY;
    }
  } else if (bridged_motion) {
    // The Vulkan bridge's copy of DLSS's vectors, in the motion region's own pixels (the scale was applied by the copy): the full subrect and a
    // scale of (1, 1), as the copy of the Direct3D 12 path above.
    inputs.motion = target.dlss_motion;
    motion_source = MotionSource::PRESENT_COPY;
  } else if (target.launchpad_motion.resource != nullptr) {
    // LaunchPad's motion, written by Uplift.fx this frame, in back-buffer pixels. Never bound as it is. The
    // motion copy resamples it to the work image in its pixels, drops non-finite vectors and clamps the rest (motion_cs.hlsl), as the DLSS copy does,
    // and NR binds the result whole at a scale of (1, 1), right at every Resolution.
    if (const nr::BoundResource converted =
            ConvertLaunchpadMotion(list, slot, target.launchpad_motion, target.motion_scale_x, target.motion_scale_y, work);
        converted.resource != nullptr) {
      inputs.motion = converted;
      motion_source = MotionSource::LAUNCHPAD;
    }
  }
  // Source depth cannot describe warped colour.
  if (!foveated && depth_copies_.textures[0]) {
    // The depth has to describe the frame whose vectors NR reprojects with, so the copy recorded by the same submission as the motion copy bound above
    // is the exact pair and is tried first. When that pair is not ready this frame, the newest ordered copy within the same age still beats no depth:
    // DLSSNR.Depth is an aid to NR's own estimate, and one late copy must not drop it until the next recording.
    std::array<uint64_t, MOTION_COPY_SLOTS> paired_frames = {};
    std::array<uint64_t, MOTION_COPY_SLOTS> ordered_frames = {};
    for (size_t index = 0u; index < MOTION_COPY_SLOTS; ++index) {
      if (!timeline_.OrderedBefore(depth_copies_.producers[index].get(), timing_queue)) continue;
      ordered_frames[index] = depth_copies_.frames[index];
      if (!pick.slot || depth_copies_.producers[index] == motion_copies_.producers[*pick.slot]) {
        paired_frames[index] = depth_copies_.frames[index];
      }
    }
    PresentMotionPick depth_pick = PickPresentMotion(paired_frames, frame, PRESENT_MOTION_MAX_AGE);
    if (!depth_pick.slot) {
      depth_pick = PickPresentMotion(ordered_frames, frame, PRESENT_MOTION_MAX_AGE);
    }
    if (depth_pick.slot) {
      const size_t copy = *depth_pick.slot;
      inputs.depth = {.resource = depth_copies_.textures[copy].Get()};  // full subrect: the copy is the depth's own region
      inputs.depth_inverted = depth_copies_.inverted[copy] ? 1 : 0;
      depth_copies_.last_use[copy] = slot_marks_[slot];
    }
  }
  if (motion_source != MotionSource::LAUNCHPAD && launchpad_motion_.texture) {
    RetireLaunchpadMotion();  // Launchpad's vectors stopped coming: its copy goes, behind the recordings that used it
  }
  if (foveated && motion_source == MotionSource::PRESENT_COPY && !ConvertWorkMotion(list, slot, inputs, work, flip_y)) {
    motion_source = MotionSource::NONE;
  }
  if (motion_copies_.textures[0] || present_motion_counters_.evaluates != 0u) {
    PresentMotionCounters& counters = present_motion_counters_;
    ++counters.presents;
    counters.gpu_waits += gpu_waits;
    counters.gpu_wait_failures += gpu_wait_failures;
    if (!pick.slot) {
      ++counters.missed;
    } else if (pick.age == 0u) {
      ++counters.own;
    } else {
      ++counters.older;
    }
    counters.age_max = std::max(counters.age_max, pick.age);
    counters.resets += (inputs.reset_hint || motion_source != set_->present_motion ? 1u : 0u);
    if (!pick.slot && counters.miss_sample.empty()) {
      std::string sample = std::format("present-frame={} foveated={} pick=none; bridge={} raw={} bridge-age={} launchpad={} final-source={}; exposure='{}' factor={} metered={} probe={}; slots=[",
          frame, foveated, bridged_motion, target.dlss_motion_raw, target.dlss_motion_age,
          target.launchpad_motion.resource != nullptr, static_cast<uint32_t>(motion_source), ExposureLine(set_->exposure_report),
          exposure.factor, exposure.metered, exposure.probe);
      bool first = true;
      for (size_t index = 0u; index < motion_copies_.slots; ++index) {
        if (!first) sample += ";";
        first = false;
        const auto& status = copy_status[index];
        sample += std::format("{}:frame={},proof={},ordered={},submit={},sameq={},fence={},ready={},gpuwait={},waitfail={},done={},replay={}",
            index, motion_copies_.frames[index], status.has_proof, status.ordered, status.submitted_frame,
            status.same_queue, status.fence, status.fence_ready, status.gpu_wait_enqueued, status.gpu_wait_failed, status.complete, status.replayed);
      }
      sample += "]";
      counters.miss_sample = std::move(sample);
    }
    const auto now = std::chrono::steady_clock::now();
    if (counters.since == std::chrono::steady_clock::time_point{}) {
      counters.since = now;
    } else if (now - counters.since >= std::chrono::seconds(1)) {
      nr::Logf(nr::LogLevel::TRACE, "Direct3D 12: Present motion: {} copy attempts for {} presents; {} copies, skipped {} (input) {} (ring slot) {} (no writable slot); {} writes redirected; presents bound {} own, {} older, {} none; max copy age {}, max GPU lag {}; {} producer GPU waits ({} failed); {} pipeline reset hints; latest jitter correction {} x {} px{}{}",
               counters.evaluates, counters.presents, counters.copies, counters.skipped_input, counters.skipped_ring, counters.skipped_slot,
               counters.slot_redirects, counters.own, counters.older, counters.missed, counters.age_max, counters.lag_max, counters.gpu_waits, counters.gpu_wait_failures,
               counters.resets, counters.offset_x, counters.offset_y,
               counters.miss_sample.empty() ? "" : "; miss sample: ", counters.miss_sample);
      counters = {.since = now};
    }
  }
  inputs.reset_hint = (inputs.reset_hint || motion_source != set_->present_motion);  // a provider switch
  set_->present_motion = motion_source;
  // 1. Source copy ← target.
  Transition(list, source_copy, PIXEL_AND_COMPUTE_READ, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(source_copy, target.resource);
  Transition(list, source_copy, D3D12_RESOURCE_STATE_COPY_DEST, PIXEL_AND_COMPUTE_READ);
  profile.Copied();
  const nr::Rect copied = {.x = 0u, .y = 0u, .width = size.width, .height = size.height};
  if (exposure.metered || exposure.probe) {
    RecordMeter(list, slot,
                {.source = source_copy, .source_view_format = format->source_view_format, .region = copied, .encoding = target.encoding,
                 .primaries = (options & color::shader_options::PRIMARIES_MASK), .input_scale = input_scale},
                exposure);
  }
  // 2. Encode into A, resampled to the work image.
  const color::EncodePass encode = {
      .source = source_copy,
      .source_view_format = format->source_view_format,
      .width = work.image.width,
      .height = work.image.height,
      .encoding = target.encoding,
      .input_scale = input_scale,
      .exposure = exposure.texture,
      .exposure_factor = exposure.factor,
      .region = copied,
      .canvas = work.image,
      .options = options,
      .foveation = work.foveation,
  };
  if (!Encode(list, slot, encode, inputs.motion.resource == nullptr)) return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source};
  // 3. NR.
  const bool look_reset = (inputs.reset_hint || set_->intermediates.reset_pending);  // Evaluate pays the owed reset off
  const bool change_reset = (reset_hint || set_->intermediates.reset_pending);
  profile.Encoded();
  const nr::EvaluateResult evaluated = Evaluate(list, slot, inputs, controls, part.slice, profile.Sample());
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = nr::SessionWroteMessage(evaluated.reason),
            .motion_source = motion_source};
  }
  // 4. The change field and optional look stage.
  const auto change_input = set_->present_change_history.Advance(
      {.source = motion_source, .fresh = motion_source != MotionSource::PRESENT_COPY || (pick.slot ? pick.age : target.dlss_motion_age) == 0u,
       .allow_gap = target.motion_enabled}, change_reset);
  const AfterNr after = RecordAfterNr(list, slot, encode, evaluated.output, evaluated.passes_run, reduced, look_ready, work.upsampling,
                                      StabilizeMotionOf(inputs.motion, inputs.motion_scale_x, inputs.motion_scale_y), look_reset, change_input);
  if (!after.ok) {
    return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source};
  }
  // 5. The decode or compose through an RTV.
  Transition(list, after.texture, COMPUTE_READ, PIXEL_AND_COMPUTE_READ);
  if (after.model_input) Transition(list, after.model_input, COMPUTE_READ, PIXEL_AND_COMPUTE_READ);
  Transition(list, target.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const MaskBinding mask = BoundMask(slot, target.shared_mask);
  const bool decoded = color_.RecordDecode(
      list, slot,
      {.source = source_copy, .source_view_format = format->source_view_format, .nr_output = after.texture,
       .target = target.resource, .target_view_format = format->target_view_format,
       .target_view_srgb = format->target_view_srgb, .width = size.width, .height = size.height,
       .encoding = target.encoding, .input_scale = input_scale, .transfer_strength = target.transfer_strength,
       .color_strength = target.color_strength, .exposure = exposure.texture, .exposure_factor = exposure.factor,
       .upsampling = after.upsampling, .options = options | (after.fused ? color::shader_options::FUSED_CHANGE : 0u), .chroma_clamp = config_.fixes.chroma_clamp,
       .guided_low_gain = config_.fixes.guided_low_gain,
       .mask = mask.texture, .mask_view_format = mask.format,
       .foveation = work.foveation, .model_input = after.model_input});
  Transition(list, target.resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(list, after.texture, PIXEL_AND_COMPUTE_READ, COMPUTE_READ);
  if (after.model_input) Transition(list, after.model_input, PIXEL_AND_COMPUTE_READ, COMPUTE_READ);
  if (!decoded) return {.recorded = true, .reason = nr::SkipReason::NO_TARGET_PIPELINE, .motion_source = motion_source};
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason,
          .from_session = nr::SessionWroteMessage(evaluated.reason), .motion_source = motion_source};
}

PipelineResult NrPipeline::RecordUav(ID3D12GraphicsCommandList* list, const UavTarget& target,
                                     const nr::FrameInputs& inputs, const nr::Controls& controls, const WorkLayout& layout,
                                     const ChainPart& part, DlssFrameHost* timing_host) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = nr::SkipReason::INACTIVE};
  if (list == nullptr || target.resource == nullptr || part.set >= STAGE_SETS) return {.reason = nr::SkipReason::INVALID_INPUT};
  set_ = &sets_[part.set];
  if (!session_.SliceRuns(part.slice)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::BUDGET};
  }
  if (!config_.keep_faces.show_mask && !session_.SliceHasIntensity(part.slice, controls, config_.later_controls)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::INTENSITY_ZERO};
  }
  const D3D12_RESOURCE_DESC description = target.resource->GetDesc();
  const std::optional<color::UavFormatInfo> format = color::DescribeUavFormat(description.Format);
  // For UAV targets: without typed UAV stores the decode could not write NR's result back.
  if (!format || !color_.SupportsTypedUavStore(format->uav_format)) return {.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
  ID3D12Resource* const color_input = target.source.resource != nullptr ? target.source.resource : target.resource;
  const auto color_desc = color_input->GetDesc();
  auto color_format = color::DescribeFormat(color_desc.Format);
  if (!color_format) {
    if (const auto uav = color::DescribeUavFormat(color_desc.Format)) {
      color_format = color::FormatInfo{.copy_format = uav->copy_format, .source_view_format = uav->source_view_format,
                                        .bytes_per_pixel = uav->bytes_per_pixel};
    }
  }
  if (!color_format) return {.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
  if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1u
      || description.SampleDesc.Count != 1u || (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0u
      || color_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || color_desc.DepthOrArraySize != 1u
      || color_desc.SampleDesc.Count != 1u) return {.reason = nr::SkipReason::INVALID_INPUT};
  const bool separate_source = color_input != target.resource;
  if (separate_source && ((color_desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0u
      || (target.source_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS
          && (target.source_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0u))) {
    return {.reason = nr::SkipReason::INVALID_INPUT};
  }
  nr::Rect region = target.region;
  if (region.width == 0u && region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  const nr::Size size = {region.width, region.height};
  if (size.Empty() || uint64_t{region.x} + region.width > description.Width
      || uint64_t{region.y} + region.height > description.Height) {
    return {.reason = nr::SkipReason::INVALID_INPUT};
  }
  nr::Rect color_region = target.source.resource != nullptr ? target.source.rect : region;
  if (color_region.width == 0u && color_region.x < color_desc.Width) color_region.width = static_cast<uint32_t>(color_desc.Width - color_region.x);
  if (color_region.height == 0u && color_region.y < color_desc.Height) color_region.height = color_desc.Height - color_region.y;
  if (color_region.width != size.width || color_region.height != size.height
      || uint64_t{color_region.x} + color_region.width > color_desc.Width
      || uint64_t{color_region.y} + color_region.height > color_desc.Height) return {.reason = nr::SkipReason::INVALID_INPUT};
  if (!nr::MeetsNrFloor(size)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::FRAME_TOO_SMALL};
  }
  const uint32_t slot = timeline_.FindReusableSlot(slot_marks_, next_slot_);
  if (slot == slot_marks_.size()) return {.reason = nr::SkipReason::DESCRIPTORS_BUSY};
  const WorkLayout work = ResolveLayout(layout, size);
  const bool foveated = work.foveation.active();
  const bool in_place = !separate_source && color_region.x == region.x && color_region.y == region.y
      && description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.DepthOrArraySize == 1u
      && description.MipLevels == 1u && description.SampleDesc.Count == 1u
      && (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0u
      && (description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0u
      && format->source_view_format == format->uav_format && color_.CanDecodeInPlace(format->uav_format);
  const std::array native_inputs = {inputs.depth, inputs.control_mask, inputs.backbuffer, inputs.ui, inputs.ui_alpha};
  std::array<std::optional<NativeInputFormat>, 5> native_views;
  std::array<DXGI_FORMAT, 5> native_formats{};
  if (inputs.native_nr && foveated) {
    for (size_t i = 0u; i < native_inputs.size(); ++i) {
      native_views[i] = NativeInputView(native_inputs[i], i == 0u);
      if (native_views[i] && color_.SupportsTypedUavStore(native_views[i]->target)) native_formats[i] = native_views[i]->target;
    }
  }
  if (const nr::SkipReason problem = EnsureIntermediates({
          .sizes = {.output = size, .image = work.image, .canvas = work.image},
          .copy_format = in_place || separate_source ? DXGI_FORMAT_UNKNOWN : color_format->copy_format,
          .source_bytes_per_pixel = in_place || separate_source ? 0u : color_format->bytes_per_pixel,
          .kind = TargetKind::UAV,
          .canvas_motion = foveated,
          .foveation = work.foveation,
          .native_input_formats = native_formats,
      });
      problem != nr::SkipReason::NONE) {
    return {.reason = problem};
  }
  const bool reduced = (work.image != size);
  const LookPlan look_plan = LookPlanFor(work.image);
  if (!EnsureChange(reduced || foveated || look_plan.shape || look_plan.change)) return {.reason = nr::SkipReason::OUT_OF_MEMORY};
  const bool look_ready = EnsureLook(look_plan);
  CommitSlot(slot, true);
  NrGpuTiming::Recording profile(timing_, list, part.slice.stage, nullptr, timing_host);
  const float input_scale = color::InputScale(target.encoding, target.diffuse_white_nits, config_.fixes.linear_unit_nits);
  NoteEncoding(target.encoding, input_scale);
  const uint32_t options = ShaderOptions(config_.fixes);
  NoteMeterBasis(part.slice.stage, target.encoding, input_scale);
  const ExposureChoice exposure = ChooseExposure(target.encoding, target.exposure, target.exposure_factor);
  ID3D12Resource* const source = separate_source ? color_input : in_place ? target.resource : set_->intermediates.source_copy.Get();

  // The typed load reads each target pixel just before that same thread writes it.
  // Meter, encode and change use the resource as an SRV, before composition starts.
  struct RestoreOutput {
    ID3D12GraphicsCommandList* list;
    ID3D12Resource* resource;
    void Now() {
      if (resource == nullptr) return;
      Transition(list, resource, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      resource = nullptr;
    }
    ~RestoreOutput() { Now(); }
  } restore{list, !separate_source && (description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0u ? target.resource : nullptr};
  struct RestoreSource {
    ID3D12GraphicsCommandList* list;
    ID3D12Resource* resource;
    ~RestoreSource() { if (resource != nullptr) Transition(list, resource, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
  } restore_source{list, separate_source && target.source_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ? color_input : nullptr};
  if (separate_source) {
    if (restore_source.resource != nullptr) Transition(list, color_input, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  } else if (in_place) {
    Transition(list, target.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  } else {
    Transition(list, target.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list, source, COMPUTE_READ, D3D12_RESOURCE_STATE_COPY_DEST);
    const D3D12_TEXTURE_COPY_LOCATION destination = {
        .pResource = source, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0u};
    const D3D12_TEXTURE_COPY_LOCATION origin = {
        .pResource = target.resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0u};
    const D3D12_BOX box = {.left = color_region.x, .top = color_region.y, .front = 0u, .right = color_region.x + color_region.width,
                           .bottom = color_region.y + color_region.height, .back = 1u};
    list->CopyTextureRegion(&destination, 0u, 0u, 0u, &origin, &box);
    Transition(list, source, D3D12_RESOURCE_STATE_COPY_DEST, COMPUTE_READ);
    Transition(list, target.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, restore.resource != nullptr ? COMPUTE_READ : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  const nr::Rect copied = in_place || separate_source ? color_region : nr::Rect{.width = size.width, .height = size.height};
  nr::FrameInputs nr_inputs = inputs;
  nr::NativeInputReport report{.status = inputs.native_input_status, .foveated = foveated};
  // Direct pipeline clients need no capture metadata. Preserve an explicit rejection/disable reason.
  const std::array reported_inputs = {inputs.depth, inputs.motion, inputs.control_mask, inputs.backbuffer, inputs.ui, inputs.ui_alpha};
  for (size_t i = 0u; i < reported_inputs.size(); ++i) {
    if (reported_inputs[i].resource != nullptr) report.status[i] = nr::NativeInputStatus::READY;
  }
  if (foveated) {
    if (inputs.motion.resource != nullptr) {
      const bool supported = MotionViewFormat(inputs.motion.resource->GetDesc().Format).has_value();
      const bool converted = ConvertWorkMotion(list, slot, nr_inputs, work);
      report.status[1] = converted ? nr::NativeInputStatus::REMAPPED
          : supported ? nr::NativeInputStatus::REMAP_FAILED : nr::NativeInputStatus::FORMAT_UNSUPPORTED;
    }
    if (inputs.native_nr) {
      const std::array bindings = {&nr_inputs.depth, &nr_inputs.control_mask, &nr_inputs.backbuffer, &nr_inputs.ui, &nr_inputs.ui_alpha};
      for (uint32_t i = 0u; i < bindings.size(); ++i) {
        *bindings[i] = {};
        if (native_inputs[i].resource == nullptr) continue;
        ID3D12Resource* const remapped = set_->intermediates.native_inputs[i].Get();
        bool recorded = false;
        if (remapped != nullptr && native_views[i]) {
          Transition(list, remapped, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          recorded = color_.RecordNativeInput(list, slot, i, native_inputs[i], native_views[i]->source, remapped,
                                              native_formats[i], work.image, work.foveation);
          Transition(list, remapped, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
        }
        const size_t report_index = i == 0u ? 0u : i + 1u;
        report.status[report_index] = recorded ? nr::NativeInputStatus::REMAPPED
            : native_formats[i] == DXGI_FORMAT_UNKNOWN ? nr::NativeInputStatus::FORMAT_UNSUPPORTED : nr::NativeInputStatus::REMAP_FAILED;
        if (recorded) *bindings[i] = {remapped, {0u, 0u, work.image.width, work.image.height}};
        else WarnOnce(&native_input_warned_, "Native NR: an optional input could not follow the foveated grid; binding null instead");
      }
      if (nr_inputs.backbuffer.resource == nullptr || (nr_inputs.ui.resource == nullptr && nr_inputs.ui_alpha.resource == nullptr)) {
        nr_inputs.ui_correction = false;
      }
    } else nr_inputs.depth = {};  // the ordinary After-DLSS path has no warped depth
  }
  if (inputs.native_nr) {
    report.zero_motion = nr_inputs.motion.resource == nullptr;
    report.ui_correction = nr_inputs.ui_correction;
    if (!native_input_report_ || *native_input_report_ != report) {
      native_input_report_ = report;
      nr::Logf(nr::LogLevel::INFO,
          "Native NR inputs: Depth={}; Motion={}{}; ControlMask={}; Backbuffer={}; UI={}; UIAlpha={}; foveated={}; UI correction={}; work={}x{}",
          nr::NativeInputStatusName(report.status[0]), nr::NativeInputStatusName(report.status[1]),
          report.zero_motion ? " -> zero-motion" : "", nr::NativeInputStatusName(report.status[2]),
          nr::NativeInputStatusName(report.status[3]), nr::NativeInputStatusName(report.status[4]),
          nr::NativeInputStatusName(report.status[5]), foveated, report.ui_correction, work.image.width, work.image.height);
    }
  }
  const MotionSource motion_source = (nr_inputs.motion.resource != nullptr ? MotionSource::DIRECT : MotionSource::NONE);
  profile.Copied();
  if (exposure.metered || exposure.probe) {
    RecordMeter(list, slot,
                {.source = source, .source_view_format = color_format->source_view_format, .region = copied, .encoding = target.encoding,
                 .primaries = (options & color::shader_options::PRIMARIES_MASK), .input_scale = input_scale},
                exposure);
  }
  // 2. Encode into A with the game's exposure, resampled to the work image.
  const color::EncodePass encode = {
      .source = source,
      .source_view_format = color_format->source_view_format,
      .width = work.image.width,
      .height = work.image.height,
      .encoding = target.encoding,
      .input_scale = input_scale,
      .exposure = exposure.texture,
      .exposure_factor = exposure.factor,
      .region = copied,
      .canvas = work.image,
      .options = options,
      .foveation = work.foveation,
  };
  if (!Encode(list, slot, encode, nr_inputs.motion.resource == nullptr)) return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source};
  // 3. NR uses the motion in the same coordinates as the encoded input.
  const bool look_reset = (nr_inputs.reset_hint || set_->intermediates.reset_pending);  // Evaluate pays the owed reset off
  profile.Encoded();
  const nr::EvaluateResult evaluated = Evaluate(list, slot, nr_inputs, controls, part.slice, profile.Sample());
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = nr::SessionWroteMessage(evaluated.reason),
            .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  // 4. The change field and optional look stage.
  const AfterNr after = RecordAfterNr(list, slot, encode, evaluated.output, evaluated.passes_run, reduced, look_ready, work.upsampling,
                                      StabilizeMotionOf(nr_inputs.motion, nr_inputs.motion_scale_x, nr_inputs.motion_scale_y), look_reset);
  if (!after.ok) {
    return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source,
            .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  // 5. The compute decode into the region, then a UAV barrier so the game's next reads see NR's result.
  restore.Now();
  const MaskBinding mask = BoundMask(slot, timing_host ? timing_host->SharedMask() : nullptr);
  const bool decoded = color_.RecordComputeDecode(
      list, slot,
      {.source = in_place ? nullptr : source, .source_view_format = color_format->source_view_format, .nr_output = after.texture,
       .target = target.resource, .target_uav_format = format->uav_format, .origin_x = region.x, .origin_y = region.y,
       .target_width = static_cast<uint32_t>(description.Width), .target_height = description.Height,
       .width = size.width, .height = size.height, .encoding = target.encoding, .input_scale = input_scale,
       .transfer_strength = target.transfer_strength, .color_strength = target.color_strength,
       .exposure = exposure.texture, .exposure_factor = exposure.factor, .source_x = copied.x, .source_y = copied.y,
       .upsampling = after.upsampling, .options = options | (after.fused ? color::shader_options::FUSED_CHANGE : 0u), .chroma_clamp = config_.fixes.chroma_clamp,
       .guided_low_gain = config_.fixes.guided_low_gain,
       .mask = mask.texture, .mask_view_format = mask.format,
       .foveation = work.foveation, .model_input = after.model_input});
  UavBarrier(list, target.resource);
  if (!decoded) {
    return {.recorded = true, .reason = nr::SkipReason::DECODE_FAILED, .motion_source = motion_source,
            .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason,
          .from_session = nr::SessionWroteMessage(evaluated.reason), .motion_source = motion_source,
          .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
}

PipelineResult NrPipeline::RecordPreSr(ID3D12GraphicsCommandList* list, const UavTarget& color, const nr::FrameInputs& inputs,
                                       const nr::Controls& controls, const WorkLayout& layout, const ChainPart& part, DlssFrameHost* timing_host) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = nr::SkipReason::INACTIVE};
  if (list == nullptr || color.resource == nullptr || part.set >= STAGE_SETS) return {.reason = nr::SkipReason::INVALID_INPUT};
  set_ = &sets_[part.set];
  if (!session_.SliceRuns(part.slice)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::BUDGET};
  }
  if (!config_.keep_faces.show_mask && !session_.SliceHasIntensity(part.slice, controls, config_.later_controls)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::INTENSITY_ZERO};
  }
  const D3D12_RESOURCE_DESC description = color.resource->GetDesc();
  const std::optional<color::UavFormatInfo> format = color::DescribeUavFormat(description.Format);
  // A 2D, single-sample texture Uplift reads through a typed, non-sRGB view.
  if (!format || description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1u
      || description.SampleDesc.Count != 1u) {
    return {.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
  }
  const nr::Rect region = color.region;
  const nr::Size size = {region.width, region.height};
  if (size.Empty() || uint64_t{region.x} + region.width > description.Width
      || uint64_t{region.y} + region.height > description.Height) {
    return {.reason = nr::SkipReason::INVALID_INPUT};
  }
  if (!nr::MeetsNrFloor(size)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::FRAME_TOO_SMALL};
  }
  const uint32_t slot = timeline_.FindReusableSlot(slot_marks_, next_slot_);
  if (slot == slot_marks_.size()) return {.reason = nr::SkipReason::DESCRIPTORS_BUSY};
  const WorkLayout work = ResolveLayout(layout, size);
  const bool foveated = work.foveation.active();
  ID3D12Resource* const external_output = timing_host ? timing_host->PreSrOutput() : nullptr;
  // Keyed on the settled work image, never the per-frame render region --
  // a game with dynamic resolution sets `size` on almost every evaluate, and the old `.output = size`
  // reallocated (and restarted NR's history on) the whole set for each step. EnsureIntermediates always
  // allocates the change texture on demand (its own `reduced` computation), so the set is stable
  // here even though `size` still varies frame to frame.
  if (const nr::SkipReason problem = EnsureIntermediates({
          .sizes = {.output = work.image, .image = work.image, .canvas = work.image,
                    .private_color = external_output ? nr::Size{} : nr::Size{static_cast<uint32_t>(description.Width), description.Height}},
          .kind = TargetKind::PRE_SR,
          .canvas_motion = foveated,
          .foveation = work.foveation,
      });
      problem != nr::SkipReason::NONE) {
    return {.reason = problem};
  }
  const bool reduced = (work.image != size);
  const LookPlan look_plan = LookPlanFor(work.image);
  if (!EnsureChange(reduced || foveated || look_plan.shape || look_plan.change)) return {.reason = nr::SkipReason::OUT_OF_MEMORY};
  const bool look_ready = EnsureLook(look_plan);
  CommitSlot(slot, true);
  NrGpuTiming::Recording profile(timing_, list, part.slice.stage, nullptr, timing_host);
  profile.Copied();  // this path samples Color directly; there is no source copy
  const float input_scale = color::InputScale(color.encoding, color.diffuse_white_nits, config_.fixes.linear_unit_nits);
  NoteEncoding(color.encoding, input_scale);
  const uint32_t options = ShaderOptions(config_.fixes);
  NoteMeterBasis(part.slice.stage, color.encoding, input_scale);
  const ExposureChoice exposure = ChooseExposure(color.encoding, color.exposure, color.exposure_factor);
  if (exposure.metered || exposure.probe) {
    RecordMeter(list, slot,
                {.source = color.resource, .source_view_format = format->source_view_format, .region = region, .encoding = color.encoding,
                 .primaries = (options & color::shader_options::PRIMARIES_MASK), .input_scale = input_scale},
                exposure);
  }
  // 1. Encode the region straight from the game's Color: it is only read, never copied or transitioned.
  const color::EncodePass encode = {
      .source = color.resource,
      .source_view_format = format->source_view_format,
      .width = work.image.width,
      .height = work.image.height,
      .encoding = color.encoding,
      .input_scale = input_scale,
      .exposure = exposure.texture,
      .exposure_factor = exposure.factor,
      .region = region,
      .canvas = work.image,
      .options = options,
      .foveation = work.foveation,
  };
  // 2. Remap motion into the foveated work grid.
  nr::FrameInputs nr_inputs = inputs;
  if (foveated) {
    ConvertWorkMotion(list, slot, nr_inputs, work);
    nr_inputs.depth = {};
  }
  // Reported as bound in place either way: a canvas copy still carries the game's own motion vectors,
  // scaled by what the caller asked for (inputs.motion_scale_x/y), not nr_inputs' own post-copy (1, 1).
  const MotionSource motion_source = (nr_inputs.motion.resource != nullptr ? MotionSource::DIRECT : MotionSource::NONE);
  if (!Encode(list, slot, encode, nr_inputs.motion.resource == nullptr)) return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED};
  // 3. NR at the work size.
  const bool look_reset = (nr_inputs.reset_hint || set_->intermediates.reset_pending);  // Evaluate pays the owed reset off
  profile.Encoded();
  const nr::EvaluateResult evaluated = Evaluate(list, slot, nr_inputs, controls, part.slice, profile.Sample());
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = nr::SessionWroteMessage(evaluated.reason),
            .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  // 4. The change field and optional look stage, at the work-image size.
  const AfterNr after = RecordAfterNr(list, slot, encode, evaluated.output, evaluated.passes_run, reduced, look_ready, work.upsampling,
                                      StabilizeMotionOf(nr_inputs.motion, nr_inputs.motion_scale_x, nr_inputs.motion_scale_y), look_reset);
  if (!after.ok) {
    return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source,
            .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  // 5. The compose into the private colour's region.
  ID3D12Resource* const private_color = external_output ? external_output : set_->intermediates.private_color.Get();
  Transition(list, private_color, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const MaskBinding mask = BoundMask(slot, timing_host ? timing_host->SharedMask() : nullptr);
  const bool composed = color_.RecordComputeDecode(
      list, slot,
      {
          .source = color.resource,
          .source_view_format = format->source_view_format,
          .nr_output = after.texture,
          .target = private_color,
          .target_uav_format = DXGI_FORMAT_R16G16B16A16_FLOAT,
          .origin_x = region.x,
          .origin_y = region.y,
          .target_width = static_cast<uint32_t>(description.Width),
          .target_height = description.Height,
          .width = size.width,
          .height = size.height,
          .encoding = color.encoding,
          .input_scale = input_scale,
          .transfer_strength = color.transfer_strength,
          .color_strength = color.color_strength,
          .exposure = exposure.texture,
          .exposure_factor = exposure.factor,
          .source_x = region.x,
          .source_y = region.y,
          .upsampling = after.upsampling,
          .options = options | (after.fused ? color::shader_options::FUSED_CHANGE : 0u),
          .chroma_clamp = config_.fixes.chroma_clamp,
       .guided_low_gain = config_.fixes.guided_low_gain,
          .mask = mask.texture,
          .mask_view_format = mask.format,
          .foveation = work.foveation,
          .model_input = after.model_input,
      });
  // DLSS reads it next, in NON_PIXEL_SHADER_RESOURCE; leaving UNORDERED_ACCESS also finishes the writes.
  Transition(list, private_color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (!composed) {
    return {.recorded = true, .reason = nr::SkipReason::DECODE_FAILED, .motion_source = motion_source,
            .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason,
          .from_session = nr::SessionWroteMessage(evaluated.reason), .motion_source = motion_source,
          .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
}

PipelineResult NrPipeline::PrepareNativeNrHandle(DlssFrameHost& host, state::RestoreMode restore,
                                                  const ngx_hooks::DlssFrame& frame, const WorkLayout& layout,
                                                  NativeNrHandleWork* work) {
  if (work == nullptr || frame.color == nullptr || frame.output == nullptr || !frame.native_nr || !frame.native_nr->enabled) {
    return {.reason = nr::SkipReason::INVALID_INPUT};
  }
  ID3D12GraphicsCommandList* const list = host.NativeList();
  if (list == nullptr || !host.StateKnown()) return {.reason = nr::SkipReason::LIST_STATE_UNKNOWN};
  if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return {.reason = nr::SkipReason::COMPUTE_QUEUE};

  const D3D12_RESOURCE_DESC color_desc = frame.color->GetDesc();
  const D3D12_RESOURCE_DESC output_desc = frame.output->GetDesc();
  const nr::Rect input_region = frame.color_region;
  const nr::Rect output_region = frame.output_region;
  const bool color_output_alias = (frame.color == frame.output);
  const nr::Size output_size{output_region.width, output_region.height};
  const nr::Size image = (layout.image.Empty() ? output_size : layout.image);
  if (input_region.width != output_size.width || input_region.height != output_size.height || image.Empty()
      || !nr::MeetsNrFloor(image) || uint64_t{input_region.x} + input_region.width > color_desc.Width
      || uint64_t{input_region.y} + input_region.height > color_desc.Height || uint64_t{output_region.x} + output_region.width > output_desc.Width
      || uint64_t{output_region.y} + output_region.height > output_desc.Height
      || color_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || output_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
      || color_desc.DepthOrArraySize != 1u || output_desc.DepthOrArraySize != 1u || color_desc.SampleDesc.Count != 1u
      || output_desc.SampleDesc.Count != 1u || (output_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0u) {
    return {.reason = nr::SkipReason::INVALID_INPUT};
  }
  if (image == output_size && !layout.foveation.active()) return {.reason = nr::SkipReason::NONE};
  if (!color_.NativeInputReady() || !color_.NativeCompositeReady()) return {.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
  const auto color_format = color::DescribeUavFormat(color_desc.Format);
  const auto output_format = color::DescribeUavFormat(output_desc.Format);
  if (!color_format || !output_format || !color_.SupportsTypedUavStore(color_format->uav_format)
      || !color_.SupportsTypedUavStore(output_format->uav_format)) {
    return {.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
  }
  if (!frame.native_nr->color_state || (color_desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0u) {
    return {.reason = nr::SkipReason::LIST_STATE_UNKNOWN};
  }

  const std::array<nr::BoundResource, 5> auxiliary = {
      nr::BoundResource{frame.depth, frame.depth_region}, frame.native_nr->control_mask, frame.native_nr->backbuffer,
      frame.native_nr->ui, frame.native_nr->ui_alpha};
  std::array<std::optional<NativeInputFormat>, 5> auxiliary_formats;
  std::array<DXGI_FORMAT, 5> auxiliary_views{};
  std::array<DXGI_FORMAT, 5> auxiliary_uavs{};
  for (size_t i = 0u; i < auxiliary.size(); ++i) {
    auxiliary_formats[i] = NativeInputView(auxiliary[i], i == 0u);
    if (auxiliary_formats[i] && color_.SupportsTypedUavStore(auxiliary_formats[i]->target)) {
      auxiliary_views[i] = auxiliary_formats[i]->source;
      auxiliary_uavs[i] = auxiliary_formats[i]->target;
    }
  }
  const std::optional<DXGI_FORMAT> motion_view = (frame.motion_vectors != nullptr
      ? MotionViewFormat(frame.motion_vectors->GetDesc().Format) : std::nullopt);
  const bool remap_motion = motion_view.has_value() && frame.motion_region.width != 0u && frame.motion_region.height != 0u
      && uint64_t{frame.motion_region.x} + frame.motion_region.width <= frame.motion_vectors->GetDesc().Width
      && uint64_t{frame.motion_region.y} + frame.motion_region.height <= frame.motion_vectors->GetDesc().Height;

  uint32_t descriptor_slots[2] = {UINT32_MAX, UINT32_MAX};
  size_t found_slots = 0u;
  for (uint32_t offset = 0u; offset < slot_marks_.size() && found_slots < 2u; ++offset) {
    const uint32_t slot = (next_slot_ + offset) % static_cast<uint32_t>(slot_marks_.size());
    if (timeline_.IsComplete(slot_marks_[slot])) descriptor_slots[found_slots++] = slot;
  }
  if (found_slots != 2u) return {.reason = nr::SkipReason::DESCRIPTORS_BUSY};

  size_t surface_index = native_handle_surfaces_.size();
  for (size_t i = 0u; i < native_handle_surfaces_.size(); ++i) {
    if (native_handle_surfaces_[i] && native_handle_surfaces_[i]->active) continue;
    if (native_handle_surfaces_[i] && !timeline_.IsComplete(native_handle_surfaces_[i]->last_use)) continue;
    surface_index = i;
    break;
  }
  if (surface_index == native_handle_surfaces_.size()) return {.reason = nr::SkipReason::DESCRIPTORS_BUSY};

  auto matches = [&](const std::shared_ptr<NativeHandleSurfaces>& held) {
    if (!held || held->size != image || held->original_size != output_size || held->color_view != color_format->source_view_format
        || held->color_uav != color_format->uav_format || held->output_view != output_format->source_view_format
        || held->output_uav != output_format->uav_format || !held->color || !held->output || !held->baseline || !held->original
        || held->color_output_alias != color_output_alias || static_cast<bool>(held->motion) != remap_motion) return false;
    for (size_t i = 0u; i < auxiliary.size(); ++i) {
      if (held->auxiliary_view[i] != auxiliary_views[i] || held->auxiliary_uav[i] != auxiliary_uavs[i]
          || static_cast<bool>(held->auxiliary[i]) != (auxiliary_uavs[i] != DXGI_FORMAT_UNKNOWN)) return false;
    }
    return true;
  };
  auto& held = native_handle_surfaces_[surface_index];
  if (!matches(held)) {
    auto created = std::make_shared<NativeHandleSurfaces>();
    created->size = image;
    created->original_size = output_size;
    created->color_view = color_format->source_view_format;
    created->color_uav = color_format->uav_format;
    created->output_view = output_format->source_view_format;
    created->output_uav = output_format->uav_format;
    created->color_output_alias = color_output_alias;
    created->color = CreateTexture(image, color_format->uav_format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Uplift native-handle Color");
    created->baseline = CreateTexture(image, color_format->uav_format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Uplift native-handle baseline");
    created->original = CreateTexture(output_size, color_format->uav_format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Uplift native-handle original Color");
    if (color_output_alias) {
      // Preserve the game's in-place Color/Output contract for native NR. A separate read-only Color
      // resource and UAV Output are only safe when the game's original parameters were separate too.
      created->output = created->color;
    } else {
      created->output = CreateTexture(image, output_format->uav_format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Uplift native-handle Output");
    }
    if (remap_motion) created->motion = CreateTexture(image, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Uplift native-handle motion");
    for (size_t i = 0u; i < auxiliary.size(); ++i) {
      created->auxiliary_view[i] = auxiliary_views[i];
      created->auxiliary_uav[i] = auxiliary_uavs[i];
      if (auxiliary_uavs[i] != DXGI_FORMAT_UNKNOWN) {
        created->auxiliary[i] = CreateTexture(image, auxiliary_uavs[i], D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Uplift native-handle guide");
      }
    }
    if (!created->color || !created->output || !created->baseline || !created->original || (remap_motion && !created->motion)) {
      return {.reason = nr::SkipReason::OUT_OF_MEMORY};
    }
    for (size_t i = 0u; i < auxiliary.size(); ++i) {
      if (auxiliary_uavs[i] != DXGI_FORMAT_UNKNOWN && !created->auxiliary[i]) {
        created->auxiliary_view[i] = DXGI_FORMAT_UNKNOWN;
        created->auxiliary_uav[i] = DXGI_FORMAT_UNKNOWN;
        created->auxiliary[i].Reset();  // optional guides fall back to null
      }
    }
    const auto bytes_per_pixel = [](DXGI_FORMAT format) -> uint64_t {
      if (const auto info = color::DescribeUavFormat(format)) return info->bytes_per_pixel;
      if (format == DXGI_FORMAT_R32_FLOAT) return 4u;
      if (format == DXGI_FORMAT_R16G16_FLOAT) return 4u;
      if (format == DXGI_FORMAT_R8_UNORM) return 1u;
      return 8u;
    };
    created->bytes = image.Pixels() * (2u * bytes_per_pixel(color_format->uav_format)
        + (color_output_alias ? 0u : bytes_per_pixel(output_format->uav_format))
        + (remap_motion ? 4u : 0u)) + output_size.Pixels() * bytes_per_pixel(color_format->uav_format);
    for (const DXGI_FORMAT format : created->auxiliary_uav) if (format != DXGI_FORMAT_UNKNOWN) created->bytes += image.Pixels() * bytes_per_pixel(format);
    held = std::move(created);
  }
  held->active = true;

  auto ticket = std::make_shared<NativeNrHandleTicket>();
  ticket->surfaces = held;
  ticket->original_output = frame.output;
  ticket->output_region = output_region;
  ticket->layout = layout;
  ticket->post_slot = descriptor_slots[1];
  ticket->output_view = output_format->source_view_format;
  ticket->output_uav = output_format->uav_format;

  PipelineResult recorded = RecordOnGameList(host, timeline_, restore, [&] {
    const uint32_t slot = descriptor_slots[0];
    const D3D12_RESOURCE_STATES color_state = frame.native_nr->color_state;
    Transition(list, frame.color, color_state, COMPUTE_READ);
    const bool color_ok = color_.RecordNativeInput(list, slot, 0u, {frame.color, input_region}, color_format->source_view_format,
        held->color.Get(), color_format->uav_format, image, layout.foveation, true, false);
    if (!color_ok) {
      Transition(list, frame.color, COMPUTE_READ, color_state);
      return PipelineResult{.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
    }
    static_cast<void>(color_.RecordNativeInput(list, slot, 6u, {frame.color, input_region}, color_format->source_view_format,
        held->baseline.Get(), color_format->uav_format, image, layout.foveation, true, false));
    static_cast<void>(color_.RecordNativeInput(list, slot, 7u, {frame.color, input_region}, color_format->source_view_format,
        held->original.Get(), color_format->uav_format, output_size, {}, false, false));
    if (!color_output_alias) {
      Transition(list, held->color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    } else {
      // The game binds this same surface as both Color and Output. Keep it in UAV state for NGX,
      // but order the preprocessing dispatch's writes before the native evaluate's UAV accesses.
      UavBarrier(list, held->color.Get());
    }
    Transition(list, held->baseline.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    Transition(list, held->original.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    for (size_t i = 0u; i < auxiliary.size(); ++i) {
      if (auxiliary[i].resource == nullptr || !held->auxiliary[i] || !auxiliary_formats[i]) continue;
      static_cast<void>(color_.RecordNativeInput(list, slot, static_cast<uint32_t>(i + 1u), auxiliary[i],
          auxiliary_formats[i]->source, held->auxiliary[i].Get(), auxiliary_uavs[i], image, layout.foveation));
      Transition(list, held->auxiliary[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    }
    if (remap_motion && held->motion) {
      static_cast<void>(color_.RecordMotion(list, slot, {.source = frame.motion_vectors, .source_view_format = *motion_view,
          .region = frame.motion_region, .target = held->motion.Get(), .image = image, .canvas = image,
          .scale_x = frame.mv_scale_x * static_cast<float>(image.width) / static_cast<float>(frame.motion_region.width),
          .scale_y = frame.mv_scale_y * static_cast<float>(image.height) / static_cast<float>(frame.motion_region.height),
          .foveation = layout.foveation}));
      Transition(list, held->motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    }
    Transition(list, frame.color, COMPUTE_READ, color_state);
    slot_marks_[descriptor_slots[0]] = timeline_.MarkNow(true);
    slot_marks_[descriptor_slots[1]] = slot_marks_[descriptor_slots[0]];  // reserved until the game's evaluate returns
    held->last_use = slot_marks_[descriptor_slots[0]];
    next_slot_ = (descriptor_slots[1] + 1u) % static_cast<uint32_t>(slot_marks_.size());
    return PipelineResult{.recorded = true};
  });
  if (!recorded.recorded) {
    held->active = false;
    return recorded;
  }

  work->resources = {held->color.Get(), held->output.Get(),
      (auxiliary[0].resource != nullptr && held->auxiliary[0] ? held->auxiliary[0].Get() : nullptr),
      (remap_motion && held->motion ? held->motion.Get() : nullptr),
      (auxiliary[1].resource != nullptr && held->auxiliary[1] ? held->auxiliary[1].Get() : nullptr),
      (auxiliary[2].resource != nullptr && held->auxiliary[2] ? held->auxiliary[2].Get() : nullptr),
      (auxiliary[3].resource != nullptr && held->auxiliary[3] ? held->auxiliary[3].Get() : nullptr),
      (auxiliary[4].resource != nullptr && held->auxiliary[4] ? held->auxiliary[4].Get() : nullptr)};
  work->image = image;
  work->motion_rescaled = (work->resources[3] != nullptr);
  work->motion_scale_x = 1.f;
  work->motion_scale_y = 1.f;
  work->ui_correction = frame.native_nr->ui_correction && work->resources[5] != nullptr
      && (work->resources[6] != nullptr || work->resources[7] != nullptr);
  work->ticket = std::move(ticket);
  return recorded;
}

PipelineResult NrPipeline::CompleteNativeNrHandle(DlssFrameHost& host, state::RestoreMode restore,
                                                   const NativeNrHandleWork& work, bool native_succeeded) {
  const auto ticket = std::static_pointer_cast<NativeNrHandleTicket>(work.ticket);
  if (!ticket || !ticket->surfaces) return {.reason = nr::SkipReason::INVALID_INPUT};
  NativeHandleSurfaces& surfaces = *ticket->surfaces;
  ID3D12GraphicsCommandList* const list = host.NativeList();
  if (list == nullptr || !host.StateKnown()) {
    surfaces.active = false;
    return {.reason = nr::SkipReason::LIST_STATE_UNKNOWN};
  }
  return RecordOnGameList(host, timeline_, restore, [&] {
    const uint32_t slot = ticket->post_slot;
    bool upsampled = false;
    if (native_succeeded) {
      Transition(list, surfaces.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
      upsampled = color_.RecordNativeComposite(list, slot,
          {.original = surfaces.original.Get(), .original_view_format = surfaces.color_view,
           .baseline = surfaces.baseline.Get(), .baseline_view_format = surfaces.color_view,
           .nr_output = surfaces.output.Get(), .nr_output_view_format = surfaces.output_view,
           .target = ticket->original_output.Get(), .target_uav_format = ticket->output_uav,
           .output_size = {ticket->output_region.width, ticket->output_region.height}, .work_size = work.image,
           .origin_x = ticket->output_region.x, .origin_y = ticket->output_region.y, .foveation = ticket->layout.foveation});
      UavBarrier(list, ticket->original_output.Get());
      Transition(list, surfaces.output.Get(), COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // Restore every substituted guide to its initial state. The next evaluate can then use the same
    // resource set even when this command list is replayed.
    if (surfaces.color && surfaces.color.Get() != surfaces.output.Get()) {
      Transition(list, surfaces.color.Get(), COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    Transition(list, surfaces.baseline.Get(), COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, surfaces.original.Get(), COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (surfaces.motion && work.motion_rescaled) Transition(list, surfaces.motion.Get(), COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (size_t i = 0u; i < surfaces.auxiliary.size(); ++i) {
      if (surfaces.auxiliary[i] && work.resources[(i == 0u) ? 2u : (i + 3u)] != nullptr) {
        Transition(list, surfaces.auxiliary[i].Get(), COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
    }
    slot_marks_[slot] = timeline_.MarkNow(true);
    surfaces.last_use = slot_marks_[slot];
    surfaces.active = false;
    if (!native_succeeded) return PipelineResult{.recorded = true};
    return PipelineResult{.recorded = true, .nr_applied = upsampled,
                          .reason = (upsampled ? nr::SkipReason::NONE : nr::SkipReason::UNSUPPORTED_FORMAT)};
  });
}

bool NrPipeline::ConvertWorkMotion(ID3D12GraphicsCommandList* list, uint32_t slot, nr::FrameInputs& inputs, const WorkLayout& work, bool flip_y) {
  const color::StabilizeMotion source = StabilizeMotionOf(inputs.motion, inputs.motion_scale_x, inputs.motion_scale_y);
  inputs.motion = {};  // do not bind the unwarped source if conversion fails
  const nr::Rect region = source.vectors.rect;
  ID3D12Resource* const target = set_->intermediates.canvas_motion.Get();
  if (source.vectors.resource == nullptr || target == nullptr) return false;
  Transition(list, target, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const bool recorded = color_.RecordMotion(list, slot,
      {.source = source.vectors.resource, .source_view_format = source.view_format, .region = region, .target = target,
       .image = work.image, .canvas = work.image,
       .scale_x = source.scale_x * static_cast<float>(work.image.width) / static_cast<float>(region.width),
       .scale_y = source.scale_y * static_cast<float>(work.image.height) / static_cast<float>(region.height),
       .flip_y = flip_y, .foveation = work.foveation});
  Transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (!recorded) return false;
  inputs.motion = {.resource = target};
  inputs.motion_scale_x = 1.f;
  inputs.motion_scale_y = 1.f;
  return true;
}

bool NrPipeline::RecordMotionCopy(ID3D12GraphicsCommandList* list, const nr::BoundResource& motion, float scale_x, float scale_y, bool flip_y,
                                  float offset_x, float offset_y, const std::shared_ptr<nr::Timeline::SubmissionProof>& producer) {
  PresentMotionCounters& counters = present_motion_counters_;
  ++counters.evaluates;
  counters.offset_x = offset_x;
  counters.offset_y = offset_y;
  const uint64_t current = timeline_.CurrentFrame();
  const uint64_t completed = timeline_.CompletedFrame();
  counters.lag_max = std::max(counters.lag_max, current > completed ? current - completed : 0u);
  if (session_.State() != nr::SessionState::ACTIVE || list == nullptr || motion.resource == nullptr) {
    ++counters.skipped_input;
    return false;
  }
  const D3D12_RESOURCE_DESC description = motion.resource->GetDesc();
  const std::optional<DXGI_FORMAT> view_format = MotionViewFormat(description.Format);
  nr::Rect region = motion.rect;
  if (region.width == 0u || region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  if (!view_format || uint64_t{region.x} + region.width > description.Width
      || uint64_t{region.y} + region.height > description.Height) {
    ++counters.skipped_input;
    return false;
  }
  // The present that closes the current frame binds the copy made for it (RecordPresent).
  const uint64_t frame = timeline_.CurrentFrame() + 1u;
  const uint32_t slot = timeline_.FindReusableSlot(slot_marks_, next_slot_);
  if (slot == slot_marks_.size()) {
    ++counters.skipped_ring;
    return false;
  }
  const nr::Size size = {region.width, region.height};
  if (motion_copies_.size != size || !motion_copies_.textures[0]) {
    RetireMotionCopies();
    if (!MakeMotionCopies(0u, PRESENT_MOTION_MIN_SLOTS, size)) {
      motion_copies_ = {};
      ++counters.skipped_input;
      return false;
    }
    motion_copies_.size = size;
  }
  const size_t preferred = PresentMotionSlotOf(frame, motion_copies_.slots);
  std::array<uint8_t, MOTION_COPY_SLOTS> writable{};
  const auto refresh_writable = [&](size_t first, size_t end) {
    for (size_t candidate = first; candidate < end; ++candidate) {
      // The writer's own game-list recording must be reset before this texture can be overwritten, and its GPU work must be complete.
      // Unrelated earlier executable lists do not own this slot; a Present read is one-shot and needs only execution completion.
      writable[candidate] = (timeline_.IsReusable(motion_copies_.last_write[candidate])
                             && timeline_.IsExecutionComplete(motion_copies_.last_read[candidate])) ? 1u : 0u;
    }
  };
  refresh_writable(0u, motion_copies_.slots);
  auto selected = PickPresentMotionWriteSlot(
      std::span<const uint64_t>{motion_copies_.frames.data(), motion_copies_.slots},
      std::span<const uint8_t>{writable.data(), motion_copies_.slots}, preferred);
  if (!selected && PresentMotionGrows(motion_copies_.slots, PresentMotionWrite::SLOT_BUSY)
      && MakeMotionCopies(motion_copies_.slots, MOTION_COPY_SLOTS, size)) {
    const size_t previous_slots = motion_copies_.slots;
    motion_copies_.slots = MOTION_COPY_SLOTS;
    refresh_writable(previous_slots, motion_copies_.slots);  // new slots have no writer or reader
    selected = PickPresentMotionWriteSlot(
        std::span<const uint64_t>{motion_copies_.frames.data(), motion_copies_.slots},
        std::span<const uint8_t>{writable.data(), motion_copies_.slots}, preferred);
    nr::Logf(nr::LogLevel::INFO, "Direct3D 12: DLSS's motion copies for Present grow from {} to {} slots",
             previous_slots, motion_copies_.slots);
  }
  if (!selected) {
    ++counters.skipped_slot;
    return false;
  }
  const size_t index = *selected;
  if (index != preferred) ++counters.slot_redirects;
  CommitSlot(slot, true);
  ID3D12Resource* const target = motion_copies_.textures[index].Get();
  Transition(list, target, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  color_.RecordMotion(list, slot,
                      {
                          .source = motion.resource,
                          .source_view_format = *view_format,
                          .region = region,
                          .target = target,
                          .image = size,
                          .canvas = size,
                          .scale_x = scale_x,
                          .scale_y = scale_y,
                          .offset_x = offset_x,
                          .offset_y = offset_y,
                          .flip_y = flip_y,
                      });
  Transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  ++counters.copies;
  motion_copies_.frames[index] = frame;
  motion_copies_.producers[index] = producer;
  motion_copies_.last_write[index] = slot_marks_[slot];
  return true;
}

bool NrPipeline::RecordDepthCopy(ID3D12GraphicsCommandList* list, const nr::BoundResource& depth, bool inverted,
                                 const std::shared_ptr<nr::Timeline::SubmissionProof>& producer) {
  if (session_.State() != nr::SessionState::ACTIVE || list == nullptr || depth.resource == nullptr) return false;
  const D3D12_RESOURCE_DESC description = depth.resource->GetDesc();
  const std::optional<DXGI_FORMAT> format = ReadDepthFormat(description.Format);
  nr::Rect region = depth.rect;
  if (region.width == 0u || region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  if (!format || description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1u
      || description.SampleDesc.Count != 1u || (description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0u
      || uint64_t{region.x} + region.width > description.Width || uint64_t{region.y} + region.height > description.Height) {
    WarnOnce(&depth_copies_.warned_format,
             "Direct3D 12: the game's depth (DXGI_FORMAT {}) cannot be read for DLSSNR.Depth; NR runs without it", static_cast<uint32_t>(description.Format));
    return false;
  }
  // The present that closes the current frame binds the copy made for it, as the motion copies do (RecordPresent).
  const uint64_t frame = timeline_.CurrentFrame() + 1u;
  const uint32_t slot = timeline_.FindReusableSlot(slot_marks_, next_slot_);
  if (slot == slot_marks_.size()) return false;
  const nr::Size size = {region.width, region.height};
  if (depth_copies_.size != size || !depth_copies_.textures[0]) {
    RetireDepthCopies();
    if (!MakeDepthCopies(0u, PRESENT_MOTION_MIN_SLOTS, size)) {
      depth_copies_ = {};
      return false;
    }
    depth_copies_.size = size;
  }
  const size_t index = PresentMotionSlotOf(frame, depth_copies_.slots);
  if (!timeline_.IsComplete(depth_copies_.last_use[index])) {
    // No growth (2026-11-07): the depth is not worth four more full-size textures, and a busy slot loses only this frame's copy -- the present falls
    // back to the newest copy within its age, and DLSSNR.Depth is optional (NR runs on its own estimate without it).
    return false;
  }
  CommitSlot(slot, true);
  ID3D12Resource* const target = depth_copies_.textures[index].Get();
  // Like the direct After DLSS input, depth is already a compute-readable DLSS input. A Load uses the same
  // plane and preserves the sampled depth value, without transitioning the game's resource or copying its stencil.
  Transition(list, target, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const bool copied = color_.RecordDepth(list, slot, depth.resource, *format, region, target);
  Transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  depth_copies_.frames[index] = copied ? frame : 0u;
  depth_copies_.inverted[index] = inverted;
  depth_copies_.producers[index] = producer;
  depth_copies_.last_use[index] = slot_marks_[slot];
  return copied;
}

nr::BoundResource NrPipeline::ConvertDlssMotion(ID3D12GraphicsCommandList* list, uint32_t slot, const nr::BoundResource& motion, float scale_x,
                                                float scale_y, bool flip_y) {
  const D3D12_RESOURCE_DESC description = motion.resource->GetDesc();
  const std::optional<DXGI_FORMAT> view_format = MotionViewFormat(description.Format);
  nr::Rect region = motion.rect;
  if (region.width == 0u || region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  if (!view_format || uint64_t{region.x} + region.width > description.Width || uint64_t{region.y} + region.height > description.Height) {
    WarnOnce(&dlss_present_motion_warned_, "Direct3D 12: DLSS's motion vectors for Present are unreadable (format {}); NR runs on zero motion",
             static_cast<uint32_t>(description.Format));
    return {};
  }
  const nr::Size size = {region.width, region.height};
  if (dlss_present_motion_.size != size || !dlss_present_motion_.texture) {
    RetireDlssPresentMotion();
    dlss_present_motion_.texture = CreateTexture(size, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, COMPUTE_READ,
                                                 L"Uplift DLSS motion for Present");
    if (!dlss_present_motion_.texture) {
      dlss_present_motion_ = {};
      WarnOnce(&dlss_present_motion_warned_, "Direct3D 12: no video memory for DLSS's motion copy for Present; NR runs on zero motion");
      return {};
    }
    dlss_present_motion_.size = size;
  }
  ID3D12Resource* const target = dlss_present_motion_.texture.Get();
  Transition(list, target, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const bool recorded = color_.RecordMotion(list, slot,
                                            {
                                                .source = motion.resource,
                                                .source_view_format = *view_format,
                                                .region = region,
                                                .target = target,
                                                .image = size,
                                                .canvas = size,
                                                .scale_x = scale_x,
                                                .scale_y = scale_y,
                                                .flip_y = flip_y,
                                            });
  Transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  dlss_present_motion_.last_use = slot_marks_[slot];
  if (!recorded) return {};  // nothing was written: NR runs with the zero motion, never with this texture's old or undefined contents
  return {.resource = target};
}

nr::BoundResource NrPipeline::ConvertLaunchpadMotion(ID3D12GraphicsCommandList* list, uint32_t slot, const nr::BoundResource& motion, float scale_x,
                                                     float scale_y, const WorkLayout& work) {
  const D3D12_RESOURCE_DESC description = motion.resource->GetDesc();
  const std::optional<DXGI_FORMAT> view_format = MotionViewFormat(description.Format);
  nr::Rect region = motion.rect;
  if (region.width == 0u || region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  if (!view_format || region.width == 0u || region.height == 0u || uint64_t{region.x} + region.width > description.Width
      || uint64_t{region.y} + region.height > description.Height || work.image.Empty()) {
    WarnOnce(&launchpad_motion_warned_, "Direct3D 12: Launchpad's motion vectors are unreadable; NR runs on zero motion");
    return {};
  }
  if (launchpad_motion_.size != work.image || !launchpad_motion_.texture) {
    RetireLaunchpadMotion();
    launchpad_motion_.texture = CreateTexture(work.image, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, COMPUTE_READ,
                                              L"Uplift Launchpad motion");
    if (!launchpad_motion_.texture) {
      launchpad_motion_ = {};
      WarnOnce(&launchpad_motion_warned_, "Direct3D 12: no video memory for Launchpad's motion conversion; NR runs on zero motion");
      return {};
    }
    launchpad_motion_.size = work.image;
  }
  ID3D12Resource* const target = launchpad_motion_.texture.Get();
  Transition(list, target, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  // UPLIFT_MV holds back-buffer (region) pixels: times MotionScale, and times image / region into the work image's pixels (Resolution below Full).
  const bool recorded = color_.RecordMotion(list, slot,
                                            {
                                                .source = motion.resource,
                                                .source_view_format = *view_format,
                                                .region = region,
                                                .target = target,
                                                .image = work.image,
                                                .canvas = work.image,
                                                .scale_x = scale_x * static_cast<float>(work.image.width) / static_cast<float>(region.width),
                                                .scale_y = scale_y * static_cast<float>(work.image.height) / static_cast<float>(region.height),
                                                .foveation = work.foveation,
                                            });
  Transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  launchpad_motion_.last_use = slot_marks_[slot];
  // Nothing was written (a slot out of range): NR runs with the zero motion, never with this texture's old or undefined contents.
  if (!recorded) return {};
  return {.resource = target};
}

}  // namespace uplift::sources
