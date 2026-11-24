#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "color/color_pipeline.hpp"
#include "look/look_math.hpp"
#include "nr/timeline.hpp"
#include "sources/after_dlss_source.hpp"
#include "testing/identity_host.hpp"

using Microsoft::WRL::ComPtr;
using namespace uplift;
void Check(bool yes, const char* why) { if (!yes) throw std::runtime_error(why); }
void Hr(HRESULT r) { if (FAILED(r)) throw std::runtime_error("D3D12 operation failed: " + std::to_string(r)); }

void RecordingBatches() {
  nr::Timeline timeline([] { return UINT64_MAX; }, [](auto*, auto) {}, [](auto, auto) { return true; });
  struct Host final : sources::DlssFrameHost {
    unsigned restores = 0u; std::vector<uint64_t> tokens;
    ID3D12GraphicsCommandList* NativeList() override { return nullptr; }
    bool StateKnown() override { return true; }
    void ReplayState(state::RestoreMode) override { ++restores; }
    void AttachToken(uint64_t token) override { tokens.push_back(token); }
    void HoldTiming(const std::shared_ptr<sources::NrTimingSlot>&) override {}
  } host;
  {
    sources::DlssRecordingBatch batch(host, state::RestoreMode::FULL);
    auto skip = sources::RecordOnGameList(batch, timeline, state::RestoreMode::FULL, [] { return sources::PipelineResult{}; });
    Check(skip.token == 0u && host.tokens.empty(), "empty recording attached a token");
    auto capture = sources::RecordOnGameList(batch, timeline, state::RestoreMode::FULL, [](uint64_t) { return sources::PipelineResult{.recorded = true}; });
    auto after = sources::RecordOnGameList(batch, timeline, state::RestoreMode::FULL, [](uint64_t) { return sources::PipelineResult{.recorded = true}; });
    skip = sources::RecordOnGameList(batch, timeline, state::RestoreMode::FULL, [] { return sources::PipelineResult{}; });
    Check(capture.token == after.token && host.tokens.size() == 1 && host.restores == 0u &&
          timeline.OutstandingTokens() == 1u, "capture and NR did not share one attached token");
  }
  Check(host.restores == 1u, "batch did not restore exactly once");
  timeline.Drop(host.tokens); host.tokens.clear();
  try {
    sources::DlssRecordingBatch batch(host, state::RestoreMode::FULL);
    sources::RecordOnGameList(batch, timeline, state::RestoreMode::FULL, []() -> sources::PipelineResult { throw std::runtime_error("injected recording failure"); });
  } catch (const std::runtime_error&) {}
  Check(host.restores == 2u && host.tokens.size() == 1u, "throwing recording lost lifetime or state restore");
  timeline.Drop(host.tokens); timeline.Flush(std::chrono::milliseconds(0));
  testing::IdentityHost runtime; nr::Session session(runtime, timeline);
  session.SetPassCount(3u);
  nr::Controls zero; zero.intensity = 0.f; nr::Controls active; active.intensity = 1.f;
  std::array<std::optional<nr::Controls>, 2> later = {active, std::nullopt};
  Check(session.SliceHasIntensity({.first = 0u, .count = 3u}, zero, later), "later active pass was skipped with pass 1 at zero");
  Check(!session.SliceHasIntensity({.first = 2u, .count = 1u}, zero, later), "slice included a different pass's intensity");
  std::cout << "PASS recording batching, exception restore and per-slice intensity\n";
}

