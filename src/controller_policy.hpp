#pragma once

#include <stdint.h>
#include <stddef.h>

// Per-controller lifecycle only. No IOKit calls, pointer dereferences, clocks or
// allocation. The owner serializes calls, retains all objects, and performs the
// final binding/revalidation under that controller's work-loop gate.
namespace USBXHCIWakeup {

enum class ControllerPhase : uint8_t {
    Empty, Disabled, Discovering, Binding, Active, Suspended, Terminated, Blocked
};

enum class ControllerBlock : uint8_t {
    None, ExplicitStop, NoProgress, WakeFailed, InvalidState, ClockRegressed
};

class ControllerPolicy {
public:
    // Retry after a failed structural/lifecycle bind. This is separate from
    // the hot-path sampling cadence and the event-stall threshold.
    enum : uint32_t { RetryUs = 100000 };

    ControllerPolicy() = default;

    // A context identifies one IOService lifetime. New registry IDs or object
    // tokens require a new context; a stale callback cannot retarget this one.
    bool published(uint64_t registryID, uintptr_t objectToken,
                   uint64_t nowUs = 0) {
        if (!registryID || !objectToken) return false;
        if (registryID_) return registryID_ == registryID && token_ == objectToken && present_;
        registryID_ = registryID;
        token_ = objectToken;
        present_ = true;
        nextAttemptUs_ = nowUs;
        refreshPhase();
        return true;
    }

    // A user disable/enable round trip can restart an ordinary session, but
    // does not clear an explicit stop, failed wake, or no-progress quarantine.
    void configure(bool enabled, uint64_t nowUs = 0) {
        if (enabled_ == enabled) return;
        enabled_ = enabled;
        invalidate();
        nextAttemptUs_ = nowUs;
        refreshPhase();
    }

    void beginMutation() {
        if (mutationDepth_ == UINT32_MAX) { block(ControllerBlock::InvalidState); return; }
        if (!mutationDepth_) invalidate();
        ++mutationDepth_;
        refreshPhase();
    }

    bool endMutation(uint64_t nowUs) {
        if (!mutationDepth_) { block(ControllerBlock::InvalidState); return false; }
        --mutationDepth_;
        if (!mutationDepth_) nextAttemptUs_ = nowUs;
        refreshPhase();
        return true;
    }

    void terminated() {
        present_ = false;
        invalidate();
        refreshPhase();
    }

    // Safety stops survive publication, power cycling and failed rediscovery.
    // Only a newly allocated context for a different service lifetime starts
    // with a fresh policy. An explicit user reset may reconstruct the context.
    void block(ControllerBlock reason) {
        if (reason == ControllerBlock::None) reason = ControllerBlock::InvalidState;
        if (block_ == ControllerBlock::None) block_ = reason;
        invalidate();
        refreshPhase();
    }

    bool due(uint64_t nowUs) {
        if (!checkClock(nowUs)) return false;
        return phase_ == ControllerPhase::Discovering && nowUs >= nextAttemptUs_;
    }

    // The returned generation travels with the retained controller and loop.
    // Zero means no attempt is authorized. The callback must compare it before
    // dereferencing any mapped driver state, and again before publishing it.
    uint64_t beginBinding(uint64_t nowUs) {
        if (!due(nowUs)) return 0;
        invalidate();
        binding_ = true;
        phase_ = ControllerPhase::Binding;
        return epoch_;
    }

    bool bindingSucceeded(uint64_t candidateEpoch, uint64_t nowUs) {
        if (!checkClock(nowUs) || !bindingCurrent(candidateEpoch)) return false;
        binding_ = false;
        active_ = true;
        refreshPhase();
        return phase_ == ControllerPhase::Active;
    }

    bool bindingFailed(uint64_t candidateEpoch, uint64_t nowUs) {
        if (!checkClock(nowUs) || !bindingCurrent(candidateEpoch)) return false;
        binding_ = false;
        active_ = false;
        nextAttemptUs_ = nowUs > UINT64_MAX - RetryUs ? UINT64_MAX : nowUs + RetryUs;
        refreshPhase();
        return true;
    }

    bool bindingCurrent(uint64_t candidateEpoch) const {
        return candidateEpoch && candidateEpoch == epoch_ && binding_ &&
            phase_ == ControllerPhase::Binding && enabled_ && present_ &&
            !mutationDepth_ && block_ == ControllerBlock::None;
    }

    bool activeFor(uint64_t candidateEpoch) const {
        return candidateEpoch && candidateEpoch == epoch_ && active_ &&
            phase_ == ControllerPhase::Active;
    }

    ControllerPhase phase() const { return phase_; }
    ControllerBlock stopReason() const { return block_; }
    uint64_t epoch() const { return epoch_; }
    uint64_t registryID() const { return registryID_; }
    uintptr_t objectToken() const { return token_; }
    uint32_t mutationDepth() const { return mutationDepth_; }
    uint64_t nextAttemptUs() const { return nextAttemptUs_; }

private:
    void invalidate() {
        if (++epoch_ == 0) ++epoch_;
        active_ = false;
        binding_ = false;
    }

    void refreshPhase() {
        if (!registryID_) phase_ = ControllerPhase::Empty;
        else if (!present_) phase_ = ControllerPhase::Terminated;
        else if (block_ != ControllerBlock::None) phase_ = ControllerPhase::Blocked;
        else if (!enabled_) phase_ = ControllerPhase::Disabled;
        else if (mutationDepth_) phase_ = ControllerPhase::Suspended;
        else if (active_) phase_ = ControllerPhase::Active;
        else if (binding_) phase_ = ControllerPhase::Binding;
        else phase_ = ControllerPhase::Discovering;
    }

    bool checkClock(uint64_t nowUs) {
        if (haveTime_ && nowUs < lastTimeUs_) {
            block(ControllerBlock::ClockRegressed);
            return false;
        }
        haveTime_ = true;
        lastTimeUs_ = nowUs;
        return true;
    }

    uint64_t registryID_ {};
    uintptr_t token_ {};
    uint64_t epoch_ {1};
    uint64_t nextAttemptUs_ {};
    uint64_t lastTimeUs_ {};
    uint32_t mutationDepth_ {};
    bool enabled_ {};
    bool present_ {};
    bool active_ {};
    bool binding_ {};
    bool haveTime_ {};
    ControllerPhase phase_ {ControllerPhase::Empty};
    ControllerBlock block_ {ControllerBlock::None};
};

} // namespace USBXHCIWakeup
