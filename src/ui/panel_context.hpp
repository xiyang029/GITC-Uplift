#pragma once

#include <cstdint>
#include <span>

struct ImDrawVert;

namespace uplift::ui {

struct OverlayView;
struct Settings;
struct OverlayState;

struct PanelInput {
  float width, height, dt, mouse_x, mouse_y, wheel_x, wheel_y;
  bool mouse_down[5];
  bool ctrl, shift, alt, super, focused;
  std::span<const bool> keys;
  std::span<const unsigned short> characters;
};

class PanelFrameSink {
 public:
  virtual uint64_t CreateAtlas(const void* rgba, uint32_t width, uint32_t height, uint32_t pitch) = 0;
  virtual bool UpdateAtlas(uint64_t id, const void* rgba, uint32_t pitch, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height) = 0;
  virtual void RetireAtlas(uint64_t id) = 0;
  virtual void Draw(uint64_t texture, const float clip[4], const ImDrawVert* vertices,
                    std::span<const unsigned short> indices) = 0;
 protected:
  ~PanelFrameSink() = default;
};

class PanelContext {
 public:
  PanelContext() = default;
  ~PanelContext();
  PanelContext(const PanelContext&) = delete;
  PanelContext& operator=(const PanelContext&) = delete;

  bool DrawFrame(const PanelInput& input, PanelFrameSink& sink, void* window, const OverlayView& view,
                 Settings* settings, OverlayState* state);
  void Suspend();

 private:
  void* context_ = nullptr;
  bool suspended_ = false;
};

}  // namespace uplift::ui
