#pragma once

#include <stdint.h>
#include <stddef.h>

// Pure policy only. The caller must serialize every method, verify the driver
// lifecycle under its gate, and call commit immediately before the native wake call.
// No pointer in this file is dereferenced; no clock, IOKit, allocation, or MMIO.
namespace USBXHCIWakeup {

enum class Mode : uint8_t { Disabled = 0, Single = 1, Watchdog = 2 };

enum class StopReason : uint8_t {
    None = 0,
    Disabled,
    InvalidConfiguration,
    Cancelled,
    ClockRegressed,
    LifecycleChanged,
    SerialRegressed,
    InvalidIdentity,
    WakeFailed,
    NoProgress,
    SingleComplete
};

struct HeadIdentity {
    uint64_t epoch;
    uintptr_t ring;
    uint32_t consumerIndex;
    uint32_t consumerCycle;
    uint64_t consumerSerial;
    uint32_t words[4];

    HeadIdentity() : epoch(0), ring(0), consumerIndex(0), consumerCycle(0),
                     consumerSerial(0), words{0, 0, 0, 0} {}
};

inline bool sameHead(const HeadIdentity &a, const HeadIdentity &b) {
    return a.epoch == b.epoch && a.ring == b.ring &&
        a.consumerIndex == b.consumerIndex &&
        a.consumerCycle == b.consumerCycle &&
        a.consumerSerial == b.consumerSerial &&
        a.words[0] == b.words[0] && a.words[1] == b.words[1] &&
        a.words[2] == b.words[2] && a.words[3] == b.words[3];
}

struct Observation {
    HeadIdentity head;
    // valid: cycle-valid event at the current consumer. coherent: the caller's
    // double read and lifecycle/consumer recheck all agreed for this sample.
    bool valid;
    bool coherent;
    Observation() : head(), valid(false), coherent(false) {}
};

struct Candidate {
    HeadIdentity head;
    uint64_t session;
    uint64_t token;
    bool valid;
    Candidate() : head(), session(0), token(0), valid(false) {}
};

struct Stats {
    uint64_t observations;
    uint64_t invalidObservations;
    uint64_t raceResets;
    uint64_t progressTransitions;
    uint64_t progressAfterWake;
    uint64_t wakesCommitted;
    uint64_t wakesAccepted;
    uint64_t wakeFailures;
    uint64_t noProgressStops;
    uint64_t clockRegressions;
    uint64_t rejectedCommits;
    uint64_t cancellations;

    Stats() : observations(0), invalidObservations(0), raceResets(0),
        progressTransitions(0), progressAfterWake(0), wakesCommitted(0),
        wakesAccepted(0), wakeFailures(0), noProgressStops(0),
        clockRegressions(0), rejectedCommits(0), cancellations(0) {}
};

class Policy {
public:
    enum : uint32_t {
        DefaultStallUs = 200,
        MinimumStallUs = 100,
        MaximumStallUs = 10000,
        NoProgressUs = 10000
    };

    Policy() : session_(0), token_(0) { start(Mode::Disabled); }

    // Starting a session resets all counters and the single-wake budget.
    // Mode::Disabled is a successful configuration which never offers a wake.
    bool start(Mode mode, uint32_t stallUs = DefaultStallUs) {
        bump(session_);
        bump(token_);
        mode_ = mode;
        stallUs_ = stallUs;
        stats_ = Stats();
        active_ = false;
        reason_ = StopReason::None;
        haveTime_ = false;
        lastTimeUs_ = 0;
        haveConsumer_ = false;
        consumer_ = HeadIdentity();
        stableHead_ = HeadIdentity();
        haveStable_ = false;
        stableCount_ = 0;
        firstSeenUs_ = 0;
        candidatePending_ = false;
        wakePending_ = false;
        awaitingResult_ = false;
        wakeAtUs_ = 0;
        if ((mode != Mode::Disabled && mode != Mode::Single &&
             mode != Mode::Watchdog) || stallUs < MinimumStallUs ||
             stallUs > MaximumStallUs) {
            reason_ = StopReason::InvalidConfiguration;
            return false;
        }
        if (mode == Mode::Disabled) {
            reason_ = StopReason::Disabled;
            return true;
        }
        active_ = true;
        return true;
    }

