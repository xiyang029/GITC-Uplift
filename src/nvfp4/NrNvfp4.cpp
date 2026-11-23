// See NrNvfp4.h. Self-contained (no precompiled header). The settings are the ones the Display Filter application
// gives the same three layers (standalone/NrCapture/Processor.cpp: ApplyNvfp4, ApplyNvfp4Swin, ApplyNvfp4Net,
// Processor::Evaluate).
#include "NrNvfp4.h"

#include <windows.h>
#include <d3d12.h>

#include "NrFfnV2.h"
#include "NrFp8.h"
#include "NrNet4.h"
#include "NrSwin4.h"

#include <cctype>
#include <cstring>
#include <exception>
#include <mutex>

namespace DlssNr::Nvfp4
{
namespace
{
// Evaluations in a row that may fail to run our kernels before it is called a failure: the layers recognise the model
// from the runtime's own launches, which takes the first evaluations after they are switched on.
constexpr unsigned kStarting = 30;

struct Work
{
    unsigned long long swin = 0, net = 0;
    unsigned long long swinFaults = 0, netFaults = 0;
    NrFfnV2::Work ffn{};
};

void (*gLog)(const char*) = nullptr;
std::mutex gMutex;
bool gOn = false;
std::filesystem::path gAssets;
bool gAskedDestroy = false;
std::string gEnableProblem; // a layer that could not be switched on: nothing can run
std::string gRefused;       // a path that cannot run these models at all
bool gOurs = false;         // the current evaluation requested a local model
bool gModelB = false;       // ... or Model B's 4-bit feed-forward alone: only that layer is ours
std::string gSetMissing;     // the pass-in-one model asked for has no weight files: nothing else is run in its place
Work gBefore;
unsigned gMissed = 0; // evaluations in a row that asked for our kernels and did not get them
std::string gWhy;     // why the last of them did not
bool gRunning = false, gToldFailed = false;
// The FP8 pass-in-one models (NrFp8): loaded beside the 4-bit layers, independent of them (they run on RTX 40 too).
std::string gFp8Problem;   // the FP8 sets could not be loaded
bool gFp8 = false;         // the evaluation in progress asked for an FP8 model
unsigned long long gFp8Faults = 0, gFp8Completed = 0;

void Say(const std::string& line)
{
    if (gLog)
        gLog(line.c_str());
}

bool EndsWith(const std::string& text, size_t end, const char* tail)
{
    const size_t length = strlen(tail);
    return end >= length && text.compare(end - length, length, tail) == 0;
}

unsigned long long Number(const std::string& text, size_t& at)
{
    unsigned long long n = 0;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])))
        n = n * 10 + static_cast<unsigned long long>(text[at++] - '0');
    return n;
}

// The work counters of the layers, read from their status lines (the fused-block layer has no other way to ask).
// Fused blocks: every "name: n" and "name: n/m" after "blocks replaced", except what was left to the runtime or left
// out. Only whether the sum grew during an evaluation matters.
unsigned long long SwinWork(const std::string& status)
{
    const size_t from = status.find(" | blocks replaced: ");
    if (from == std::string::npos)
        return 0;
    unsigned long long sum = 0;
    for (size_t i = from; i + 2 < status.size(); ++i)
    {
        if (status[i] != ':' || status[i + 1] != ' ' || !std::isdigit(static_cast<unsigned char>(status[i + 2])))
            continue;
        const bool left = EndsWith(status, i, "left to the runtime") || EndsWith(status, i, "left out");
        size_t at = i + 2;
        for (;;)
        {
            const auto n = Number(status, at);
            if (!left)
                sum += n;
            if (at + 1 < status.size() && status[at] == '/' && std::isdigit(static_cast<unsigned char>(status[at + 1])))
            {
                ++at;
                continue;
            }
            break;
        }
        i = at - 1;
    }
    return sum;
}

unsigned long long After(const std::string& status, const char* label)
{
    unsigned long long sum = 0;
    const size_t length = strlen(label);
    for (size_t at = status.find(label); at != std::string::npos; at = status.find(label, at))
    {
        at += length;
        sum += Number(status, at);
    }
    return sum;
}

