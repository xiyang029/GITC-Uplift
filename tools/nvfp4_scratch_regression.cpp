#include <iostream>
#include <stdexcept>
#include "nr/timeline.hpp"
// Exercise cache ownership without exposing private production state or allocating GPU memory.
#include "nvfp4/NrFfnV2.cpp"

void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

int main() {
  try {
    using namespace NrFfnV2;
    auto* device = reinterpret_cast<ID3D12Device*>(uintptr_t{0x1000});
    auto* other = reinterpret_cast<ID3D12Device*>(uintptr_t{0x2000});
    auto& state = S();
    auto add = [&](ID3D12Device* d, uint64_t size, uint64_t used, bool tracked = true) {
      auto s = std::make_shared<Session>();
      s->lastUsed = used; s->tracked = tracked;
      state.sessions[{d, size}] = s;
      return std::weak_ptr<Session>(s);
    };
    auto oldest = add(device, 100, 1);
    add(device, 200, 2); add(device, 300, 3); add(other, 100, 1);
    TrimSessions(device, 400);
    Check(state.sessions.size() == 4, "unscoped cache was trimmed");
    uplift::nr::Timeline timeline([] { return UINT64_MAX; }, [](auto*, auto) {}, [](auto, auto) { return true; });
    bool gpu_done = false;
    timeline.SetTokenProbe([&](auto) { return gpu_done; });
    const auto token = timeline.IssueToken();
    const auto mark = timeline.MarkNow();
    unsigned pins = 0;
    {
      RecordingScope scope([&](std::shared_ptr<void> resource) {
        ++pins; timeline.ReleaseAfter(mark, [resource = std::move(resource)] {});
      });
      auto held = oldest.lock();
      scope.Pin(held); scope.Pin(held);
      Check(pins == 1, "scratch was retained more than once per evaluation");
      {
        RecordingScope nested([](auto) {});
        Check(recordingScope == &nested, "nested scope was not installed");
      }
      Check(recordingScope == &scope, "nested scope did not restore its parent");
      held.reset();
      TrimSessions(device, 400);
      Check(state.sessions.size() == 2 && state.sessions.contains({device, 300}) &&
            state.sessions.contains({other, 100}), "eviction kept the wrong alternate or changed another device");
      Check(!oldest.expired(), "eviction released a recorded scratch buffer");
    }
    Check(recordingScope == nullptr, "scope leaked across evaluations");
    gpu_done = true; timeline.Poll();
    Check(!oldest.expired(), "first execution completion freed a reusable recording");
    timeline.ReleaseRecordings({&token, 1}); timeline.Poll();
    Check(oldest.expired(), "completed and reset recording retained evicted scratch");

    state.sessions.clear();
    auto untracked = add(device, 100, 1, false);
    add(device, 200, 2); add(device, 300, 3);
    { RecordingScope scope([](auto) {}); TrimSessions(device, 400); }
    // An untracked session is never a trim target, so it must not count towards the warm set either: the tracked pair trims down to one.
    Check(!untracked.expired() && state.sessions.size() == 2 && state.sessions.contains({device, 100}) && state.sessions.contains({device, 300}),
          "untracked scratch lost its device lifetime or counted as a warm session");
    auto object = std::make_shared<int>(1);
    bool threw = false;
    {
      RecordingScope scope([&](auto) { if (!threw) throw std::runtime_error("injected retention failure"); ++pins; });
      try { scope.Pin(object); } catch (...) { threw = true; }
      const unsigned before = pins; scope.Pin(object);
      Check(threw && pins == before + 1, "failed retention was incorrectly marked pinned");
    }
    // Reset before completion is the opposite ordering: execution must still retain the resource.
    gpu_done = false;
    const auto second = timeline.IssueToken();
    std::weak_ptr<int> pending = object;
    timeline.ReleaseAfter(timeline.MarkNow(), [object] {}); object.reset();
    timeline.ReleaseRecordings({&second, 1}); timeline.Poll();
    Check(!pending.expired(), "recording reset freed scratch still on the GPU");
    gpu_done = true; timeline.Poll();
    Check(pending.expired(), "completed scratch was not freed after reset");
    timeline.Flush(std::chrono::milliseconds(0));
    state.sessions.clear();
    std::cout << "PASS NVFP4 cache eviction, recording/execution leases, nested scopes and failure recovery\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
