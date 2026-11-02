// The XMX mode's minimal runnable check: the ABI layouts the generator writes into XeFG (the
// static_asserts in src/xmx/generator.hpp restate the offsets; this proves the header compiles
// and links against what the add-on uses), the output-resource rule that a dispatch depends on, and
// the panel line's states.
#include <cstddef>
#include <cstdio>

#include "ui/status_text.hpp"
#include "xmx/generator.hpp"
#include "xmx/host.hpp"

int main() {
  using namespace uplift;
  int failures = 0;
  const auto check = [&failures](bool ok, const char* what) {
    if (!ok) {
      std::printf("FAIL %s\n", what);
      ++failures;
    }
  };
  check(sizeof(xmx::generator::InitParams) == 80, "InitParams is 80 bytes");
  check(sizeof(xmx::generator::FrameData) == 0x118, "FrameData is 0x118");
  check(sizeof(xmx::generator::ExecuteParams) == 0x260, "ExecuteParams is 0x260");
  check(offsetof(xmx::generator::ExecuteParams, outputs) == 0x230, "ExecuteParams.outputs is 0x230");
  check(xmx::generator::kMaxFrames == 5, "the model serves 5 generated frames");

  // The output-resource rule. XeFG dispatches into its output by UAV, so the texture must have started
  // in UNORDERED_ACCESS and carry the UAV flag. The regression this guards is the removed "write
  // straight into DLSS-G's buffer" path: it handed XeFG a buffer NGX owned, whose current state is not
  // knowable here. The counters looked healthy while the picture stopped interpolating.
  using xmx::generator::XefgOutputEligible;
  check(XefgOutputEligible(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        "our own output textures are eligible");
  check(!XefgOutputEligible(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        "a buffer the GPU still has as a render target is not");
  check(!XefgOutputEligible(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        "a presentable buffer is not");
  check(!XefgOutputEligible(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_NONE),
        "the state alone is not enough: the resource must also carry the UAV flag");
  check(!XefgOutputEligible(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        "a copy destination is not");
  check(ui::FormatXmxLive({.failure = "libxell.dll not found"}) == "XMX: cannot start (libxell.dll not found)",
        "a refusal names its reason");
  check(ui::FormatXmxLive({.generation = true, .hooked = true}) == "XMX: Intel XeFG is generating",
        "generating is reported");
  check(ui::FormatXmxLive({.generation = true, .hooked = true, .layers = 28, .launches = 3600}) ==
            "XMX: Intel XeFG is generating (tensor cores 28 layers, 3600 launches)",
        "the tensor-core counters are reported");
  check(ui::FormatXmxLive({.generation = true, .hooked = true, .layers = 28, .launches = 3600, .fallbacks = 2}) ==
            "XMX: Intel XeFG is generating (tensor cores 28 layers, 3600 launches, 2 fell back)",
        "a fallback is never hidden");
  check(ui::FormatXmxLive({.hooked = true}) == "XMX: XeFG is up, waiting for the game's first DLSS-G frame",
        "up and idle is reported");
  check(ui::FormatXmxLive({}) == "XMX: waiting for the game's DLSS-G to be evaluated",
        "the waiting state is reported");

  // The panel's switch is live: off stops generating without a restart, and a null list or block
  // is refused rather than dereferenced. (RepeatRealFrame needs a real command list, so only its
  // guards are reachable here; the rejection contract is what a wrong turn would break.)
  xmx::SetEnabled(false);
  check(!xmx::Enabled(), "off is latched at once");
  check(!xmx::generator::Evaluate(nullptr, nullptr), "a null evaluate is refused");
  xmx::SetEnabled(true);
  check(xmx::Enabled(), "on is latched at once");

  std::printf(failures == 0 ? "xmx checks passed\n" : "%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
