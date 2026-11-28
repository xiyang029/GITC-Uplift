#include "sources/vk_nr_pipeline.hpp"

#include "sources/stage_sets.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>

#include "color/color_pipeline.hpp"
#include "memory_units.hpp"
#include "nr/log.hpp"
#include "nr/vk_handles.hpp"
#include "sources/pipeline_rules.hpp"

namespace uplift::sources {
namespace {

constexpr uint64_t MODEL_BYTES_PER_PIXEL = 8u;   // RGBA16F
constexpr uint64_t MOTION_BYTES_PER_PIXEL = 4u;  // RG16F
constexpr uint64_t STATE_BYTES = 16u;            // a 1x1 RGBA32F state image (the stabiliser's scene cut)
// The meter's state is 2x1 (the second texel is Auto's check), one 256 B slot of the readback ring each.
constexpr uint32_t EXPOSURE_STATE_WIDTH = 2u;
constexpr VkDeviceSize CHECK_STRIDE = 256u;
// A and B are NR's ping-pong images (colour in, output out) and the identity smokes' copies read and write them: storage, sampled, transfer. Before upscaling's
// private colour is the same (its smoke reads it back), and so is the change field (the look smoke reads it).
constexpr VkImageUsageFlags MODEL_USAGE =
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageUsageFlags MOTION_USAGE = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
// The look's images are written as storage images and read as sampled ones; the scene-cut state only as a storage image.
constexpr VkImageUsageFlags LOOK_USAGE = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
// The meter's state is written as a storage image and read by the encode and the decode as a sampled one. It is also copied out for Auto's check.
constexpr VkImageUsageFlags EXPOSURE_USAGE = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
// The mask copy is a copy's destination (ReShade's command list) and the decode samples it.
constexpr VkImageUsageFlags MASK_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
// A global dependency: whatever was written is visible to whatever reads or writes next.
constexpr VkMemoryBarrier GLOBAL_DEPENDENCY = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
};
// The game's motion vectors in a canvas are sampled as floats (D3D12's MotionViewFormat: unconverted ones cannot be copied there, tools/nrspike's e9).
constexpr std::array<VkFormat, 4> READABLE_MOTION_FORMATS = {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                             VK_FORMAT_R32G32B32A32_SFLOAT};
// The Present motion copies: RG16F, written by the motion copy, handed over sampled, and read by the bridge's copy-in.
constexpr VkImageUsageFlags PRESENT_MOTION_USAGE = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
// Native Present's staging image is only copied and blitted; SAMPLED is there because an image view (CreateNrImage makes one) needs a view usage.
constexpr VkImageUsageFlags PRESENT_STAGING_USAGE = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageSubresourceRange COLOR_RANGE = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
constexpr VkImageSubresourceLayers COLOR_LAYERS = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};

// The bytes per pixel of a swap-chain copy format (vk::VkFormatOf(vk::SharedFormatOf(...))'s display formats); 0 for any other.
uint64_t PresentBytesPerPixel(VkFormat format) {
  switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4u;
    case VK_FORMAT_R16G16B16A16_SFLOAT:      return 8u;
    case VK_FORMAT_R32G32B32A32_SFLOAT:      return 16u;
    default:                                 return 0u;
  }
}

// One layout transition of a whole single-mip, single-layer colour image.
VkImageMemoryBarrier ImageTransition(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags source_access,
                                     VkAccessFlags destination_access) {
  return {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = source_access,
      .dstAccessMask = destination_access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = COLOR_RANGE,
  };
}

// The formats the mask copy takes (color::DescribeMaskFormat's view formats, as Vulkan names them: the decode samples them raw), and their bytes.
struct MaskFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint64_t bytes_per_pixel = 0u;
};
constexpr std::array<MaskFormat, 10> MASK_FORMATS = {{
    {VK_FORMAT_R8_UNORM, 1u},
    {VK_FORMAT_R16_SFLOAT, 2u},
    {VK_FORMAT_R16_UNORM, 2u},
    {VK_FORMAT_R32_SFLOAT, 4u},
    {VK_FORMAT_R8G8_UNORM, 2u},
    {VK_FORMAT_R16G16_SFLOAT, 4u},
    {VK_FORMAT_R8G8B8A8_UNORM, 4u},
    {VK_FORMAT_B8G8R8A8_UNORM, 4u},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4u},
    {VK_FORMAT_R16G16B16A16_SFLOAT, 8u},
}};

// The readback ring's buffer and memory (either may be null). Device idle for them, or the GPU past their last copy.
void DestroyCheckBuffer(const vk::NrFunctions& functions, VkDevice device, VkBuffer buffer, VkDeviceMemory memory) {
  if (buffer != VK_NULL_HANDLE) {
    functions.vkDestroyBuffer(device, buffer, nullptr);
  }
  if (memory != VK_NULL_HANDLE) {
    functions.vkFreeMemory(device, memory, nullptr);  // unmaps it
  }
}

}  // namespace

VkNrPipeline::VkNrPipeline(const vk::NrFunctions& functions, VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, nr::Session& session,
                           nr::Timeline& timeline)
    : functions_(functions), device_(device), session_(session), timeline_(timeline), memory_(memory) {}

VkNrPipeline::~VkNrPipeline() {
  for (StageSet& set : sets_) {
    for (vk::NrImage* image : {&set.intermediates.a, &set.intermediates.b, &set.intermediates.zero_motion, &set.intermediates.canvas_motion,
                               &set.intermediates.private_color, &set.intermediates.change, &set.look.basis, &set.look.gauss, &set.look.peaks,
                               &set.look.history[0], &set.look.history[1], &set.look.detail_history[0], &set.look.detail_history[1], &set.look.state,
                               &set.look.change_history[0], &set.look.change_history[1], &set.exposure.state, &set.faces.twin,
                               &set.faces.detail,
                               &set.faces.atlas, &set.faces.fill}) {
      vk::DestroyNrImage(functions_, device_, image);
    }
    DestroyCheckBuffer(functions_, device_, set.check.buffer, set.check.memory);
  }
  for (vk::NrImage* image : {&mask_.image, &present_.staging, &present_.color, &present_.launchpad, &present_.stand_in}) {
    vk::DestroyNrImage(functions_, device_, image);
  }
  for (PresentMotionSlot& slot : present_motion_) {
    vk::DestroyNrImage(functions_, device_, &slot.image);
  }
  for (RetiredImage& retired : reshade_retired_) {
    vk::DestroyNrImage(functions_, device_, &retired.image);
  }
}

uint64_t VkNrPipeline::HeldBytes() const {
  uint64_t sets = 0u;
  for (const StageSet& set : sets_) {
    const Intermediates& surfaces = set.intermediates;
    const uint64_t motion_images = (surfaces.zero_motion.image != VK_NULL_HANDLE ? 1u : 0u) + (surfaces.canvas_motion.image != VK_NULL_HANDLE ? 1u : 0u);
    sets += (surfaces.a.image != VK_NULL_HANDLE ? surfaces.bytes : 0u) + motion_images * surfaces.motion_bytes + set.look.bytes
            + (set.exposure.state.image != VK_NULL_HANDLE ? EXPOSURE_STATE_BYTES : 0u) + set.faces.bytes;
  }
  return sets + present_motion_bytes_ + (mask_.image.image != VK_NULL_HANDLE ? mask_.bytes : 0u) + present_.bytes + present_.launchpad_bytes;
}

bool VkNrPipeline::Initialize(std::string* error) {
  return color_.Initialize(functions_, device_, memory_, error);
}

bool VkNrPipeline::PreSrColorFormatSupported(VkFormat format) {
  return color::VkColorPipeline::OutputVariantOf(format).has_value() || format == VK_FORMAT_B8G8R8A8_UNORM;
}

bool VkNrPipeline::MotionFormatReadable(VkFormat format) {
  return std::ranges::find(READABLE_MOTION_FORMATS, format) != READABLE_MOTION_FORMATS.end();
}

void VkNrPipeline::SetLookConfig(const LookConfig& config) {
  ApplyLookConfig(config_, config, sets_);
}

void VkNrPipeline::RetireSetsFrom(uint32_t first) {
  RetireStageSets(*this, sets_, first, &VkNrPipeline::RetireSet, &VkNrPipeline::RetireLook,
                  &VkNrPipeline::RetireFaces, &VkNrPipeline::RetireExposure);
}

void VkNrPipeline::RetireSet(StageSet* set) {
  if (set->intermediates.a.image == VK_NULL_HANDLE) return;
  // Every recording takes its mark before its first command, so once the GPU has passed the newest one the set goes at once; otherwise the
  // timeline's callback keeps the handles until then.
  auto destroy = [functions = functions_, device = device_,
                  images = std::array<vk::NrImage, 6>{set->intermediates.a, set->intermediates.b, set->intermediates.zero_motion, set->intermediates.canvas_motion,
                                                      set->intermediates.private_color, set->intermediates.change}]() mutable {  // all but A and B may be null
    for (vk::NrImage& image : images) {
      vk::DestroyNrImage(functions, device, &image);
    }
  };
  if (timeline_.IsComplete(set->intermediates.last_use)) {
    destroy();
  } else {
    timeline_.ReleaseAfter(set->intermediates.last_use, std::move(destroy));
  }
  set->intermediates = {};
}

void VkNrPipeline::RetireLook(StageSet* set) {
  if (set->look.plan == LookPlan{}) return;
  auto destroy = [functions = functions_, device = device_,
                  images = std::array<vk::NrImage, 10>{set->look.basis, set->look.gauss, set->look.peaks, set->look.history[0], set->look.history[1],
                                                       set->look.detail_history[0], set->look.detail_history[1], set->look.state,
                                                       set->look.change_history[0], set->look.change_history[1]}]() mutable {  // the plan's only
    for (vk::NrImage& image : images) {
      vk::DestroyNrImage(functions, device, &image);
    }
  };
  if (timeline_.IsComplete(set->look.last_use)) {
    destroy();
  } else {
    timeline_.ReleaseAfter(set->look.last_use, std::move(destroy));
  }
  set->look = {};
}

void VkNrPipeline::RetireFaces(StageSet* set) {
  if (set->faces.twin.image == VK_NULL_HANDLE) return;
  auto destroy = [functions = functions_, device = device_, images = std::array<vk::NrImage, 4>{set->faces.twin, set->faces.detail, set->faces.atlas, set->faces.fill}]() mutable {
    for (vk::NrImage& image : images) {
      vk::DestroyNrImage(functions, device, &image);
    }
  };
  if (timeline_.IsComplete(set->faces.last_use)) {
    destroy();
  } else {
    timeline_.ReleaseAfter(set->faces.last_use, std::move(destroy));
  }
  set->faces = {};
}

