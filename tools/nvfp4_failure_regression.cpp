#include <iostream>
#include <stdexcept>
#include "bridge/d3d12_side.hpp"
// Fault injection into the actual layer boundaries without loading NVIDIA's NR runtime or CUDA kernels.
#include "nvfp4/NrFp8.cpp"
#include "nvfp4/NrFfnV2.cpp"
#include "nvfp4/NrNet4.cpp"
#include "nvfp4/NrSwin4.cpp"
#include "nvfp4/NrNvfp4.cpp"

namespace {
void Check(bool yes, const char* why) { if (!yes) throw std::runtime_error(why); }
void Hr(HRESULT result) { if (FAILED(result)) throw std::runtime_error("D3D12 test setup failed"); }
unsigned launches = 0;
NvAPI_Status launchResult = NVAPI_OK;
NvAPI_Status __cdecl Driver(ID3D12GraphicsCommandList*, const NVAPI_CU_KERNEL_LAUNCH_PARAMS*, NvU32) {
    ++launches;
    return launchResult;
}
using LaunchFn = decltype(&NvAPI_D3D12_LaunchCuKernelChain);
LaunchFn InstallDriver() {
    return reinterpret_cast<LaunchFn>(DlssNr::Nvfp4::WrapNvapi(0x24973538u, reinterpret_cast<void*>(&Driver)));
}
NVDX_ObjectHandle Handle(uintptr_t n) { return reinterpret_cast<NVDX_ObjectHandle>(n); }
void Manifest(const std::filesystem::path& folder, unsigned set, bool valid) {
    std::filesystem::create_directories(folder);
    const auto stem = folder / ("fp8_set" + std::to_string(set));
    std::ofstream bin(stem.string() + ".bin", std::ios::binary);
    const std::array<char, 512> bytes{};
    bin.write(bytes.data(), bytes.size());
    std::ofstream txt(stem.string() + ".txt");
    txt << (valid ? "evaluation 3\nlaunch 1 name expected params 16 record 8 offset 0 bytes 512\n" : "evaluation 0\n");
    Check(bool(bin) && bool(txt), "could not write FP8 fixtures");
}

void AssetIsolation(const std::filesystem::path& root, ID3D12GraphicsCommandList* commands) {
    const auto assets = root / "fp8";
    Manifest(assets, 5, true);
    Manifest(assets, 6, false);
    DlssNr::Nvfp4::Configure(true, root);
    std::string why;
    Check(NrFp8::SetComplete(5, &why), "bad set disabled a healthy FP8 set");
    Check(!NrFp8::SetComplete(6, &why) && why.find("fp8_set6.txt line 1") != std::string::npos,
          "corrupt FP8 set lost its parser diagnostic");
    Check(!DlssNr::Nvfp4::BeginEvaluation(19), "corrupt FP8 set reached Evaluate");
    DlssNr::Nvfp4::EndEvaluation(commands);
    Check(!DlssNr::Nvfp4::OursRan(true) && DlssNr::Nvfp4::Problem().find("fp8_set6.txt line 1") != std::string::npos,
          "early refusal lost the selected set's error");
    Check(DlssNr::Nvfp4::BeginEvaluation(18), "unavailable 4-bit layers disabled healthy FP8");
    DlssNr::Nvfp4::EndEvaluation(commands);
    NrSwin4::S().lists[commands].level.saved = 1;
    const auto before_refused = launches;
    {
        nvfp4::EvaluationScope evaluation(commands, true);
        Check(!DlssNr::Nvfp4::BeginEvaluation(4), "unavailable NVFP4 layers reached Evaluate");
        evaluation.Reject("selected custom model is unavailable");
        DlssNr::Nvfp4::EndEvaluation(commands);
        Check(evaluation.failed && launches == before_refused, "pre-Evaluate refusal replayed a deferred kernel prefix");
    }
    Check(NrSwin4::S().lists.empty(), "pre-Evaluate refusal retained command-list state");
    Check(!DlssNr::Nvfp4::BeginEvaluation(22), "missing Model B weights reached Evaluate");
    DlssNr::Nvfp4::EndEvaluation(commands);
    Check(!DlssNr::Nvfp4::BeginEvaluation(17), "retired model reached Evaluate");
    DlssNr::Nvfp4::EndEvaluation(commands);
    Check(DlssNr::Nvfp4::BeginEvaluation(1), "built-in NR was refused by custom model checks");
    DlssNr::Nvfp4::EndEvaluation(commands);
    Manifest(assets, 6, true);
    DlssNr::Nvfp4::Retry();
    Check(NrFp8::SetComplete(5, &why) && NrFp8::SetComplete(6, &why), "same-path retry did not repair a failed set");
    auto* device = reinterpret_cast<ID3D12Device*>(uintptr_t{0x4000});
    NrFp8::S().devices[device].sets[5].ready = true;
    Manifest(assets, 5, false);
    NrFp8::Enable(assets);
    Check(NrFp8::SetComplete(5, &why), "retry changed a manifest whose GPU records are live");
    NrFp8::S().devices.clear();
    const auto broken = root / "all-broken";
    Manifest(broken, 5, false);
    NrFp8::Enable(broken);
    Check(!NrFp8::SetComplete(5, &why) && why.find("line 1") != std::string::npos, "all-bad package lost the per-set error");
    NrFp8::Enable(root / "absent");
    Check(!NrFp8::SetComplete(5, &why) && why.find("fp8_set5") != std::string::npos, "missing package lost its set name");
    NrFp8::Enable(assets);
    std::cout << "PASS FP8 set isolation, early refusal, same-path repair and live-manifest preservation\n";
}

void LaunchBoundary(ID3D12GraphicsCommandList* commands) {
    const auto launch = InstallDriver();
    Check(DlssNr::Nvfp4::WrapNvapi(0x24973538u, reinterpret_cast<void*>(launch)) == reinterpret_cast<void*>(launch),
          "repeated hook installation recursed");
    NVAPI_CU_KERNEL_LAUNCH_PARAMS kernel{};
    launches = 0;
    {
        nvfp4::EvaluationScope evaluation(commands, true);
        launchResult = NVAPI_ERROR;
        Check(launch(commands, &kernel, 1) == NVAPI_ERROR && evaluation.failed, "driver failure was not latched");
        launchResult = NVAPI_OK;
        Check(launch(commands, &kernel, 1) == NVAPI_ERROR && launches == 1, "a kernel ran after this evaluation failed");
        auto* other = reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t{0x3000});
        Check(launch(other, &kernel, 1) == NVAPI_OK && launches == 2, "failure leaked into another command list");
        {
            nvfp4::EvaluationScope native(commands, false);
            Check(launch(commands, &kernel, 1) == NVAPI_OK, "custom failure leaked into built-in NR");
        }
        Check(launch(commands, &kernel, 1) == NVAPI_ERROR, "nested scope did not restore the failed boundary");
    }
    {
        nvfp4::EvaluationScope next(commands, true);
        Check(launch(commands, &kernel, 1) == NVAPI_OK && !next.failed, "failure poisoned the next evaluation");
    }
    std::cout << "PASS cross-layer launch stop, command-list isolation, nested scopes and next-evaluation recovery\n";
}

