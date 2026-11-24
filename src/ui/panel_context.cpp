#include "ui/panel_context.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

#include "ui/overlay.hpp"
#include "ui/panel_font.hpp"

namespace uplift::ui {
namespace {

class ContextScope {
 public:
  explicit ContextScope(ImGuiContext* context) : previous_(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(context); }
  ~ContextScope() { ImGui::SetCurrentContext(previous_); }
 private:
  ImGuiContext* previous_;
};

bool SyncAtlas(ImDrawData& data, PanelFrameSink& sink) {
  bool ready = true;
  for (ImTextureData* texture : *data.Textures) {
    switch (texture->Status) {
      case ImTextureStatus_WantCreate: {
        const auto id = sink.CreateAtlas(texture->GetPixels(), texture->Width, texture->Height, texture->GetPitch());
        if (id != ImTextureID_Invalid) {
          texture->SetTexID(id);
          texture->SetStatus(ImTextureStatus_OK);
        } else {
          ready = false;
        }
        break;
      }
      case ImTextureStatus_WantUpdates: {
        const auto& rect = texture->UpdateRect;
        if (sink.UpdateAtlas(texture->GetTexID(), texture->GetPixelsAt(rect.x, rect.y), texture->GetPitch(),
                             rect.x, rect.y, rect.w, rect.h)) {
          texture->SetStatus(ImTextureStatus_OK);
        } else {
          ready = false;
        }
        break;
      }
      case ImTextureStatus_WantDestroy:
        if (texture->UnusedFrames > 0) {
          sink.RetireAtlas(texture->GetTexID());
          texture->SetTexID(ImTextureID_Invalid);
          texture->SetStatus(ImTextureStatus_Destroyed);
        }
        break;
      default:
        break;
    }
  }
  return ready;
}

}  // namespace

PanelContext::~PanelContext() {
  if (context_) ImGui::DestroyContext(static_cast<ImGuiContext*>(context_));
}

bool PanelContext::DrawFrame(const PanelInput& input, PanelFrameSink& sink, void* window, const OverlayView& view,
                             Settings* settings, OverlayState* state) {
  if (input.width <= 0.f || input.height <= 0.f) return false;
  const ContextScope context_scope(static_cast<ImGuiContext*>(context_));
  if (!context_) {
    context_ = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendFlags = ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigFlags = ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigInputTrickleEventQueue = false;  // The host has already processed its input queue.
    ImGui::GetMainViewport()->PlatformHandleRaw = window;
    LoadPanelTypeface();
  }
  auto& io = ImGui::GetIO();
  io.DisplaySize = {input.width, input.height};
  io.DeltaTime = (std::max)(input.dt, 0.0001f);
  io.AddFocusEvent(input.focused);
  io.AddMousePosEvent(input.mouse_x, input.mouse_y);
  for (int i = 0; i < IM_ARRAYSIZE(input.mouse_down); ++i) io.AddMouseButtonEvent(i, input.mouse_down[i]);
  io.AddMouseWheelEvent(input.wheel_x, input.wheel_y);
  io.AddKeyEvent(ImGuiMod_Ctrl, input.ctrl);
  io.AddKeyEvent(ImGuiMod_Shift, input.shift);
  io.AddKeyEvent(ImGuiMod_Alt, input.alt);
  io.AddKeyEvent(ImGuiMod_Super, input.super);
  for (size_t i = 0; i < input.keys.size(); ++i) {
    io.AddKeyEvent(static_cast<ImGuiKey>(ImGuiKey_NamedKey_BEGIN + i), input.keys[i]);
  }
  for (const auto c : input.characters) io.AddInputCharacter(c);
  suspended_ = false;

  ImGui::NewFrame();
  const bool edited = DrawOverlayWindow(view, settings, state, window);
  ImGui::Render();
  auto& data = *ImGui::GetDrawData();
  if (!SyncAtlas(data, sink)) {
    state->panel_drew = false;  // Do not block game input when the panel could not be rendered.
    return edited;
  }
  for (const auto* list : data.CmdLists) {
    for (const auto& cmd : list->CmdBuffer) {
      if (cmd.UserCallback || !cmd.ElemCount || cmd.GetTexID() == ImTextureID_Invalid) continue;
      const float clip[] = {cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w};
      sink.Draw(cmd.GetTexID(), clip, list->VtxBuffer.Data + cmd.VtxOffset,
                {list->IdxBuffer.Data + cmd.IdxOffset, cmd.ElemCount});
    }
  }
  return edited;
}

void PanelContext::Suspend() {
  if (!context_ || suspended_) return;
  const ContextScope scope(static_cast<ImGuiContext*>(context_));
  auto& io = ImGui::GetIO();
  io.ClearInputKeys();
  io.ClearInputMouse();
  ImGui::ClearActiveID();
  ImGui::ClearDragDrop();
  if (!ImGui::GetCurrentContext()->OpenPopupStack.empty()) ImGui::ClosePopupToLevel(0, false);
  suspended_ = true;
}

}  // namespace uplift::ui
