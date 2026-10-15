#include "../src/controller_policy.hpp"
#include <assert.h>
#include <stdio.h>

using namespace USBXHCIWakeup;

static uint64_t bind(ControllerPolicy &p, uint64_t at) {
    uint64_t epoch = p.beginBinding(at);
    assert(epoch);
    assert(p.bindingSucceeded(epoch, at));
    assert(p.activeFor(epoch));
    return epoch;
}

int main() {
    ControllerPolicy a, b;
    a.configure(true);
    b.configure(true);
    assert(a.published(100, 0x1000));
    assert(b.published(200, 0x2000));
    const uint64_t oldA = bind(a, 10), oldB = bind(b, 10);
    a.beginMutation();
    a.beginMutation();
    assert(a.phase() == ControllerPhase::Suspended);
    assert(!a.activeFor(oldA));
    assert(b.activeFor(oldB));
    assert(a.endMutation(20));
    assert(!a.due(20));
    assert(a.endMutation(30));
    assert(a.due(30));
    assert(!a.bindingSucceeded(oldA, 30));
    const uint64_t newA = bind(a, 30);
    assert(newA != oldA);

    // An asynchronous binding callback is revoked by reset, disable or detach.
    a.beginMutation();
    assert(a.endMutation(40));
    const uint64_t stale = a.beginBinding(40);
    assert(stale);
    a.beginMutation();
    assert(!a.bindingSucceeded(stale, 40));
    assert(a.endMutation(50));
    const uint64_t disabled = a.beginBinding(50);
    assert(disabled);
    a.configure(false, 50);
    assert(!a.bindingSucceeded(disabled, 50));
    assert(!a.due(50));
    a.configure(true, 60);
    bind(a, 60);

    // Rediscovery retries cannot override quarantine.
    a.block(ControllerBlock::NoProgress);
    assert(a.phase() == ControllerPhase::Blocked);
    a.beginMutation();
    assert(a.endMutation(70));
    assert(a.published(100, 0x1000, 70));
    a.configure(false, 70);
    a.configure(true, 80);
    assert(!a.due(80));
    assert(a.stopReason() == ControllerBlock::NoProgress);
    assert(b.activeFor(oldB));
    assert(!a.published(300, 0x3000, 80));
    assert(!a.published(100, 0x3000, 80));

    ControllerPolicy retry;
    retry.configure(true);
    assert(retry.published(300, 0x3000));
    const uint64_t first = retry.beginBinding(0);
    assert(retry.bindingFailed(first, 0));
    assert(!retry.due(ControllerPolicy::RetryUs - 1));
    const uint64_t next = retry.beginBinding(ControllerPolicy::RetryUs);
    assert(next && next != first);
    retry.terminated();
    assert(!retry.bindingSucceeded(next, ControllerPolicy::RetryUs));
    assert(!retry.published(300, 0x3000, ControllerPolicy::RetryUs));
    assert(retry.phase() == ControllerPhase::Terminated);

    ControllerPolicy clock;
    clock.configure(true);
    assert(clock.published(400, 0x4000));
    assert(clock.due(100));
    assert(!clock.due(99));
    assert(clock.stopReason() == ControllerBlock::ClockRegressed);

    ControllerPolicy bad;
    assert(!bad.published(0, 0x1000));
    assert(!bad.published(100, 0));
    bad.configure(true);
    assert(bad.published(500, 0x5000));
    assert(!bad.endMutation(0));
    assert(bad.stopReason() == ControllerBlock::InvalidState);
    puts("controller policy: passed");
}
