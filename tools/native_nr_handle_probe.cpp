// Standalone gate for a future game-handle wrapper: one native feature, full -> small -> full.
// --self-test uses an identity evaluator and proves plumbing only, never native-NR compatibility.
#include <DirectXPackedVector.h>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <numeric>
#include "native_nr_parameter_view.hpp"
#include "color/color_pipeline.hpp"
#include "nr/keys.hpp"
#include "nr/log.hpp"
#include "nr/snippet.hpp"

using namespace uplift;
using namespace uplift::native_nr_test;
namespace {
constexpr nr::Size FULL{1280u, 720u}, SMALL{640u, 360u};
constexpr DXGI_FORMAT FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;

void Bound(NVSDK_NGX_Parameter& p, const nr::keys::Resource& key, ID3D12Resource* image, nr::Size size) {
  p.Set(key.resource, image);
  p.Set(key.subrect.base_x, 0u); p.Set(key.subrect.base_y, 0u);
  p.Set(key.subrect.width, image ? size.width : 0u); p.Set(key.subrect.height, image ? size.height : 0u);
}
struct Backend {
  nr::Snippet snippet;
  Parameters identity;
  NVSDK_NGX_Parameter* parameters = nullptr;
  NVSDK_NGX_Handle* handle = nullptr;
  bool self_test = false;
  unsigned calls = 0u;
  ~Backend() {
    if (!self_test) {
      if (handle) static_cast<void>(snippet.ReleaseFeature(handle));
      if (parameters) snippet.DestroyParameters(parameters);
    }
  }
  void Create(Gpu& gpu, const std::filesystem::path& runtime, const std::filesystem::path& core) {
    self_test = runtime.empty();
    if (self_test) parameters = &identity;
    else {
      nr::SnippetConfig config{.snippet_path = std::filesystem::absolute(runtime),
          .application_data_path = std::filesystem::current_path() / L"build" / L"native-nr-probe-data"};
      if (!core.empty()) config.core_path_override = std::filesystem::absolute(core);
      Check(NVSDK_NGX_SUCCEED(snippet.Load(config)), "cannot load NR runtime");
      Check(NVSDK_NGX_SUCCEED(snippet.Bind(gpu.device.Get())), "cannot bind NR runtime/core");
      parameters = snippet.AllocateParameters(); Check(parameters != nullptr, "cannot allocate native parameters");
    }
    parameters->Set(nr::keys::CREATION_NODE_MASK, 1u); parameters->Set(nr::keys::VISIBILITY_NODE_MASK, 1u);
    parameters->Set(nr::keys::WIDTH, FULL.width); parameters->Set(nr::keys::HEIGHT, FULL.height);
    parameters->Set(nr::keys::PRESET, 1u); parameters->Set(nr::keys::SCALING_RATIO, 1.f);
    parameters->Set(nr::keys::INTENSITY, 1.f); parameters->Set(nr::keys::LOCAL_TONE, 1.f);
    parameters->Set(nr::keys::LOCAL_STRUCTURE, 1.f); parameters->Set(nr::keys::SKIN_STRUCTURE, 1.f);
    parameters->Set(nr::keys::STYLE, 0u); parameters->Set(nr::keys::USE_AUTO_MASK, 0);
    parameters->Set(nr::keys::MVEC_SCALE_X, 1.f); parameters->Set(nr::keys::MVEC_SCALE_Y, 1.f);
    parameters->Set(nr::keys::ENABLED, 1); parameters->Set(nr::keys::RESET, 1);
    parameters->Set(nr::keys::UI_CORRECTION, 0); parameters->Set(nr::keys::DEPTH_INVERTED, 0);
    for (const auto& key : {nr::keys::DEPTH, nr::keys::CONTROL_MASK, nr::keys::BACKBUFFER,
                           nr::keys::UI, nr::keys::UI_ALPHA, nr::keys::BIDIRECTIONAL_DISTORTION_FIELD}) Bound(*parameters, key, nullptr, {});
    if (self_test) handle = reinterpret_cast<NVSDK_NGX_Handle*>(this);  // identity token, never dereferenced
    else {
      std::string network;
      const auto result = snippet.CreateFeature(gpu.list.Get(), parameters, &handle, &network);
      gpu.Submit();
      Check(NVSDK_NGX_SUCCEED(result) && handle, "native create failed");
      std::cout << "Runtime network: " << network << '\n';
    }
  }
  NVSDK_NGX_Result Evaluate(Gpu& gpu, const NVSDK_NGX_Handle* same_handle, const NVSDK_NGX_Parameter& p) {
    Check(same_handle == handle, "probe changed/recreated the native handle"); ++calls;
    if (!self_test) return snippet.EvaluateFeature(gpu.list.Get(), handle, &p);
    ID3D12Resource *input = nullptr, *output = nullptr;
    Check(NVSDK_NGX_SUCCEED(p.Get(nr::keys::COLOR.resource, &input)) && input, "identity input missing");
    Check(NVSDK_NGX_SUCCEED(p.Get(nr::keys::OUTPUT.resource, &output)) && output, "identity output missing");
    // The self-test cannot copy in place; treat the preprocessed alias as an identity result.
    if (input == output) return NVSDK_NGX_Result_Success;
    Barrier(gpu.list.Get(), input, READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(gpu.list.Get(), output, UAV, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.list->CopyResource(output, input);
    Barrier(gpu.list.Get(), input, D3D12_RESOURCE_STATE_COPY_SOURCE, READ);
    Barrier(gpu.list.Get(), output, D3D12_RESOURCE_STATE_COPY_DEST, UAV);
    return NVSDK_NGX_Result_Success;
  }
};
struct Surfaces {
  nr::Size size;
  ComPtr<ID3D12Resource> input, output, change, motion;
  Surfaces(Gpu& gpu, nr::Size value) : size(value) {
    std::vector<uint16_t> zeros(size_t(size.width) * size.height * 4u, 0u);
    input = gpu.Texture(size.width, size.height, FORMAT, zeros.data(), 8u);
    output = gpu.Texture(size.width, size.height, FORMAT, zeros.data(), 8u);
    change = gpu.Texture(size.width, size.height, FORMAT, zeros.data(), 8u);
    motion = gpu.Texture(size.width, size.height, DXGI_FORMAT_R16G16_FLOAT, zeros.data(), 4u);
  }
};
struct Timer {
  ComPtr<ID3D12QueryHeap> heap;
  ComPtr<ID3D12Resource> result;
  UINT64 frequency = 0u;
  explicit Timer(Gpu& gpu) {
    D3D12_QUERY_HEAP_DESC desc{}; desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; desc.Count = 4u;
    Hr(gpu.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap)));
    result = gpu.Buffer(4u * sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK);
    Hr(gpu.queue->GetTimestampFrequency(&frequency));
  }
  void Mark(Gpu& gpu, UINT index) { gpu.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index); }
  std::array<double, 4> Read() {
    void* mapped = nullptr; D3D12_RANGE range{0u, 4u * sizeof(uint64_t)};
    Hr(result->Map(0u, &range, &mapped));
    const auto* data = static_cast<const uint64_t*>(mapped);
    std::array<double, 4> times{};
    for (size_t i = 0u; i < 3u; ++i) times[i] = double(data[i + 1u] - data[i]) * 1000.0 / frequency;
    times[3] = double(data[3] - data[0]) * 1000.0 / frequency;
    D3D12_RANGE written{0u, 0u}; result->Unmap(0u, &written); return times;
  }
};
void Run(Gpu& gpu, Backend& backend, unsigned frames) {
  using DirectX::PackedVector::XMConvertFloatToHalf;
  std::vector<uint16_t> pixels(size_t(FULL.width) * FULL.height * 4u);
  for (UINT y = 0u; y < FULL.height; ++y) for (UINT x = 0u; x < FULL.width; ++x) {
    const size_t i = (size_t(y) * FULL.width + x) * 4u;
    pixels[i] = XMConvertFloatToHalf(.02f + .7f * float(x) / FULL.width);
    pixels[i + 1u] = XMConvertFloatToHalf(.02f + .7f * float(y) / FULL.height);
    pixels[i + 2u] = XMConvertFloatToHalf(((x / 32u + y / 32u) % 2u) ? .2f : .4f);
    pixels[i + 3u] = XMConvertFloatToHalf(.5f);
  }
  auto source = gpu.Texture(FULL.width, FULL.height, FORMAT, pixels.data(), 8u);
  auto result = gpu.Texture(FULL.width, FULL.height, FORMAT, pixels.data(), 8u, UAV);
  Surfaces full(gpu, FULL), small_surfaces(gpu, SMALL); Timer timer(gpu);
  color::ColorPipeline color; std::string error; Check(color.Initialize(gpu.device.Get(), &error), error.c_str());
  Bound(*backend.parameters, nr::keys::COLOR, full.input.Get(), FULL);
  Bound(*backend.parameters, nr::keys::OUTPUT, full.output.Get(), FULL);
  Bound(*backend.parameters, nr::keys::MVEC, full.motion.Get(), FULL);
  const auto original_handle = backend.handle;
  const auto original_values = backend.self_test ? backend.identity.values : Parameters{}.values;
  gpu.Submit();
  if (backend.self_test) std::cout << "IDENTITY evaluator: GPU copy only; these are NOT DLSS-NR performance measurements.\n";
  std::cout << "One handle " << original_handle << ", create capacity 1280x720; evaluate sizes change with each phase.\n"
            << "phase,pre_ms,nr_ms,post_ms,total_ms,p95_total_ms\n";
  for (unsigned phase = 0u; phase < 4u; ++phase) {
    auto& surfaces = (phase == 1u || phase == 2u) ? small_surfaces : full;
    const bool alias_color_output = (phase == 2u);
    // In particular, full-return must not pass by reading full's previous output.
    {
      std::vector<uint16_t> zeros(size_t(surfaces.size.width) * surfaces.size.height * 4u, 0u);
      auto cleared = gpu.Texture(surfaces.size.width, surfaces.size.height, FORMAT, zeros.data(), 8u, D3D12_RESOURCE_STATE_COPY_SOURCE);
      Barrier(gpu.list.Get(), surfaces.output.Get(), READ, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.list->CopyResource(surfaces.output.Get(), cleared.Get());
      Barrier(gpu.list.Get(), surfaces.output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, READ);
      gpu.Submit();  // Outside measured frames; keep the feature and output resource unchanged.
    }
    std::array<std::vector<double>, 4> samples;
    for (unsigned frame = 0u; frame < frames + 4u; ++frame) {
      ParameterView view(*backend.parameters);
      ID3D12Resource* const evaluate_color = (alias_color_output ? surfaces.output.Get() : surfaces.input.Get());
      Bound(view, nr::keys::COLOR, evaluate_color, surfaces.size);
      Bound(view, nr::keys::OUTPUT, surfaces.output.Get(), surfaces.size);
      Bound(view, nr::keys::MVEC, surfaces.motion.Get(), surfaces.size);
      view.Set(nr::keys::WIDTH, surfaces.size.width); view.Set(nr::keys::HEIGHT, surfaces.size.height);
      view.Set(nr::keys::RESET, frame == 0u ? 1 : 0);
      color::EncodePass encode{.source = source.Get(), .source_view_format = FORMAT, .model = surfaces.input.Get(),
          .motion = surfaces.motion.Get(), .width = surfaces.size.width, .height = surfaces.size.height,
          .encoding = color::Encoding::LINEAR_BT709, .region = {0u, 0u, FULL.width, FULL.height}, .canvas = surfaces.size};
      timer.Mark(gpu, 0u);
      if (alias_color_output) {
        Barrier(gpu.list.Get(), surfaces.output.Get(), READ, UAV);
        Check(color.RecordNativeInput(gpu.list.Get(), 0u, 0u, {source.Get(), {0u, 0u, FULL.width, FULL.height}}, FORMAT,
            surfaces.output.Get(), FORMAT, surfaces.size, {}, true), "aliased Color/Output preprocess failed");
        D3D12_RESOURCE_BARRIER uav{}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = surfaces.output.Get();
        gpu.list->ResourceBarrier(1u, &uav);
      } else {
        Barrier(gpu.list.Get(), surfaces.input.Get(), READ, UAV); Barrier(gpu.list.Get(), surfaces.motion.Get(), READ, UAV);
        Check(color.RecordEncode(gpu.list.Get(), 0u, encode), "preprocess failed");
        Barrier(gpu.list.Get(), surfaces.input.Get(), UAV, READ); Barrier(gpu.list.Get(), surfaces.motion.Get(), UAV, READ);
        Barrier(gpu.list.Get(), surfaces.output.Get(), READ, UAV);
      }
      timer.Mark(gpu, 1u);
      const auto evaluated = backend.Evaluate(gpu, original_handle, view);
      timer.Mark(gpu, 2u);
      Barrier(gpu.list.Get(), surfaces.output.Get(), UAV, READ);
      if (NVSDK_NGX_SUCCEED(evaluated)) {
        if (alias_color_output) {
          Check(color.RecordNativeInput(gpu.list.Get(), 0u, 0u,
              {surfaces.output.Get(), {0u, 0u, surfaces.size.width, surfaces.size.height}}, FORMAT,
              result.Get(), FORMAT, FULL, {}, true), "aliased Output upsample failed");
        } else {
          Barrier(gpu.list.Get(), surfaces.change.Get(), READ, UAV);
          Check(color.RecordChange(gpu.list.Get(), 0u, {.encode = encode, .nr_output = surfaces.output.Get(),
              .change = surfaces.change.Get(), .model_input = surfaces.input.Get()}), "change extraction failed");
          Barrier(gpu.list.Get(), surfaces.change.Get(), UAV, READ);
          Check(color.RecordComputeDecode(gpu.list.Get(), 0u,
              {.source = source.Get(), .source_view_format = FORMAT, .nr_output = surfaces.change.Get(), .target = result.Get(),
               .target_uav_format = FORMAT, .target_width = FULL.width, .target_height = FULL.height,
               .width = FULL.width, .height = FULL.height, .encoding = color::Encoding::LINEAR_BT709,
               .upsampling = color::Upsampling::CLASSIC}), "postprocess failed");
        }
        D3D12_RESOURCE_BARRIER uav{}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = result.Get();
        gpu.list->ResourceBarrier(1u, &uav);
      }
      timer.Mark(gpu, 3u);
      gpu.list->ResolveQueryData(timer.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0u, 4u, timer.result.Get(), 0u);
      gpu.Submit();  // Standalone benchmark only: no claim about in-game input latency.
      if (NVSDK_NGX_FAILED(evaluated)) throw std::runtime_error("same-handle evaluate failed in phase " + std::to_string(phase)
          + ", result=" + std::to_string(static_cast<uint32_t>(evaluated)) + "; do not enable a game wrapper");
      const auto times = timer.Read();
      if (frame >= 4u) for (size_t i = 0u; i < times.size(); ++i) samples[i].push_back(times[i]);
      ID3D12Resource* original_input = nullptr; unsigned original_width = 0u;
      Check(NVSDK_NGX_SUCCEED(backend.parameters->Get(nr::keys::COLOR.resource, &original_input)) && original_input == full.input.Get()
          && NVSDK_NGX_SUCCEED(backend.parameters->Get(nr::keys::WIDTH, &original_width)) && original_width == FULL.width,
          "original input/create dimensions changed");
      if (backend.self_test) Check(backend.identity.values == original_values, "parameter overlay mutated original block");
    }
    auto read = gpu.Readback(result.Get(), UAV); auto raw = gpu.Readback(surfaces.output.Get(), READ); gpu.Submit();
    float raw_min = INFINITY, raw_max = -INFINITY;
    for (UINT y = 0u; y < surfaces.size.height; y += 37u) for (UINT x = 0u; x < surfaces.size.width; x += 41u) {
      const float value = DirectX::PackedVector::XMConvertHalfToFloat(raw.At<uint16_t>(x, y, 4u));
      Check(std::isfinite(value), "native model output contains non-finite values");
      raw_min = std::min(raw_min, value); raw_max = std::max(raw_max, value);
    }
    if (!backend.self_test) Check(raw_max - raw_min > .001f,
        "native output is constant/untouched despite success; do not enable a game wrapper");
    for (UINT y = 0u; y < FULL.height; y += 53u) for (UINT x = 0u; x < FULL.width; x += 71u) {
      for (UINT c = 0u; c < 4u; ++c) {
        const float value = DirectX::PackedVector::XMConvertHalfToFloat(read.At<uint16_t>(x, y, 4u, c));
        Check(std::isfinite(value), "NR returned non-finite pixels");
        if (backend.self_test && !alias_color_output) {
          const float expected = DirectX::PackedVector::XMConvertHalfToFloat(pixels[(size_t(y) * FULL.width + x) * 4u + c]);
          Check(std::abs(value - expected) < .003f, "identity wrapper changed source colour/alpha");
        }
      }
    }
    std::cout << (phase == 0u ? "full" : phase == 1u ? "small" : phase == 2u ? "small-alias" : "full-return")
              << std::fixed << std::setprecision(4);
    for (auto& values : samples) {
      std::sort(values.begin(), values.end());
      std::cout << ',' << .5 * (values[(values.size() - 1u) / 2u] + values[values.size() / 2u]);
    }
    const size_t p95 = (samples[3].size() * 95u + 99u) / 100u - 1u;
    std::cout << ',' << samples[3][p95] << '\n';
    if (!backend.self_test) if (const auto bytes = backend.snippet.QueryAllocatedBytes()) std::cout << "native_allocated_bytes=" << *bytes << '\n';
  }
  Check(backend.calls == 4u * (frames + 4u), "probe ran an unexpected extra NR evaluation");
  std::cout << (backend.self_test ? "PASS identity plumbing only; native runtime/game-handle compatibility NOT tested\n"
      : "PASS same runtime handle accepted full-small-small-alias-full; game-owned handle, visual quality and in-game latency still need validation\n");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    std::filesystem::path runtime, core; bool self_test = false; unsigned frames = 30u;
    for (int i = 1; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg == L"--self-test") self_test = true;
      else if (arg == L"--runtime" && i + 1 < argc) runtime = argv[++i];
      else if (arg == L"--core" && i + 1 < argc) core = argv[++i];
      else if (arg == L"--frames" && i + 1 < argc) frames = std::clamp(std::stoul(argv[++i]), 8ul, 240ul);
      else throw std::runtime_error("usage: uplift_native_nr_handle_probe --self-test | --runtime <nvngx_dlssnr.dll> [--core <_nvngx.dll>] [--frames 8..240]");
    }
    Check(self_test != !runtime.empty(), "choose exactly one: --self-test or --runtime <dll>");
    if (!runtime.empty()) Check(std::filesystem::is_regular_file(runtime), "runtime path is not a file");
    if (!core.empty()) Check(std::filesystem::is_regular_file(core), "core path is not a file");
    nr::SetLogSink([](nr::LogLevel, std::string_view text, void*) { std::cerr << text << '\n'; }, nullptr, nr::LogLevel::INFO);
    Gpu gpu; Backend backend; backend.Create(gpu, runtime, core);
    try { Run(gpu, backend, frames); }
    catch (...) { try { gpu.Submit(); } catch (...) { backend.snippet.Abandon(); backend.handle = nullptr; backend.parameters = nullptr; } throw; }
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