bool VkNrPipeline::EnsureFaces(nr::Size canvas) {
  if (set_->faces.twin.image != VK_NULL_HANDLE && set_->faces.size == canvas) return true;
  RetireFaces(set_);
  FaceSurfaces created = {.layout = look::MakeAtlas(canvas), .size = canvas};
  created.bytes = FaceSurfaceBytes(canvas);
  // The twin is an NGX output like A and B (read_write); the atlas is the faces pass's own, like the look's.
  const bool made =
      vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, canvas.width, canvas.height, MODEL_USAGE, true, &created.twin)
      && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, canvas.width, canvas.height, LOOK_USAGE, false, &created.detail)
      && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, created.layout.size.width, created.layout.size.height, LOOK_USAGE,
                           false, &created.atlas)
      && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, created.layout.size.width, created.layout.size.height, LOOK_USAGE,
                           false, &created.fill);
  if (!made) {
    for (vk::NrImage* image : {&created.twin, &created.detail, &created.atlas, &created.fill}) {  // never used by the GPU: freed here
      vk::DestroyNrImage(functions_, device_, image);
    }
    if (!allocation_failure_logged_) {
      nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} Keep faces surfaces (Vulkan)", canvas.width, canvas.height);
      allocation_failure_logged_ = true;
    }
    return false;
  }
  allocation_failure_logged_ = false;
  set_->faces = std::move(created);
  return true;
}

void VkNrPipeline::PrepareFaces(nr::Size canvas, const nr::Controls& controls, const nr::StageSlice& slice, nr::FrameInputs* frame_inputs) {
  faces_ran_ = false;
  faces_first_ = slice.first;
  // Paused by the Session, its surfaces go; a frame whose surfaces cannot be made only goes without the twin.
  if (config_.keep_faces.enabled && look_storage_ && color_.FacesReady()) {
    frame_inputs->faces = {.wanted = true, .controls = FaceTwinControls(controls, config_.keep_faces.protection), .surface_bytes = FaceSurfaceBytes(canvas)};
    if (session_.FacesPaused(slice.stage)) {
      RetireFaces(set_);
    } else if (EnsureFaces(canvas)) {
      frame_inputs->faces.output = nr::AsResource(&set_->faces.twin.ngx);
    }
  } else {
    RetireFaces(set_);
  }
}

void VkNrPipeline::RecordFaces(VkCommandBuffer buffer, color::VkFacesPass pass) {
  pass.atlas = set_->faces.atlas.view;
  pass.detail = set_->faces.detail.view;
  pass.fill = set_->faces.fill.view;
  pass.layout = set_->faces.layout;
  pass.parameters = look::MakeFacesParameters(config_.keep_faces.lighting_scale, config_.keep_faces.tuning, set_->intermediates.sizes.image.height, set_->faces.layout.levels);
  set_->faces.last_use = slot_marks_[resolve_slot_];
  Barrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);  // what wrote the pass's images (NGX) is visible to the pyramid
  color_.RecordFacesPyramid(buffer, resolve_slot_, pass);
  if (pass.parameters.speck_radius != 0u) {
    color_.RecordFacesDespike(buffer, resolve_slot_, pass);  // before the combine rewrites `changed` (and pass 1's twin) in place
  }
  Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  color_.RecordFacesCombine(buffer, resolve_slot_, pass);  // the Session's next transition, or the barrier after NR, makes its writes visible
}

void VkNrPipeline::CombineFaces(ID3D12GraphicsCommandList* list, ID3D12Resource* lighting, ID3D12Resource* faces) {
  RecordFaces(nr::VkListOf(list), {.changed = nr::VkResourceOf(lighting)->Resource.ImageViewInfo.ImageView,
                                   .reference = nr::VkResourceOf(faces)->Resource.ImageViewInfo.ImageView});
  faces_ran_ = true;
}

void VkNrPipeline::RetireMask() {
  RetireReshadeImage(mask_.image, mask_.last_use);  // the add-on copies into it on ReShade's queue
  mask_ = {};
}

void VkNrPipeline::RetireReshadeImage(const vk::NrImage& image, const nr::Mark& mark) {
  if (image.image == VK_NULL_HANDLE) return;
  if (timeline_.IsComplete(mark)) {
    vk::NrImage destroyed = image;
    vk::DestroyNrImage(functions_, device_, &destroyed);
  } else {
    reshade_retired_.push_back({.image = image, .mark = mark});
  }
}

size_t VkNrPipeline::FreeFinished() {
  std::erase_if(reshade_retired_, [this](const RetiredImage& retired) {
    if (!timeline_.IsComplete(retired.mark)) return false;
    vk::NrImage destroyed = retired.image;
    vk::DestroyNrImage(functions_, device_, &destroyed);
    return true;
  });
  return reshade_retired_.size();
}

void VkNrPipeline::RetireExposure(StageSet* set) {
  if (set->exposure.state.image != VK_NULL_HANDLE) {
    auto destroy = [functions = functions_, device = device_, image = set->exposure.state]() mutable { vk::DestroyNrImage(functions, device, &image); };
    if (timeline_.IsComplete(set->exposure.last_use)) {
      destroy();
    } else {
      timeline_.ReleaseAfter(set->exposure.last_use, std::move(destroy));
    }
  }
  set->exposure = {};
  RetireExposureCheck(set);
}

void VkNrPipeline::RetireExposureCheck(StageSet* set) {
  if (set->check.buffer != VK_NULL_HANDLE) {
    // The samples still in flight are dropped; the ring goes once the GPU has passed the newest copy it has not finished.
    std::optional<nr::Mark> in_flight;
    for (uint32_t step = 1u; step <= CHECK_SLOTS && !in_flight; ++step) {
      const uint32_t index = (set->check.next + CHECK_SLOTS - step) % CHECK_SLOTS;  // the newest first
      if (set->check.pending[index] && !timeline_.IsComplete(set->check.marks[index])) {
        in_flight = set->check.marks[index];
      }
    }
    auto destroy = [functions = functions_, device = device_, buffer = set->check.buffer, memory = set->check.memory] { DestroyCheckBuffer(functions, device, buffer, memory); };
    if (in_flight) {
      timeline_.ReleaseAfter(*in_flight, std::move(destroy));
    } else {
      destroy();
    }
  }
  set->check = {};
  set->auto_exposure.DiscardPending();
}

bool VkNrPipeline::EnsureExposureCheck() {
  if (set_->check.buffer != VK_NULL_HANDLE) return true;
  const VkBufferCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = CHECK_SLOTS * CHECK_STRIDE,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  ExposureCheck made;
  bool worked = (functions_.vkCreateBuffer(device_, &create, nullptr, &made.buffer) == VK_SUCCESS);
  if (worked) {
    VkMemoryRequirements requirements = {};
    functions_.vkGetBufferMemoryRequirements(device_, made.buffer, &requirements);
    uint32_t type = vk::FindMemoryType(memory_, requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX) {
      type = vk::FindMemoryType(memory_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type};
    void* mapped = nullptr;
    worked = (type != UINT32_MAX && functions_.vkAllocateMemory(device_, &allocate, nullptr, &made.memory) == VK_SUCCESS
              && functions_.vkBindBufferMemory(device_, made.buffer, made.memory, 0u) == VK_SUCCESS
              && functions_.vkMapMemory(device_, made.memory, 0u, VK_WHOLE_SIZE, 0u, &mapped) == VK_SUCCESS);
    made.mapped = static_cast<const std::byte*>(mapped);
  }
  if (!worked) {
    DestroyCheckBuffer(functions_, device_, made.buffer, made.memory);  // never used by the GPU
    return false;
  }
  set_->check = made;
  return true;
}

void VkNrPipeline::PollExposureChecks() {
  if (set_->check.buffer == VK_NULL_HANDLE) return;
  for (uint32_t step = 0u; step < CHECK_SLOTS; ++step) {
    const uint32_t index = (set_->check.next + step) % CHECK_SLOTS;  // the oldest first
    if (!set_->check.pending[index] || !timeline_.IsComplete(set_->check.marks[index])) continue;
    set_->check.pending[index] = false;
    std::array<float, METER_SAMPLE_FLOATS> texels = {};
    std::memcpy(texels.data(), set_->check.mapped + index * CHECK_STRIDE, sizeof(texels));
    ObserveExposureSample(set_->auto_exposure, ExposureSampleFromTexels(texels), set_->check.recorded_seconds[index], config_.fixes.auto_blend);
  }
}

void VkNrPipeline::ReleaseIntermediates() {
  ReleaseNrSurfaces();
  RetireMask();
  RetirePresent();
}

void VkNrPipeline::RetirePresent() {
  // Used in Uplift's own command buffers on ReShade's queue alone, which no completion token covers.
  RetireReshadeImage(present_.staging, present_.last_use);
  RetireReshadeImage(present_.color, present_.last_use);
  RetireReshadeImage(present_.stand_in, present_.last_use);
  RetirePresentLaunchpad();
  present_ = {};
}

void VkNrPipeline::RetirePresentLaunchpad() {
  RetireReshadeImage(present_.launchpad, present_.launchpad_last_use);
  present_.launchpad = {};
  present_.launchpad_size = {};
  present_.launchpad_bytes = 0u;
}

void VkNrPipeline::ReleaseNrSurfaces() {
  RetireSetsFrom(0u);
}

void VkNrPipeline::LogAllocationFailure(nr::Size size) {
  if (allocation_failure_logged_) return;
  nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} NR surfaces (Vulkan)", size.width, size.height);
  allocation_failure_logged_ = true;
}

LookPlan VkNrPipeline::LookPlanFor(nr::Size image) const {
  return PlanLook(config_, image, look_storage_);
}

bool VkNrPipeline::EnsureIntermediates(const SetSizes& sizes, bool pre_sr, const nr::foveation::Grid& foveation) {
  auto& held = set_->intermediates;
  if (held.a.image != VK_NULL_HANDLE && held.foveation != foveation) {
    held.foveation = foveation;
    held.reset_pending = true;
    set_->look.history_valid = false;
    set_->look.change_valid = false;
  }
  if (held.a.image != VK_NULL_HANDLE && held.pre_sr == pre_sr && held.sizes == sizes) return true;
  const bool same_canvas = held.a.image != VK_NULL_HANDLE && held.sizes.canvas == sizes.canvas;
  Intermediates created = {
      .a = same_canvas ? held.a : vk::NrImage{},
      .b = same_canvas ? held.b : vk::NrImage{},
      .zero_motion = same_canvas ? held.zero_motion : vk::NrImage{},
      .canvas_motion = same_canvas ? held.canvas_motion : vk::NrImage{},
      .private_color = held.sizes.private_color == sizes.private_color ? held.private_color : vk::NrImage{},
      .change = held.sizes.image == sizes.image ? held.change : vk::NrImage{},
      .sizes = sizes, .pre_sr = pre_sr, .foveation = foveation,
      .motion_bytes = sizes.canvas.Pixels() * MOTION_BYTES_PER_PIXEL,
      .last_use = held.last_use,
  };
  std::array<vk::NrImage*, 3> allocated{};
  size_t count = 0u;
  const auto make = [&](vk::NrImage* image, nr::Size size, bool ngx) {
    if (image->image != VK_NULL_HANDLE || size.Empty()) return true;
    allocated[count++] = image;
    return vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, size.width, size.height, MODEL_USAGE, ngx, image);
  };
  if (!make(&created.a, sizes.canvas, true) || !make(&created.b, sizes.canvas, true)
      || !make(&created.private_color, sizes.private_color, false)) {
    for (size_t i = 0; i < count; ++i) vk::DestroyNrImage(functions_, device_, allocated[i]);
    LogAllocationFailure(sizes.canvas);
    return false;
  }
  created.bytes = (2u * sizes.canvas.Pixels() + sizes.private_color.Pixels()
                   + (created.change.image != VK_NULL_HANDLE ? sizes.image.Pixels() : 0u)) * MODEL_BYTES_PER_PIXEL;
  // Transfer unchanged handles to the new set; only replaced surfaces enter deferred destruction.
  std::array<vk::NrImage, 6> retired = {held.a, held.b, held.zero_motion, held.canvas_motion, held.private_color, held.change};
  const std::array<vk::NrImage, 6> kept = {created.a, created.b, created.zero_motion, created.canvas_motion, created.private_color, created.change};
  bool any = false;
  for (size_t i = 0; i < retired.size(); ++i) {
    if (retired[i].image == kept[i].image) retired[i] = {};
    any |= retired[i].image != VK_NULL_HANDLE;
  }
  if (any) {
    auto destroy = [functions = functions_, device = device_, retired]() mutable {
      for (auto& image : retired) vk::DestroyNrImage(functions, device, &image);
    };
    if (timeline_.IsComplete(held.last_use)) destroy();
    else timeline_.ReleaseAfter(held.last_use, std::move(destroy));
  }
  set_->look.history_valid = false;
  set_->look.change_valid = false;
  held = std::move(created);
  allocation_failure_logged_ = false;
  return true;
}

