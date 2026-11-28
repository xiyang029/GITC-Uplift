#pragma once

#include <cstdint>
#include "sources/look_plan.hpp"

namespace uplift::sources {

// A config change invalidates the same histories in every stage, independent of the API owning the surfaces.
template <typename Sets>
void ApplyLookConfig(LookConfig& current, const LookConfig& next, Sets& sets) {
  const bool restart_stabilizer = (next.look.detail_radius != current.look.detail_radius || next.look.stabilize != current.look.stabilize
                                  || next.look.stabilize_detail != current.look.stabilize_detail || next.encoding != current.encoding
                                  || next.diffuse_white_nits != current.diffuse_white_nits || next.fixes.primaries != current.fixes.primaries
                                  || next.fixes.linear_unit_nits != current.fixes.linear_unit_nits);
  // Changing the strength freely is safe; switching or reshaping the change stabilizer restarts it.
  const bool restart_change = (!next.look.stabilize_change || !current.look.stabilize_change || next.look.shape != current.look.shape);
  for (auto& set : sets) {
    if (restart_stabilizer) set.look.history_valid = false;
    if (restart_change) set.look.change_valid = false;
  }
  current = next;
}

// Retire each set in index order, invoking its backend's operations in the supplied order.
template <typename Owner, typename Sets, typename... Retire>
void RetireStageSets(Owner& owner, Sets& sets, uint32_t first, Retire... retire) {
  for (uint32_t index = first; index < sets.size(); ++index) {
    ((owner.*retire)(&sets[index]), ...);
  }
}

}  // namespace uplift::sources
