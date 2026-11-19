#include "sources/look_plan.hpp"

namespace uplift::sources {

LookPlan PlanLook(const LookConfig& config, nr::Size image, bool look_loads) {
  const bool shape = (look::LookRuns(config.look) && look_loads);
  const bool stabilize = (shape && config.look.stabilize != look::StabilizeMode::OFF);
  // The change stabiliser is NR's own fix, not part of shaping: it runs with Shape settings at their defaults, needs no typed UAV loads (it reads C as
  // a texture), and is switched independently, so turning it on never makes the shape pass run and never allocates the shape's surfaces.
  const bool change = config.look.stabilize_change;
  return {
      .image = ((shape || change) ? image : nr::Size{}),
      .shape = shape,
      .stabilize = stabilize,
      .detail = (stabilize && config.look.stabilize_detail),
      .change = change,
  };
}

uint64_t FaceSurfaceBytes(nr::Size canvas) {
  constexpr uint64_t RGBA16F_BYTES = 8u;
  return (2u * canvas.Pixels() + 2u * look::MakeAtlas(canvas).size.Pixels()) * RGBA16F_BYTES;
}

nr::Controls FaceTwinControls(const nr::Controls& pass1, float protection) {
  nr::Controls twin = pass1;
  twin.auto_mask = true;
  twin.skin_structure = protection;
  return twin;
}

}  // namespace uplift::sources