bool VkNrPipeline::EnsureChange(bool required) {
  auto& set = set_->intermediates;
  const auto bytes = set.sizes.image.Pixels() * MODEL_BYTES_PER_PIXEL;
  if (!required) {
    if (set.change.image != VK_NULL_HANDLE) {
      auto destroy = [functions = functions_, device = device_, image = set.change]() mutable { vk::DestroyNrImage(functions, device, &image); };
      if (timeline_.IsComplete(set.last_use)) destroy();
      else timeline_.ReleaseAfter(set.last_use, std::move(destroy));
      set.change = {};
      set.bytes -= bytes;
    }
    return true;
  }
  if (set.change.image != VK_NULL_HANDLE) return true;
  if (!vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, set.sizes.image.width, set.sizes.image.height,
                         MODEL_USAGE, false, &set.change)) {
    LogAllocationFailure(set.sizes.image);  // the caller only learns OUT_OF_MEMORY: the size and the budget are logged here
    return false;
  }
  set.bytes += bytes;
  return true;
}

bool VkNrPipeline::EnsureLook(const LookPlan& plan) {
  const look::Atlas atlas = plan.shape ? look::MakeAtlas(plan.image) : look::Atlas{};
  const nr::Size history_size = plan.stabilize ? look::LowHistorySize(atlas, config_.look.detail_radius) : nr::Size{};
  if (plan == set_->look.plan && history_size == set_->look.history_size) return true;
  RetireLook(set_);
  if (plan == LookPlan{}) return true;
  LookSurfaces created = {.plan = plan, .history_size = history_size, .atlas = atlas};
  bool made = true;
  const auto make = [&](bool wanted, nr::Size size, VkFormat format, uint64_t bytes_per_pixel, VkImageUsageFlags usage, vk::NrImage* image) {
    if (!wanted || !made) return;
    created.bytes += size.Pixels() * bytes_per_pixel;
    made = vk::CreateNrImage(functions_, device_, memory_, format, size.width, size.height, usage, false, image);
  };
  make(plan.shape, plan.image, VK_FORMAT_R16G16_SFLOAT, 4u, LOOK_USAGE, &created.basis);
  make(plan.shape, created.atlas.size, VK_FORMAT_R16G16B16A16_SFLOAT, MODEL_BYTES_PER_PIXEL, LOOK_USAGE, &created.gauss);
  make(plan.shape, created.atlas.size, VK_FORMAT_R16_SFLOAT, 2u, LOOK_USAGE, &created.peaks);
  for (vk::NrImage& history : created.history) {
    make(plan.stabilize, history_size, VK_FORMAT_R16G16B16A16_SFLOAT, MODEL_BYTES_PER_PIXEL, LOOK_USAGE, &history);
  }
  for (vk::NrImage& history : created.detail_history) {
    make(plan.detail, plan.image, VK_FORMAT_R16G16B16A16_SFLOAT, MODEL_BYTES_PER_PIXEL, LOOK_USAGE, &history);
  }
  make(plan.stabilize, {1u, 1u}, VK_FORMAT_R32G32B32A32_SFLOAT, STATE_BYTES, VK_IMAGE_USAGE_STORAGE_BIT, &created.state);
  // The change field's own stabiliser, at the work image whatever the look does with it.
  for (vk::NrImage& history : created.change_history) {
    make(plan.change, plan.image, VK_FORMAT_R16G16B16A16_SFLOAT, MODEL_BYTES_PER_PIXEL, LOOK_USAGE, &history);
  }
  if (!made) {
    // Never used by the GPU: freed here.
    for (vk::NrImage* image : {&created.basis, &created.gauss, &created.peaks, &created.history[0], &created.history[1],
                               &created.detail_history[0], &created.detail_history[1], &created.state, &created.change_history[0],
                               &created.change_history[1]}) {
      vk::DestroyNrImage(functions_, device_, image);
    }
    if (!allocation_failure_logged_) {
      nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} look surfaces (Vulkan)", plan.image.width, plan.image.height);
      allocation_failure_logged_ = true;
    }
    return false;
  }
  allocation_failure_logged_ = false;
  set_->look = std::move(created);
  return true;
}

color::VkSampledView VkNrPipeline::BoundMask(uint32_t slot) {
  if (!config_.mask || !mask_.copied) return {};
  mask_.last_use = slot_marks_[slot];
  return {.view = mask_.image.view, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

VkImage VkNrPipeline::PrepareMaskCopy(VkFormat format, nr::Size size, bool* first) {
  const auto found = std::ranges::find(MASK_FORMATS, format, &MaskFormat::format);
  if (found == MASK_FORMATS.end() || size.Empty()) {
    RetireMask();
    return VK_NULL_HANDLE;
  }
  if (mask_.image.image == VK_NULL_HANDLE || mask_.size != size || mask_.format != format) {
    RetireMask();
    MaskCopy created = {.size = size, .format = format, .bytes = size.Pixels() * found->bytes_per_pixel};
    if (!vk::CreateNrImage(functions_, device_, memory_, format, size.width, size.height, MASK_USAGE, false, &created.image)) {
      LogAllocationFailure(size);
      return VK_NULL_HANDLE;
    }
    allocation_failure_logged_ = false;
    mask_ = std::move(created);
  }
  mask_.last_use = timeline_.MarkNow();  // the add-on copies into it on this frame's list of ReShade's queue
  *first = !mask_.copied;
  return mask_.image.image;
}

void VkNrPipeline::Barrier(VkCommandBuffer buffer, VkPipelineStageFlags source_stage) const {
  functions_.vkCmdPipelineBarrier(buffer, source_stage, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 1u, &GLOBAL_DEPENDENCY, 0u, nullptr, 0u, nullptr);
}

void VkNrPipeline::OpenRecording(VkCommandBuffer buffer, std::span<const vk::NrImage* const> images, bool with_look) {
  std::array<VkImageMemoryBarrier, MAX_OPENED> transitions = {};
  uint32_t count = 0u;
  const auto open = [&transitions, &count](const vk::NrImage& image) {
    if (image.image == VK_NULL_HANDLE || count == transitions.size()) return;
    transitions[count++] = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
    };
  };
  for (const vk::NrImage* image : images) {
    open(*image);
  }
  if (with_look) {
    for (const vk::NrImage* image : {&set_->look.basis, &set_->look.gauss, &set_->look.peaks}) {
      open(*image);
    }
    // The stabiliser's histories and scene-cut state carry across frames: GENERAL from their first recording on (a reset clears them in the shader).
    if (!set_->look.carried) {
      for (const vk::NrImage* image : {&set_->look.history[0], &set_->look.history[1], &set_->look.detail_history[0], &set_->look.detail_history[1], &set_->look.state}) {
        open(*image);
      }
      // The change field's pair carries too.
      if (set_->look.plan.change) {
        open(set_->look.change_history[0]);
        open(set_->look.change_history[1]);
      }
      set_->look.carried = true;
    }
  }
  // The meter's state carries across frames likewise (its first meter snaps, whatever it holds).
  if (set_->exposure.state.image != VK_NULL_HANDLE && !set_->exposure.carried) {
    open(set_->exposure.state);
    set_->exposure.carried = true;
  }
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 1u, &GLOBAL_DEPENDENCY, 0u, nullptr,
                                  count, transitions.data());
  color_.PrepareRecording(buffer);
}

void VkNrPipeline::HandToNgx(VkCommandBuffer buffer, const vk::NrImage& image) const {
  const VkImageMemoryBarrier handed = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image.image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
  };
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 1u, &GLOBAL_DEPENDENCY, 0u, nullptr,
                                  1u, &handed);
}