void ProducerProofs() {
  struct Fences final : nr::QueueFences {
    uint64_t completed = 0u, signalled = 0u;
    bool Signal(ID3D12CommandQueue*, uint64_t value) override { signalled = value; return true; }
    uint64_t Completed(ID3D12CommandQueue*) const override { return completed; }
    bool Wait(ID3D12CommandQueue*, uint64_t value, std::chrono::milliseconds) override { return completed >= value; }
  };
  nr::Timeline timeline([] { return UINT64_MAX; }, [](auto*, auto) {}, [](auto, auto) { return true; });
  auto fences = std::make_unique<Fences>(); auto* progress = fences.get(); timeline.SetQueueFences(std::move(fences));
  auto* producer = reinterpret_cast<ID3D12CommandQueue*>(uintptr_t{1u});
  auto* other = reinterpret_cast<ID3D12CommandQueue*>(uintptr_t{2u});
  const auto now = std::chrono::steady_clock::now();
  const std::array recordings = {timeline.IssueToken(), timeline.IssueToken()};
  auto first = timeline.TrackSubmission(recordings[0]), second = timeline.TrackSubmission(recordings[1]);
  Check(!timeline.OrderedBefore(first.get(), producer), "unsubmitted producer was ready");
  timeline.Submit(recordings, producer, 17u, now);
  Check(!timeline.OrderedBefore(first.get(), producer), "pre-execute callback was mistaken for a completed native submission");
  // One whole frame later that window is closed: ReShade raised execute_command_list for that list a frame ago, so the
  // native ExecuteCommandLists it guards had returned before this frame began, and the queue alone orders the write.
  timeline.BeginFrame(producer);
  Check(timeline.OrderedBefore(first.get(), producer) && !timeline.OrderedBefore(first.get(), other),
        "a copy submitted on this queue in an earlier frame was refused");
  timeline.StampThread(17u);
  Check(timeline.OrderedBefore(first.get(), producer) && !timeline.OrderedBefore(first.get(), other), "producer queue ordering was not checked");
  progress->completed = progress->signalled; timeline.Poll();
  Check(timeline.OrderedBefore(first.get(), other), "completed producer proof did not survive token pruning");
  timeline.Resubmit(recordings.front(), producer, 17u, now, recordings);
  Check(!timeline.OrderedBefore(first.get(), producer) && !timeline.OrderedBefore(second.get(), other), "replayed copy retained stale first-execution proof");
  timeline.StampThread(17u); progress->completed = progress->signalled;
  timeline.ReleaseRecordings(recordings); timeline.Poll();
  const auto discarded = timeline.IssueToken(); auto absent = timeline.TrackSubmission(discarded);
  timeline.Drop({&discarded, 1u}); timeline.Poll();
  Check(!timeline.OrderedBefore(absent.get(), producer), "discarded producer was mistaken for a completed write");
  timeline.Flush(std::chrono::milliseconds(0));
  std::cout << "PASS producer submission, queue ordering, discard and replay after token pruning" << std::endl;
}

void Lifetime() {
  nr::Timeline t([] { return UINT64_MAX; }, [](auto*, auto) {}, [](auto, auto) { return true; });
  bool done = false, freed = false;
  t.SetTokenProbe([&](auto) { return done; });
  const auto token = t.IssueToken();
  t.ReleaseAfter(t.MarkNow(), [&] { freed = true; });
  done = true; t.Poll();
  Check(!freed && t.TokenComplete(token), "executable recording lost its resources after first completion");
  t.ReleaseRecordings({&token, 1}); t.Poll();
  Check(freed, "reset did not release completed recording");
  done = false;
  const auto executable = t.IssueToken();
  const auto owned = t.MarkNow(true);
  done = true; t.Poll();
  const auto once = t.MarkNow();
  Check(!t.IsReusable(owned) && t.IsReusable(once), "unrelated executable list blocked a one-shot slot");
  const auto later = t.IssueToken();
  const auto later_mark = t.MarkNow(true);
  t.ReleaseRecordings({&later, 1}); t.Poll();
  Check(t.IsReusable(later_mark) && !t.IsComplete(later_mark), "recording ownership and resource retirement were conflated");
  std::array<nr::Mark, 3> slots = {owned, once, later_mark};
  Check(t.FindReusableSlot(slots, 0u) == 1u && t.FindReusableSlot(slots, 0u, 1u) == 2u, "ring did not skip a pinned recording slot");
  done = false;
  const auto pending = t.IssueToken();
  const auto pending_mark = t.MarkNow(true);
  t.ReleaseRecordings({&pending, 1});
  Check(!t.IsReusable(pending_mark), "reset released a descriptor before GPU completion");
  done = true; t.Poll();
  Check(t.IsReusable(pending_mark), "completed reset slot remained busy");
  t.ReleaseRecordings({&executable, 1});
  const auto discarded = t.IssueToken(); t.Drop({&discarded, 1});
  Check(t.IsComplete(t.MarkNow()), "discarded recording retained resources");
  t.Flush(std::chrono::milliseconds(0));
  for (nr::Size size : {nr::Size{67, 53}, {1920, 1080}, {3840, 2160}}) {
    auto atlas = look::MakeAtlas(size);
    for (float radius : {0.f, .25f, 1.f, 5.f, 50.f}) {
      auto packed = look::LowHistorySize(atlas, radius);
      auto level = look::MakeBands(radius, size.height, atlas.levels).stable_level;
      Check(packed.height == look::LevelSize(size, level).height && packed.width <= atlas.size.width,
            "compact history does not contain its two stable levels");
    }
  }
  std::cout << "PASS recording lifetime, discard and compact Look history dimensions\n";
}

