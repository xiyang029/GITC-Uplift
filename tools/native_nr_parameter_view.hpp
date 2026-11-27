#pragma once

#include <type_traits>
#include "native_nr_test_gpu.hpp"

namespace uplift::native_nr_test {

// A call-local parameter overlay. Unknown keys still come from the original block;
// every Set stays in the overlay, so neither success nor an exception mutates it.
// Prototype only: not wired into the game's evaluate hook.
struct ParameterView final : Parameters {
  const NVSDK_NGX_Parameter& source;
  explicit ParameterView(const NVSDK_NGX_Parameter& original) : source(original) {}

  template<class T> NVSDK_NGX_Result Read(const char* key, T* result) const {
    const auto found = values.find(key);
    if (found == values.end()) return source.Get(key, result);
    const bool converted = std::visit([&](const auto& stored) {
      using S = std::decay_t<decltype(stored)>;
      if constexpr (std::is_same_v<T, S>) {
        *result = stored; return true;
      } else if constexpr (std::is_arithmetic_v<T> && std::is_arithmetic_v<S>) {
        *result = static_cast<T>(stored); return true;
      } else if constexpr (std::is_same_v<T, void*> && std::is_pointer_v<S>) {
        *result = static_cast<void*>(stored); return true;
      } else if constexpr (std::is_pointer_v<T> && std::is_same_v<S, void*>) {
        *result = static_cast<T>(stored); return true;
      } else return false;
    }, found->second);
    return converted ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
#define READ_PARAMETER(Type) \
  NVSDK_NGX_Result Get(const char* key, Type* value) const override { return Read(key, value); }
  READ_PARAMETER(unsigned long long)
  READ_PARAMETER(float)
  READ_PARAMETER(double)
  READ_PARAMETER(unsigned int)
  READ_PARAMETER(int)
  READ_PARAMETER(ID3D11Resource*)
  READ_PARAMETER(ID3D12Resource*)
  READ_PARAMETER(void*)
#undef READ_PARAMETER
};

}  // namespace uplift::native_nr_test