color::VkSampledView VkNrPipeline::GameExposureView(const NVSDK_NGX_Resource_VK* exposure) {
  if (exposure == nullptr || exposure->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return {};
  return {.view = exposure->Resource.ImageViewInfo.ImageView, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

void VkNrPipeline::NoteMeterBasis(uint8_t stage, color::Encoding encoding, float input_scale) {
  if (UpdateMeterBasis(set_->exposure.basis, set_->exposure.snap, stage, encoding, config_.fixes.primaries, input_scale)) {
    RetireExposureCheck(set_);
  }
}

VkNrPipeline::ExposureChoice VkNrPipeline::ChooseExposure(color::Encoding encoding, const NVSDK_NGX_Resource_VK* game_texture, float game_factor) {
  const bool game_exposure = (game_texture != nullptr || game_factor != 1.f);
  // Only relative scene-linear images are metered, and the meter needs the look's storage formats (its state is RGBA32F).
  const bool can_meter = (color::Meterable(encoding) && look_storage_);
  ExposureDecision decision = DecideExposure(config_.fixes, game_exposure, can_meter, set_->auto_exposure.Latched());
  if (decision.Probes()) {
    PollExposureChecks();
    decision = DecideExposure(config_.fixes, game_exposure, can_meter, set_->auto_exposure.Latched());
  }
  if (!decision.Probes()) RetireExposureCheck(set_);
  if (!decision.WantsState() && set_->exposure.state.image != VK_NULL_HANDLE) {
    RetireExposure(set_);  // InputExposure left Metered or auto-metered: the state is no longer read
  }
  if (decision.WantsState() && set_->exposure.state.image == VK_NULL_HANDLE) {
    if (vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R32G32B32A32_SFLOAT, EXPOSURE_STATE_WIDTH, 1u, EXPOSURE_USAGE, false, &set_->exposure.state)) {
      set_->exposure.snap = true;  // undefined until the meter's first snap
    } else {
      LogAllocationFailure({EXPOSURE_STATE_WIDTH, 1u});
    }
  }
  const ExposureOutcome outcome =
      PickExposure(decision, config_.fixes, game_exposure, set_->exposure.state.image != VK_NULL_HANDLE, set_->auto_exposure);
  set_->exposure_report = outcome.report;
  ExposureChoice choice = {.metered = outcome.metered, .probe = outcome.probe, .game_weight = outcome.game_weight};
  switch (outcome.source) {
    case ExposureSource::STATE: choice.view = {.view = set_->exposure.state.view, .layout = VK_IMAGE_LAYOUT_GENERAL}; break;
    case ExposureSource::GAME:
      choice.view = GameExposureView(game_texture);
      choice.factor = game_factor;
      break;
    case ExposureSource::NONE: break;
  }
  if (outcome.with_game) {
    choice.game_view = GameExposureView(game_texture);
    choice.game_factor = game_factor;
  }
  return choice;
}

bool VkNrPipeline::RecordMeter(VkCommandBuffer buffer, uint32_t slot, color::VkMeterPass pass, const ExposureChoice& exposure) {
  pass.state = set_->exposure.state.view;
  pass.snap = set_->exposure.snap;
  pass.smooth = config_.fixes.smooth_adapt;
  pass.brighter_rate = config_.fixes.adapt_brighter;
  pass.darker_rate = config_.fixes.adapt_darker;
  pass.frame_seconds = config_.frame_seconds;
  pass.probe = exposure.probe;
  pass.game_exposure = (exposure.probe ? exposure.game_view : color::VkSampledView{});
  pass.game_exposure_factor = (exposure.probe ? exposure.game_factor : 1.f);
  pass.game_weight = (exposure.probe ? exposure.game_weight : 0.f);
  set_->exposure.last_use = slot_marks_[slot];
  if (!color_.RecordMeter(buffer, slot, pass)) return false;
  Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);  // the encode reads the state (and Auto's check copies it)
  set_->exposure.snap = false;
  if (!exposure.probe) return true;
  // Auto's check. The state's two texels into the next readback slot (GENERAL is a legal copy source), unless the GPU has not passed that slot's
  // last copy yet (skipped); then the host may read it once the recording completes.
  const uint32_t index = set_->check.next;
  if (set_->check.pending[index] || !EnsureExposureCheck()) return true;
  const VkBufferImageCopy copy = {
      .bufferOffset = index * CHECK_STRIDE,
      .bufferRowLength = 0u,
      .bufferImageHeight = 0u,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
      .imageOffset = {0, 0, 0},
      .imageExtent = {EXPOSURE_STATE_WIDTH, 1u, 1u},
  };
  functions_.vkCmdCopyImageToBuffer(buffer, set_->exposure.state.image, VK_IMAGE_LAYOUT_GENERAL, set_->check.buffer, 1u, &copy);
  const VkMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0u, 1u, &to_host, 0u, nullptr, 0u, nullptr);
  set_->check.recorded_seconds[index] = ExposureCheckSeconds();
  set_->check.marks[index] = slot_marks_[slot];
  set_->check.pending[index] = true;
  set_->check.next = (index + 1u) % CHECK_SLOTS;
  return true;
}

void VkNrPipeline::ResolvePass(ID3D12GraphicsCommandList* list, uint32_t index, ID3D12Resource* given, ID3D12Resource* returned) {
  if (index == 0u || index > LATER_PASSES || !look_storage_) return;
  const VkImageView given_view = nr::VkResourceOf(given)->Resource.ImageViewInfo.ImageView;
  const VkImageView returned_view = nr::VkResourceOf(returned)->Resource.ImageViewInfo.ImageView;
  if (faces_ran_ && index != faces_first_) {
    // Keep faces: inside pass 1's face mask only this pass's broad change is kept. Split chain: a later group's own mask,
    // from its first pass's twin, whose recombination already made that pass's result.
    RecordFaces(nr::VkListOf(list), {.changed = returned_view, .reference = given_view, .mask = set_->faces.twin.view});
  }
}

