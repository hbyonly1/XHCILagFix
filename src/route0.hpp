#pragma once
#include <stdint.h>
#include <stddef.h>
#include "memory.hpp"

namespace XhciLagFix { namespace AllRoute0 {
static constexpr uint32_t TargetMask=0xffc00000U;
static constexpr uint32_t MaximumCapacity=256; // bounded runtime allocation metadata
static constexpr uint32_t MaxTDs=256;
static constexpr size_t RequestQueue=0x68, RequestPreviousTD=0x70;
static constexpr size_t TDArray=0, TDCapacity=8, TDNext=0x18;

inline bool pointerShape(uintptr_t value,size_t alignment) {
    if (!value || (value&(alignment-1))) return false;
#if defined(KERNEL)
    // Objects are owned by the successful original prepare, not arbitrary inputs.
    // This is a shape check, not a general-purpose mapped-memory validator.
    return (value>>48)==0xffff;
#else
    return true; // Userspace synthetic fixtures; kernel checks remain compiled in.
#endif
}
// Call ONLY on a new request at enqueueRequest entry, before the original links it.
// All prepare subclasses have finished; the original enqueue has not published it. Neither Cycle,
// pointers nor queue topology is changed here. First validate the entire chain;
// an unsupported layout leaves every TRB untouched, rather than partially routing.
inline bool rewritePreparedQueue(void *request) {
    uint32_t tdCount=0;
    if (!pointerShape(reinterpret_cast<uintptr_t>(request),8)) return false;
    if (loadField<uintptr_t>(request,RequestPreviousTD)) {
        return false;
    }
    const uintptr_t queue=loadField<uintptr_t>(request,RequestQueue);
    if (!pointerShape(queue,8)) return false;
    const uintptr_t head=loadField<uintptr_t>(reinterpret_cast<void*>(queue),0);
    if (!head) return false;
    uintptr_t td=head;
    while (td) {
        if (tdCount==MaxTDs || !pointerShape(td,8)) return false;
        const uintptr_t array=loadField<uintptr_t>(reinterpret_cast<void*>(td),TDArray);
        const uint32_t capacity=loadField<uint32_t>(reinterpret_cast<void*>(td),TDCapacity);
        if (!pointerShape(array,16) || !capacity || capacity>MaximumCapacity) return false;
        bool linkSeen=false;
        for (uint32_t i=0;i<capacity;i++) {
            const auto *trb=reinterpret_cast<const void*>(array+uintptr_t(i)*16);
            const uint32_t control=loadField<uint32_t>(trb,12),type=(control>>10)&0x3f;
            if (!type) {
                if (loadField<uint64_t>(trb,0) || loadField<uint64_t>(trb,8)) return false;
                continue;
            }
            if ((type<1 || type>8) || linkSeen) return false;
            if (type==6) linkSeen=true;
        }
        if (!linkSeen) return false;
        ++tdCount;
        td=loadField<uintptr_t>(reinterpret_cast<void*>(td),TDNext);
    }
    // No callback/original driver code runs between validation and mutation.
    // The original enqueue has not yet published these new TDs to hardware.
    td=head;
    for (uint32_t segment=0;segment<tdCount;segment++) {
        const uintptr_t array=loadField<uintptr_t>(reinterpret_cast<void*>(td),TDArray);
        const uint32_t capacity=loadField<uint32_t>(reinterpret_cast<void*>(td),TDCapacity);
        for (uint32_t i=0;i<capacity;i++) {
            const uintptr_t trb=array+uintptr_t(i)*16;
            const uint32_t type=(loadField<uint32_t>(reinterpret_cast<void*>(trb),12)>>10)&0x3f;
            if (!type) continue;
            const uint32_t before=loadField<uint32_t>(reinterpret_cast<void*>(trb),8);
            const uint32_t after=before&~TargetMask;
            if (before!=after) {
                *reinterpret_cast<volatile uint32_t*>(trb+8)=after;
            }
        }
        td=loadField<uintptr_t>(reinterpret_cast<void*>(td),TDNext);
    }
    return true;
}
} }