    // A candidate is advisory: observe does not spend the wake budget. Two
    // coherent, identical, cycle-valid observations and stallUs are necessary.
    // Invalid/torn samples invalidate an uncommitted candidate and restart its
    // stability interval. They never replenish an already committed head.
    bool observe(const Observation &sample, uint64_t nowUs, Candidate &out) {
        out = Candidate();
        if (!active_) return false;
        ++stats_.observations;
        if (!checkClock(nowUs)) return false;

        if (!sample.coherent) {
            ++stats_.raceResets;
            resetCandidate();
            checkNoProgress(nowUs);
            return false;
        }
        if (!sample.head.ring || sample.head.consumerCycle > 1) {
            stop(StopReason::InvalidIdentity);
            return false;
        }
        if (sample.valid &&
            (sample.head.words[3] & 1U) != sample.head.consumerCycle) {
            ++stats_.raceResets;
            resetCandidate();
            checkNoProgress(nowUs);
            return false;
        }
        if (!haveConsumer_) {
            consumer_ = sample.head;
            haveConsumer_ = true;
        } else {
            if (sample.head.epoch != consumer_.epoch ||
                sample.head.ring != consumer_.ring) {
                stop(StopReason::LifecycleChanged);
                return false;
            }
            if (sample.head.consumerSerial < consumer_.consumerSerial) {
                stop(StopReason::SerialRegressed);
                return false;
            }
            if (sample.head.consumerSerial > consumer_.consumerSerial) {
                ++stats_.progressTransitions;
                if (wakePending_) ++stats_.progressAfterWake;
                consumer_ = sample.head;
                resetCandidate();
                wakePending_ = false;
                awaitingResult_ = false;
                if (mode_ == Mode::Single && stats_.wakesCommitted != 0) {
                    stop(StopReason::SingleComplete);
                    return false;
                }
            } else if (sample.head.consumerIndex != consumer_.consumerIndex ||
                       sample.head.consumerCycle != consumer_.consumerCycle) {
                // Index/cycle cannot move while the hooked consume serial is
                // unchanged. Treat a publication race as an unusable sample.
                ++stats_.raceResets;
                resetCandidate();
                checkNoProgress(nowUs);
                return false;
            }
        }

        if (!sample.valid) ++stats_.invalidObservations;
        if (wakePending_) {
            // Only a serial advance proves normal consumption. Changed words,
            // an invalid cycle, or extra IRQs do not authorize a second wake.
            checkNoProgress(nowUs);
            return false;
        }
        if (!sample.valid) {
            resetCandidate();
            return false;
        }
        if (!haveStable_ || !sameHead(stableHead_, sample.head)) {
            if (haveStable_) ++stats_.raceResets;
            resetCandidate();
            stableHead_ = sample.head;
            haveStable_ = true;
            stableCount_ = 1;
            firstSeenUs_ = nowUs;
            return false;
        }
        if (stableCount_ < 2) ++stableCount_;
        if (stableCount_ < 2 || nowUs - firstSeenUs_ < stallUs_) return false;
        if (mode_ == Mode::Single && stats_.wakesCommitted != 0) return false;

        candidatePending_ = true;
        out.head = stableHead_;
        out.session = session_;
        out.token = token_;
        out.valid = true;
        return true;
    }

    // Invoke inside the caller's final gate after a fresh coherent head read.
    // On true, invoke the validated native wake path immediately, then reportWakeResult. The
    // identity/token recheck rejects normal IRQ progress which won the race.
    bool commit(const Candidate &candidate, const Observation &finalSample,
                uint64_t nowUs) {
        if (!active_ || !candidate.valid || !candidatePending_ ||
            candidate.session != session_ || candidate.token != token_ ||
            !sameHead(candidate.head, stableHead_)) {
            ++stats_.rejectedCommits;
            return false;
        }
        Candidate checked;
        if (!observe(finalSample, nowUs, checked) || !checked.valid ||
            checked.session != candidate.session ||
            checked.token != candidate.token ||
            !sameHead(candidate.head, checked.head)) {
            ++stats_.rejectedCommits;
            return false;
        }
        // Count an attempt only here, never while merely offering a candidate.
        ++stats_.wakesCommitted;
        candidatePending_ = false;
        bump(token_);
        wakePending_ = true;
        awaitingResult_ = true;
        wakeAtUs_ = nowUs;
        return true;
    }

    // Accepted means the software dispatch call was made; it does not claim
    // event consumption or request completion. A failure disables the session.
    void reportWakeResult(bool accepted) {
        if (!awaitingResult_) return;
        awaitingResult_ = false;
        if (!accepted) {
            ++stats_.wakeFailures;
            stop(StopReason::WakeFailed);
        } else {
            ++stats_.wakesAccepted;
        }
    }

    void cancel() {
        if (!active_) return;
        ++stats_.cancellations;
        stop(StopReason::Cancelled);
    }

    bool active() const { return active_; }
    Mode mode() const { return mode_; }
    StopReason stopReason() const { return reason_; }
    uint32_t stallUs() const { return stallUs_; }
    const Stats &stats() const { return stats_; }

private:
    static void bump(uint64_t &value) { if (++value == 0) ++value; }

    void resetCandidate() {
        bump(token_);
        candidatePending_ = false;
        haveStable_ = false;
        stableCount_ = 0;
        firstSeenUs_ = 0;
    }

    void stop(StopReason reason) {
        active_ = false;
        reason_ = reason;
        awaitingResult_ = false;
        resetCandidate();
    }

    bool checkClock(uint64_t nowUs) {
        if (haveTime_ && nowUs < lastTimeUs_) {
            ++stats_.clockRegressions;
            stop(StopReason::ClockRegressed);
            return false;
        }
        haveTime_ = true;
        lastTimeUs_ = nowUs;
        return true;
    }

    void checkNoProgress(uint64_t nowUs) {
        if (wakePending_ && nowUs - wakeAtUs_ >= NoProgressUs) {
            ++stats_.noProgressStops;
            stop(StopReason::NoProgress);
        }
    }

    Mode mode_;
    uint32_t stallUs_;
    Stats stats_;
    bool active_;
    StopReason reason_;
    uint64_t session_;
    uint64_t token_;
    bool haveTime_;
    uint64_t lastTimeUs_;
    bool haveConsumer_;
    HeadIdentity consumer_;
    HeadIdentity stableHead_;
    bool haveStable_;
    uint32_t stableCount_;
    uint64_t firstSeenUs_;
    bool candidatePending_;
    bool wakePending_;
    bool awaitingResult_;
    uint64_t wakeAtUs_;
};

} // namespace USBXHCIWakeup