static color::VkStabilizeMotion SampleMotion(const NVSDK_NGX_ImageViewInfo_VK& image, nr::Rect rect,
                                             const nr::FrameInputs& inputs) {
  return {
      .vectors = {.view = image.ImageView, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      .rect = rect,
      .scale_x = inputs.motion_scale_x,
      .scale_y = inputs.motion_scale_y,
  };
}

color::VkStabilizeMotion VkNrPipeline::StabilizeMotionOf(const nr::FrameInputs& inputs) {
  if (inputs.motion.resource == nullptr) return {};
  const NVSDK_NGX_Resource_VK& motion = *nr::VkResourceOf(inputs.motion.resource);
  if (motion.Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return {};
  const NVSDK_NGX_ImageViewInfo_VK& image = motion.Resource.ImageViewInfo;
  const std::optional<nr::Rect> rect = MotionRectOf(inputs.motion.rect, {image.Width, image.Height});
  if (!MotionFormatReadable(image.Format) || !rect) return {};
  return SampleMotion(image, *rect, inputs);
}

bool VkNrPipeline::ConvertWorkMotion(VkCommandBuffer buffer, uint32_t slot, nr::FrameInputs& inputs,
                                     const color::VkStabilizeMotion& source, const WorkLayout& work) {
  const vk::NrImage& motion = set_->intermediates.canvas_motion;
  if (source.vectors.view == VK_NULL_HANDLE || motion.view == VK_NULL_HANDLE || source.rect.width == 0u || source.rect.height == 0u) return false;
  if (!color_.RecordMotion(buffer, slot,
      {.source = source.vectors, .region = source.rect, .target = motion.view, .image = work.image, .canvas = work.image,
       .scale_x = source.scale_x * static_cast<float>(work.image.width) / static_cast<float>(source.rect.width),
       .scale_y = source.scale_y * static_cast<float>(work.image.height) / static_cast<float>(source.rect.height),
       .foveation = work.foveation})) return false;
  HandToNgx(buffer, motion);
  inputs.motion = {.resource = nr::AsResource(&motion.ngx)};
  inputs.motion_scale_x = 1.f;
  inputs.motion_scale_y = 1.f;
  return true;
}

float VkNrPipeline::NoteEncoding(const VkDlssTarget& target) {
  const float input_scale = color::InputScale(target.encoding, target.diffuse_white_nits, config_.fixes.linear_unit_nits);
  const color::Primaries primaries = color::ResolvePrimaries(config_.fixes.primaries, target.encoding);
  // Model and Look histories belong to one colour basis, independently in each stage set.
  if (target.encoding != set_->intermediates.encoding || primaries != set_->intermediates.primaries || input_scale != set_->intermediates.input_scale) {
    set_->intermediates.encoding = target.encoding;
    set_->intermediates.primaries = primaries;
    set_->intermediates.input_scale = input_scale;
    set_->intermediates.reset_pending = true;
  }
  return input_scale;
}

VkNrPipeline::AfterNr VkNrPipeline::RecordAfterNr(VkCommandBuffer buffer, uint32_t slot, const color::VkEncodePass& encode, VkImageView nr_output, uint32_t passes_run,
                                                  bool reduced, bool look_ready, color::Upsampling upsampling, const color::VkStabilizeMotion& motion,
                                                  bool reset, std::optional<ChangeStabilizeInput> change_input) {
  if (set_->look.plan != LookPlan{}) {
    set_->look.last_use = slot_marks_[slot];
  }
  const VkImageView change = set_->intermediates.change.view;
  if (change == VK_NULL_HANDLE) {
    return {.view = nr_output, .fused = true, .model_input = passes_run == 1u ? set_->intermediates.a.view : VK_NULL_HANDLE};
  }
  const bool shape = (look_ready && set_->look.plan.shape);
  // 1. NR's change field and, for the look, NR's input chroma κ_M.
  // A still holds the original FP16 input after one pass; later passes overwrite it through ping-pong.
  if (!color_.RecordChange(buffer, slot, {.encode = encode, .nr_output = nr_output, .change = change, .basis = (shape ? set_->look.basis.view : VK_NULL_HANDLE),
                                          .model_input = (passes_run == 1u ? set_->intermediates.a.view : VK_NULL_HANDLE)})) {
    return {.ok = false};
  }
  color_.ComputeBarrier(buffer);
  AfterNr after = {.view = change, .upsampling = (reduced ? std::optional{upsampling} : std::nullopt)};
  if (shape) {
    // 2. The pyramids.
    const look::Atlas& atlas = set_->look.atlas;
    const look::Bands bands = look::MakeBands(config_.look.detail_radius, atlas.image.height, atlas.levels);
    color_.RecordPyramid(buffer, slot, {.change = change, .gauss = set_->look.gauss.view, .peaks = set_->look.peaks.view, .atlas = atlas});
    color_.ComputeBarrier(buffer);
    // 3. The stabiliser: the two levels the low band reads, then the fine band.
    VkImageView history = VK_NULL_HANDLE;
    VkImageView detail = VK_NULL_HANDLE;
    if (set_->look.plan.stabilize) {
      const uint32_t current = set_->look.current;
      history = set_->look.history[current].view;
      color::VkStabilizePass stabilize = {
          .gauss = set_->look.gauss.view,
          .previous = set_->look.history[1u - current].view,
          .current = history,
          .state = set_->look.state.view,
          .atlas = atlas,
          .bands = bands,
          .rate = look::StabilizeRate(config_.frame_seconds, config_.look.stabilize_ms),
          .motion_mode = (config_.look.stabilize == look::StabilizeMode::MOTION),
          .reset = (reset || !set_->look.history_valid),
          .motion = motion, .packed_history = true,
      };
      color_.RecordStabilize(buffer, slot, stabilize);
      color_.ComputeBarrier(buffer);
      if (set_->look.plan.detail) {
        detail = set_->look.detail_history[current].view;
        stabilize.change = change;
        stabilize.previous = set_->look.detail_history[1u - current].view;
        stabilize.current = detail;
        color_.RecordDetail(buffer, slot, stabilize);
        color_.ComputeBarrier(buffer);
      }
      set_->look.current = 1u - current;
      set_->look.history_valid = true;
    }
    // 4. The shape pass rewrites C in place.
    color_.RecordShape(buffer, slot,
                       {.change = change,
                        .basis = set_->look.basis.view,
                        .gauss = set_->look.gauss.view,
                        .peaks = set_->look.peaks.view,
                        .history = history,
                        .detail = detail,
                        .atlas = atlas,
                        .bands = bands,
                        .settings = config_.look.shape,
                        .sdr = color::IsSdr(encode.encoding), .packed_history = true});
    // C is the shape's storage output and what follows samples it: the change stabiliser, or the decode when it is off.
    color_.ComputeBarrier(buffer);
  }
  // 5. The change field's own stabiliser: the field the decode consumes, averaged along the vectors, into the other buffer
  //    of the pair. It is not part of the look - a look that shapes nothing still has a change field to steady - so it is
  //    its own step, taken whether or not the look's passes below run.
  if (look_ready && set_->look.plan.change) {
    const uint32_t current = set_->look.change_current;
    const VkImageView stable = set_->look.change_history[current].view;
    const ChangeStabilizeInput history_input = change_input.value_or(ChangeStabilizeInput{.reset = reset});
    const bool reset_history = history_input.reset || !set_->look.change_valid;
    const bool recorded = color_.RecordChangeStabilize(buffer, slot,
        {.change = change, .previous = set_->look.change_history[1u - current].view, .current = stable,
         .image = set_->look.plan.image,
         .rate = look::StabilizeRate(config_.frame_seconds, static_cast<float>(config_.look.stabilize_change_ms)),
         .reset = reset_history, .motion = history_input.reproject ? motion : color::VkStabilizeMotion{},
         .flat_denoise = change_input.has_value()});
    set_->look.change_valid = recorded;
    if (recorded) {
      color_.ComputeBarrier(buffer);
      set_->look.change_current = 1u - current;
      after.view = stable;
    }
  }
  return after;
}

PipelineResult VkNrPipeline::RecordAfterDlss(VkCommandBuffer buffer, const VkDlssTarget& target, const nr::FrameInputs& inputs, const nr::Controls& controls,
                                             const WorkLayout& layout, const ChainPart& part) {
  return RecordAfterDlssImpl(buffer, target, inputs, controls, layout, part, false, std::nullopt);
}

PipelineResult VkNrPipeline::RecordAfterDlssImpl(VkCommandBuffer buffer, const VkDlssTarget& target, const nr::FrameInputs& inputs, const nr::Controls& controls,
                                                 const WorkLayout& layout, const ChainPart& part, bool motion_is_canvas,
                                                 std::optional<PresentChangeInput> present_change) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = nr::SkipReason::INACTIVE};
  // The union is read only for an image view (NGX's resource can also be a buffer).
  if (buffer == VK_NULL_HANDLE || target.resource == nullptr || target.resource->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || part.set >= STAGE_SETS) {
    return {.reason = nr::SkipReason::INVALID_INPUT};
  }
  set_ = &sets_[part.set];
  if (!session_.SliceRuns(part.slice)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::BUDGET};
  }
  const NVSDK_NGX_ImageViewInfo_VK& output = target.resource->Resource.ImageViewInfo;
  nr::Rect region = target.region;
  if (region.width == 0u && region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = output.Width, .height = output.Height};
  }
  const nr::Size size = {region.width, region.height};
  if (size.Empty() || uint64_t{region.x} + region.width > output.Width || uint64_t{region.y} + region.height > output.Height) {
    return {.reason = nr::SkipReason::INVALID_INPUT};
  }
  if (!nr::MeetsNrFloor(size)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::FRAME_TOO_SMALL};
  }
  const WorkLayout work = ResolveLayout(layout, size);
  if (!color::VkColorPipeline::OutputVariantOf(output.Format)) return {.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
  if (!config_.keep_faces.show_mask && !session_.SliceHasIntensity(part.slice, controls, config_.later_controls)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::INTENSITY_ZERO};
  }
  // Every pipeline exists before the encode is recorded, so a decode that cannot be built never leaves NR running unused.
  if (!color_.Prepare(output.Format)) return {.reason = nr::SkipReason::PIPELINE_FAILED};

  const uint32_t slot = timeline_.FindReusableSlot(slot_marks_, next_slot_);
  if (slot == slot_marks_.size()) return {.reason = nr::SkipReason::DESCRIPTORS_BUSY};
  const bool foveated = work.foveation.active();
  const bool remap_motion = foveated && !motion_is_canvas;
  const color::VkStabilizeMotion source_motion = StabilizeMotionOf(inputs);
  const bool copy_motion = remap_motion && source_motion.vectors.view != VK_NULL_HANDLE;
  const bool game_motion = copy_motion || (!remap_motion && inputs.motion.resource != nullptr);
  if (copy_motion && !color_.PrepareMotion()) return {.reason = nr::SkipReason::PIPELINE_FAILED};
  const bool reduced = (work.image != size);
  const SetSizes sizes = {.output = size, .image = work.image, .canvas = work.image};
  if (!EnsureIntermediates(sizes, false, work.foveation)) return {.reason = nr::SkipReason::OUT_OF_MEMORY};
  // Allocate either the remap target or fallback lazily; a frame that binds the source in place needs neither.
  vk::NrImage* const motion_target = copy_motion ? &set_->intermediates.canvas_motion
                                    : !game_motion ? &set_->intermediates.zero_motion : nullptr;
  if (motion_target != nullptr && motion_target->image == VK_NULL_HANDLE
      && !vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, work.image.width, work.image.height, MOTION_USAGE, false,
                            motion_target)) {
    LogAllocationFailure(work.image);
    return {.reason = nr::SkipReason::OUT_OF_MEMORY};
  }
  const auto look_plan = LookPlanFor(work.image);
  if (!EnsureChange(reduced || foveated || look_plan.shape || look_plan.change)) return {.reason = nr::SkipReason::OUT_OF_MEMORY};
  const bool look_ready = EnsureLook(look_plan);  // before the fit, like the set
  nr::FrameInputs frame_inputs = inputs;
  PrepareFaces(work.image, controls, part.slice, &frame_inputs);  // Keep faces: its surfaces before the fit and the opening, like the look's
  // The GPU reads this slot's constants, and the intermediates, until this recording completes: the current frame, and the token the caller issued before
  // recording (every mark taken from here on includes it).
  slot_marks_[slot] = timeline_.MarkNow(!present_change.has_value());
  set_->intermediates.last_use = slot_marks_[slot];
  // OpenRecording below transitions the look's and Keep faces' images whenever they exist, whether or not NR then runs and uses them, so
  // this recording is their last use too.
  if (set_->look.plan != LookPlan{}) {
    set_->look.last_use = slot_marks_[slot];
  }
  if (set_->faces.twin.image != VK_NULL_HANDLE) {
    set_->faces.last_use = slot_marks_[slot];
  }
  next_slot_ = (slot + 1u) % color::VkColorPipeline::RING_SLOTS;

  const float input_scale = NoteEncoding(target);
  const uint32_t options = ShaderOptions(config_.fixes);
  NoteMeterBasis(part.slice.stage, target.encoding, input_scale);
  const ExposureChoice exposure = ChooseExposure(target.encoding, target.exposure, target.exposure_factor);

  // 1. The opening barrier: DLSS's write of the Output (and whatever else the game recorded before) is visible to Uplift's reads and writes, and A, B, the
  // change field, the zero motion and the look's surfaces move UNDEFINED -> GENERAL (their contents are rewritten in full every recording).
  const std::array<const vk::NrImage*, 9> opened = {&set_->intermediates.a, &set_->intermediates.b, &set_->intermediates.zero_motion, &set_->intermediates.change,
                                                    &set_->intermediates.canvas_motion, &set_->faces.twin,
                                                    &set_->faces.detail, &set_->faces.atlas, &set_->faces.fill};
  OpenRecording(buffer, opened, true);

  // 2. Encode the output region, in place, into A at the work image with the game's (or the meter's) exposure; MotionVectors = None clears the zero motion in
  // the same dispatch.
  const MotionSource motion_source = (game_motion ? MotionSource::DIRECT : MotionSource::NONE);
  const color::VkEncodePass encode = {
      .source = output.ImageView,
      .source_format = output.Format,
      .model = set_->intermediates.a.view,
      .motion = (game_motion ? VK_NULL_HANDLE : set_->intermediates.zero_motion.view),
      .width = work.image.width,
      .height = work.image.height,
      .encoding = target.encoding,
      .input_scale = input_scale,
      .exposure = exposure.view,
      .exposure_factor = exposure.factor,
      .region = region,
      .canvas = work.image,
      .options = options,
      .foveation = work.foveation,
  };
  // InputExposure = Metered: the meter reads the region the encode reads, in place, and the encode and the decode read its state as their exposure.
  // it also runs for Auto's check, where a failed dispatch only skips that sample. A dispatch fails when the source format's meter pipeline is missing;
  // Direct3D 12's single meter pipeline cannot be, so only this path aborts a Metered recording.
  if (exposure.metered || exposure.probe) {
    const bool recorded = RecordMeter(buffer, slot,
                                      {.source = output.ImageView,
                                       .source_format = output.Format,
                                       .region = region,
                                       .encoding = target.encoding,
                                       .primaries = (options & color::shader_options::PRIMARIES_MASK),
                                       .input_scale = input_scale},
                                      exposure);
    if (!recorded && exposure.metered) return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source};
  }
  if (!color_.RecordEncode(buffer, slot, encode)) {
    return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source};
  }

  // 3. NR reads the vectors in the same coordinates as the encoded colour.
  if (copy_motion) {
    if (!ConvertWorkMotion(buffer, slot, frame_inputs, source_motion, work)) {
      set_->intermediates.reset_pending = true;
      return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED};
    }
  } else if (game_motion) {
    Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  } else {
    HandToNgx(buffer, set_->intermediates.zero_motion);
    frame_inputs.motion = {.resource = nr::AsResource(&set_->intermediates.zero_motion.ngx)};
    frame_inputs.motion_scale_x = 1.f;
    frame_inputs.motion_scale_y = 1.f;
  }
  frame_inputs.reset_hint = (inputs.reset_hint || set_->intermediates.reset_pending);
  const bool change_reset = present_change && (present_change->reset || set_->intermediates.reset_pending);
  if (foveated) {
    frame_inputs.depth = {};
  }
  frame_inputs.later_passes = config_.later_controls;
  frame_inputs.resolver = this;
  resolve_slot_ = slot;
  const uint64_t held = HeldBytes();  // allocated before the fit: charged as the chain's surfaces and credited as held
  const nr::EvaluateResult evaluated = session_.Evaluate(
      nr::AsList(buffer),
      {.a = nr::AsResource(&set_->intermediates.a.ngx), .b = nr::AsResource(&set_->intermediates.b.ngx), .size = work.image, .surface_bytes = held, .held_intermediate_bytes = held},
      frame_inputs, controls, part.slice);
  const bool from_session = nr::SessionWroteMessage(evaluated.reason);
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = from_session, .motion_source = motion_source, .motion_scale_x = frame_inputs.motion_scale_x, .motion_scale_y = frame_inputs.motion_scale_y};
  }
  set_->intermediates.reset_pending = false;  // only a frame that ran NR pays off the owed reset

  // 4. The required change field and optional look stage. NR's writes are visible first.
  Barrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  const VkImageView nr_output = (evaluated.output == nr::AsResource(&set_->intermediates.a.ngx) ? set_->intermediates.a.view : set_->intermediates.b.view);
  if (faces_ran_ && config_.keep_faces.show_mask) {
    // Show the face mask: the mask tinted into NR's result, which the change field and the decode then carry to the image.
    color_.RecordFacesShow(buffer, slot, nr_output, set_->faces.twin.view, work.image);
    Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  }
  // NGX's zero-filled inputs are not usable reprojection vectors.
  const color::VkStabilizeMotion stabilize_motion = !game_motion || inputs.motion.resource == nr::AsResource(&present_.stand_in.ngx)
      ? color::VkStabilizeMotion{} : (copy_motion ? StabilizeMotionOf(frame_inputs) : source_motion);
  std::optional<ChangeStabilizeInput> change_input;
  if (present_change) {
    if (stabilize_motion.vectors.view == VK_NULL_HANDLE) present_change->motion.source = MotionSource::NONE;
    change_input = set_->present_change_history.Advance(present_change->motion, change_reset);
  }
  const AfterNr after = RecordAfterNr(buffer, slot, encode, nr_output, evaluated.passes_run, reduced, look_ready, work.upsampling,
                                      stabilize_motion, frame_inputs.reset_hint, change_input);
  if (!after.ok) {
    return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source, .motion_scale_x = frame_inputs.motion_scale_x, .motion_scale_y = frame_inputs.motion_scale_y};
  }

  // 5. Compose the change field in place, with the original pixel read from the Output itself, then the closing barrier: whatever the
  // game does next sees Uplift's writes, whatever source stage its own barrier names.
  if (!color_.RecordDecode(buffer, slot,
                           {.target = output.ImageView,
                            .target_format = output.Format,
                            .nr_output = after.view,
                            .origin_x = region.x,
                            .origin_y = region.y,
                            .target_width = output.Width,
                            .target_height = output.Height,
                            .width = size.width,
                            .height = size.height,
                            .encoding = target.encoding,
                            .input_scale = input_scale,
                            .transfer_strength = target.transfer_strength,
                            .color_strength = target.color_strength,
                            .exposure = exposure.view,
                            .exposure_factor = exposure.factor,
                            .source_x = region.x,
                            .source_y = region.y,
                            .upsampling = after.upsampling,
                            .options = options | (after.fused ? color::shader_options::FUSED_CHANGE : 0u),
                            .chroma_clamp = config_.fixes.chroma_clamp,
                            .guided_low_gain = config_.fixes.guided_low_gain,
                            .mask = BoundMask(slot), .foveation = work.foveation, .model_input = after.model_input})) {
    return {.recorded = true, .reason = nr::SkipReason::DECODE_FAILED, .motion_source = motion_source, .motion_scale_x = frame_inputs.motion_scale_x, .motion_scale_y = frame_inputs.motion_scale_y};
  }
  Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason, .from_session = from_session, .motion_source = motion_source, .motion_scale_x = frame_inputs.motion_scale_x, .motion_scale_y = frame_inputs.motion_scale_y};
}

