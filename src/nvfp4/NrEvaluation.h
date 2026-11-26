#pragma once
#include <string>

struct ID3D12GraphicsCommandList;

namespace nvfp4 {
// Like RecordingScope, this brackets the synchronous NGX call on its recording thread. Only the
// selected custom model is strict; other callers of the same NvAPI entry points stay transparent.
class EvaluationScope {
 public:
    EvaluationScope(ID3D12GraphicsCommandList* commands, bool selected) noexcept
        : commands_(commands), selected_(selected), previous_(current_) { current_ = this; }
    ~EvaluationScope() { current_ = previous_; }
    EvaluationScope(const EvaluationScope&) = delete;
    EvaluationScope& operator=(const EvaluationScope&) = delete;
    static EvaluationScope* For(ID3D12GraphicsCommandList* commands) noexcept {
        return current_ && current_->selected_ && current_->commands_ == commands ? current_ : nullptr;
    }
    void Reject(const char* reason) noexcept {
        failed = true;
        try { if (failure.empty()) failure = reason; } catch (...) {}
    }
    bool failed = false;
    std::string failure;

 private:
    ID3D12GraphicsCommandList* commands_;
    bool selected_;
    EvaluationScope* previous_;
    static inline thread_local EvaluationScope* current_ = nullptr;
};

} // namespace nvfp4