void Fp8Failure(ID3D12GraphicsCommandList* commands) {
    auto& s = NrFp8::S();
    auto* device = reinterpret_cast<ID3D12Device*>(uintptr_t{0x4000});
    s.functions[Handle(1)] = {device, "cc_cb_clear", 1};
    s.functions[Handle(2)] = {device, "wrong_kernel", 1};
    NrFp8::UseSet(6);
    const auto launch = InstallDriver();
    NVAPI_CU_KERNEL_LAUNCH_PARAMS kernel{};
    const auto faults = NrFp8::Faults(), completed = NrFp8::Completed();
    const auto before = launches;
    {
        nvfp4::EvaluationScope evaluation(commands, true);
        kernel.hFunction = Handle(1);
        Check(launch(commands, &kernel, 1) == NVAPI_OK, "FP8 clear failed");
        kernel.hFunction = Handle(2);
        Check(launch(commands, &kernel, 1) == NVAPI_ERROR && evaluation.failed, "FP8 kernel mismatch was not rejected");
        kernel.hFunction = Handle(1);
        Check(launch(commands, &kernel, 1) == NVAPI_ERROR && launches == before + 1, "FP8 failure recorded fallback work");
        NrFp8::EndEvaluation(commands);
    }
    Check(NrFp8::Faults() == faults + 1 && NrFp8::Completed() == completed && s.lists.empty(), "failed FP8 evaluation was accepted or retained");
    NrFp8::UseSet(0);
    s.functions.clear();
    std::cout << "PASS actual FP8 mismatch refuses output and records no fallback kernels\n";
}

