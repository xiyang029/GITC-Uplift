#include "nr/feature.hpp"

#include "nr/feature_state.hpp"

#include "nr/keys.hpp"
#include "nr/log.hpp"
#include "nvfp4/NrNvfp4.h"

namespace uplift::nr {
namespace {

void SetResource(NVSDK_NGX_Parameter* parameters, const keys::Resource& key, const BoundResource& bound) {
  parameters->Set(key.resource, bound.resource);
  parameters->Set(key.subrect.base_x, bound.rect.x);
  parameters->Set(key.subrect.base_y, bound.rect.y);
  parameters->Set(key.subrect.width, bound.rect.width);
  parameters->Set(key.subrect.height, bound.rect.height);
}

}  // namespace

Feature::Feature(const Snippet& snippet) : snippet_(snippet) {}

Feature::~Feature() {
  if (handle_ != nullptr) {
    Log(LogLevel::ERR, "Feature destroyed while still created; the handle is leaked, not released unsafely");
  }
  if (parameters_ != nullptr) {
    snippet_.DestroyParameters(parameters_);
  }
}

NVSDK_NGX_Result Feature::Create(ID3D12GraphicsCommandList* list, const CreateInfo& info) {
  if (handle_ != nullptr || list == nullptr || info.capacity.Empty()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (parameters_ == nullptr) {
    parameters_ = snippet_.AllocateParameters();
  }
  if (parameters_ == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
  parameters_->Set(keys::CREATION_NODE_MASK, 1u);
  parameters_->Set(keys::VISIBILITY_NODE_MASK, 1u);
  parameters_->Set(keys::WIDTH, info.capacity.width);
  parameters_->Set(keys::HEIGHT, info.capacity.height);
  requested_preset_ = info.preset;
  // Custom weights use runtime preset 1 (Model A), whose launches they replace one by one.
  parameters_->Set(keys::PRESET, DlssNr::Nvfp4::RuntimePreset(info.preset));
  parameters_->Set(keys::SCALING_RATIO, 1.f);
  const NVSDK_NGX_Result result = snippet_.CreateFeature(list, parameters_, &handle_, &network_);
  if (NVSDK_NGX_FAILED(result) || handle_ == nullptr) {
    Logf(LogLevel::ERR, "CreateFeature {}x{} failed: {:#010x}", info.capacity.width, info.capacity.height, static_cast<uint32_t>(result));
    handle_ = nullptr;
    return NVSDK_NGX_FAILED(result) ? result : NVSDK_NGX_Result_FAIL_PlatformError;
  }
  capacity_ = info.capacity;
  enabled_evaluations_ = 0u;
  last_enabled_ = false;
  has_evaluation_ = false;
  Logf(LogLevel::INFO, "NR feature created {}x{} (D3D12, preset request {}, handle {}); runtime network: {}",
       capacity_.width, capacity_.height, info.preset, static_cast<const void*>(handle_),
       network_.empty() ? std::string_view("unconfirmed: no native creation message") : std::string_view(network_));
  return result;
}

NVSDK_NGX_Result Feature::Evaluate(ID3D12GraphicsCommandList* list, const EvaluateInputs& inputs, const Controls& controls) {
  if (handle_ == nullptr || list == nullptr || inputs.color == nullptr || inputs.output == nullptr) {
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  // The NR floor, enforced here too so no caller can reach NGX below it.
  const D3D12_RESOURCE_DESC output_desc = inputs.output->GetDesc();
  const Size output_size = {static_cast<uint32_t>(output_desc.Width), output_desc.Height};
  if (!MeetsNrFloor(output_size)) {
    Logf(LogLevel::ERR, "NR refused a {}x{} output: the minimum is {}x{}", output_size.width, output_size.height,
         MIN_NR_LONG_SIDE, MIN_NR_SHORT_SIDE);
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
  // Bracket each evaluation so the NvAPI layers can verify the requested model ran.
  DlssNr::Nvfp4::BeginEvaluation(requested_preset_);
  const NVSDK_NGX_Result result = snippet_.EvaluateFeature(list, handle_, parameters_);
  DlssNr::Nvfp4::EndEvaluation(list);
  if (DlssNr::Nvfp4::IsNvfp4(requested_preset_) && !DlssNr::Nvfp4::OursRan(true)) {
    // Use the session failure path rather than showing the runtime model as a fallback.
    Logf(LogLevel::ERR, "NR model {} did not run as requested ({}); refusing this pass, no fallback",
         requested_preset_, DlssNr::Nvfp4::Problem());
    return NVSDK_NGX_Result_FAIL_PlatformError;
  }
  if (NVSDK_NGX_SUCCEED(result)) {
    has_evaluation_ = true;
    const bool enabled = !inputs.bypass && controls.intensity > 0.f;
    if (enabled && (enabled_evaluations_ == 0u || !last_enabled_ || last_style_ != controls.style || last_auto_mask_ != controls.auto_mask)) {
      Logf(LogLevel::INFO, "NR diagnostic: enabled D3D12 evaluation recorded (NGX {:#010x}, handle {}, style {}, intensity {}, character mask {}, motion {}, native control mask {}); runtime network: {}",
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

std::string Feature::Diagnostic(uint32_t pass) const {
  return FeatureDiagnostic(pass, handle_ != nullptr, network_, has_evaluation_, enabled_evaluations_, last_enabled_, last_style_);
}

NVSDK_NGX_Result Feature::Release() {
  return ReleaseFeatureState(snippet_, handle_, capacity_);
}

void Feature::Abandon() {
  // Device-lost path: never call ReleaseFeature or DestroyParameters. The
  // handle and the parameter object are deliberately leaked; the dtor's
  // "leaked" warning is suppressed because both are now null.
  handle_ = nullptr;
  parameters_ = nullptr;
  capacity_ = {};
}

}  // namespace uplift::nr
