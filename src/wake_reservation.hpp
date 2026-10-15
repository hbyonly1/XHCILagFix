#pragma once
#include <stdint.h>

namespace USBXHCIWakeup {
// Serialized by the sampler lock. Tokens only: never dereference an object.
// A reservation spans the complete call into the original driver, including
// callbacks. Same-thread lifecycle reentry must cancel, rather than wait on
// itself. Other matching lifecycle callers drain before their original call.
class WakeReservation {
    uintptr_t owner_ {},controller_ {},rings_[3] {};
public:
    bool active() const { return owner_!=0; }
    bool begin(uintptr_t owner,uintptr_t controller,const uintptr_t (&rings)[3]) {
        if (active()||!owner||!controller) return false;
        owner_=owner;controller_=controller;
        for (unsigned i=0;i<3;++i) rings_[i]=rings[i];
        return true;
    }
    bool matches(uintptr_t token,bool ring) const {
        if (!active()||!token) return false;
        if (!ring) return token==controller_;
        for (unsigned i=0;i<3;++i) if (rings_[i]==token) return true;
        return false;
    }
    bool mustDrain(uintptr_t caller,uintptr_t token,bool ring) const {
        return caller!=owner_&&matches(token,ring);
    }
    bool mustDrainAll(uintptr_t caller) const { return active()&&caller!=owner_; }
    bool finish(uintptr_t owner) {
        if (!active()||owner!=owner_) return false;
        owner_=controller_=0;
        for (auto &ring:rings_) ring=0;
        return true;
    }
};
}