PipelineResult VkNrPipeline::RecordPreSr(VkCommandBuffer buffer, const VkDlssTarget& color, const nr::FrameInputs& inputs, const nr::Controls& controls,
                                         const WorkLayout& layout, const ChainPart& part) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = nr::SkipReason::INACTIVE};
  if (buffer == VK_NULL_HANDLE || color.resource == nullptr || color.resource->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || part.set >= STAGE_SETS) {
    return {.reason = nr::SkipReason::INVALID_INPUT};
  }
  set_ = &sets_[part.set];
  const NVSDK_NGX_ImageViewInfo_VK& game = color.resource->Resource.ImageViewInfo;
  const nr::Rect region = color.region;
  const nr::Size size = {region.width, region.height};
  if (size.Empty() || uint64_t{region.x} + region.width > game.Width || uint64_t{region.y} + region.height > game.Height) {
    return {.reason = nr::SkipReason::INVALID_INPUT};
  }
  if (!nr::MeetsNrFloor(size)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::FRAME_TOO_SMALL};
  }
  // A colour format the sampled encode reads; Intensity 0 swaps nothing either (exact pass-through).
  if (!PreSrColorFormatSupported(game.Format)) return {.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
  if (!session_.SliceRuns(part.slice)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::BUDGET};
  }
  if (!config_.keep_faces.show_mask && !session_.SliceHasIntensity(part.slice, controls, config_.later_controls)) {
    set_->intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::INTENSITY_ZERO};
  }
  // The set and the feature are keyed on the settled work image, never the per-frame render region. A region that is not the
  // settled image is resampled into it, and the decode upsamples NR's change back to the region, as on Direct3D 12.
  const WorkLayout work = ResolveLayout(layout, size);
  const nr::Size image = work.image;
  const nr::Size canvas = work.image;
  // Remapping requires readable float vectors. Otherwise use zero motion when a copy is needed.
  const bool game_motion = (inputs.motion.resource != nullptr);
  const bool foveated = work.foveation.active();
  std::optional<color::VkStabilizeMotion> source_motion;
  if (game_motion) {
    const NVSDK_NGX_Resource_VK& motion = *nr::VkResourceOf(inputs.motion.resource);
    if (motion.Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return {.reason = nr::SkipReason::INVALID_INPUT};
    const NVSDK_NGX_ImageViewInfo_VK& motion_image = motion.Resource.ImageViewInfo;
    const auto rect = MotionRectOf(inputs.motion.rect, {motion_image.Width, motion_image.Height});
    if (!rect) return {.reason = nr::SkipReason::MOTION_OUTSIDE_TEXTURE};
    if (MotionFormatReadable(motion_image.Format)) source_motion = SampleMotion(motion_image, *rect, inputs);
  }
  const bool copy_motion = foveated && source_motion.has_value();
  const bool use_zero_motion = !game_motion || (foveated && !copy_motion);
  if (!color_.PreparePreSr() || (copy_motion && !color_.PrepareMotion())) return {.reason = nr::SkipReason::PIPELINE_FAILED};

  const uint32_t slot = timeline_.FindReusableSlot(slot_marks_, next_slot_);
  if (slot == slot_marks_.size()) return {.reason = nr::SkipReason::DESCRIPTORS_BUSY};
  const nr::Size color_size = {game.Width, game.Height};
  const SetSizes sizes = {.output = image, .image = image, .canvas = canvas, .private_color = color_size};
  if (!EnsureIntermediates(sizes, true, work.foveation)) return {.reason = nr::SkipReason::OUT_OF_MEMORY};
  // The motion images, made at the first frame that needs each, at the canvas size, and freed with the set.
  const bool motion_failed =
      (use_zero_motion && set_->intermediates.zero_motion.image == VK_NULL_HANDLE
       && !vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, canvas.width, canvas.height, MOTION_USAGE, false, &set_->intermediates.zero_motion))
      || (copy_motion && set_->intermediates.canvas_motion.image == VK_NULL_HANDLE
          && !vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, canvas.width, canvas.height, MOTION_USAGE, false,
                                &set_->intermediates.canvas_motion));
  if (motion_failed) {
    LogAllocationFailure(canvas);
    return {.reason = nr::SkipReason::OUT_OF_MEMORY};
  }
  const bool reduced = (image != size);
  const auto look_plan = LookPlanFor(image);
  if (!EnsureChange(reduced || foveated || look_plan.shape || look_plan.change)) return {.reason = nr::SkipReason::OUT_OF_MEMORY};
  const bool look_ready = EnsureLook(look_plan);  // before the fit, like the set; C stays the main set's
  nr::FrameInputs frame_inputs = inputs;
  PrepareFaces(canvas, controls, part.slice, &frame_inputs);  // Keep faces: its surfaces before the fit and the opening, like the look's
  slot_marks_[slot] = timeline_.MarkNow(true);
  set_->intermediates.last_use = slot_marks_[slot];
  // OpenRecording below transitions the look's and Keep faces' images whenever they exist, whether or not NR then runs and uses them, so
  // this recording is their last use too.
  if (set_->look.plan != LookPlan{}) {
    set_->look.last_use = slot_marks_[slot];
  }
  if (set_->faces.twin.image != VK_NULL_HANDLE) {
    set_->faces.last_use = slot_marks_[slot];
  }
  next_slot_ = (slot + 1u) % color::VkColorPipeline::RING_SLOTS;

  const float input_scale = NoteEncoding(color);
  const uint32_t options = ShaderOptions(config_.fixes);
  NoteMeterBasis(part.slice.stage, color.encoding, input_scale);
  const ExposureChoice exposure = ChooseExposure(color.encoding, color.exposure, color.exposure_factor);
  const color::VkSampledView game_color = {.view = game.ImageView, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};  // the game's: only read

  // 1. The opening barrier; A, B, the private colour, the change field, the motion images and the look's surfaces move UNDEFINED -> GENERAL.
  const std::array<const vk::NrImage*, 10> opened = {&set_->intermediates.a, &set_->intermediates.b, &set_->intermediates.private_color, &set_->intermediates.change,
                                                     &set_->intermediates.zero_motion, &set_->intermediates.canvas_motion, &set_->faces.twin, &set_->faces.detail,
                                                     &set_->faces.atlas, &set_->faces.fill};
  OpenRecording(buffer, opened, true);

  // 2. Encode the region straight from the game's Color, resampled to the work image; no Uplift copy of it, and no
  // transition of it.
  const MotionSource motion_source = (use_zero_motion ? MotionSource::NONE : MotionSource::DIRECT);
  const color::VkEncodePass encode = {
      .source = game.ImageView,
      .source_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .source_format = VK_FORMAT_UNDEFINED,
      .model = set_->intermediates.a.view,
      .motion = (use_zero_motion ? set_->intermediates.zero_motion.view : VK_NULL_HANDLE),
      .width = image.width,
      .height = image.height,
      .encoding = color.encoding,
      .input_scale = input_scale,
      .exposure = exposure.view,
      .exposure_factor = exposure.factor,
      .region = region,
      .canvas = canvas,
      .options = options,
      .foveation = work.foveation,
  };
  if (exposure.metered || exposure.probe) {
    const bool recorded = RecordMeter(buffer, slot,
                                      {.source = game.ImageView,
                                       .source_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       .region = region,
                                       .encoding = color.encoding,
                                       .primaries = (options & color::shader_options::PRIMARIES_MASK),
                                       .input_scale = input_scale},
                                      exposure);
    if (!recorded && exposure.metered) return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED};  // A failed check only skips its sample
  }
  if (!color_.RecordEncode(buffer, slot, encode)) {
    return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED};
  }

  // 3. Remap motion into the foveated work grid.
  if (copy_motion) {
    if (!ConvertWorkMotion(buffer, slot, frame_inputs, *source_motion, work)) {
      set_->intermediates.reset_pending = true;
      return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED};
    }
  } else if (use_zero_motion) {
    HandToNgx(buffer, set_->intermediates.zero_motion);
    frame_inputs.motion = {.resource = nr::AsResource(&set_->intermediates.zero_motion.ngx)};
    frame_inputs.motion_scale_x = 1.f;
    frame_inputs.motion_scale_y = 1.f;
  } else {
    Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);  // the game's own vectors, bound in place
  }
  frame_inputs.reset_hint = (inputs.reset_hint || set_->intermediates.reset_pending);
  if (foveated) {
    frame_inputs.depth = {};
  }
  frame_inputs.later_passes = config_.later_controls;
  frame_inputs.resolver = this;
  resolve_slot_ = slot;
  // 4. NR at the work size, with Keep faces recorded after each later pass (the resolver).
  const uint64_t held = HeldBytes();
  const nr::EvaluateResult evaluated = session_.Evaluate(
      nr::AsList(buffer),
      {.a = nr::AsResource(&set_->intermediates.a.ngx), .b = nr::AsResource(&set_->intermediates.b.ngx), .size = canvas, .surface_bytes = held, .held_intermediate_bytes = held},
      frame_inputs, controls, part.slice);
  const bool from_session = nr::SessionWroteMessage(evaluated.reason);
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = from_session, .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  set_->intermediates.reset_pending = false;  // only a frame that ran NR pays off the owed reset

  // 5. The required change field and optional look stage, at the work-image size.
  Barrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  const VkImageView nr_output = (evaluated.output == nr::AsResource(&set_->intermediates.a.ngx) ? set_->intermediates.a.view : set_->intermediates.b.view);
  if (faces_ran_ && config_.keep_faces.show_mask) {
    color_.RecordFacesShow(buffer, slot, nr_output, set_->faces.twin.view, canvas);  // Show the face mask, as after DLSS
    Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  }
  const color::VkStabilizeMotion stabilize_motion = use_zero_motion ? color::VkStabilizeMotion{}
      : (copy_motion ? StabilizeMotionOf(frame_inputs) : source_motion.value_or(color::VkStabilizeMotion{}));
  const AfterNr after = RecordAfterNr(buffer, slot, encode, nr_output, evaluated.passes_run, reduced, look_ready, work.upsampling,
                                      stabilize_motion, frame_inputs.reset_hint);
  if (!after.ok) {
    return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }

  // 6. The compose into the private colour's region (the game's pixel read from its Color, NR's result or change), then the private colour is handed to DLSS
  // in SHADER_READ_ONLY_OPTIMAL; the same dependency finishes the writes.
  if (!color_.RecordDecode(buffer, slot,
                           {.source = game_color,
                            .target = set_->intermediates.private_color.view,
                            .nr_output = after.view,
                            .origin_x = region.x,
                            .origin_y = region.y,
                            .target_width = game.Width,
                            .target_height = game.Height,
                            .width = size.width,
                            .height = size.height,
                            .encoding = color.encoding,
                            .input_scale = input_scale,
                            .transfer_strength = color.transfer_strength,
                            .color_strength = color.color_strength,
                            .exposure = exposure.view,
                            .exposure_factor = exposure.factor,
                            .source_x = region.x,
                            .source_y = region.y,
                            .upsampling = after.upsampling,
                            .options = options | (after.fused ? color::shader_options::FUSED_CHANGE : 0u),
                            .chroma_clamp = config_.fixes.chroma_clamp,
                            .guided_low_gain = config_.fixes.guided_low_gain,
                            .mask = BoundMask(slot), .foveation = work.foveation, .model_input = after.model_input})) {
    return {.recorded = true, .reason = nr::SkipReason::DECODE_FAILED, .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  HandToNgx(buffer, set_->intermediates.private_color);
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason, .from_session = from_session, .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
}

PresentMotionWrite VkNrPipeline::RecordPresentMotion(VkCommandBuffer buffer, const color::VkSampledView& motion, nr::Rect region, float scale_x,
                                                     float scale_y, uint64_t frame, bool flip_y, float offset_x, float offset_y,
                                                     std::shared_ptr<vk::NrCompletion::MotionProducer> producer) {
  const nr::Size size = {region.width, region.height};
  if (buffer == VK_NULL_HANDLE || motion.view == VK_NULL_HANDLE || size.Empty() || frame == 0u || !color_.PrepareMotion()) return PresentMotionWrite::NO_INPUT;
  const uint32_t ring_slot = timeline_.FindReusableSlot(slot_marks_, next_slot_);
  if (ring_slot == slot_marks_.size()) return PresentMotionWrite::RING_BUSY;
  // A new size: the ring is made again at its smallest (the old slots retire at their marks). A copy that is still in use is never overwritten: the frame goes
  // without one (its present binds an older copy: PresentMotion).
  if (present_motion_size_ != size || present_motion_[0].image.image == VK_NULL_HANDLE) {
    ReleasePresentMotion();
    if (!MakePresentMotionSlots(0u, PRESENT_MOTION_MIN_SLOTS, size)) return PresentMotionWrite::NO_INPUT;
    present_motion_slots_ = PRESENT_MOTION_MIN_SLOTS;
    present_motion_size_ = size;
    present_motion_bytes_ = present_motion_slots_ * size.Pixels() * MOTION_BYTES_PER_PIXEL;
  }
  PresentMotionSlot& copy = present_motion_[PresentMotionSlotOf(frame, present_motion_slots_)];
  if (!timeline_.IsComplete(copy.last_use)) {
    // The GPU runs too far behind for the minimum motion ring: the ring grows once, its new slots made beside the first ones (none of those is freed or
    // moved, and the frame mapping change only picks other slots, each still behind its own last use). This frame goes without its copy.
    if (PresentMotionGrows(present_motion_slots_, PresentMotionWrite::SLOT_BUSY)
        && MakePresentMotionSlots(present_motion_slots_, PRESENT_MOTION_SLOTS, size)) {
      nr::Logf(nr::LogLevel::INFO, "Vulkan: the GPU runs too far behind for four slots: DLSS's motion copies for Present grow from {} to {} slots ({:.1f} MiB)",
               present_motion_slots_, PRESENT_MOTION_SLOTS,
               static_cast<double>(PRESENT_MOTION_SLOTS * size.Pixels() * MOTION_BYTES_PER_PIXEL) / BYTES_PER_MIB);
      present_motion_slots_ = PRESENT_MOTION_SLOTS;
      present_motion_bytes_ = present_motion_slots_ * size.Pixels() * MOTION_BYTES_PER_PIXEL;
    }
    return PresentMotionWrite::SLOT_BUSY;
  }
  // The GPU reads this ring slot's constants until this recording completes: the current frame, and the token the caller issued before recording.
  slot_marks_[ring_slot] = timeline_.MarkNow(true);
  next_slot_ = (ring_slot + 1u) % color::VkColorPipeline::RING_SLOTS;
  // The slot's contents are rewritten in full: UNDEFINED -> GENERAL, the motion copy at the region's own size (an image = canvas = region copy), then handed over
  // in SHADER_READ_ONLY_OPTIMAL for the bridge's copy-in.
  const std::array<const vk::NrImage*, 1> opened = {&copy.image};
  OpenRecording(buffer, opened, false);
  color_.RecordMotion(buffer, ring_slot,
                      {.source = motion, .region = region, .target = copy.image.view, .image = size, .canvas = size, .scale_x = scale_x, .scale_y = scale_y,
                       .offset_x = offset_x, .offset_y = offset_y, .flip_y = flip_y});
  HandToNgx(buffer, copy.image);
  copy.frame = frame;
  copy.producer = std::move(producer);
  copy.last_use = slot_marks_[ring_slot];
  return PresentMotionWrite::WRITTEN;
}

std::array<uint64_t, VkNrPipeline::PRESENT_MOTION_SLOTS> VkNrPipeline::PresentMotionFrames(const MotionReady& ready) const {
  std::array<uint64_t, PRESENT_MOTION_SLOTS> frames = {};
  for (size_t index = 0u; index < PRESENT_MOTION_SLOTS; ++index) {
    const auto& copy = present_motion_[index];
    if (copy.image.image != VK_NULL_HANDLE && (!ready || (copy.producer && ready(*copy.producer)))) frames[index] = copy.frame;
  }
  return frames;
}

vk::NrImage VkNrPipeline::PresentMotion(uint64_t frame, uint64_t* age, uint64_t max_age, const MotionReady& ready) {
  const std::array<uint64_t, PRESENT_MOTION_SLOTS> frames = PresentMotionFrames(ready);
  const PresentMotionPick pick = PickPresentMotion(frames, frame, max_age);
  if (!pick.slot) return {};
  PresentMotionSlot& copy = present_motion_[*pick.slot];
  copy.last_use = timeline_.MarkNow();  // NR or the bridge's copy-in reads it in this frame
  if (age != nullptr) {
    *age = pick.age;
  }
  return copy.image;
}

bool VkNrPipeline::HasPresentMotion(uint64_t frame, uint64_t max_age, const MotionReady& ready) const {
  // The same readiness filter PresentMotion binds with: an unfiltered answer calls a copy in flight "present", which hides the very gap this reports.
  const std::array<uint64_t, PRESENT_MOTION_SLOTS> frames = PresentMotionFrames(ready);
  return PickPresentMotion(frames, frame, max_age).slot.has_value();
}

bool VkNrPipeline::MakePresentMotionSlots(size_t first, size_t end, nr::Size size) {
  bool made = true;
  for (size_t index = first; index < end; ++index) {
    made = made && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, size.width, size.height, PRESENT_MOTION_USAGE, false,
                                     &present_motion_[index].image);
  }
  if (!made) {
    for (size_t index = first; index < end; ++index) {
      vk::DestroyNrImage(functions_, device_, &present_motion_[index].image);  // never used: none was handed to a recording
      present_motion_[index] = {};
    }
    LogAllocationFailure(size);
    return false;
  }
  allocation_failure_logged_ = false;
  return true;
}

