#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "build_id.hpp"
#include "addon/frame_trigger.hpp"
#include "ipc/helper_launcher.hpp"
#include "ipc/control_block.hpp"
#include "ipc/settings_text.hpp"
#include "nr/session_status.hpp"
#include "ui/settings.hpp"

using Microsoft::WRL::ComPtr;
using namespace uplift;
void Check(bool condition, const char* what) { if (!condition) throw std::runtime_error(what); }
void Hr(HRESULT hr) { Check(SUCCEEDED(hr), "D3D12 regression operation failed"); }

int wmain(int argc, wchar_t** argv) {
  try {
    Check(argc == 2, "pass the identity helper executable path");
    SetEnvironmentVariableW(L"UPLIFT_D3D12_DEBUG_LAYER", L"1");
    ComPtr<IDXGIFactory6> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter; ComPtr<ID3D12Device> device; DXGI_ADAPTER_DESC1 desc{};
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == S_OK; ++i) {
      Hr(adapter->GetDesc1(&desc));
      if (desc.VendorId == 0x10de && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) break;
    }
    Check(device != nullptr, "NVIDIA D3D12 adapter unavailable");
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{}; Hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> commands;
    Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    Hr(device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    Hr(commands->Close());
    ComPtr<ID3D12Fence> local; Hr(device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&local)));
    uint64_t local_value = 0u;
    const auto idle = [&] {
      Hr(queue->Signal(local.Get(), ++local_value));
      HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr); Check(done != nullptr, "event creation failed");
      Hr(local->SetEventOnCompletion(local_value, done));
      const auto waited = WaitForSingleObject(done, 10000u); CloseHandle(done); Check(waited == WAIT_OBJECT_0, "GPU did not finish");
    };
    bool debug_error = false;
    unsigned executed = 0u, skipped = 0u;
    for (auto transport : {ipc::Transport::FENCED, ipc::Transport::SHARED_CPU}) {
      std::string error; auto helper = ipc::HelperLauncher::Start(argv[1], "nr-regression", UPLIFT_BUILD_ID, desc.AdapterLuid, &error);
      Check(helper != nullptr, error.c_str());
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (helper->State() == ipc::HelperState::STARTING && !helper->ExitCode() && std::chrono::steady_clock::now() < deadline) Sleep(1u);
      Check(helper->State() == ipc::HelperState::READY, "helper did not start");
      auto send = [&](const ipc::Request& request) {
        auto wait = helper->Send(request, 10000u);
        Check(wait == ipc::HelperLauncher::Wait::REPLIED, "helper request did not reply");
        helper->DrainLog([&](std::string_view line) {
          if (line.find("D3D12 debug layer:") != std::string_view::npos) { debug_error = true; std::cerr << line << '\n'; }
        });
        auto reply = helper->Block()->reply;
        Check(reply.ok != 0u, ipc::TextOf(reply.error).data());
        return reply;
      };
      ipc::Request attach{.kind = ipc::RequestKind::ATTACH};
      attach.attach = {.luid_low = desc.AdapterLuid.LowPart, .luid_high = desc.AdapterLuid.HighPart, .api = ipc::Api::D3D12, .transport = transport};
      send(attach);
      ui::Settings settings; settings.enabled = true; settings.source = ui::PlacementSource::PRESENT; settings.grace_seconds = 0.f;
      ipc::TextConfigStore store; ui::SaveSettings(settings, &store); ipc::CopyText(helper->Block()->settings_text, store.Serialize());
      ipc::Request frame{.kind = ipc::RequestKind::FRAME}; frame.frame.settings_generation = 1u;
      frame.frame.target = {.width = 1280u, .height = 720u, .dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM};
      ComPtr<ID3D12Resource> color; ComPtr<ID3D12Fence> to12, to11; uint64_t value = 0u;
      auto import = [&](const ipc::Reply& reply) {
        for (const auto& pair : {std::pair{reply.handles.to12, 0}, {reply.handles.to11, 1}, {reply.handles.color, 2}}) {
          if (!pair.first) continue; HANDLE handle = nullptr; Check(helper->Pull(pair.first, &handle), "remote handle lost");
          HRESULT hr = pair.second == 0 ? device->OpenSharedHandle(handle, IID_PPV_ARGS(&to12))
                     : pair.second == 1 ? device->OpenSharedHandle(handle, IID_PPV_ARGS(&to11))
                                        : device->OpenSharedHandle(handle, IID_PPV_ARGS(&color));
          CloseHandle(handle); Hr(hr);
        }
      };
      ipc::Reply ready;
      for (unsigned i = 0; i < 30u; ++i) { ready = send(frame); import(ready); if (ready.frame_ready && color) break; Sleep(1u); }
      Check(ready.frame_ready && color, "identity helper never became frame-ready");
      ComPtr<ID3D12DescriptorHeap> rtv; D3D12_DESCRIPTOR_HEAP_DESC heap{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1u};
      Hr(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv)));
      auto clear = [&] {
        idle(); Hr(allocator->Reset()); Hr(commands->Reset(allocator.Get(), nullptr));
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {color.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET};
        commands->ResourceBarrier(1u, &b); device->CreateRenderTargetView(color.Get(), nullptr, rtv->GetCPUDescriptorHandleForHeapStart());
        const float rgba[4] = {.125f, .25f, .5f, 1.f}; commands->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(), rgba, 0u, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter); commands->ResourceBarrier(1u, &b); Hr(commands->Close());
        ID3D12CommandList* lists[] = {commands.Get()}; queue->ExecuteCommandLists(1u, lists);
      };
      for (unsigned i = 0; i < 48u; ++i) {
        clear();
        ipc::Request request = frame; request.kind = (transport == ipc::Transport::FENCED && i > 0u) ? ipc::RequestKind::FRAME_RUN : ipc::RequestKind::RUN;
        if (request.kind == ipc::RequestKind::RUN) { auto reply = send(frame); import(reply); }
        request.run.point = static_cast<uint32_t>(addon::TriggerPoint::PRESENT);
        if (to12) { request.run.in = ++value; request.run.out = value; Hr(queue->Signal(to12.Get(), value)); }
        else idle();
        auto reply = send(request);
        Check(reply.run_skipped == 0u && reply.submitted && reply.wrote, "steady handoff did not record its result");
        Check(reply.status.nr_applied && reply.status.passes_run == 1u, "RUN status was not published with the reply");
        if (to11) { Check(reply.signalled, "helper refused the output signal"); Hr(queue->Wait(to11.Get(), value)); }
        idle(); ++executed;
      }
      if (transport == ipc::Transport::FENCED) {
        // Never signal these input values: a FRAME_RUN that invalidates the handoff must not wait.
        auto resized = frame; resized.kind = ipc::RequestKind::FRAME_RUN; resized.frame.target.width += 64u;
        resized.run.in = 1000000u; resized.run.out = 1000000u;
        auto reply = send(resized);
        Check(reply.run_skipped && !reply.waited && !reply.signalled && reply.handles.color, "resize consumed an obsolete handoff");
        import(reply); ++skipped;
        auto off = resized; off.frame.nr_allowed = 0u; off.frame.drain_now = 1u;
        reply = send(off); Check(reply.run_skipped && !reply.waited && !reply.signalled, "disabled FRAME_RUN queued an unconfirmed wait"); ++skipped;
      }
      send(ipc::Request{.kind = ipc::RequestKind::QUIT});
    }
    Check(!debug_error, "helper reported a D3D12 validation error");
    std::cout << "PASS helper process: " << executed << " FENCED/SHARED_CPU handoffs, " << skipped << " invalidated combined handoffs, reply publication and GPU debug layer\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