struct Gpu {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  color::ColorPipeline color;
  UINT64 value = 0;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::vector<ComPtr<ID3D12Resource>> uploads;
  ComPtr<ID3D12InfoQueue> validation;
  Gpu() {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == S_OK; ++i) {
      DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
      if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        std::wcout << L"GPU: " << d.Description << L'\n'; break;
      }
    }
    Check(device != nullptr, "hardware D3D12 GPU unavailable");
    device.As(&validation);
    D3D12_COMMAND_QUEUE_DESC q{}; Hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    std::string error; Check(color.Initialize(device.Get(), &error), error.c_str());
  }
  ~Gpu() { CloseHandle(event); }
  ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)));
    return r;
  }
  void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to}; list->ResourceBarrier(1, &b);
  }
  ComPtr<ID3D12Resource> Texture(UINT w, UINT h, DXGI_FORMAT format, const void* data = nullptr, UINT pixel_bytes = 0,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = d.MipLevels = 1; d.Format = format; d.SampleDesc.Count = 1; d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, data ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&r)));
    if (data) {
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT f{}; UINT64 bytes = 0; device->GetCopyableFootprints(&d, 0, 1, 0, &f, nullptr, nullptr, &bytes);
      auto upload = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD); void* p = nullptr; Hr(upload->Map(0, nullptr, &p));
      for (UINT y = 0; y < h; ++y) std::memcpy(static_cast<char*>(p) + y * f.Footprint.RowPitch, static_cast<const char*>(data) + y * w * pixel_bytes, w * pixel_bytes);
      upload->Unmap(0, nullptr);
      D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = f;
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = r.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); Barrier(r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); uploads.push_back(upload);
    }
    return r;
  }
  struct Read { ComPtr<ID3D12Resource> buffer; UINT pitch; };
  Read Readback(ID3D12Resource* t) {
    auto d = t->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT f{}; UINT64 bytes = 0;
    device->GetCopyableFootprints(&d, 0, 1, 0, &f, nullptr, nullptr, &bytes); auto buffer = Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
    Barrier(t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = t; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = buffer.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = f;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); return {buffer, f.Footprint.RowPitch};
  }
  void Submit() {
    Hr(list->Close()); ID3D12CommandList* commands = list.Get(); queue->ExecuteCommandLists(1, &commands);
    Hr(queue->Signal(fence.Get(), ++value)); Hr(fence->SetEventOnCompletion(value, event));
    Check(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU regression timed out");
    if (validation) {
      for (UINT64 i=0;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T bytes=0u;validation->GetMessage(i,nullptr,&bytes);std::vector<std::byte> storage(bytes);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Hr(validation->GetMessage(i,message,&bytes));
        if (message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {std::cerr<<message->pDescription<<'\n';throw std::runtime_error("D3D12 validation error");}
      } validation->ClearStoredMessages();
    }
    uploads.clear(); Hr(allocator->Reset()); Hr(list->Reset(allocator.Get(), nullptr));
  }
};

void ColorHistory() {
  Gpu gpu;
  testing::IdentityHost host;
  nr::Timeline timeline([] { return UINT64_MAX; }, [](auto*, auto) {}, [](auto, auto) { return true; });
  nr::Session session(host, timeline);
  session.SetEnabled(true); session.NoteFrameSize({640u, 360u});
  for (unsigned i = 0u; i < 1000u && session.State() != nr::SessionState::ACTIVE; ++i) { session.Tick(); Sleep(1u); }
  Check(session.State() == nr::SessionState::ACTIVE, "identity session unavailable");
  {
    sources::NrPipeline pipeline(gpu.device.Get(), session, timeline);
    std::string error; Check(pipeline.Initialize(&error), error.c_str());
    std::vector<uint16_t> pixels(size_t(640u) * 360u * 4u, DirectX::PackedVector::XMConvertFloatToHalf(.25f));
    auto target = gpu.Texture(640u, 360u, DXGI_FORMAT_R16G16B16A16_FLOAT, pixels.data(), 8u);
    gpu.Barrier(target.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sources::LookConfig config;
    const auto frame = [&](color::Primaries primaries, bool reset, bool expected) {
      config.fixes.primaries = primaries; pipeline.SetLookConfig(config);
      const auto result = pipeline.RecordUav(gpu.list.Get(), {.resource = target.Get(), .encoding = color::Encoding::LINEAR_BT709},
                                              {.reset_hint = reset}, nr::Controls{});
      Check(result.nr_applied, "colour-history frame did not apply NR");
      Check(!host.resets.empty() && host.resets.back() == expected, "colour basis or explicit reset was lost/repeated");
      gpu.Submit();
    };
    frame(color::Primaries::AUTO, false, true);
    frame(color::Primaries::BT709, false, false);  // the same resolved primaries
    frame(color::Primaries::BT2020, false, true);
    frame(color::Primaries::BT2020, false, false);
    frame(color::Primaries::AP1, false, true);
    frame(color::Primaries::AP1, true, true); frame(color::Primaries::AP1, true, true);
    frame(color::Primaries::AP1, false, false);
    pipeline.ReleaseIntermediates(); timeline.Flush(std::chrono::milliseconds(0));
  }
  session.OnDeviceDestroyed();
  std::cout << "PASS resolved colour-basis history invalidation and consecutive explicit resets\n";
}

void LookHistory() {
  Gpu gpu;
  constexpr nr::Size image{67u, 53u};
  const auto atlas = look::MakeAtlas(image);
  unsigned cases = 0u;
  for (const float radius : {0.f, 2.f, 12.f, 50.f}) for (const bool motion : {false, true}) for (const bool reset : {false, true}) {
    const auto bands = look::MakeBands(radius, image.height, atlas.levels);
    const auto packed_size = look::LowHistorySize(atlas, radius);
    const auto offset = look::LevelOffset(image, bands.stable_level);
    const auto half = [](float f) { return DirectX::PackedVector::XMConvertFloatToHalf(f); };
    std::vector<uint16_t> gauss(size_t(atlas.size.Pixels()) * 4u), previous(gauss.size()), packed(size_t(packed_size.Pixels()) * 4u);
    for (size_t i = 0u; i < gauss.size(); ++i) { gauss[i] = half(float((i * 79u) % 503u) / 1024.f); previous[i] = half(float((i * 31u) % 503u) / 1024.f); }
    for (UINT y = 0u; y < packed_size.height; ++y)
      std::memcpy(packed.data() + size_t(y) * packed_size.width * 4u, previous.data() + (size_t(y) * atlas.size.width + offset) * 4u, packed_size.width * 8u);
    std::vector<uint16_t> changes(size_t(image.Pixels()) * 4u), basis(size_t(image.Pixels()) * 2u), vectors(basis.size()), peaks(size_t(atlas.size.Pixels()), half(.8f));
    for (size_t i = 0u; i < changes.size(); ++i) changes[i] = half(float(int((i * 53u) % 461u) - 230) / 1024.f);
    for (size_t i = 0u; i < vectors.size(); ++i) vectors[i] = half(i % 2u ? -.45f : .3f);
    auto g = gpu.Texture(atlas.size.width, atlas.size.height, DXGI_FORMAT_R16G16B16A16_FLOAT, gauss.data(), 8u);
    auto peak = gpu.Texture(atlas.size.width, atlas.size.height, DXGI_FORMAT_R16_FLOAT, peaks.data(), 2u);
    auto chroma = gpu.Texture(image.width, image.height, DXGI_FORMAT_R16G16_FLOAT, basis.data(), 4u);
    auto mv = gpu.Texture(image.width, image.height, DXGI_FORMAT_R16G16_FLOAT, vectors.data(), 4u);
    auto previous_full = gpu.Texture(atlas.size.width, atlas.size.height, DXGI_FORMAT_R16G16B16A16_FLOAT, previous.data(), 8u);
    auto previous_small = gpu.Texture(packed_size.width, packed_size.height, DXGI_FORMAT_R16G16B16A16_FLOAT, packed.data(), 8u);
    auto full = gpu.Texture(atlas.size.width, atlas.size.height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto compact_history = gpu.Texture(packed_size.width, packed_size.height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    const std::array<float, 4> zero{};
    auto full_state = gpu.Texture(1u, 1u, DXGI_FORMAT_R32G32B32A32_FLOAT, zero.data(), 16u);
    auto small_state = gpu.Texture(1u, 1u, DXGI_FORMAT_R32G32B32A32_FLOAT, zero.data(), 16u);
    for (auto* resource : {full_state.Get(), small_state.Get()}) gpu.Barrier(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color::StabilizePass stabilize{.gauss = g.Get(), .previous = previous_full.Get(), .current = full.Get(), .state = full_state.Get(),
      .atlas = atlas, .bands = bands, .rate = .2f, .motion_mode = motion, .reset = reset,
      .motion = {.vectors = {.resource = mv.Get(), .rect = {.width = image.width, .height = image.height}}, .view_format = DXGI_FORMAT_R16G16_FLOAT}};
    Check(gpu.color.RecordStabilize(gpu.list.Get(), 0u, stabilize), "full history stabilization failed");
    stabilize.previous = previous_small.Get(); stabilize.current = compact_history.Get(); stabilize.state = small_state.Get(); stabilize.packed_history = true;
    Check(gpu.color.RecordStabilize(gpu.list.Get(), 1u, stabilize), "packed history stabilization failed");
    for (auto* resource : {full.Get(), compact_history.Get()}) gpu.Barrier(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto reference = gpu.Texture(image.width, image.height, DXGI_FORMAT_R16G16B16A16_FLOAT, changes.data(), 8u);
    auto compact = gpu.Texture(image.width, image.height, DXGI_FORMAT_R16G16B16A16_FLOAT, changes.data(), 8u);
    for (auto* resource : {reference.Get(), compact.Get()}) gpu.Barrier(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color::ShapePass shape{.change = reference.Get(), .basis = chroma.Get(), .gauss = g.Get(), .peaks = peak.Get(), .history = full.Get(),
      .atlas = atlas, .bands = bands, .settings = {.tone = .7f, .detail = .8f, .halo = .3f}};
    Check(gpu.color.RecordShape(gpu.list.Get(), 0u, shape), "full history shape failed");
    shape.change = compact.Get(); shape.history = compact_history.Get(); shape.packed_history = true;
    Check(gpu.color.RecordShape(gpu.list.Get(), 1u, shape), "packed history shape failed");
    const auto a = gpu.Readback(reference.Get()), b = gpu.Readback(compact.Get()); gpu.Submit();
    void *pa = nullptr, *pb = nullptr; Hr(a.buffer->Map(0u, nullptr, &pa)); Hr(b.buffer->Map(0u, nullptr, &pb));
    for (UINT y = 0u; y < image.height; ++y)
      Check(std::memcmp(static_cast<std::byte*>(pa) + y * a.pitch, static_cast<std::byte*>(pb) + y * b.pitch, image.width * 8u) == 0, "packed Look history changed output pixels");
    a.buffer->Unmap(0u, nullptr); b.buffer->Unmap(0u, nullptr); ++cases;
  }
  std::cout << "PASS " << cases << " GPU bit-exact compact Look history cases: radius, motion reprojection, reset and shaped output" << std::endl;
}

void DepthCopies() {
  Gpu gpu;
  constexpr UINT width = 31u, height = 29u;
  constexpr nr::Rect region{3u, 5u, 17u, 13u};
  const std::array formats = {std::pair{DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT},
                             std::pair{DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM},
                             std::pair{DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS}};
  for (const auto [texture_format, view_format] : formats) {
    const UINT stride = texture_format == DXGI_FORMAT_R16_TYPELESS ? 2u : 4u;
    std::vector<std::byte> data(size_t(width) * height * stride);
    std::vector<float> expected(size_t(width) * height);
    for (UINT i = 0u; i < width * height; ++i) {
      const uint32_t code = (i * 197u) % 65536u;
      if (view_format == DXGI_FORMAT_R32_FLOAT) {
        expected[i] = float(code) / 65535.f;
        std::memcpy(data.data() + i * stride, &expected[i], stride);
      } else if (view_format == DXGI_FORMAT_R16_UNORM) {
        const auto value = static_cast<uint16_t>(code); expected[i] = float(value) / 65535.f;
        std::memcpy(data.data() + i * stride, &value, stride);
      } else {
        const uint32_t value = code * 251u; expected[i] = float(value) / 16777215.f;
        std::memcpy(data.data() + i * stride, &value, stride);
      }
    }
    auto source = gpu.Texture(width, height, texture_format, data.data(), stride, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto target = gpu.Texture(region.width, region.height, DXGI_FORMAT_R32_FLOAT);
    Check(gpu.color.RecordDepth(gpu.list.Get(), 0u, source.Get(), view_format, region, target.Get()), "depth extraction failed");
    const auto read = gpu.Readback(target.Get()); gpu.Submit();
    void* mapped = nullptr; Hr(read.buffer->Map(0u, nullptr, &mapped));
    for (UINT y = 0u; y < region.height; ++y) {
      const auto* row = reinterpret_cast<const float*>(static_cast<const std::byte*>(mapped) + y * read.pitch);
      for (UINT x = 0u; x < region.width; ++x)
        Check(std::abs(row[x] - expected[(y + region.y) * width + x + region.x]) <= 1e-7f, "depth subrect or normalized value changed");
    }
    read.buffer->Unmap(0u, nullptr);
  }
  std::cout << "PASS D32/D16/D24S8 depth SRV subrect extraction with D3D12 validation" << std::endl;
}

void Timings() {
  Gpu gpu;nr::Timeline timeline([]{return UINT64_MAX;},[](auto*,auto){},[](auto,auto){return true;});
  sources::NrGpuTiming timing(gpu.device.Get(),timeline);
  const auto now=std::chrono::steady_clock::now();timing.Watch(now,std::bitset<nr::SESSION_STAGES>{1u});
  {
    sources::NrGpuTiming::Recording record(timing,gpu.list.Get(),0u,gpu.queue.Get(),nullptr);
    Check(record.Sample()!=nullptr,"timing sample unavailable");
    record.Copied();record.Encoded();timing.End(record.Sample(),gpu.list.Get(),1u);
  }
  gpu.Submit();
  const auto collected=now+std::chrono::milliseconds(120);timing.Watch(collected,std::bitset<nr::SESSION_STAGES>{1u});
  auto sample=timing.LatestBreakdown(0u,1u,collected);Check(sample.has_value(),"completed GPU phase timestamps unavailable");
  Check(std::abs(sample->total_ms-sample->copy_ms-sample->encode_ms-sample->evaluate_ms-sample->compose_ms)<1e-8,"phase timings do not sum to total");
  Check(timing.Latest(0u,1u,collected)==std::optional<double>(sample->evaluate_ms),"overlay evaluate timing changed meaning");
  {sources::NrGpuTiming::Recording failed(timing,gpu.list.Get(),0u,gpu.queue.Get(),nullptr);Check(failed.Sample()!=nullptr,"failed-recording timing test was not sampled");}
  gpu.Submit();timing.Watch(collected+std::chrono::milliseconds(120),std::bitset<nr::SESSION_STAGES>{1u});
  std::cout<<"PASS GPU phase timestamps, unchanged evaluate readout and incomplete-recording query safety\n";
}

void Pixels() {
  Gpu gpu; constexpr UINT w = 79, h = 61, iw = 67, ih = 53; unsigned cases = 0;
  for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT}) {
  const UINT pixel_bytes = format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16u : format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
  for (auto encoding : {color::Encoding::SRGB, color::Encoding::PQ_BT2100, color::Encoding::SCRGB, color::Encoding::LINEAR_BT709}) {
    std::vector<float> source(w * h * 4), mask(w * h * 4); std::vector<uint16_t> output(iw * ih * 4);
    for (UINT i = 0; i < w * h; ++i) {
      for (UINT c = 0; c < 4; ++c) source[4 * i + c] = float((i * 97u + c * 31u) % 1024) / 1023.f;
      if (encoding == color::Encoding::SCRGB) source[4 * i] = source[4 * i] * 12.f - .2f;
      mask[4 * i] = i % 3 == 0 ? 0.f : (i % 3 == 1 ? .35f : 1.f);
    }
    for (UINT i = 0; i < output.size(); ++i) output[i] = DirectX::PackedVector::XMConvertFloatToHalf(float((i * 37u) % 911) / 1024.f);
    std::vector<std::byte> packed(size_t(w)*h*pixel_bytes);
    for (UINT i = 0; i < w*h; ++i) {
      if (pixel_bytes == 16u) std::memcpy(packed.data()+i*16u, source.data()+i*4u, 16u);
      else if (pixel_bytes == 8u) for (UINT c=0;c<4u;++c) {
        auto half=DirectX::PackedVector::XMConvertFloatToHalf(source[i*4u+c]); std::memcpy(packed.data()+i*8u+c*2u,&half,2u);
      } else {
        uint32_t code=0u;for (UINT c=0;c<4u;++c) {
          const UINT bits=format==DXGI_FORMAT_R8G8B8A8_UNORM?8u:c==3u?2u:10u;
          code |= uint32_t(std::lround(std::clamp(source[i*4u+c],0.f,1.f)*float((1u<<bits)-1u))) << (format==DXGI_FORMAT_R8G8B8A8_UNORM?c*8u:c*10u);
        } std::memcpy(packed.data()+i*4u,&code,4u);
      }
    }
    for (bool preserved : {false, true}) for (bool masked : {false, true}) for (bool subrect : {false, true}) for (bool exposed : {false,true}) {
      auto src = gpu.Texture(w, h, format, packed.data(), pixel_bytes), m = gpu.Texture(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, mask.data(), 16);
      const float exposure_value=1.7f;auto exposure=gpu.Texture(1u,1u,DXGI_FORMAT_R32_FLOAT,&exposure_value,4u);
      auto out = gpu.Texture(iw, ih, DXGI_FORMAT_R16G16B16A16_FLOAT, output.data(), 8), input = gpu.Texture(iw, ih, DXGI_FORMAT_R16G16B16A16_FLOAT), change = gpu.Texture(iw, ih, DXGI_FORMAT_R16G16B16A16_FLOAT);
      auto old = gpu.Texture(w,h,format,packed.data(),pixel_bytes), fused=gpu.Texture(w,h,format,packed.data(),pixel_bytes), inplace=gpu.Texture(w,h,format,packed.data(),pixel_bytes);
      const nr::Rect region{.x = subrect ? 5u : 0u, .y = subrect ? 3u : 0u, .width = iw, .height = ih};
      color::EncodePass encode{.source = src.Get(), .source_view_format = format, .model = input.Get(), .width = iw, .height = ih, .encoding = encoding, .input_scale = .7f, .exposure=exposed?exposure.Get():nullptr,.exposure_factor=.6f,.region = region, .canvas = {iw, ih}, .options = color::shader_options::NEAR_BLACK_GUARD};
      Check(gpu.color.RecordEncode(gpu.list.Get(), 0, encode), "encode failed"); gpu.Barrier(input.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      Check(gpu.color.RecordChange(gpu.list.Get(), 0, {.encode = encode, .nr_output = out.Get(), .change = change.Get(), .model_input = preserved ? input.Get() : nullptr}), "change failed");
      gpu.Barrier(change.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      for (auto* target : {old.Get(), fused.Get(), inplace.Get()}) gpu.Barrier(target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      color::ComputeDecodePass d{.source = src.Get(), .source_view_format = format, .nr_output = change.Get(), .target = old.Get(), .target_uav_format = format,
        .origin_x = region.x, .origin_y = region.y, .target_width = w, .target_height = h, .width = iw, .height = ih, .encoding = encoding, .input_scale = .7f, .transfer_strength = .8f, .color_strength = .6f,
        .exposure=exposed?exposure.Get():nullptr,.exposure_factor=.6f,.source_x = region.x, .source_y = region.y, .options = color::shader_options::NEAR_BLACK_GUARD | color::shader_options::CONSISTENT, .mask = masked ? m.Get() : nullptr, .mask_view_format = DXGI_FORMAT_R32G32B32A32_FLOAT};
      Check(gpu.color.RecordComputeDecode(gpu.list.Get(), 0, d), "reference compose failed");
      d.nr_output = out.Get(); d.target = fused.Get(); d.options |= color::shader_options::FUSED_CHANGE; d.model_input = preserved ? input.Get() : nullptr;
      Check(gpu.color.RecordComputeDecode(gpu.list.Get(), 1, d), "fused compose failed"); d.target = inplace.Get(); d.source = nullptr;
      Check(gpu.color.RecordComputeDecode(gpu.list.Get(), 2, d), "in-place compose failed");
      auto a = gpu.Readback(old.Get()), b = gpu.Readback(fused.Get()), c = gpu.Readback(inplace.Get()); gpu.Submit();
      void *pa = nullptr, *pb = nullptr, *pc = nullptr; Hr(a.buffer->Map(0, nullptr, &pa)); Hr(b.buffer->Map(0, nullptr, &pb)); Hr(c.buffer->Map(0, nullptr, &pc));
      for (UINT y = 0; y < h; ++y) {
        auto row = static_cast<const char*>(pa) + y * a.pitch;
        Check(std::memcmp(row, static_cast<const char*>(pb) + y * b.pitch, w * pixel_bytes) == 0, "fused differs from two-pass reference");
        Check(std::memcmp(row, static_cast<const char*>(pc) + y * c.pitch, w * pixel_bytes) == 0, "in-place differs from two-pass reference");
      }
      a.buffer->Unmap(0, nullptr); b.buffer->Unmap(0, nullptr); c.buffer->Unmap(0, nullptr); ++cases;
    }
  }
  }
  std::cout << "PASS " << cases << " GPU bit-exact cases: 8/10/16/32-bit, SDR/HDR10/scRGB/linear, exposure, preserved/recomputed, alpha, mask zero, subrect, fused/in-place\n";
}

int main() {
  try { Lifetime(); ProducerProofs(); RecordingBatches(); ColorHistory(); Pixels(); LookHistory(); DepthCopies(); Timings(); return 0; }
  catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
