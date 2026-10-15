#include "../src/wakeup_policy.hpp"
#include <assert.h>
#include <stdio.h>

using namespace USBXHCIWakeup;

static Observation head(uint64_t serial = 1, uint32_t index = 0,
                        uint32_t cycle = 1) {
    Observation o;
    o.head.epoch = 5;
    o.head.ring = 0x1000;
    o.head.consumerIndex = index;
    o.head.consumerCycle = cycle;
    o.head.consumerSerial = serial;
    o.head.words[0] = 0x1234;
    o.head.words[1] = 0;
    o.head.words[2] = 0x01000000;
    o.head.words[3] = (32U << 10) | cycle;
    o.valid = true;
    o.coherent = true;
    return o;
}

static Candidate ready(Policy &p, const Observation &o, uint64_t begin,
                       uint64_t wait = 2000) {
    Candidate candidate;
    assert(!p.observe(o, begin, candidate));
    assert(!candidate.valid);
    assert(p.observe(o, begin + wait, candidate));
    assert(candidate.valid);
    return candidate;
}

static void stableThresholdAndDeferredCommit() {
    Policy p;
    assert(Policy::DefaultStallUs == 200);
    assert(p.start(Mode::Single));
    Observation o = head();
    Candidate c;
    assert(!p.observe(o, 0, c));
    assert(!p.observe(o, 100, c));
    assert(!p.observe(o, 199, c));
    assert(p.observe(o, 200, c));
    assert(p.stats().wakesCommitted == 0);
    Candidate c2;
    assert(p.observe(o, 3000, c2));
    assert(c.token == c2.token);
    assert(p.stats().wakesCommitted == 0);
    assert(p.commit(c, o, 3000));
    assert(p.stats().wakesCommitted == 1);
    assert(p.stats().wakesAccepted == 0);
    p.reportWakeResult(true);
    assert(p.stats().wakesAccepted == 1);
    assert(!p.commit(c2, o, 3001));
    assert(p.stats().wakesCommitted == 1);
}

static void singleBudgetAcrossNewHeads() {
    Policy p;
    assert(p.start(Mode::Single, 2000));
    Observation o = head();
    Candidate c = ready(p, o, 10);
    assert(p.commit(c, o, 2010));
    p.reportWakeResult(true);
    Candidate next;
    Observation consumed = head(2, 1);
    assert(!p.observe(consumed, 3000, next));
    assert(!p.active());
    assert(p.stopReason() == StopReason::SingleComplete);
    assert(p.stats().progressAfterWake == 1);
    assert(!p.observe(consumed, 10000, next));
    p.cancel();
    assert(p.stopReason() == StopReason::SingleComplete);
    assert(p.stats().wakesCommitted == 1);
}

static void watchdogAdvancesToNewHeadAndWrapsCycle() {
    Policy p;
    assert(p.start(Mode::Watchdog, 500));
    Observation o = head(10, 255, 1);
    Candidate c = ready(p, o, 0, 500);
    assert(p.commit(c, o, 500));
    p.reportWakeResult(true);
    Observation wrapped = head(11, 0, 0);
    Candidate c2;
    assert(!p.observe(wrapped, 600, c2));
    assert(p.observe(wrapped, 1100, c2));
    assert(p.commit(c2, wrapped, 1100));
    p.reportWakeResult(true);
    assert(p.active());
    assert(p.stats().wakesCommitted == 2);
    assert(p.stats().progressAfterWake == 1);
}

static void normalIrqWinsBeforeCandidate() {
    Policy p;
    p.start(Mode::Watchdog, 2000);
    Candidate c;
    Observation o = head();
    assert(!p.observe(o, 0, c));
    Observation next = head(2, 1);
    assert(!p.observe(next, 1900, c));
    assert(!p.observe(next, 2000, c));
    assert(p.observe(next, 3900, c));
    assert(p.stats().wakesCommitted == 0);
    assert(p.stats().progressTransitions == 1);
}

static void normalIrqWinsAtCommitGate() {
    Policy p;
    p.start(Mode::Single, 2000);
    Observation o = head();
    Candidate stale = ready(p, o, 0);
    Observation next = head(2, 1);
    assert(!p.commit(stale, next, 2001));
    assert(p.active());
    assert(p.stats().wakesCommitted == 0);
    Candidate fresh;
    assert(p.observe(next, 4001, fresh));
    assert(p.commit(fresh, next, 4001));
    assert(p.stats().wakesCommitted == 1);
}

