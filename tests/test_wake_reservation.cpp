#include "wake_reservation.hpp"
#include <cassert>
#include <cstdio>
using USBXHCIWakeup::WakeReservation;
int main() {
    WakeReservation r;
    uintptr_t rings[3]={100,101,102};
    assert(!r.active()&&!r.mustDrain(2,10,false));
    assert(!r.begin(0,10,rings)&&!r.begin(1,0,rings));
    assert(r.begin(1,10,rings)&&r.active());
    assert(!r.begin(2,10,rings));
    assert(r.mustDrain(2,10,false));
    for (auto ring:rings) assert(r.mustDrain(2,ring,true));
    assert(!r.mustDrain(2,100,false)&&!r.mustDrain(2,10,true));
    assert(!r.mustDrain(2,999,true)&&!r.mustDrain(2,999,false));
    assert(!r.mustDrain(1,10,false)&&!r.mustDrain(1,101,true));
    assert(r.mustDrainAll(2)&&!r.mustDrainAll(1));
    assert(!r.finish(2)&&r.active());
    assert(r.finish(1)&&!r.active()&&!r.mustDrainAll(2));
    assert(!r.finish(1));
    assert(r.begin(2,20,rings)&&!r.mustDrain(1,10,false));
    assert(r.mustDrain(1,20,false)&&r.finish(2));
    puts("wake_reservation: invalid/nested reserve, matching scope, foreign drain, same-thread reentry, owner finish and reuse passed");
}