void FourBitFailure(ID3D12GraphicsCommandList* commands) {
    auto& s = NrFfnV2::S();
    s.enabled = true;
    s.broken = s.failed = false;
    for (bool half : {false, true}) {
        auto& list = s.lists[commands];
        list.counting = true;
        list.pending.active = half;
        const auto before = launches;
        {
            nvfp4::EvaluationScope evaluation(commands, true);
            Check(NrFfnV2::Fail(s, list, commands, nullptr, 0, "injected pair failure") == NVAPI_ERROR, "FFN failure was hidden");
            Check(evaluation.failed && !list.pending.active && launches == before && s.enabled && !s.broken && !s.failed,
                  "FFN failure ran fallback work or permanently disabled healthy models");
            // A saved Swin prefix must not be replayed during failed-evaluation cleanup.
            NrSwin4::S().lists[commands].level.saved = 1;
            NrNet4::S().lists[commands] = {};
            NrFfnV2::EndEvaluation(commands);
            NrSwin4::EndEvaluation(commands);
            NrNet4::EndEvaluation(commands);
        }
        Check(s.lists.empty() && NrSwin4::S().lists.empty() && NrNet4::S().lists.empty() && launches == before,
              "failed evaluation cleanup replayed work or retained list state");
    }
    NrFfnV2::ModelWeights weights;
    s.passSet = 10;
    bool rejected = false;
    try { NrFfnV2::SetAt(s, weights, s.options, 0); } catch (const std::exception&) { rejected = true; }
    Check(rejected, "missing numbered FFN set silently used another model");
    weights.setAt[10] = 512;
    Check(NrFfnV2::SetAt(s, weights, s.options, 0) == 512, "valid FFN set was refused");
    s.passSet = NrFfnV2::kDefaultSet;
    std::cout << "PASS FFN pre/half-pair failure, no cleanup replay and strict weight selection\n";
}

void RemovedAllocator() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Device5> removable;
    ComPtr<ID3D12CommandQueue> queue;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    Hr(device.As(&removable));
    D3D12_COMMAND_QUEUE_DESC desc{};
    Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
    auto side = uplift::bridge::D3D12Side::OnGameQueue(queue.Get());
    Check(side && side->SlotFree(), "healthy allocator was refused");
    auto* commands = side->BeginList();
    Check(commands != nullptr, "healthy command list was refused");
    auto& swin = NrSwin4::S();
    const auto previousLaunch = swin.launch;
    swin.launch = &Driver;
    swin.lists[commands].level.saved = 1;
    launchResult = NVAPI_ERROR;
    {
        nvfp4::EvaluationScope evaluation(commands, true);
        NrSwin4::EndEvaluation(commands);
        Check(evaluation.failed && !evaluation.failure.empty(), "cleanup kernel failure was not propagated to the evaluation");
    }
    swin.launch = previousLaunch;
    launchResult = NVAPI_OK;
    Check(swin.lists.empty(), "cleanup kernel failure retained command-list state");
    std::cout << "PASS cleanup kernel failure rejects the evaluation and releases list state\n";
    side->CloseList();
    removable->RemoveDevice();
    Check(side->Progress()->GetCompletedValue() == UINT64_MAX, "test device did not report removal");
    Check(!side->SlotFree() && !side->BeginList(), "removed device allocator was reused");
    std::cout << "PASS removed WARP device refuses allocator/list reuse\n";
}
} // namespace

int main() {
    try {
        DlssNr::Nvfp4::SetLog([](const char*) {});
        const auto fixtures = std::filesystem::path("build") / "nr-failure-fixtures" /
                              (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        auto* commands = reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t{0x1000});
        AssetIsolation(fixtures, commands);
        LaunchBoundary(commands);
        Fp8Failure(commands);
        FourBitFailure(commands);
        RemovedAllocator();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