void VkNrPipeline::ReleasePresentMotion() {
  for (const PresentMotionSlot& slot : present_motion_) {
    RetireReshadeImage(slot.image, slot.last_use);  // the bridge's copy-in reads it on ReShade's queue
  }
  present_motion_ = {};
  present_motion_slots_ = PRESENT_MOTION_MIN_SLOTS;  // the next copies start small again
  present_motion_size_ = {};
  present_motion_bytes_ = 0u;
}

bool VkNrPipeline::ConvertLaunchpadMotion(VkCommandBuffer buffer, const VkPresentTarget& target, const WorkLayout& work) {
  const nr::Rect region = {.x = 0u, .y = 0u, .width = target.launchpad_size.width, .height = target.launchpad_size.height};
  if (target.launchpad_motion.view == VK_NULL_HANDLE || !MotionFormatReadable(target.launchpad_format) || region.width == 0u || region.height == 0u
      || work.image.Empty() || !color_.PrepareMotion()) {
    return false;
  }
  const uint32_t slot = timeline_.FindReusableSlot(slot_marks_, next_slot_);
  if (slot == slot_marks_.size()) return false;
  if (present_.launchpad.image == VK_NULL_HANDLE || present_.launchpad_size != work.image) {
    RetirePresentLaunchpad();
    if (!vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, work.image.width, work.image.height, MOTION_USAGE, false,
                           &present_.launchpad)) {
      present_.launchpad = {};
      LogAllocationFailure(work.image);
      return false;
    }
    present_.launchpad_size = work.image;
    present_.launchpad_bytes = work.image.Pixels() * MOTION_BYTES_PER_PIXEL;
  }
  slot_marks_[slot] = timeline_.MarkNow();
  present_.launchpad_last_use = slot_marks_[slot];
  next_slot_ = (slot + 1u) % color::VkColorPipeline::RING_SLOTS;
  // Its contents are rewritten in full: UNDEFINED -> GENERAL, the copy, then handed to NGX in SHADER_READ_ONLY_OPTIMAL as a game's own vectors are.
  // UPLIFT_MV holds back-buffer pixels: times MotionScale, and times image / region into the work image's pixels (Resolution below Full), as Direct3D 12's.
  const std::array<const vk::NrImage*, 1> opened = {&present_.launchpad};
  OpenRecording(buffer, opened, false);
  const bool recorded = color_.RecordMotion(buffer, slot,
                                            {.source = target.launchpad_motion,
                                             .region = region,
                                             .target = present_.launchpad.view,
                                             .image = work.image,
                                             .canvas = work.image,
                                             .scale_x = target.motion_scale_x * static_cast<float>(work.image.width) / static_cast<float>(region.width),
                                             .scale_y = target.motion_scale_y * static_cast<float>(work.image.height) / static_cast<float>(region.height),
                                             .foveation = work.foveation});
  HandToNgx(buffer, present_.launchpad);
  return recorded;
}

