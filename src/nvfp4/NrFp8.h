#pragma once
#include <filesystem>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

// NVIDIA's FP8 kernels with local weights. Match each weighted launch to its manifest and patch the
// record pointers in a copy of its parameters. A mismatch faults the evaluation; do not use its output.
// Assets: fp8_set<n>.txt and .bin. The manifest starts with `evaluation <count>` (includes cc_cb_clear),
// then `launch <n> name <kernel> params <size> record <pointer_offset> [params ...] offset <bin_offset>
// bytes <length> [blend <pointer_offset>]`. Records are 512-byte aligned.
namespace NrFp8 {
void* WrapNvapi(unsigned id, void* original);
// Validate the manifests now; upload a set's weights on its first evaluation.
void Enable(const std::filesystem::path& assets);
void UseSet(unsigned set);  // between evaluations only; 0 leaves the runtime's records alone
bool SetComplete(unsigned set);
unsigned long long Faults();
std::string Problem();
unsigned long long Completed();
void EndEvaluation(ID3D12GraphicsCommandList* commands) noexcept;
// Release only after the GPU has finished using the weights.
void ReleaseDevice(ID3D12Device* device);
std::string Status(bool counters);
void SetLog(void (*log)(const char* line));
}
