#include "ngx_hooks/dlss_capture.hpp"

#include <cmath>
#include <utility>

#include "ngx_hooks/ngx_parameters.hpp"
#include "nr/d3d11_handles.hpp"
#include "nr/keys.hpp"

namespace uplift::ngx_hooks {

namespace {

// The capture of every API: the scalars and regions are read the same; `read` fetches the five resources, as the API's block holds them.
DlssFrame CaptureWith(const NVSDK_NGX_Parameter& parameters, const NVSDK_NGX_Handle* handle, const FeatureRecord& record,
                      ID3D12Resource* (*read)(const NVSDK_NGX_Parameter&, const char*)) {
  const auto one_when_unset = [&parameters](const char* key) {
    const std::optional<float> value = ReadFloat(parameters, key);
    return ((value.has_value() && *value != 0.f && std::isfinite(*value)) ? *value : 1.f);
  };
  const auto base = [&parameters](const char* key) { return ReadUint(parameters, key).value_or(0u); };
  const nr::Size render_subrect = {base(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width),
                                   base(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height)};
  const nr::Size render = (render_subrect.Empty() ? record.snapshot.render : render_subrect);
  const nr::Size output = record.snapshot.output;
  const nr::Size motion = (record.snapshot.MvLowRes() ? render : output);
  return {
      .handle = handle,
      .feature = record.feature,
      .snapshot = record.snapshot,
      .serial = record.serial,
      .color = read(parameters, NVSDK_NGX_Parameter_Color),
      .output = read(parameters, NVSDK_NGX_Parameter_Output),
      .motion_vectors = read(parameters, NVSDK_NGX_Parameter_MotionVectors),
      .depth = read(parameters, NVSDK_NGX_Parameter_Depth),
      .exposure_texture = read(parameters, NVSDK_NGX_Parameter_ExposureTexture),
      .jitter_x = ReadFloat(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X).value_or(0.f),
      .jitter_y = ReadFloat(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y).value_or(0.f),
      .mv_scale_x = one_when_unset(NVSDK_NGX_Parameter_MV_Scale_X),
      .mv_scale_y = one_when_unset(NVSDK_NGX_Parameter_MV_Scale_Y),
      .pre_exposure = one_when_unset(NVSDK_NGX_Parameter_DLSS_Pre_Exposure),
      .exposure_scale = one_when_unset(NVSDK_NGX_Parameter_DLSS_Exposure_Scale),
      .reset = (ReadInt(parameters, NVSDK_NGX_Parameter_Reset).value_or(0) != 0),
      .render = render,
      .color_region = {base(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X),
                       base(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y), render.width, render.height},
      .motion_region = {base(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X),
                        base(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y), motion.width, motion.height},
      .depth_region = {base(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X),
                       base(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y), render.width, render.height},
      .output_region = {base(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X),
                        base(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y), output.width, output.height},
  };
}

}  // namespace

DlssFrame CaptureDlssFrame(const NVSDK_NGX_Parameter& parameters, const NVSDK_NGX_Handle* handle, const FeatureRecord& record) {
  return CaptureWith(parameters, handle, record, &ReadResource);
}

DlssFrame CaptureNativeNrFrame(const NVSDK_NGX_Parameter& parameters, const NVSDK_NGX_Handle* handle, const FeatureRecord& record) {
  const auto resource = [&parameters](const char* key) { return ReadResource(parameters, key); };
  const auto base = [&parameters](const char* key) { return ReadUint(parameters, key).value_or(0u); };
  const auto region = [&](const nr::keys::Resource& key, ID3D12Resource* image) {
    const uint32_t x = base(key.subrect.base_x), y = base(key.subrect.base_y);
    uint32_t width = base(key.subrect.width), height = base(key.subrect.height);
    if ((width == 0u || height == 0u) && image != nullptr) {
      const D3D12_RESOURCE_DESC desc = image->GetDesc();
      if (width == 0u && x < desc.Width) width = static_cast<uint32_t>(desc.Width - x);
      if (height == 0u && y < desc.Height) height = desc.Height - y;
    }
    return nr::Rect{x, y, width, height};
  };
  const auto bound = [&](const nr::keys::Resource& key) {
    ID3D12Resource* const image = resource(key.resource);
    return nr::BoundResource{image, image != nullptr ? region(key, image) : nr::Rect{}};
  };
  const auto flag = [&parameters](const char* key) -> std::optional<bool> {
    if (const auto value = ReadInt(parameters, key)) return *value != 0;
    if (const auto value = ReadUint(parameters, key)) return *value != 0u;
    return std::nullopt;
  };
  const auto scale = [&parameters](const char* key) {
    const std::optional<float> value = ReadFloat(parameters, key);
    return value && std::isfinite(*value) && *value != 0.f ? *value : 1.f;
  };
  ID3D12Resource* const color = resource(nr::keys::COLOR.resource);
  ID3D12Resource* const output = resource(nr::keys::OUTPUT.resource);
  ID3D12Resource* const motion = resource(nr::keys::MVEC.resource);
  ID3D12Resource* const depth = resource(nr::keys::DEPTH.resource);
  DlssFrame frame = {
      .handle = handle, .feature = record.feature, .snapshot = record.snapshot, .serial = record.serial,
      .color = color, .output = output, .motion_vectors = motion, .depth = depth,
      .mv_scale_x = scale(nr::keys::MVEC_SCALE_X), .mv_scale_y = scale(nr::keys::MVEC_SCALE_Y),
      .reset = flag(nr::keys::RESET).value_or(false),
      .render = record.snapshot.render,
      .color_region = region(nr::keys::COLOR, color), .motion_region = region(nr::keys::MVEC, motion),
      .depth_region = region(nr::keys::DEPTH, depth), .output_region = region(nr::keys::OUTPUT, output),
      .native_nr = NativeNrInputs{
          .control_mask = bound(nr::keys::CONTROL_MASK), .backbuffer = bound(nr::keys::BACKBUFFER),
          .ui = bound(nr::keys::UI), .ui_alpha = bound(nr::keys::UI_ALPHA),
          .depth_inverted = flag(nr::keys::DEPTH_INVERTED),
          .ui_correction = flag(nr::keys::UI_CORRECTION).value_or(false),
          .enabled = flag(nr::keys::ENABLED).value_or(true)},
  };
  if (frame.render.Empty()) frame.render = {frame.color_region.width, frame.color_region.height};
  return frame;
}

DlssFrame CaptureD3D11DlssFrame(const NVSDK_NGX_Parameter& parameters, const NVSDK_NGX_Handle* handle, const FeatureRecord& record) {
  // The ID3D11Resource type first, so a block that also answers a Direct3D 12 read never hands a wrong pointer type back.
  return CaptureWith(parameters, handle, record,
                     [](const NVSDK_NGX_Parameter& block, const char* key) { return nr::AsResource(ReadD3D11Resource(block, key)); });
}

DlssFrame CaptureVkDlssFrame(const NVSDK_NGX_Parameter& parameters, const NVSDK_NGX_Handle* handle, const FeatureRecord& record,
                             VkDlssResources* copies) {
  DlssFrame frame = CaptureDlssFrame(parameters, handle, record);
  *copies = {};
  const auto copy = [](ID3D12Resource*& field, NVSDK_NGX_Resource_VK* copied) {
    if (field == nullptr) return;
    *copied = *nr::VkResourceOf(field);  // the game's struct, read during the call
    field = nr::AsResource(copied);
  };
  copy(frame.color, &copies->color);
  copy(frame.output, &copies->output);
  copy(frame.motion_vectors, &copies->motion_vectors);
  copy(frame.depth, &copies->depth);
  copy(frame.exposure_texture, &copies->exposure_texture);
  return frame;
}

nr::Rect ResolveVkOutputRegion(const DlssFrame& frame, const VkDlssResources& copies) {
  if (frame.output_region.width != 0u && frame.output_region.height != 0u) return frame.output_region;
  if (frame.output == nullptr || copies.output.Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return {};
  const NVSDK_NGX_ImageViewInfo_VK& output = copies.output.Resource.ImageViewInfo;
  return {.x = 0u, .y = 0u, .width = output.Width, .height = output.Height};
}

MotionJitterOffset MotionJitterTracker::Update(const DlssFrame& frame) {
  if (!frame.snapshot.MvJittered() || frame.render.Empty() || !std::isfinite(frame.jitter_x) || !std::isfinite(frame.jitter_y)) {
    Reset();
    return {};
  }
  const Sample current = {.serial = frame.serial, .x = frame.jitter_x / static_cast<float>(frame.render.width),
                          .y = frame.jitter_y / static_cast<float>(frame.render.height)};
  const auto previous = std::exchange(previous_, current);
  if (!previous || previous->serial != current.serial || frame.reset) return {};
  // Motion points current -> previous: jittered motion contains previous jitter minus current jitter (NGX guide 3.6/3.7).
  return {.x = current.x - previous->x, .y = current.y - previous->y};
}

}  // namespace uplift::ngx_hooks
