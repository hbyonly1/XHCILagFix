#include "fix.hpp"
#include "route0.hpp"
#include "symbols.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace XhciLagFix;
struct TRB {uint32_t word[4];};
namespace R=XhciLagFix::AllRoute0;
alignas(32) static uint8_t image[3][64];
static mach_vm_address_t wrappers[3];
static const char *scenario;
static unsigned routeCalls,originalCalls;
static bool expectRewrite;
struct TD {uintptr_t array;uint32_t capacity,reserved;uint64_t dma;uintptr_t next;};
static_assert(offsetof(TD,next)==R::TDNext,"TD ABI");
struct Fixture {
    alignas(16) uint8_t request[0x100]{};
    alignas(16) TRB trbs[16]{};
    TD td{reinterpret_cast<uintptr_t>(trbs),16,0,0,0};
    uintptr_t queue=reinterpret_cast<uintptr_t>(&td);
    Fixture() {
        const uintptr_t queuePointer=reinterpret_cast<uintptr_t>(&queue);
        std::memcpy(request+R::RequestQueue,&queuePointer,sizeof queuePointer);
        const uint32_t types[]={2,3,4,5,1,7,8,6};
        for (unsigned i=0;i<8;i++) {trbs[i].word[2]=(2U<<22)|71;trbs[i].word[3]=(types[i]<<10)|33;}
    }
};
static int32_t originalEnqueue(void *ring,void *request) {
    assert(ring==reinterpret_cast<void*>(0x1234));originalCalls++;
    if (request) {
        const auto q=loadField<uintptr_t>(request,R::RequestQueue);
        const auto td=loadField<uintptr_t>(reinterpret_cast<void*>(q),0);
        const auto array=loadField<uintptr_t>(reinterpret_cast<void*>(td),0);
        for (unsigned i=0;i<8;i++) assert(loadField<uint32_t>(reinterpret_cast<void*>(array+i*16),8)==(expectRewrite ? 71:((2U<<22)|71)));
    }
    return -17;
}
static void *sourceContext;
static int32_t originalAddress(void *ring,uint32_t slot,void *context,void *endpoint,bool block) {
    assert(ring==reinterpret_cast<void*>(0x1230) && endpoint==reinterpret_cast<void*>(0x1240) && block && slot==3);originalCalls++;
    assert((context!=sourceContext)==expectRewrite);
    auto *words=static_cast<uint32_t*>(context);
    assert(words[2]==(expectRewrite ? 19:((1U<<22)|19)));
    for (unsigned i=0;i<8;i++) if (i!=2) assert(words[i]==static_cast<uint32_t*>(sourceContext)[i]);
    words[3]&=0x07ffff00;return -29;
}
mach_vm_address_t KernelPatcher::solveSymbol(size_t index,const char *symbol) {
    assert(index==5);
    for (unsigned i=0;i<3;i++) if (!std::strcmp(symbol,Symbols[i])) {
        if (!std::strcmp(scenario,"missing") && i==1) return 0;
        if (!std::strcmp(scenario,"bounds") && i==2) return reinterpret_cast<uintptr_t>(image)+sizeof(image)-8;
        return reinterpret_cast<uintptr_t>(image[i]);
    }
    assert(false);return 0;
}
bool KernelPatcher::routeMultiple(size_t index,RouteRequest *requests,size_t count,mach_vm_address_t start,size_t size,bool kernel,bool force) {
    assert(index==5 && count==3 && !start && !size && kernel && !force);routeCalls++;
    for (unsigned i=0;i<3;i++) {
        assert(!std::strcmp(requests[i].symbol,Symbols[i]));wrappers[i]=requests[i].to;
        *requests[i].org=i==2 ? reinterpret_cast<uintptr_t>(originalAddress):reinterpret_cast<uintptr_t>(originalEnqueue);
        if (!std::strcmp(scenario,"partial") && i==0) return false;
    }
    return true;
}
int main(int argc,char **argv) {
    assert(argc==2);scenario=argv[1];KernelPatcher patcher;
    // Arbitrary non-reference bytes must not block symbol-based installation.
    std::memset(image,0x90,sizeof(image));install(patcher,5,reinterpret_cast<uintptr_t>(image),sizeof(image));
    const bool success=!std::strcmp(scenario,"success");auto s=status();assert(s.active==success);
    if (!std::strcmp(scenario,"missing") || !std::strcmp(scenario,"bounds")) {assert(!routeCalls && s.failure[0]);}
    else if (!std::strcmp(scenario,"partial")) {
        assert(routeCalls==1 && !s.active && s.failure[0]);
        expectRewrite=false;Fixture f;assert(reinterpret_cast<int32_t(*)(void*,void*)>(wrappers[0])(reinterpret_cast<void*>(0x1234),f.request)==-17);
    } else {
        assert(success && s.active && !s.failure[0]);
        for (unsigned i=0;i<2;i++) {
            Fixture f;expectRewrite=true;
            assert(reinterpret_cast<int32_t(*)(void*,void*)>(wrappers[i])(reinterpret_cast<void*>(0x1234),f.request)==-17);
            for (unsigned j=0;j<8;j++) assert(f.trbs[j].word[3]&1);
        }
        Fixture published;uintptr_t previous=1;std::memcpy(published.request+R::RequestPreviousTD,&previous,sizeof previous);expectRewrite=false;
        assert(reinterpret_cast<int32_t(*)(void*,void*)>(wrappers[0])(reinterpret_cast<void*>(0x1234),published.request)==-17);
        assert(reinterpret_cast<int32_t(*)(void*,void*)>(wrappers[0])(reinterpret_cast<void*>(0x1234),nullptr)==-17);
        alignas(16) uint32_t context[8]={1,2,(1U<<22)|19,0xffffffff,5,6,7,8};sourceContext=context;expectRewrite=true;
        assert(reinterpret_cast<int32_t(*)(void*,uint32_t,void*,void*,bool)>(wrappers[2])(reinterpret_cast<void*>(0x1230),3,context,reinterpret_cast<void*>(0x1240),true)==-29);
        assert(context[2]==((1U<<22)|19) && context[3]==0x07ffff00);
        assert(originalCalls==5);
    }
    std::printf("PASS standalone %s: actual fix.cpp, mocked detours/originals, no kernel writes\n",scenario);
}
