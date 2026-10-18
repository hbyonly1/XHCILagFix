#include "fix.hpp"
#include "route0.hpp"
#include "symbols.hpp"
namespace XhciLagFix {
namespace {
mach_vm_address_t originals[3];
Status state{false,"waiting for AppleUSBXHCI"};
void route(void *request) {
    if (__atomic_load_n(&state.active,__ATOMIC_ACQUIRE))
        AllRoute0::rewritePreparedQueue(request);
}
int32_t wrappedEnqueue(void *ring,void *request) {
    route(request);
    return reinterpret_cast<int32_t(*)(void*,void*)>(originals[0])(ring,request);
}
int32_t enqueueIso(void *ring,void *request) {
    route(request);
    return reinterpret_cast<int32_t(*)(void*,void*)>(originals[1])(ring,request);
}
int32_t setAddress(void *ring,uint32_t slot,void *context,void *endpoint,bool block) {
    alignas(16) uint32_t copy[8];
    const bool rewrite=__atomic_load_n(&state.active,__ATOMIC_ACQUIRE) && slot && slot<=255 && endpoint &&
        AllRoute0::pointerShape(reinterpret_cast<uintptr_t>(ring),8) &&
        AllRoute0::pointerShape(reinterpret_cast<uintptr_t>(context),4);
    if (rewrite) {
        for (unsigned i=0;i<8;i++) copy[i]=loadField<uint32_t>(context,i*4);
        copy[2]&=~AllRoute0::TargetMask;
    }
    // Native setAddress synchronously copies source into its own DMA input.
    const int32_t result=reinterpret_cast<int32_t(*)(void*,uint32_t,void*,void*,bool)>(originals[2])
        (ring,slot,rewrite ? copy:context,endpoint,block);
    // Preserve the native source-DW3 sanitization, leaving source Target intact.
    if (rewrite && loadField<uint32_t>(context,12)!=copy[3])
        *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(context)+12)=copy[3];
    return result;
}
}
Status status() { return {__atomic_load_n(&state.active,__ATOMIC_ACQUIRE),state.failure}; }
void install(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size) {
    for (unsigned i=0;i<3;i++) {
        const auto found=patcher.solveSymbol(index,Symbols[i]);
        if (!found || found<address || size<14 || found-address>size-14) {
            state.failure=Symbols[i];patcher.clearError();return;
        }
    }
    KernelPatcher::RouteRequest routes[]={
        {Symbols[0],reinterpret_cast<mach_vm_address_t>(wrappedEnqueue),originals[0]},
        {Symbols[1],reinterpret_cast<mach_vm_address_t>(enqueueIso),originals[1]},
        {Symbols[2],reinterpret_cast<mach_vm_address_t>(setAddress),originals[2]}
    };
    const bool routed=patcher.routeMultiple(index,routes,3,0,0,true,false);
    const bool success=routed && originals[0] && originals[1] && originals[2];
    state.failure=success ? "":"detour installation";
    patcher.clearError();__atomic_store_n(&state.active,success,__ATOMIC_RELEASE);
}
}