const NVSDK_NGX_Resource_VK* VkNrPipeline::ZeroStandIn(VkCommandBuffer buffer, nr::Size size) {
  if (size.Empty()) return nullptr;
  if (present_.stand_in.image == VK_NULL_HANDLE || present_.stand_in_size != size) {
    RetireReshadeImage(present_.stand_in, present_.last_use);
    present_.stand_in = {};
    if (!vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, size.width, size.height,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, &present_.stand_in)) {
      present_.stand_in = {};
      LogAllocationFailure(size);
      return nullptr;
    }
    present_.stand_in_size = size;
  }
  // Rewritten in full at each use: UNDEFINED -> TRANSFER_DST, zeros, then SHADER_READ_ONLY_OPTIMAL as a game's own vectors are handed over.
  const VkImageMemoryBarrier to_clear =
      ImageTransition(present_.stand_in.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT);
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &to_clear);
  const VkClearColorValue zero = {};
  functions_.vkCmdClearColorImage(buffer, present_.stand_in.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1u, &COLOR_RANGE);
  const VkImageMemoryBarrier to_read = ImageTransition(present_.stand_in.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &to_read);
  return &present_.stand_in.ngx;
}

PipelineResult VkNrPipeline::RecordPresent(VkCommandBuffer buffer, const VkPresentTarget& target, const nr::Controls& controls, bool reset_hint,
                                           const WorkLayout& layout, const ChainPart& part) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = nr::SkipReason::INACTIVE};
  const uint64_t bytes_per_pixel = PresentBytesPerPixel(target.copy_format);
  if (buffer == VK_NULL_HANDLE || target.image == VK_NULL_HANDLE || target.size.Empty() || bytes_per_pixel == 0u || part.set >= STAGE_SETS) return {.reason = nr::SkipReason::INVALID_INPUT};
  if (!session_.SliceRuns(part.slice)) {
    sets_[part.set].intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::BUDGET};
  }
  if (!nr::MeetsNrFloor(target.size)) {
    sets_[part.set].intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::FRAME_TOO_SMALL};
  }
  const WorkLayout work = ResolveLayout(layout, target.size);
  // RecordAfterDlss's own checks, made before anything is recorded, so a frame it would skip copies nothing either.
  if (!config_.keep_faces.show_mask && !session_.SliceHasIntensity(part.slice, controls, config_.later_controls)) {
    sets_[part.set].intermediates.reset_pending = true;
    return {.reason = nr::SkipReason::INTENSITY_ZERO};
  }
  if (!color_.Prepare(VK_FORMAT_R16G16B16A16_SFLOAT)) return {.reason = nr::SkipReason::PIPELINE_FAILED};
  // Launchpad may consume a slot before NR; check capacity before the copy in.
  if (timeline_.FindReusableSlot(slot_marks_, next_slot_, 1u) == slot_marks_.size()) return {.reason = nr::SkipReason::DESCRIPTORS_BUSY};
  // Preserve both FP16 conversions for integer formats. When the actual image format is known
  // to equal the raw-copy format, the same blits can read/write it directly: no staging or copies.
  // sRGB and typeless/mutable images keep raw-copy staging so a blit never linearises their bits.
  const bool needs_blit = target.copy_format != VK_FORMAT_R16G16B16A16_SFLOAT;
  const bool direct_blit = needs_blit && target.image_format == target.copy_format;
  const bool needs_staging = needs_blit && !direct_blit;
  if (present_.color.image == VK_NULL_HANDLE || present_.size != target.size || present_.format != target.copy_format
      || present_.direct_blit != direct_blit) {
    RetirePresent();
    PresentSurfaces created = {.size = target.size, .format = target.copy_format,
                               .direct_blit = direct_blit,
                               .bytes = target.size.Pixels() * (MODEL_BYTES_PER_PIXEL + (needs_staging ? bytes_per_pixel : 0u))};
    const bool made = (!needs_staging || vk::CreateNrImage(functions_, device_, memory_, target.copy_format, target.size.width, target.size.height, PRESENT_STAGING_USAGE,
                                        false, &created.staging))
                      && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, target.size.width, target.size.height,
                                           MODEL_USAGE, true, &created.color);
    if (!made) {
      for (vk::NrImage* image : {&created.staging, &created.color}) {
        vk::DestroyNrImage(functions_, device_, image);
      }
      LogAllocationFailure(target.size);
      return {.reason = nr::SkipReason::OUT_OF_MEMORY};
    }
    allocation_failure_logged_ = false;
    present_ = std::move(created);
  }
  // Both are rewritten in full every frame and used on ReShade's queue alone: the frame's signal there follows this buffer's submission.
  present_.last_use = timeline_.MarkNow();
  const VkImage color = present_.color.image;
  const VkImage staging = needs_staging ? present_.staging.image : color;
  const VkExtent3D extent = {target.size.width, target.size.height, 1u};
  const VkImageCopy copy = {.srcSubresource = COLOR_LAYERS, .dstSubresource = COLOR_LAYERS, .extent = extent};
  const VkImageBlit blit = {
      .srcSubresource = COLOR_LAYERS,
      .srcOffsets = {{0, 0, 0}, {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1}},
      .dstSubresource = COLOR_LAYERS,
      .dstOffsets = {{0, 0, 0}, {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1}},
  };

  // 1. The copy in. A real execution and memory dependency on everything submitted earlier on this queue (the game's frame, and ReShade's effects so far,
  // which the caller flushed): a transition from the present layout alone orders nothing. The image's bits go raw into the staging image (size-compatible
  // formats), whose blit converts them into the RGBA16F intermediate (an sRGB-created image's stored values, never linearised: the staging format is UNORM).
  const VkMemoryBarrier earlier_writes = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
  const std::array<VkImageMemoryBarrier, 2> into_copy = {
      ImageTransition(target.image, target.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
      ImageTransition(staging, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT),
  };
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 1u, &earlier_writes, 0u, nullptr,
                                  static_cast<uint32_t>(into_copy.size()), into_copy.data());
  if (!direct_blit) functions_.vkCmdCopyImage(buffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
  if (needs_staging) {
    const std::array<VkImageMemoryBarrier, 2> into_blit = {
        ImageTransition(staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT),
        ImageTransition(color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                                    static_cast<uint32_t>(into_blit.size()), into_blit.data());
    functions_.vkCmdBlitImage(buffer, staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, VK_FILTER_NEAREST);
  } else if (direct_blit) {
    functions_.vkCmdBlitImage(buffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, VK_FILTER_NEAREST);
  }
  const VkImageMemoryBarrier into_nr =
      ImageTransition(color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &into_nr);

  // 2. Prefer DLSS, then Launchpad. Only the uniform grid reuses history on a zero stand-in.
  nr::FrameInputs inputs = {.ui_correction = config_.present_ui_correction, .reset_hint = reset_hint};
  const bool foveated = work.foveation.active();
  bool motion_is_canvas = false;
  MotionSource motion_source = MotionSource::NONE;
  if (target.dlss_motion != nullptr) {
    inputs.motion = {.resource = nr::AsResource(target.dlss_motion)};  // the full subrect, a scale of (1, 1)
    motion_source = MotionSource::PRESENT_COPY;
  } else if (!foveated && !target.dlss_motion_gap_size.Empty()) {
    // DLSS's copy is this load's input but missed this present: zeros of the copies' own shape, so NR runs (no skip, no reload).
    if (const NVSDK_NGX_Resource_VK* stand_in = ZeroStandIn(buffer, target.dlss_motion_gap_size); stand_in != nullptr) {
      inputs.motion = {.resource = nr::AsResource(stand_in)};
      motion_source = MotionSource::PRESENT_COPY;
    }
  } else if (target.launchpad_motion.view != VK_NULL_HANDLE) {
    if (ConvertLaunchpadMotion(buffer, target, work)) {
      inputs.motion = {.resource = nr::AsResource(&present_.launchpad.ngx)};
      motion_is_canvas = true;
      motion_source = MotionSource::LAUNCHPAD;
    } else if (!foveated) {
      if (const NVSDK_NGX_Resource_VK* stand_in = ZeroStandIn(buffer, work.image); stand_in != nullptr) {
        inputs.motion = {.resource = nr::AsResource(stand_in)};
        motion_is_canvas = true;
        motion_source = MotionSource::LAUNCHPAD;
      }
    }
  }
  if (motion_source != MotionSource::LAUNCHPAD && present_.launchpad.image != VK_NULL_HANDLE) {
    RetirePresentLaunchpad();  // Launchpad's vectors stopped coming: its conversion goes, behind the recordings that used it
  }
  inputs.reset_hint = (inputs.reset_hint || motion_source != present_motion_source_);  // a provider switch
  present_motion_source_ = motion_source;

  // 3. NR in place on the intermediate, as on DLSS's Output (the encode, the passes, the change path and the look, the decode), with no game exposure.
  PipelineResult result = RecordAfterDlssImpl(buffer,
                                          {.resource = &present_.color.ngx,
                                           .encoding = target.encoding,
                                           .diffuse_white_nits = target.diffuse_white_nits,
                                           .transfer_strength = target.transfer_strength,
                                           .color_strength = target.color_strength},
                                          inputs, controls, layout, part, motion_is_canvas,
                                          PresentChangeInput{.motion = {.source = motion_source,
                                                                        .fresh = motion_source != MotionSource::PRESENT_COPY || target.dlss_motion_age == 0u,
                                                                        .allow_gap = target.motion_enabled}, .reset = reset_hint});
  result.recorded = true;
  result.motion_source = motion_source;

  // 4. The copy back, only when NR applied (otherwise the image keeps its own bits), then the image's return to its layout behind a global dependency: what
  // ReShade records next starts from that layout with no source scope of its own (its present state orders nothing).
  const VkMemoryBarrier later_reads = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                       .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
  VkImageLayout image_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  if (result.nr_applied) {
    if (needs_staging) {
      const std::array<VkImageMemoryBarrier, 2> out_of_nr = {
          ImageTransition(color, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
          ImageTransition(staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT),
      };
      functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                                      static_cast<uint32_t>(out_of_nr.size()), out_of_nr.data());
      functions_.vkCmdBlitImage(buffer, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit,
                                VK_FILTER_NEAREST);
      const std::array<VkImageMemoryBarrier, 2> into_image = {
          ImageTransition(staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT),
          ImageTransition(target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT),
      };
      functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                                      static_cast<uint32_t>(into_image.size()), into_image.data());
    } else {
      const std::array<VkImageMemoryBarrier, 2> out_of_nr = {
          ImageTransition(color, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT),
          ImageTransition(target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT),
      };
      functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                                      static_cast<uint32_t>(out_of_nr.size()), out_of_nr.data());
    }
    if (direct_blit) {
      functions_.vkCmdBlitImage(buffer, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, VK_FILTER_NEAREST);
    } else {
      functions_.vkCmdCopyImage(buffer, staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
    }
    image_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  }
  const VkImageMemoryBarrier returned =
      ImageTransition(target.image, image_layout, target.layout, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 1u, &later_reads, 0u, nullptr, 1u,
                                  &returned);
  return result;
}

}  // namespace uplift::sources
