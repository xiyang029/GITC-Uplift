#include "addon/panel_renderer.hpp"

#include <algorithm>
#include <array>

#include "addon/reshade_api.hpp"
#include "ui/overlay.hpp"

namespace uplift::addon {
namespace {
namespace api = reshade::api;

void DestroyTexture(api::device* device, uint64_t view, uint64_t resource) {
  device->destroy_resource_view({view});
  device->destroy_resource({resource});
}

class FrameSink final : public ui::PanelFrameSink {
 public:
  FrameSink(api::effect_runtime* runtime, std::map<uint64_t, uint64_t>& textures)
      : device_(runtime->get_device()), queue_(runtime->get_command_queue()), textures_(textures) {}

  uint64_t CreateAtlas(const void* rgba, uint32_t width, uint32_t height, uint32_t pitch) override {
    constexpr auto format = api::format::r8g8b8a8_unorm;
    constexpr auto shader = api::resource_usage::shader_resource;
    const api::resource_desc desc(width, height, 1, 1, format, 1, api::memory_heap::default_,
                                  shader | api::resource_usage::copy_dest);
    const api::subresource_data initial{const_cast<void*>(rgba), pitch, pitch * height};
    api::resource resource{};
    if (!device_->create_resource(desc, &initial, shader, &resource)) return 0;
    api::resource_view view{};
    if (!device_->create_resource_view(resource, shader, api::resource_view_desc(format), &view)) {
      device_->destroy_resource(resource);
      return 0;
    }
    try {
      textures_.emplace(view.handle, resource.handle);
    } catch (...) {
      DestroyTexture(device_, view.handle, resource.handle);
      throw;
    }
    return view.handle;
  }

  bool UpdateAtlas(uint64_t id, const void* rgba, uint32_t pitch, uint32_t x, uint32_t y,
                   uint32_t width, uint32_t height) override {
    const auto found = textures_.find(id);
    if (!queue_ || found == textures_.end()) return false;
    auto* cmd = queue_->get_immediate_command_list();
    if (!cmd) return false;
    const api::resource texture{found->second};
    const api::subresource_box rect{x, y, 0, x + width, y + height, 1};
    constexpr auto shader = api::resource_usage::shader_resource;
    constexpr auto copy = api::resource_usage::copy_dest;
    cmd->barrier(texture, shader, copy);
    cmd->update_texture_region({const_cast<void*>(rgba), pitch, pitch * height}, texture, 0, &rect);
    cmd->barrier(texture, copy, shader);
    return true;
  }

  void RetireAtlas(uint64_t id) override {
    if (const auto found = textures_.find(id); found != textures_.end()) {
      if (queue_) queue_->wait_idle();  // Older presents may still reference a replaced atlas.
      DestroyTexture(device_, found->first, found->second);
      textures_.erase(found);
    }
  }

  void Draw(uint64_t texture, const float clip[4], const ImDrawVert* vertices,
            std::span<const unsigned short> indices) override {
    if (!draw_) draw_ = ImGui::GetForegroundDrawList(nullptr);
    draw_->PushClipRect({clip[0], clip[1]}, {clip[2], clip[3]}, true);
    draw_->PushTexture(ImTextureRef(texture));
    // Rebase each batch on the host draw list. PrimReserve switches VtxOffset
    // when the host reaches its 16-bit index limit.
    constexpr size_t kBatch = 4095;  // A whole number of triangles.
    while (!indices.empty()) {
      const auto count = (std::min)(indices.size(), kBatch);
      draw_->PrimReserve(static_cast<int>(count), static_cast<int>(count));
      for (const auto index : indices.first(count)) {
        const auto& vertex = vertices[index];
        draw_->PrimVtx(vertex.pos, vertex.uv, vertex.col);
      }
      indices = indices.subspan(count);
    }
    draw_->PopTexture();
    draw_->PopClipRect();
  }

 private:
  api::device* device_;
  api::command_queue* queue_;
  std::map<uint64_t, uint64_t>& textures_;
  ImDrawList* draw_ = nullptr;
};

}  // namespace

bool PanelRenderer::Draw(api::effect_runtime* runtime, const ui::OverlayView& view, ui::Settings* settings,
                         ui::OverlayState* state) {
  auto& io = ImGui::GetIO();
  std::array<bool, ImGuiKey_MouseLeft - ImGuiKey_NamedKey_BEGIN> keys{};
  for (size_t i = 0; i < keys.size(); ++i) {
    keys[i] = ImGui::IsKeyDown(static_cast<ImGuiKey>(ImGuiKey_NamedKey_BEGIN + i));
  }
  const ui::PanelInput input{
      .width = io.DisplaySize.x, .height = io.DisplaySize.y, .dt = io.DeltaTime,
      .mouse_x = io.MousePos.x, .mouse_y = io.MousePos.y, .wheel_x = io.MouseWheelH, .wheel_y = io.MouseWheel,
      .mouse_down = {io.MouseDown[0], io.MouseDown[1], io.MouseDown[2], io.MouseDown[3], io.MouseDown[4]},
      .ctrl = io.KeyCtrl, .shift = io.KeyShift, .alt = io.KeyAlt, .super = io.KeySuper, .focused = !io.AppFocusLost,
      .keys = keys, .characters = {io.InputQueueCharacters.Data, static_cast<size_t>(io.InputQueueCharacters.Size)},
  };
  auto& panel = panels_[runtime];
  FrameSink sink(runtime, panel.textures);
  state->panel_drew = false;
  const bool edited = panel.context.DrawFrame(input, sink, runtime->get_hwnd(), view, settings, state);
  if (!state->panel_open) panel.context.Suspend();
  if (state->panel_drew) io.MouseDrawCursor = true;
  return edited;
}

void PanelRenderer::Suspend(api::effect_runtime* runtime) {
  if (const auto found = panels_.find(runtime); found != panels_.end()) found->second.context.Suspend();
}

void PanelRenderer::Release(api::effect_runtime* runtime) {
  if (const auto found = panels_.find(runtime); found != panels_.end()) {
    // ReShade waits for its graphics queue before dispatching destroy_effect_runtime.
    for (const auto& [view, resource] : found->second.textures) DestroyTexture(runtime->get_device(), view, resource);
    panels_.erase(found);
  }
}

}  // namespace uplift::addon
