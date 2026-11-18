// See NrNvfp4.h. Self-contained (no precompiled header). The settings are the ones the Display Filter application
// gives the same three layers (standalone/NrCapture/Processor.cpp: ApplyNvfp4, ApplyNvfp4Swin, ApplyNvfp4Net,
// Processor::Evaluate).
#include "NrNvfp4.h"

#include <windows.h>
#include <d3d12.h>

#include "NrFfnV2.h"
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
    unsigned long long swin = 0, net = 0, ffn = 0;
    unsigned long long swinFaults = 0, netFaults = 0, ffnFaults = 0;
};

void (*gLog)(const char*) = nullptr;
std::mutex gMutex;
bool gOn = false;
std::filesystem::path gAssets;
bool gAskedDestroy = false;
std::string gEnableProblem; // a layer that could not be switched on: nothing can run
std::string gRefused;       // a path that cannot run these models at all
bool gOurs = false;         // the evaluation in progress asked for a 4-bit model
bool gPassInOne = false;
bool gFeedForwardOnly = false; // ... and only its feed-forward is ours (Model B)
std::string gSetMissing;     // the pass-in-one model asked for has no weight files: nothing else is run in its place
Work gBefore;
unsigned gMissed = 0; // evaluations in a row that asked for our kernels and did not get them
std::string gWhy;     // why the last of them did not
bool gRunning = false, gToldFailed = false;

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
    work.ffn = After(NrFfnV2::Status(true), "feed-forward pairs replaced: ");
    work.swinFaults = NrSwin4::Faults();
    work.netFaults = NrNet4::Faults();
    work.ffnFaults = NrFfnV2::Faults();
    return work;
}

// Pause what a pass must not take over. Model B's feed-forward has only its feed-forward in 4 bits: the fused blocks
// and the split blocks/transformer keep the runtime's kernels for it, so they stay paused -- switched on, they would
// leave out launches Model B does not have.
void PauseForEvaluation(bool paused, bool feedForwardOnly)
{
    NrSwin4::Pause(paused || feedForwardOnly);
    NrNet4::Pause(paused || feedForwardOnly);
    NrFfnV2::Pause(paused);
}
} // namespace

void SetLog(void (*log)(const char* line))
{
    gLog = log;
    NrFfnV2::SetLog(log);
    NrSwin4::SetLog(log);
    NrNet4::SetLog(log);
}

void* WrapNvapi(unsigned id, void* original)
{
    if (!original)
        return original;
    // The rest-of-the-network layer outermost (it recognises Model A by the runtime's own sequence of launches), then
    // the fused blocks: each of their launches replaces one of the runtime's, so the feed-forward layer below still
    // counts the launches of an evaluation as the runtime made them.
    return NrNet4::WrapNvapi(id, NrSwin4::WrapNvapi(id, NrFfnV2::WrapNvapi(id, original)));
}

void Configure(bool wanted, const std::filesystem::path& assets)
{
    std::lock_guard guard(gMutex);
    if (!wanted || (gOn && assets == gAssets))
        return;
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
    // Idle until an evaluation asks: other users of the same NvAPI calls (DLSS itself) pass straight through.
    PauseForEvaluation(true, false);
    gMissed = 0;
    gRunning = gToldFailed = false;
    if (gEnableProblem.empty())
        Say("loaded, assets " + assets.string());
    else
        Say("FAILED to load, so a pass that asks for a 4-bit model is not applied: " + gEnableProblem);
}

