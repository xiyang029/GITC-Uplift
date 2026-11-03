// The XMX mode's minimal runnable check: the ABI layouts the generator writes into XeFG (the
// static_asserts in src/xmx/generator.hpp restate the offsets; this proves the header compiles
// and links against what the add-on uses), the output-resource rule that a dispatch depends on, and
// the panel line's states.
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

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
  check(ui::FormatXmxLive({.hooked = true}) ==
            "XMX: XeFG is up; DLSS-G is not being called (a menu, a cutscene, or the game's own frame generation off)",
        "a game that stopped calling DLSS-G is a pause, not a failure");
  check(ui::FormatXmxLive({.hooked = true, .evaluating = true}) ==
            "XMX: XeFG is up, waiting for a frame from the game's DLSS-G",
        "still called, still no frame, is reported as waiting");
  check(ui::FormatXmxLive({.hooked = true, .evaluating = true, .idle = "waiting for two real frames in a row"}) ==
            "XMX: not generating (waiting for two real frames in a row)",
        "the wait is named");
  check(ui::FormatXmxLive({}) == "XMX: waiting for the game's DLSS-G to be evaluated",
        "the waiting state is reported");

  // The ported decisions (src/xmx/policy.hpp), checked without a game or libxess_fg.dll: the
  // generator's waits, its copy rule, its retirement of replaced resources, and the input areas XeFG
  // compares instead of the textures.
  {
    using namespace xmx::policy;
    auto texture = std::make_shared<int>(1);
    RetireQueue<std::shared_ptr<int>> queue;
    queue.Push(texture, 100);
    queue.Collect(100 + kRetireAfterPairs - 1);
    check(queue.Size() == 1 && texture.use_count() == 2, "a retired texture outlives the frames that may use it");
    queue.Collect(100 + kRetireAfterPairs);
    check(queue.Size() == 0 && texture.use_count() == 1, "and is released once its pairs have passed");
    int device_a = 0, device_b = 0;
    const ContextShape shape{&device_a, 1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM, 1};
    ContextShape other = shape;
    other.device = &device_b;
    check(!(shape == other), "a new device needs a new context");
    check(!PairIsStale(0, 100000) && PairIsStale(1000, 1000 + kStalePairMs + 1),
          "nothing captured yet, and a gap past the threshold, are both not a pair");
    auto plan = PlanFrames(3, 1, 1);
    check(plan.count == 1 && plan.index == 1 && !plan.repeat && plan.limited, "a model below the multiplier says so");
    plan = PlanFrames(3, 3, 1);
    check(plan.repeat && plan.index == 1, "and repeats its last frame past the limit");
    plan = PlanFrames(0, 0, 5);
    check(plan.count == 1 && plan.index == 1 && !plan.repeat, "an absent count is one frame");
    check(!CanCopy(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM),
          "a copy between sizes the GPU cannot do is refused");
    check(!CanCopy(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM),
          "so is one between families");
    check(CanCopy(1920, 1080, DXGI_FORMAT_R8G8B8A8_TYPELESS, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
          "the same typeless family can");
    const auto full = Area{0, 0, 1488, 836};
    check(InputArea(1488, 836, {0, 0, 1485, 835}) == Area{0, 0, 1485, 835}, "a declared area is the area read");
    check(InputArea(1488, 836, {}) == full, "an absent declaration means the whole texture");
    check(InputArea(1488, 836, {4, 0, 1485, 836}) == full, "so does one that runs off the texture");
    check(InputArea(1488, 836, {0xFFFFFFFFu, 0, 2, 836}) == full, "and one whose origin is nonsense");
    auto shared = SharedArea(InputArea(1488, 836, {}), InputArea(1485, 836, {}));
    check(shared.first == Area{0, 0, 1485, 836} && shared.second == Area{0, 0, 1485, 836},
          "inputs of different sizes share the smaller one's area");
    shared = SharedArea({8, 4, 1480, 830}, {0, 0, 1485, 836});
    check(shared.first == Area{8, 4, 1480, 830} && shared.second == Area{0, 0, 1480, 830},
          "an input keeps the origin it declared");
    check(SameShapeWithinOnePercent(1368, 1367) && SameShapeWithinOnePercent(772, 769),
          "this game's 0.67 setting is one picture rounded");
    check(!SameShapeWithinOnePercent(1024, 769), "a differently shaped pass is not");
    check(IdleText(Idle::None)[0] == '\0' && IdleText(Idle::NoPair)[0] != '\0', "every wait has words");
  }

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