Work Read()
{
    Work work;
    work.swin = SwinWork(NrSwin4::Status(true));
    work.net = After(NrNet4::Status(true), " replaced ");
    work.ffn = NrFfnV2::CompletedWork();
    work.swinFaults = NrSwin4::Faults();
    work.netFaults = NrNet4::Faults();
    return work;
}

// Pause all 4-bit layers for passes that must use the runtime's kernels.
void PauseForEvaluation(bool paused)
{
    NrSwin4::Pause(paused);
    NrNet4::Pause(paused);
    NrFfnV2::Pause(paused);
}
} // namespace

void SetLog(void (*log)(const char* line))
{
    gLog = log;
    NrFfnV2::SetLog(log);
    NrSwin4::SetLog(log);
    NrNet4::SetLog(log);
    NrFp8::SetLog(log);
}

void* WrapNvapi(unsigned id, void* original)
{
    if (!original)
        return original;
    // FP8 and NrNet4 inspect the original launch sequence; NrSwin4 replaces launches one for one
    // so NrFfnV2 can still count them.
    return NrFp8::WrapNvapi(id, NrNet4::WrapNvapi(id, NrSwin4::WrapNvapi(id, NrFfnV2::WrapNvapi(id, original))));
}

namespace
{
// Called under gMutex. Re-enabling retains GPU allocations that recorded lists may still reference.
void ConfigureLayers(const std::filesystem::path& assets)
{
    gOn = true;
    gAssets = assets;
    gEnableProblem.clear();
    if (!gAskedDestroy)
    {
        // The runtime asks NvAPI for an entry point when it first needs it. Ask for the two destroy calls now, through
        // the hook, so the layers can destroy their own kernels even before the runtime has destroyed anything.
        gAskedDestroy = true;
        if (const HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll"))
            if (const auto query = reinterpret_cast<void* (*) (unsigned)>(GetProcAddress(nvapi, "nvapi_QueryInterface")))
                for (const unsigned id : { 0x41c65285u, 0xdf295ea6u }) // DestroyCuModule, DestroyCuFunction
                    query(id);
    }
    const auto failed = [](const char* what, const std::exception& e)
    {
        gEnableProblem += (gEnableProblem.empty() ? "" : "; ") + std::string(what) + ": " + e.what();
    };
    try
    {
        NrFfnV2::Options options;
        options.modelFolders = true;
        options.trained = true;
        options.assets = assets;
        options.minTokens = 0; // every size: the 4-bit model is the 4-bit feed-forward too, whatever the picture size
        NrFfnV2::Enable(options);
    }
    catch (const std::exception& e)
    {
        failed("feed-forward", e);
    }
    try
    {
        NrSwin4::Options options;
        options.assets = assets / L"swin4";
        options.widths = 12; // 256 and 128: the fastest mix at 4K
        options.set = 1;     // the trained codes; a pass-in-one model's own set is chosen per evaluation
        NrSwin4::Enable(options);
    }
    catch (const std::exception& e)
    {
        NrSwin4::Disable();
        failed("fused blocks", e);
    }
    try
    {
        NrNet4::Options options;
        options.assets = assets;
        options.family[NrNet4::Split].groups = 63;
        options.family[NrNet4::Vit].groups = 7;
        options.family[NrNet4::Split].trained = true;
        options.family[NrNet4::Vit].trained = true;
        NrNet4::Enable(options);
    }
    catch (const std::exception& e)
    {
        NrNet4::Disable();
        failed("split blocks and transformer", e);
    }
    // A missing FP8 package must not disable the 4-bit models.
    gFp8Problem.clear();
    try
    {
        NrFp8::Enable(assets / L"fp8");
    }
    catch (const std::exception& e)
    {
        gFp8Problem = std::string("FP8 models: ") + e.what();
        Say("FAILED to load the FP8 models, so a pass that asks for one is not applied: " + std::string(e.what()));
    }
    // Idle until an evaluation asks: other users of the same NvAPI calls (DLSS itself) pass straight through.
    PauseForEvaluation(true);
    gMissed = 0;
    gRunning = gToldFailed = false;
    if (gEnableProblem.empty())
        Say("loaded, assets " + assets.string());
    else
        Say("FAILED to load, so a pass that asks for a 4-bit model is not applied: " + gEnableProblem);
}

} // namespace

void Configure(bool wanted, const std::filesystem::path& assets)
{
    std::lock_guard guard(gMutex);
    if (!wanted || (gOn && assets == gAssets))
        return;
    ConfigureLayers(assets);
}

void Retry()
{
    std::lock_guard guard(gMutex);
    if (gOn)
        ConfigureLayers(gAssets);
}

void BeginEvaluation(unsigned requestedPreset)
{
    std::lock_guard guard(gMutex);
    gModelB = IsModelB(requestedPreset);
    gOurs = IsNvfp4(requestedPreset) && !gModelB; // Model B: only its feed-forward is ours
    gFp8 = IsFp8(requestedPreset);
    if (gFp8)
    {
        // FP8 uses the runtime's kernels; keep the 4-bit layers paused.
        PauseForEvaluation(true);
        gSetMissing.clear();
        const unsigned set = Fp8Set(requestedPreset);
        if (gOn && gFp8Problem.empty() && !NrFp8::SetComplete(set))
            gSetMissing = "missing FP8 weights (nvfp4/fp8/fp8_set" + std::to_string(set) + ".txt / .bin)";
        NrFp8::UseSet(gSetMissing.empty() && gFp8Problem.empty() ? set : 0);
        gFp8Faults = NrFp8::Faults();
        gFp8Completed = NrFp8::Completed();
        return;
    }
    NrFp8::UseSet(0);
    gSetMissing.clear();
    if (gModelB) {
        NrSwin4::Pause(true);
        NrNet4::Pause(true);
        NrFfnV2::UseSet(NrFfnV2::kDefaultSet);
        NrFfnV2::Pause(!gOn, true);
        if (gOn) gBefore.ffn = NrFfnV2::CompletedWork();
        return;
    }
    if (!gOurs) {
        PauseForEvaluation(true);
        return;
    }
    // A pass-in-one model is the same kernels with its own set of weights. The set must be chosen first: each
    // family checks the files of the set it will actually use.
    const unsigned set = WeightSet(requestedPreset);
    const unsigned taken = NrSwin4::UseSet(set ? set : NrSwin4::kDefaultSet);
    NrNet4::UseSet(taken);
    NrFfnV2::UseSet(taken);
    if (!IsKnown(requestedPreset))
        gSetMissing = "model " + std::to_string(requestedPreset) + " was removed from this build; choose another NR model";
    else if (gOn && gEnableProblem.empty())
    {
        std::string file;
        const bool complete = NrSwin4::SetComplete(taken, &file) && NrNet4::SetComplete(taken, &file) &&
                              NrFfnV2::SetComplete(taken, &file);
        if (!complete)
            gSetMissing = "a weight file of this model is missing or incomplete (nvfp4/" + file + ")";
    }
    PauseForEvaluation(!gOn || !gSetMissing.empty() || !gEnableProblem.empty());
    if (gOn && gSetMissing.empty() && gEnableProblem.empty())
        gBefore = Read();
}

void EndEvaluation(ID3D12GraphicsCommandList* list)
{
    NrFp8::EndEvaluation(list);
    NrFp8::UseSet(0);
    NrFfnV2::EndEvaluation(list);
    NrSwin4::EndEvaluation(list);
    NrNet4::EndEvaluation(list);
    PauseForEvaluation(true);
}

bool OursRan(bool whole)
{
    std::lock_guard guard(gMutex);
    if (!gOurs && !gModelB)
        return true;
    gRefused.clear();
    std::string why;
    if (gFp8)
    {
        if (!gOn)
            why = "the FP8 models were not loaded";
        else if (!gFp8Problem.empty())
            why = gFp8Problem;
        else if (!gSetMissing.empty())
            why = gSetMissing;
        else if (NrFp8::Faults() != gFp8Faults)
            why = NrFp8::Problem();
        else if (NrFp8::Completed() == gFp8Completed)
            why = "the FP8 model did not see this pass's evaluation (" + NrFp8::Status(false) + ")";
    }
    else if (gModelB)
    {
        const auto now = NrFfnV2::CompletedWork();
        if (!gOn)
            why = "the 4-bit feed-forward was not loaded";
        else if (now.pairs <= gBefore.ffn.pairs)
            why = "feed-forward: " + NrFfnV2::Status(false);
        else if (now.faults > gBefore.ffn.faults)
            why = "part of this pass ran without our kernels or weights (feed-forward: " + NrFfnV2::Status(false) + ")";
    }
    else if (!gOn)
        why = "the 4-bit kernels were not loaded";
    else if (!gEnableProblem.empty())
        why = gEnableProblem;
    else if (!gSetMissing.empty())
        why = gSetMissing;
    else
    {
        const Work now = Read();
        const std::string swin = NrSwin4::Status(false);
        if (swin.rfind("active", 0) != 0 || now.swin <= gBefore.swin)
            why = "fused blocks: " + swin;
        else if (whole && now.net <= gBefore.net)
            why = "split blocks and transformer: " + NrNet4::Status(false);
        else if (whole && now.ffn.pairs <= gBefore.ffn.pairs)
            why = "feed-forward: " + NrFfnV2::Status(false);
        else if (NrFfnV2::Failed())
            why = "a feed-forward pair failed half-way";
        else if (now.swinFaults > gBefore.swinFaults)
            why = "part of this pass ran with the runtime's own weights (fused blocks: " + NrSwin4::Status(false) + ")";
        else if (now.netFaults > gBefore.netFaults)
            why = "part of this pass ran without our kernels or weights (split blocks and transformer: " + NrNet4::Status(false) + ")";
        else if (now.ffn.faults > gBefore.ffn.faults)
            why = "part of this pass ran without our kernels or weights (feed-forward: " + NrFfnV2::Status(false) + ")";
    }
    if (why.empty())
    {
        if (!gRunning)
            Say(gFp8 ? "running: this pass is an FP8 model of ours (NVIDIA's kernels with our weights) | " + NrFp8::Status(false)
                     : gModelB ? "running: this pass is Model B with our 4-bit feed-forward | " + NrFfnV2::Status(false)
                               : "running: this pass is our 4-bit kernels | " + NrSwin4::Status(false) + " | " + NrNet4::Status(false));
        gRunning = true;
        gToldFailed = false;
        gMissed = 0;
        gWhy.clear();
        return true;
    }
    gRunning = false;
    gWhy = std::move(why);
    // FP8 does not need the 4-bit recognition grace period.
    const bool certain = gFp8 || (!gModelB && !gEnableProblem.empty()) || !gSetMissing.empty();
    if (gMissed < kStarting || certain)
        gMissed = certain ? kStarting : gMissed + 1;
    if (gMissed >= kStarting && !gToldFailed)
    {
        gToldFailed = true;
        Say(std::string("FAILED: the pass asked for ") + (gFp8 ? "an FP8" : "a 4-bit") +
            " model and it did not run, so the pass is not applied (no fallback to the original model): " + gWhy);
    }
    return false;
}

void Refuse(const char* reason)
{
    std::lock_guard guard(gMutex);
    const std::string text = reason ? reason : "";
    if (gRefused != text)
        Say("FAILED: " + text + "; the pass is not applied (no fallback to the original model)");
    gRefused = text;
    gRunning = false;
}

std::string Problem()
{
    std::lock_guard guard(gMutex);
    if (!gRefused.empty())
        return gRefused;
    if (!gFp8 && !gModelB && !gEnableProblem.empty())
        return gEnableProblem;
    return gMissed >= kStarting ? gWhy : std::string();
}

bool Running()
{
    std::lock_guard guard(gMutex);
    return gRunning;
}

void Release(ID3D12Device* device)
{
    NrFfnV2::ReleaseDevice(device);
    NrSwin4::ReleaseDevice(device);
    NrNet4::ReleaseDevice(device);
    NrFp8::ReleaseDevice(device);
}

std::string Status()
{
    return "feed-forward: " + NrFfnV2::Status(true) + " | fused blocks: " + NrSwin4::Status(true) + " | network: " +
           NrNet4::Status(true) + " | FP8 models: " + NrFp8::Status(true);
}
} // namespace DlssNr::Nvfp4