static void noDuplicateWakeAndNoInfiniteRetry() {
    Policy p;
    p.start(Mode::Watchdog, 2000);
    Observation o = head();
    Candidate c = ready(p, o, 100);
    assert(p.commit(c, o, 2100));
    p.reportWakeResult(true);
    for (uint64_t t = 2101; t < 12100; t += 499) {
        Candidate again;
        assert(!p.observe(o, t, again));
        assert(!again.valid);
    }
    Candidate ignored;
    assert(!p.observe(o, 12100, ignored));
    assert(!p.active());
    assert(p.stopReason() == StopReason::NoProgress);
    assert(p.stats().noProgressStops == 1);
    p.cancel();
    assert(p.stopReason() == StopReason::NoProgress);
    assert(p.stats().wakesCommitted == 1);
}

static void failureStopsAndCancelPreservesReason() {
    Policy p;
    p.start(Mode::Watchdog, 2000);
    Observation o = head();
    Candidate c = ready(p, o, 0);
    assert(p.commit(c, o, 2000));
    p.reportWakeResult(false);
    assert(!p.active());
    assert(p.stopReason() == StopReason::WakeFailed);
    assert(p.stats().wakeFailures == 1);
    p.cancel();
    assert(p.stopReason() == StopReason::WakeFailed);
    p.reportWakeResult(true);
    assert(p.stats().wakesAccepted == 0);
}

static void invalidResetsUncommittedCandidate() {
    Policy p;
    p.start(Mode::Watchdog, 2000);
    Observation o = head();
    Candidate stale = ready(p, o, 0);
    Observation empty = o;
    empty.valid = false;
    Candidate c;
    assert(!p.observe(empty, 2001, c));
    assert(!p.commit(stale, o, 2002));
    assert(!p.observe(o, 2003, c));
    assert(!p.observe(o, 4002, c));
    assert(p.observe(o, 4003, c));
    assert(p.stats().invalidObservations == 1);
    assert(p.stats().wakesCommitted == 0);
}

static void raceAtFinalGateAllowsStableRetry() {
    Policy p;
    p.start(Mode::Watchdog, 2000);
    Observation o = head();
    Candidate stale = ready(p, o, 0);
    Observation raced = o;
    raced.coherent = false;
    assert(!p.commit(stale, raced, 2001));
    assert(p.stats().wakesCommitted == 0);
    assert(p.stats().raceResets == 1);
    Candidate c = ready(p, o, 2002);
    assert(p.commit(c, o, 4002));
}

static void fullFourWordsArePartOfIdentity() {
    for (unsigned word = 0; word != 4; ++word) {
        Policy p;
        p.start(Mode::Watchdog, 2000);
        Observation o = head();
        Candidate stale = ready(p, o, 0);
        Observation changed = o;
        changed.head.words[word] ^= (word == 3 ? 2U : 1U);
        assert(!p.commit(stale, changed, 2001));
        Candidate c;
        assert(!p.observe(changed, 4000, c));
        assert(p.observe(changed, 4001, c));
        assert(p.commit(c, changed, 4001));
        assert(p.stats().wakesCommitted == 1);
    }
}

static void changedWordsAfterWakeDoNotReplenishBudget() {
    Policy p;
    p.start(Mode::Watchdog, 2000);
    Observation o = head();
    Candidate c = ready(p, o, 0);
    assert(p.commit(c, o, 2000));
    p.reportWakeResult(true);
    o.head.words[0] ^= 1;
    assert(!p.observe(o, 4000, c));
    o.valid = false;
    assert(!p.observe(o, 8000, c));
    assert(!p.observe(o, 12000, c));
    assert(p.stopReason() == StopReason::NoProgress);
    assert(p.stats().wakesCommitted == 1);
}

static void clockRegressionStops() {
    Policy p;
    p.start(Mode::Watchdog, 2000);
    Observation o = head();
    Candidate c = ready(p, o, 100);
    assert(!p.commit(c, o, 2099));
    assert(p.stopReason() == StopReason::ClockRegressed);
    assert(p.stats().clockRegressions == 1);
    assert(p.stats().wakesCommitted == 0);
}