void BeginEvaluation(unsigned requestedPreset)
{
    std::lock_guard guard(gMutex);
    gOurs = IsNvfp4(requestedPreset);
    gPassInOne = IsPassInOne(requestedPreset);
    gFeedForwardOnly = IsModelBFeedForward(requestedPreset);
    // A pass-in-one model is the same kernels with its own set of weights. The set must be chosen first: each
    // family checks the files of the set it will actually use. Model B's feed-forward has its own weights and no set
    // of its own: it runs the one set its folder carries.
    const unsigned set = WeightSet(requestedPreset);
    const unsigned taken = NrSwin4::UseSet(set ? set : NrSwin4::kDefaultSet);
    NrNet4::UseSet(gFeedForwardOnly ? NrNet4::kDefaultSet : taken);
    NrFfnV2::UseSet(gFeedForwardOnly ? NrFfnV2::kDefaultSet : taken);
    gSetMissing.clear();
    if (gOurs && !IsKnown(requestedPreset))
        gSetMissing = "model " + std::to_string(requestedPreset) + " was removed from this build; choose another NR model";
    else if (gOurs && gOn && gEnableProblem.empty())
    {
        std::string file;
        const bool complete = gFeedForwardOnly
            ? NrFfnV2::SetComplete(NrFfnV2::kDefaultSet, &file, 1)
            : NrSwin4::SetComplete(taken, &file) && NrNet4::SetComplete(taken, &file) &&
                  NrFfnV2::SetComplete(taken, &file);
        if (!complete)
            gSetMissing = "a weight file of this model is missing or incomplete (OptiScaler/nvfp4/" + file + ")";
    }
    PauseForEvaluation(!gOurs || !gSetMissing.empty(), gFeedForwardOnly);
    if (gOurs && gOn)
        gBefore = Read();
}

void EndEvaluation(ID3D12GraphicsCommandList* list)
{
    NrFfnV2::EndEvaluation(list);
    NrSwin4::EndEvaluation(list);
    NrNet4::EndEvaluation(list);
    PauseForEvaluation(true, gFeedForwardOnly);
}

bool OursRan(bool whole)
{
    std::lock_guard guard(gMutex);
    if (!gOurs)
        return true;
    gRefused.clear();
    std::string why;
    if (!gOn)
        why = "the 4-bit kernels were not loaded";
    else if (!gEnableProblem.empty())
        why = gEnableProblem;
    else if (!gSetMissing.empty())
        why = gSetMissing;
    else
    {
        const Work now = Read();
        // Model B's feed-forward is the only part of that pass that is ours: its fused blocks and its split
        // blocks/transformer are the runtime's, so their counters stay put by design.
        const std::string swin = NrSwin4::Status(false);
        if (!gFeedForwardOnly && (swin.rfind("active", 0) != 0 || now.swin <= gBefore.swin))
            why = "fused blocks: " + swin;
        else if (!gFeedForwardOnly && whole && now.net <= gBefore.net)
            why = "split blocks and transformer: " + NrNet4::Status(false);
        else if (whole && now.ffn <= gBefore.ffn)
            why = "feed-forward: " + NrFfnV2::Status(false);
        else if (NrFfnV2::Failed())
            why = "a feed-forward pair failed half-way";
        else if (!gFeedForwardOnly && now.swinFaults > gBefore.swinFaults)
            why = "part of this pass ran with the runtime's own weights (fused blocks: " + NrSwin4::Status(false) + ")";
        else if (!gFeedForwardOnly && now.netFaults > gBefore.netFaults)
            why = "part of this pass ran without our kernels or weights (split blocks and transformer: " + NrNet4::Status(false) + ")";
        else if (now.ffnFaults > gBefore.ffnFaults)
            why = "part of this pass ran without our kernels or weights (feed-forward: " + NrFfnV2::Status(false) + ")";
    }
    if (why.empty())
    {
        if (!gRunning)
            Say(gFeedForwardOnly ? "running: this pass is Model B, its feed-forward in our 4-bit kernels | " + NrFfnV2::Status(false)
                                 : "running: this pass is our 4-bit kernels | " + NrSwin4::Status(false) + " | " + NrNet4::Status(false));
        gRunning = true;
        gToldFailed = false;
        gMissed = 0;
        gWhy.clear();
        return true;
    }
    gRunning = false;
    gWhy = std::move(why);
    const bool certain = !gEnableProblem.empty() || !gSetMissing.empty();
    if (gMissed < kStarting || certain)
        gMissed = certain ? kStarting : gMissed + 1;
    if (gMissed >= kStarting && !gToldFailed)
    {
        gToldFailed = true;
        Say("FAILED: the pass asked for a 4-bit model and it did not run, so the pass is not applied (no fallback to "
            "the original model): " + gWhy);
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
    if (!gEnableProblem.empty())
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
}

std::string Status()
{
    return "feed-forward: " + NrFfnV2::Status(true) + " | fused blocks: " + NrSwin4::Status(true) + " | network: " +
           NrNet4::Status(true);
}
} // namespace DlssNr::Nvfp4
