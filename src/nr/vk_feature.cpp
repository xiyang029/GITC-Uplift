#include "nr/vk_feature.hpp"

#include "nr/feature_state.hpp"

#include "nr/keys.hpp"
#include "nr/log.hpp"
#include "nvfp4/NrNvfp4.h"
#include "nr/vk_handles.hpp"

namespace uplift::nr {
namespace {

// The block holds each resource as a void* to its NVSDK_NGX_Resource_VK (what NGX_VULKAN_EVALUATE_DLSS_EXT writes), not an ID3D12Resource.
void SetResource(NVSDK_NGX_Parameter* parameters, const keys::Resource& key, const BoundResource& bound) {
  parameters->Set(key.resource, static_cast<void*>(VkResourceOf(bound.resource)));
  parameters->Set(key.subrect.base_x, bound.rect.x);
  parameters->Set(key.subrect.base_y, bound.rect.y);
  parameters->Set(key.subrect.width, bound.rect.width);
  parameters->Set(key.subrect.height, bound.rect.height);
}

}  // namespace

VkFeature::VkFeature(const Snippet& snippet) : snippet_(snippet) {}

VkFeature::~VkFeature() {
  if (handle_ != nullptr) {
    Log(LogLevel::ERR, "VkFeature destroyed while still created; the handle is leaked, not released unsafely");
  }
  if (parameters_ != nullptr) {
    snippet_.DestroyParameters(parameters_);
  }
}

NVSDK_NGX_Result VkFeature::Create(ID3D12GraphicsCommandList* list, const CreateInfo& info) {
  if (handle_ != nullptr || list == nullptr || info.capacity.Empty()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  // Custom models use the D3D12 NvAPI path. Do not silently fall back on Vulkan.
  if (DlssNr::Nvfp4::IsNvfp4(info.preset)) {
    Logf(LogLevel::ERR, "NR model {} cannot run on Vulkan; refusing feature creation", info.preset);
    return NVSDK_NGX_Result_FAIL_PlatformError;
  }
  if (parameters_ == nullptr) {
    parameters_ = snippet_.AllocateParameters();
  }
  if (parameters_ == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
  parameters_->Set(keys::CREATION_NODE_MASK, 1u);
  parameters_->Set(keys::VISIBILITY_NODE_MASK, 1u);
  parameters_->Set(keys::WIDTH, info.capacity.width);
  parameters_->Set(keys::HEIGHT, info.capacity.height);
  parameters_->Set(keys::PRESET, info.preset);
  parameters_->Set(keys::SCALING_RATIO, 1.f);
  const NVSDK_NGX_Result result = snippet_.CreateFeatureVulkan(VkListOf(list), parameters_, &handle_, &network_);
  if (NVSDK_NGX_FAILED(result) || handle_ == nullptr) {
    Logf(LogLevel::ERR, "CreateFeature {}x{} failed: {:#010x}", info.capacity.width, info.capacity.height, static_cast<uint32_t>(result));
    handle_ = nullptr;
    return NVSDK_NGX_FAILED(result) ? result : NVSDK_NGX_Result_FAIL_PlatformError;
  }
  capacity_ = info.capacity;
  enabled_evaluations_ = 0u;
  last_enabled_ = false;
  has_evaluation_ = false;
  Logf(LogLevel::INFO, "NR feature created {}x{} (Vulkan, preset request {}, handle {}); runtime network: {}",
       capacity_.width, capacity_.height, info.preset, static_cast<const void*>(handle_),
       network_.empty() ? std::string_view("unconfirmed: no native creation message") : std::string_view(network_));
  return result;
}

NVSDK_NGX_Result VkFeature::Evaluate(ID3D12GraphicsCommandList* list, const EvaluateInputs& inputs, const Controls& controls) {
  if (handle_ == nullptr || list == nullptr || inputs.color == nullptr || inputs.output == nullptr) {
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  // The NR floor, enforced here too so no caller can reach NGX below it. A Vulkan image has no GetDesc: its size is the one the
  // caller's NVSDK_NGX_Resource_VK says.
  const NVSDK_NGX_ImageViewInfo_VK& output_view = VkResourceOf(inputs.output)->Resource.ImageViewInfo;
  const Size output_size = {output_view.Width, output_view.Height};
  if (!MeetsNrFloor(output_size)) {
    Logf(LogLevel::ERR, "NR refused a {}x{} output: the minimum is {}x{}", output_size.width, output_size.height, MIN_NR_LONG_SIDE,
         MIN_NR_SHORT_SIDE);
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  SetResource(parameters_, keys::COLOR, {inputs.color, {}});
  SetResource(parameters_, keys::OUTPUT, {inputs.output, {}});
  SetResource(parameters_, keys::MVEC, inputs.motion);
  SetResource(parameters_, keys::DEPTH, inputs.depth);
  SetResource(parameters_, keys::CONTROL_MASK, inputs.control_mask);
  SetResource(parameters_, keys::BACKBUFFER, inputs.backbuffer);
  SetResource(parameters_, keys::UI, inputs.ui);
  SetResource(parameters_, keys::UI_ALPHA, inputs.ui_alpha);
  SetResource(parameters_, keys::BIDIRECTIONAL_DISTORTION_FIELD, {nullptr, {}});
  parameters_->Set(keys::MVEC_SCALE_X, inputs.motion_scale_x);
  parameters_->Set(keys::MVEC_SCALE_Y, inputs.motion_scale_y);
  parameters_->Set(keys::INTENSITY, controls.intensity);
  parameters_->Set(keys::LOCAL_TONE, controls.local_tone);
  parameters_->Set(keys::LOCAL_STRUCTURE, controls.local_structure);
  parameters_->Set(keys::USE_AUTO_MASK, controls.auto_mask ? 1 : 0);
  parameters_->Set(keys::SKIN_STRUCTURE, controls.skin_structure);
  parameters_->Set(keys::STYLE, controls.style);
  parameters_->Set(keys::RESET, inputs.reset ? 1 : 0);
  parameters_->Set(keys::ENABLED, inputs.bypass ? 0 : 1);
  parameters_->Set(keys::UI_CORRECTION, inputs.ui_correction ? 1 : 0);
  parameters_->Set(keys::DEPTH_INVERTED, inputs.depth_inverted);
  const NVSDK_NGX_Result result = snippet_.EvaluateFeatureVulkan(VkListOf(list), handle_, parameters_);
  if (NVSDK_NGX_SUCCEED(result)) {
    has_evaluation_ = true;
    const bool enabled = !inputs.bypass && controls.intensity > 0.f;
    if (enabled && (enabled_evaluations_ == 0u || !last_enabled_ || last_style_ != controls.style || last_auto_mask_ != controls.auto_mask)) {
      Logf(LogLevel::INFO, "NR diagnostic: enabled Vulkan evaluation recorded (NGX {:#010x}, handle {}, style {}, intensity {}, character mask {}, motion {}, native control mask {}); runtime network: {}",
           static_cast<uint32_t>(result), static_cast<const void*>(handle_), controls.style, controls.intensity,
           controls.auto_mask, inputs.motion.resource != nullptr, inputs.control_mask.resource != nullptr,
           network_.empty() ? std::string_view("unconfirmed") : std::string_view(network_));
    }
    if (enabled) ++enabled_evaluations_;
    last_enabled_ = enabled;
    last_style_ = controls.style;
    last_auto_mask_ = controls.auto_mask;
  }
  return result;
}

std::string VkFeature::Diagnostic(uint32_t pass) const {
  return FeatureDiagnostic(pass, handle_ != nullptr, network_, has_evaluation_, enabled_evaluations_, last_enabled_, last_style_);
}

NVSDK_NGX_Result VkFeature::Release() {
  return ReleaseFeatureState(snippet_, handle_, capacity_);
}

void VkFeature::Abandon() {
  // Device-lost path: never call ReleaseFeature or DestroyParameters. The handle and the parameter object are deliberately leaked; the
  // dtor's "leaked" warning is suppressed because both are now null.
  handle_ = nullptr;
  parameters_ = nullptr;
  capacity_ = {};
}

}  // namespace uplift::nr
