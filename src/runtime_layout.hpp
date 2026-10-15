#pragma once
#include <stddef.h>
#include <stdint.h>

// No build identifiers, UUIDs, reference addresses, registry names or vendors.
// This decoder recognizes a narrow, cross-checked implementation of the
// original driver's private ABI. Unknown machine code fails closed.
namespace GenericXhci {
struct FunctionView {
    const uint8_t *bytes;
    size_t size;
    uintptr_t address;
};
struct Inputs {
    FunctionView primaryFilter,ioFilter,isoFilter,filter,dispatch,manual,next;
    FunctionView initRegisters,initWithParameters,ringFree;
    uintptr_t expectedCAS,expectedAtomicOr;
};
struct Layout {
    size_t controllerRings[3] {}; // primary, I/O, isochronous
    size_t controllerAccessible {},controllerSource {},controllerFlags {};
    size_t filterSlots[3] {},actionSlots[3] {}; // same role order
    size_t controllerFilterSlot {},controllerActionSlot {},clearSlot {};
    size_t ringController {},ringIndex {},ringConsumer {},ringBuffer {};
    size_t ringDescriptor {},ringDMA {},ringBytesSlot {};
    uint32_t ringEntries {},ringBytes {},ringInitialConsumer {};
};
enum class Error : uint8_t {
    None, MissingInput, TinyFilter, IsoFilter, DispatcherFilter, DispatcherAction,
    NativeManual, EventConsumer, RingInitialization, RingAllocation, RingFree,
    CrossRelation, Bounds, AtomicTarget
};
inline const char *errorName(Error e) {
    switch(e) {
        case Error::None:return "none";
        case Error::MissingInput:return "missing-input";
        case Error::TinyFilter:return "tiny-filter";
        case Error::IsoFilter:return "iso-filter";
        case Error::DispatcherFilter:return "dispatcher-filter";
        case Error::DispatcherAction:return "dispatcher-action";
        case Error::NativeManual:return "native-manual";
        case Error::EventConsumer:return "event-consumer";
        case Error::RingInitialization:return "ring-initialization";
        case Error::RingAllocation:return "ring-allocation";
        case Error::RingFree:return "ring-free";
        case Error::CrossRelation:return "cross-relation";
        case Error::Bounds:return "bounds";
        case Error::AtomicTarget:return "atomic-target";
    }
    return "unknown";
}
namespace Detail {
struct Operand {size_t offset,size;};
inline uint32_t little(const uint8_t *p,size_t n) {
    uint32_t value=0;
    for(size_t i=0;i<n;++i)value|=uint32_t(p[i])<<(8*i);
    return value;
}
inline unsigned nibble(char c) {return c<='9'?unsigned(c-'0'):unsigned(c-'a'+10);}
inline size_t hexBytes(const char *s) {size_t n=0;while(s[n])++n;return n/2;}
inline bool shape(FunctionView f,const char *hex,const Operand *operands,size_t count) {
    const size_t length=hexBytes(hex);
    if(!f.bytes||f.size<length)return false;
    for(size_t i=0;i<length;++i) {
        bool operand=false;
        for(size_t j=0;j<count;++j) {
            if(operands[j].offset>length||operands[j].size>length-operands[j].offset)return false;
            if(i>=operands[j].offset&&i-operands[j].offset<operands[j].size)operand=true;
        }
        if(!operand&&f.bytes[i]!=uint8_t((nibble(hex[2*i])<<4)|nibble(hex[2*i+1])))return false;
    }
    return true;
}
template<size_t N> inline bool shape(FunctionView f,const char *hex,const Operand (&o)[N]) {return shape(f,hex,o,N);}
inline bool bounded(size_t v,size_t alignment) {return v>0&&v<=0x10000&&v%alignment==0;}
inline bool overlaps(size_t a,size_t aSize,size_t b,size_t bSize) {
    return a<b+bSize&&b<a+aSize;
}
inline bool relTarget(FunctionView f,size_t displacement,uintptr_t expected) {
    if(!f.address||!expected||!f.bytes||displacement>f.size||f.size-displacement<4||
       f.address>UINTPTR_MAX-displacement-4)return false;
    const uintptr_t end=f.address+displacement+4;
    const uint32_t raw=little(f.bytes+displacement,4);
    if(raw&0x80000000U) {
        const uintptr_t magnitude=uintptr_t(uint32_t(~raw)+1U);
        return end>=magnitude&&end-magnitude==expected;
    }
    return end<=UINTPTR_MAX-raw&&end+raw==expected;
}
inline bool failure(Error value,Error *error) {if(error)*error=value;return false;}
inline FunctionView subview(FunctionView f,size_t offset) {
    if(offset>f.size)return {nullptr,0,0};
    return {f.bytes+offset,f.size-offset,f.address<=UINTPTR_MAX-offset?f.address+offset:0};
}
inline bool uniqueFragment(FunctionView f,const char *hex,const Operand *operands,size_t count,size_t &found) {
    const size_t length=hexBytes(hex);
    if(!f.bytes||f.size<length)return false;
    bool present=false;
    for(size_t i=0;i<=f.size-length;++i)if(shape(subview(f,i),hex,operands,count)) {
        if(present)return false;
        found=i;present=true;
    }
    return present;
}
}

inline bool decode(const Inputs &in,Layout &result,Error *error=nullptr) {
    using namespace Detail;
    if(error)*error=Error::None;
    Layout out {};
    const FunctionView views[]={in.primaryFilter,in.ioFilter,in.isoFilter,in.filter,in.dispatch,in.manual,in.next,
        in.initRegisters,in.initWithParameters,in.ringFree};
    // Bound scans even if a caller provided a whole module instead of a function.
    for(const auto &v:views)if(!v.bytes||!v.address||!v.size||v.size>0x10000)
        return failure(Error::MissingInput,error);
    static const char tiny[]="554889e5488bbf180e0000488b07ff9060010000b0015dc3";
    const Operand tinyOps[]={{7,4},{16,4}};
    if(!shape(in.primaryFilter,tiny,tinyOps)||!shape(in.ioFilter,tiny,tinyOps))return failure(Error::TinyFilter,error);
    out.controllerRings[0]=little(in.primaryFilter.bytes+7,4);
    out.controllerRings[1]=little(in.ioFilter.bytes+7,4);
    out.clearSlot=little(in.primaryFilter.bytes+16,4);
    if(out.clearSlot!=little(in.ioFilter.bytes+16,4))return failure(Error::CrossRelation,error);

    static const char iso[]="554889e54157415641554154534881ecd80000004989f64989fc488b05ed070300488b00488945d0e8f9becefd89c1b00184c90f8571030000418b8c24b002000083f9010f8560030000498bbc24200e0000488b07ff9060010000498bbc24200e0000488b07ff9068010000";
    const Operand isoOps[]={{29,4},{41,4},{53,4},{61,4},{70,4},{78,4},{87,4},{95,4},{104,4}};
    if(!shape(in.isoFilter,iso,isoOps))return failure(Error::IsoFilter,error);
    out.controllerAccessible=little(in.isoFilter.bytes+61,4);
    out.controllerRings[2]=little(in.isoFilter.bytes+78,4);
    if(out.controllerRings[2]!=little(in.isoFilter.bytes+95,4)||out.clearSlot!=little(in.isoFilter.bytes+87,4)||
       out.clearSlot+8!=little(in.isoFilter.bytes+104,4))return failure(Error::CrossRelation,error);

    static const char filter[]="554889e54156534889f34989fee836cdcffd89c1b00184c97551418b8eb002000083f9017545498b064c89f74889deff90a00b0000498b064c89f74889deff90b00b0000498b064c89f74889deff90c00b0000e800f575fd83f801750c488b034889dfff903802000031c05b415e5dc3";
    const Operand filterOps[]={{14,4},{29,4},{49,4},{64,4},{79,4},{84,4},{101,4}};
    if(!shape(in.filter,filter,filterOps))return failure(Error::DispatcherFilter,error);
    if(out.controllerAccessible!=little(in.filter.bytes+29,4))return failure(Error::CrossRelation,error);
    // The native filter visits iso, I/O, primary; expose arrays in role order.
    out.filterSlots[2]=little(in.filter.bytes+49,4);
    out.filterSlots[1]=little(in.filter.bytes+64,4);
    out.filterSlots[0]=little(in.filter.bytes+79,4);

    static const char dispatch[]="554889e5415741564154534189d64989f74889fbe8bfcccffd84c0740a4885db7551e9210100008b83b002000083f8017541488b034889df4c89fe4489f2ff90a80b0000488b034889df4c89fe4489f2ff90b80b0000488b03488b80c80b00004889df4c89fe4489f25b415c415e415f5dffe0";
    const Operand dispatchOps[]={{21,4},{35,4},{41,4},{64,4},{82,4},{92,4}};
    if(!shape(in.dispatch,dispatch,dispatchOps))return failure(Error::DispatcherAction,error);
    if(out.controllerAccessible!=little(in.dispatch.bytes+41,4))return failure(Error::CrossRelation,error);
    out.actionSlots[2]=little(in.dispatch.bytes+64,4);
    out.actionSlots[1]=little(in.dispatch.bytes+82,4);
    out.actionSlots[0]=little(in.dispatch.bytes+92,4);
    for(size_t i=0;i<3;++i)if(out.actionSlots[i]!=out.filterSlots[i]+8)return failure(Error::CrossRelation,error);

    static const char manual[]="554889e553504889fb488db7700e0000bf01000000e8b2f5c6fd488bb3280e0000488b034889dfff90d00a0000488bb3280e0000488b03488b80d80a00004889df31d24883c4085b5dffe0";
    const Operand manualOps[]={{12,4},{22,4},{29,4},{41,4},{48,4},{58,4}};
    if(!shape(in.manual,manual,manualOps))return failure(Error::NativeManual,error);
    out.controllerFlags=little(in.manual.bytes+12,4);out.controllerSource=little(in.manual.bytes+29,4);
    out.controllerFilterSlot=little(in.manual.bytes+41,4);out.controllerActionSlot=little(in.manual.bytes+58,4);
    if(out.controllerSource!=little(in.manual.bytes+48,4)||out.controllerActionSlot!=out.controllerFilterSlot+8)
        return failure(Error::CrossRelation,error);

    static const char next[]="554889e553504889f88b7f6089f981e1ffffff7f89fec1ee1f488b583048c1e1048b540b0c83e20139f275304883c0604801cb8d4f0131d281e1ff0300000f94c231d6c1e61f09ce4889c2e81483c6fd89c131c084c9480f45c3eb27488b7810488bb7b00500008b500cc1e20583c238480348284883c908488b07ff90800c000031c04883c4085b5dc3";
    const Operand nextOps[]={{11,1},{28,1},{47,1},{58,4},{76,4},{95,1},{99,4},{105,1},{115,1},{125,4}};
    if(!shape(in.next,next,nextOps))return failure(Error::EventConsumer,error);
    out.ringConsumer=little(in.next.bytes+11,1);out.ringBuffer=little(in.next.bytes+28,1);
    out.ringController=little(in.next.bytes+95,1);out.ringIndex=little(in.next.bytes+105,1);out.ringDMA=little(in.next.bytes+115,1);
    const uint32_t mask=little(in.next.bytes+58,4);
    if(out.ringConsumer!=little(in.next.bytes+47,1)||mask==UINT32_MAX)return failure(Error::CrossRelation,error);
    out.ringEntries=mask+1;
    if(out.ringEntries<16||out.ringEntries>4096||(out.ringEntries&(out.ringEntries-1)))return failure(Error::Bounds,error);
    out.ringBytes=out.ringEntries*16;out.ringInitialConsumer=0x80000000U;
    if(!relTarget(in.next,76,in.expectedCAS)||!relTarget(in.manual,22,in.expectedAtomicOr))return failure(Error::AtomicTarget,error);

    // Match the initialization block independently of its position in the
    // function. It zeroes the descriptor's bytes, stores its virtual buffer,
    // and resets the same index/cycle word the consumer reads.
    static const char init[]="488b7b20488b07ff90e0020000be004000004889c7e8112b32fd488b7b20488b07ff90e002000048894330c74360000000808b530c488b7b10488bb7b0050000";
    const Operand initOps[]={{3,1},{9,4},{14,4},{22,4},{29,1},{35,4},{42,1},{45,1},{52,1},{56,1},{60,4}};
    size_t initOffset=0;
    if(!uniqueFragment(in.initRegisters,init,initOps,sizeof(initOps)/sizeof(*initOps),initOffset))return failure(Error::RingInitialization,error);
    const uint8_t *ip=in.initRegisters.bytes+initOffset;
    out.ringDescriptor=little(ip+3,1);out.ringBytesSlot=little(ip+9,4);
    if(out.ringDescriptor!=little(ip+29,1)||out.ringBytesSlot!=little(ip+35,4)||out.ringBytes!=little(ip+14,4)||
       out.ringBuffer!=little(ip+42,1)||out.ringConsumer!=little(ip+45,1)||out.ringIndex!=little(ip+52,1)||
       out.ringController!=little(ip+56,1)||little(in.next.bytes+99,4)!=little(ip+60,4))return failure(Error::CrossRelation,error);

    // The ERST entry must describe exactly the same capacity and DMA address.
    // Otherwise a changed consumer mask could look compatible while the
    // controller still traversed a different number of Event TRBs.
    static const char erst[]="488b7b18488b07ff90e0020000488b4b28488908c7400800040000c7400c00000000";
    const Operand erstOps[]={{3,1},{9,4},{16,1},{23,4}};
    size_t erstOffset=0;
    if(!uniqueFragment(in.initRegisters,erst,erstOps,sizeof(erstOps)/sizeof(*erstOps),erstOffset)||
       erstOffset+hexBytes(erst)!=initOffset)return failure(Error::RingInitialization,error);
    const uint8_t *ep=in.initRegisters.bytes+erstOffset;
    const size_t ringERSTDescriptor=little(ep+3,1);
    if(little(ep+9,4)!=out.ringBytesSlot||little(ep+16,1)!=out.ringDMA||little(ep+23,4)!=out.ringEntries)
        return failure(Error::CrossRelation,error);

    static const char allocation[]="ba00400000b900400000488b7dd04489ee4d89f1e8d3e6f1ff488943204885c074204889c7e8204ad3fd48894328488b034889dfff9018010000";
    const Operand allocationOps[]={{1,4},{6,4},{21,4},{28,1},{38,4},{45,1},{54,4}};
    size_t allocationOffset=0;
    if(!uniqueFragment(in.initWithParameters,allocation,allocationOps,sizeof(allocationOps)/sizeof(*allocationOps),allocationOffset))return failure(Error::RingAllocation,error);
    const uint8_t *ap=in.initWithParameters.bytes+allocationOffset;
    if(little(ap+1,4)!=out.ringBytes||little(ap+6,4)!=out.ringBytes||little(ap+28,1)!=out.ringDescriptor||little(ap+45,1)!=out.ringDMA)
        return failure(Error::CrossRelation,error);
    static const char parameters[]="554889e54157415641554154535089570c4885f60f84fd0000004989ce4989f44889fb48897710";
    const Operand parametersOps[]={{16,1},{22,4},{38,1}};
    if(!shape(in.initWithParameters,parameters,parametersOps))return failure(Error::RingAllocation,error);
    if(little(in.initWithParameters.bytes+16,1)!=out.ringIndex||little(in.initWithParameters.bytes+38,1)!=out.ringController)
        return failure(Error::CrossRelation,error);

    static const char free[]="554889e553504889fb488b7f104885ff7406488b07ff502848c7431000000000488b7b184885ff7406488b07ff502831c04889431848894328488b7b204885ff7406488b07ff502848c7432000000000";
    const Operand freeOps[]={{12,1},{27,1},{35,1},{52,1},{56,1},{60,1},{75,1}};
    if(!shape(in.ringFree,free,freeOps))return failure(Error::RingFree,error);
    const uint8_t *fp=in.ringFree.bytes;
    if(little(fp+12,1)!=out.ringController||little(fp+27,1)!=out.ringController||little(fp+56,1)!=out.ringDMA||
       little(fp+60,1)!=out.ringDescriptor||little(fp+75,1)!=out.ringDescriptor||little(fp+35,1)!=little(fp+52,1)||
       little(fp+35,1)!=ringERSTDescriptor)
        return failure(Error::CrossRelation,error);

    const size_t wordFields[]={out.controllerAccessible,out.controllerFlags,out.ringIndex,out.ringConsumer};
    for(size_t value:wordFields)if(!bounded(value,4))return failure(Error::Bounds,error);
    const size_t pointerFields[]={out.controllerSource,out.controllerFilterSlot,out.controllerActionSlot,
        out.clearSlot,out.ringController,out.ringBuffer,out.ringDescriptor,out.ringDMA,out.ringBytesSlot};
    for(size_t value:pointerFields)if(!bounded(value,8))return failure(Error::Bounds,error);
    for(size_t i=0;i<3;++i) {
        if(!bounded(out.controllerRings[i],8)||!bounded(out.filterSlots[i],8)||!bounded(out.actionSlots[i],8))return failure(Error::Bounds,error);
        for(size_t j=0;j<i;++j)if(out.controllerRings[i]==out.controllerRings[j]||out.filterSlots[i]==out.filterSlots[j])
            return failure(Error::CrossRelation,error);
    }
    // Every extracted ring member uses signed disp8 addressing. A value >=128
    // would address memory before the object, not a larger positive member.
    const size_t ringFields[]={out.ringController,out.ringBuffer,out.ringDescriptor,out.ringDMA,out.ringConsumer,out.ringIndex,ringERSTDescriptor};
    const size_t ringFieldSizes[]={8,8,8,8,4,4,8};
    for(size_t i=0;i<sizeof(ringFields)/sizeof(*ringFields);++i)
        if(ringFields[i]>=0x80||!bounded(ringFields[i],ringFieldSizes[i]))return failure(Error::Bounds,error);
    for(size_t i=0;i<sizeof(ringFields)/sizeof(*ringFields);++i)
        for(size_t j=0;j<i;++j)if(overlaps(ringFields[i],ringFieldSizes[i],ringFields[j],ringFieldSizes[j]))
            return failure(Error::CrossRelation,error);
    const size_t controllerFields[]={out.controllerRings[0],out.controllerRings[1],out.controllerRings[2],
        out.controllerSource,out.controllerAccessible,out.controllerFlags};
    const size_t controllerFieldSizes[]={8,8,8,8,4,4};
    for(size_t i=0;i<sizeof(controllerFields)/sizeof(*controllerFields);++i)
        for(size_t j=0;j<i;++j)if(overlaps(controllerFields[i],controllerFieldSizes[i],controllerFields[j],controllerFieldSizes[j]))
            return failure(Error::CrossRelation,error);
    const size_t controllerSlots[]={out.filterSlots[0],out.filterSlots[1],out.filterSlots[2],
        out.actionSlots[0],out.actionSlots[1],out.actionSlots[2],out.controllerFilterSlot,out.controllerActionSlot};
    for(size_t i=0;i<sizeof(controllerSlots)/sizeof(*controllerSlots);++i)
        for(size_t j=0;j<i;++j)if(controllerSlots[i]==controllerSlots[j])return failure(Error::CrossRelation,error);
    result=out;return true;
}
} // namespace GenericXhci
