#pragma once
#include <vector>
#include "nr/host.hpp"

namespace uplift::testing {
// Only linked into explicit regression executables. Production never uses this runtime substitute.
class IdentityHost final : public nr::Host {
 public:
  static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1u, &b);
  }
  class IdentityFeature final : public nr::FeatureInterface {
   public:
    explicit IdentityFeature(std::vector<bool>& resets) : resets_(resets) {}
    NVSDK_NGX_Result Create(ID3D12GraphicsCommandList*, const nr::CreateInfo& info) override { size_ = info.capacity; return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* list, const nr::EvaluateInputs& inputs, const nr::Controls&) override {
      resets_.push_back(inputs.reset);
      Barrier(list, inputs.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      Barrier(list, inputs.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(inputs.output, inputs.color);
      Barrier(list, inputs.output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Barrier(list, inputs.color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Release() override { size_ = {}; return NVSDK_NGX_Result_Success; }
    void Abandon() override { size_ = {}; }
    bool Created() const override { return !size_.Empty(); }
    nr::Size Capacity() const override { return size_; }
   private:
    std::vector<bool>& resets_;
    nr::Size size_;
  };
  NVSDK_NGX_Result LoadRuntime() override { return NVSDK_NGX_Result_Success; }
  void UnloadRuntime() override {}
  void AbandonRuntime() override {}
  std::unique_ptr<nr::FeatureInterface> NewFeature() override { return std::make_unique<IdentityFeature>(resets); }
  std::vector<bool> resets;
  nr::MemoryInfo QueryMemory() override { return {.budget = uint64_t{16} << 30, .usage = 0u}; }
  std::optional<uint64_t> QueryRuntimeBytes() override { return 0u; }
  std::chrono::steady_clock::time_point Now() override { return std::chrono::steady_clock::now(); }
  void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) override {
    Barrier(list, resource, before, after);
  }
};
}