static void lifecycleChangeStops() {
    for (unsigned field = 0; field != 2; ++field) {
        Policy p;
        p.start(Mode::Watchdog, 2000);
        Observation o = head();
        Candidate c = ready(p, o, 0);
        if (field == 0) ++o.head.epoch;
        else o.head.ring += 0x1000;
        assert(!p.commit(c, o, 2001));
        assert(p.stopReason() == StopReason::LifecycleChanged);
    }
}

static void backwardsSerialStopsAndUnpublishedIndexResets() {
    Policy p;
    p.start(Mode::Watchdog, 2000);
    Observation o = head(10, 10);
    Candidate c = ready(p, o, 0);
    Observation raced = head(10, 11);
    assert(!p.commit(c, raced, 2001));
    assert(p.active());
    assert(p.stats().raceResets == 1);
    Observation backwards = head(9, 9);
    assert(!p.observe(backwards, 2002, c));
    assert(p.stopReason() == StopReason::SerialRegressed);
}

static void impossibleCycleIsNotAValidHead() {
    Policy p;
    p.start(Mode::Watchdog, 2000);
    Observation o = head();
    o.head.words[3] &= ~1U;
    Candidate c;
    assert(!p.observe(o, 0, c));
    assert(!p.observe(o, 2000, c));
    assert(p.active());
    assert(p.stats().raceResets == 2);
    o.head.consumerCycle = 2;
    assert(!p.observe(o, 2001, c));
    assert(p.stopReason() == StopReason::InvalidIdentity);
}

static void cancellationAndSessionToken() {
    Policy p;
    p.start(Mode::Single, 2000);
    Observation o = head();
    Candidate stale = ready(p, o, 0);
    p.cancel();
    assert(!p.active());
    assert(p.stopReason() == StopReason::Cancelled);
    assert(p.stats().cancellations == 1);
    p.cancel();
    assert(p.stats().cancellations == 1);
    p.start(Mode::Single, 2000);
    Candidate fresh = ready(p, o, 3000);
    assert(!p.commit(stale, o, 5000));
    assert(p.commit(fresh, o, 5000));
    assert(p.stats().wakesCommitted == 1);
}

static void progressArrivingAfterDeadlineIsStillProgress() {
    Policy p;
    p.start(Mode::Watchdog, 2000);
    Observation o = head();
    Candidate c = ready(p, o, 0);
    assert(p.commit(c, o, 2000));
    p.reportWakeResult(true);
    Observation next = head(2, 1);
    assert(!p.observe(next, 20000, c));
    assert(p.active());
    assert(p.stats().progressAfterWake == 1);
    assert(p.stats().noProgressStops == 0);
}

static void configurationAndDisabledMode() {
    Policy p;
    Candidate c;
    Observation o = head();
    assert(!p.active());
    assert(p.stopReason() == StopReason::Disabled);
    assert(!p.observe(o, 0, c));
    assert(!p.start(Mode::Single, 99));
    assert(p.stopReason() == StopReason::InvalidConfiguration);
    assert(!p.start(Mode::Watchdog, 10001));
    assert(p.start(Mode::Watchdog, 10000));
    c = ready(p, o, 0, 10000);
    assert(p.commit(c, o, 10000));
}

int main() {
    stableThresholdAndDeferredCommit();
    singleBudgetAcrossNewHeads();
    watchdogAdvancesToNewHeadAndWrapsCycle();
    normalIrqWinsBeforeCandidate();
    normalIrqWinsAtCommitGate();
    noDuplicateWakeAndNoInfiniteRetry();
    failureStopsAndCancelPreservesReason();
    invalidResetsUncommittedCandidate();
    raceAtFinalGateAllowsStableRetry();
    fullFourWordsArePartOfIdentity();
    changedWordsAfterWakeDoNotReplenishBudget();
    clockRegressionStops();
    lifecycleChangeStops();
    backwardsSerialStopsAndUnpublishedIndexResets();
    impossibleCycleIsNotAValidHead();
    cancellationAndSessionToken();
    progressArrivingAfterDeadlineIsStillProgress();
    configurationAndDisabledMode();
    puts("wakeup_policy: 18 tests passed");
    return 0;
}
