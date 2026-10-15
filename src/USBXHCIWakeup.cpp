// Experimental x86_64 AppleUSBXHCI watchdog. No machine/build allow-list.
#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/OSAtomic.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <i386/machine_routines.h>
#include <mach-o/loader.h>
#include <mach/kmod.h>
#include <string.h>
extern "C" {
#include <hde64.h>
unsigned int xhciWakeDisasm(const void *,hde64s *);
}
#include "runtime_layout.hpp"
#include "wakeup_policy.hpp"
#include "wake_reservation.hpp"
#include "controller_policy.hpp"
#include "hook_span.hpp"

namespace Wake {
using namespace USBXHCIWakeup;
constexpr uint32_t PollUs=100, StallUs=200;
constexpr unsigned Roles=3;
enum Hook : unsigned { Consume,Lower,Raise,Reset,Stop,Free,Init,Discard,RingFree,
    PCILower,PCIRaise,PCIStop,PCIFree,HookCount };
static const char *symbols[HookCount]={
    "__ZN23AppleUSBXHCIInterrupter20getNextEventInternalEv",
    "__ZN12AppleUSBXHCI20lowerOnePowerStateToEm","__ZN12AppleUSBXHCI20raiseOnePowerStateToEm",
    "__ZN12AppleUSBXHCI5resetEv","__ZN12AppleUSBXHCI19stopThreadCallGatedEv","__ZN12AppleUSBXHCI4freeEv",
    "__ZN23AppleUSBXHCIInterrupter13initRegistersEv","__ZN23AppleUSBXHCIInterrupter16discardRegistersEv",
    "__ZN23AppleUSBXHCIInterrupter4freeEv",
    "__ZN15AppleUSBXHCIPCI20lowerOnePowerStateToEm","__ZN15AppleUSBXHCIPCI20raiseOnePowerStateToEm",
    "__ZN15AppleUSBXHCIPCI19stopThreadCallGatedEv","__ZN15AppleUSBXHCIPCI4freeEv"
};
static uintptr_t targets[HookCount] {},slots[HookCount] {};
static mach_vm_address_t originals[HookCount] {};
struct Range {uintptr_t base {};size_t bytes {};bool code {},constant {};};
struct Image {
    Range ranges[64] {};unsigned count {};bool valid {};
    bool contains(uintptr_t p,size_t bytes,bool code=false) const {
        if(!bytes)return false;
        for(unsigned i=0;i<count;++i)if((code?ranges[i].code:ranges[i].constant)&&p>=ranges[i].base&&
            bytes<=ranges[i].bytes&&p-ranges[i].base<=ranges[i].bytes-bytes)return true;
        return false;
    }
    size_t available(uintptr_t p,bool code) const {
        for(unsigned i=0;i<count;++i)if((code?ranges[i].code:ranges[i].constant)&&p>=ranges[i].base&&p-ranges[i].base<ranges[i].bytes)
            return ranges[i].bytes-(p-ranges[i].base);
        return 0;
    }
    bool parse(uintptr_t address,size_t availableBytes) {
        if(availableBytes<sizeof(mach_header_64))return false;
        const auto *h=reinterpret_cast<const mach_header_64*>(address);
        if(h->magic!=MH_MAGIC_64||h->cputype!=CPU_TYPE_X86_64||h->sizeofcmds>65536||
            h->sizeofcmds>availableBytes-sizeof(*h)||h->ncmds>h->sizeofcmds/8)return false;
        uintptr_t unslid=0;
        const auto *start=reinterpret_cast<const uint8_t*>(h+1);
        size_t used=0;
        for(uint32_t i=0;i<h->ncmds;++i) {
            if(used>h->sizeofcmds||h->sizeofcmds-used<8)return false;
            const auto *c=reinterpret_cast<const load_command*>(start+used);
            if(c->cmdsize<8||c->cmdsize>h->sizeofcmds-used)return false;
            if(c->cmd==LC_SEGMENT_64) {
                if(c->cmdsize<sizeof(segment_command_64))return false;
                const auto *s=reinterpret_cast<const segment_command_64*>(c);
                if(s->nsects>(c->cmdsize-sizeof(*s))/sizeof(section_64))return false;
                if(!strncmp(s->segname,"__TEXT",16))unslid=s->vmaddr;
            }
            used+=c->cmdsize;
        }
        if(!unslid||used!=h->sizeofcmds||address<unslid)return false;
        const uintptr_t slide=address-unslid;used=0;
        for(uint32_t i=0;i<h->ncmds;++i) {
            const auto *c=reinterpret_cast<const load_command*>(start+used);
            if(c->cmd==LC_SEGMENT_64) {
                const auto *s=reinterpret_cast<const segment_command_64*>(c);
                if(s->vmsize>0x8000000||s->vmaddr>UINTPTR_MAX-slide||
                    s->vmaddr+slide>UINTPTR_MAX-s->vmsize)return false;
                const auto *sections=reinterpret_cast<const section_64*>(s+1);
                for(unsigned j=0;j<s->nsects;++j) {
                    const auto &q=sections[j];
                    if(q.addr<s->vmaddr||q.size>s->vmsize||q.addr-s->vmaddr>s->vmsize-q.size)return false;
                    const bool code=(s->initprot&VM_PROT_EXECUTE)&&!strncmp(q.sectname,"__text",16);
                    const bool constant=(s->initprot&VM_PROT_READ)&&!strncmp(q.sectname,"__const",16)&&
                        (!strncmp(s->segname,"__DATA",16)||!strncmp(s->segname,"__DATA_CONST",16));
                    if((code||constant)&&q.size) {
                        if(count==64)return false;
                        ranges[count++]={uintptr_t(q.addr+slide),size_t(q.size),code,constant};
                    }
                }
            }
            used+=c->cmdsize;
        }
        valid=count>0;return valid;
    }
};
static Image images[2];
static size_t moduleIDs[2] {};
static bool seen[2] {},runtimeReady {},attempted {},registrationOK {},running {},defaultEnabled=true;
static bool isReady() {return __atomic_load_n(&runtimeReady,__ATOMIC_ACQUIRE);}
static bool isRunning() {return __atomic_load_n(&running,__ATOMIC_ACQUIRE);}
static void setRunning(bool value) {__atomic_store_n(&running,value,__ATOMIC_RELEASE);}
static uint64_t routedMask {};
static const char *runtimeError="waiting for AppleUSBXHCI";
static void errorStatus(const char *message) {__atomic_store_n(&runtimeError,message,__ATOMIC_RELEASE);}
static GenericXhci::Layout layout {};
static uintptr_t manual {},filter {},dispatch {},primaryFilters[Roles] {},primaryActions[Roles] {};
static uintptr_t manualSlot {};
static IOLock *managerLock {},*controlLock {};
static IOService *status {};
static IOWorkLoop *managementLoop {};
static IOTimerEventSource *managementTimer {};
static bool managementAdded {};
static IONotifier *publishNotifier {};
static uint64_t nowUs() {
    uint64_t tick=0,ns=0;clock_get_uptime(&tick);absolutetime_to_nanoseconds(tick,&ns);return ns/1000;
}
template<class T> static T field(const void *object,size_t offset) {
    return *reinterpret_cast<const volatile T*>(reinterpret_cast<uintptr_t>(object)+offset);
}
static bool plausible(uintptr_t p) {return p>=uintptr_t(0xffffff0000000000ULL)&&!(p&7);}
static bool objectFits(OSObject *o,size_t offset,size_t bytes) {
    return o&&o->getMetaClass()&&offset<=o->getMetaClass()->getClassSize()&&
        bytes<=o->getMetaClass()->getClassSize()-offset;
}
static bool readable(uintptr_t p,size_t n) {
    return images[0].contains(p,n)||images[1].contains(p,n);
}
static bool tableTarget(OSObject *o,size_t slot,uintptr_t expected,uintptr_t alternative=0) {
    const auto vt=field<uintptr_t>(o,0);
    if(!plausible(vt)||slot>0x10000||!readable(vt,slot+8))return false;
    const auto target=field<uintptr_t>(reinterpret_cast<void*>(vt),slot);
    return target==expected||(alternative&&target==alternative);
}
struct Ring {
    OSObject *object {};
    IOBufferMemoryDescriptor *descriptor {};
    IOMemoryMap *map {};
    uintptr_t driverBuffer {},sampleBuffer {};
    uint32_t id {};
    // Module-lifetime counters; never reset at rebind. No locks/allocations in IRQ.
    uintptr_t token {};
    uint64_t activity {},progress {};
    uint32_t readers {};
    Policy policy;
};
struct Context {
    Context *next {};
    IOService *service {};
    IOWorkLoop *loop {};
    IOTimerEventSource *timer {};
    uint64_t registryID {};
    uintptr_t objectToken {};
    ControllerPolicy policy;
    Ring rings[Roles];
    WakeReservation reservation;
    bool dirty {true},retiring {},retired {};
    uint64_t bindingEpoch {},polls {},wakeCalls {},lastPollUs {},intervalTotalUs {},minIntervalUs {},maxIntervalUs {};
    uint64_t maxCallbackUs {},lastWakeUs {},rebinds {};
    uint32_t bindFailure {};
    const char *bindingStatus {"waiting for controller workloop"};
    char name[128] {},className[128] {};
};
static Context *head {};
struct Mutation {
    Mutation *next {};uintptr_t controller {};bool terminal {};
};
static Mutation *mutations {};
static Context *find(uint64_t id) {
    for(auto *c=__atomic_load_n(&head,__ATOMIC_ACQUIRE);c;c=c->next)if(c->registryID==id)return c;
    return nullptr;
}
static void invalidate(Context &c) {
    c.dirty=true;
    for(auto &r:c.rings)r.policy.cancel();
}
static void beginMutation(Mutation &m,void *object,bool ring,bool terminal=false) {
    m.controller=reinterpret_cast<uintptr_t>(object);m.terminal=terminal;
    // Ring ABI has already been decoded before any hook is installed. This
    // object is live at original entry. The after marker never dereferences it.
    if(ring&&object)m.controller=field<uintptr_t>(object,layout.ringController);
    IOLockLock(managerLock);
    m.next=mutations;mutations=&m;
    for(auto *c=head;c;c=c->next)if(c->objectToken==m.controller&&!c->retired) {
        c->policy.beginMutation();invalidate(*c);
        if(terminal)c->policy.terminated();
        const auto caller=reinterpret_cast<uintptr_t>(IOThreadSelf());
        while(c->reservation.mustDrainAll(caller))
            IOLockSleep(managerLock,&c->reservation,THREAD_UNINT);
    }
    IOLockUnlock(managerLock);
}
static void endMutation(Mutation &m) {
    IOLockLock(managerLock);
    Mutation **p=&mutations;while(*p&&*p!=&m)p=&(*p)->next;
    if(*p)*p=m.next;
    for(auto *c=head;c;c=c->next)if(c->objectToken==m.controller&&!c->retired)c->policy.endMutation(nowUs());
    IOLockUnlock(managerLock);
}
static Ring *consumerBegin(void *object) {
    const auto token=reinterpret_cast<uintptr_t>(object);
    for(auto *c=__atomic_load_n(&head,__ATOMIC_ACQUIRE);c;c=c->next)for(auto &r:c->rings)
        if(__atomic_load_n(&r.token,__ATOMIC_ACQUIRE)==token) {
            __atomic_add_fetch(&r.readers,1U,__ATOMIC_ACQ_REL);
            __atomic_add_fetch(&r.activity,uint64_t(1),__ATOMIC_ACQ_REL);return &r;
        }
    return nullptr;
}
static void *consume(void *object) {
    auto *r=consumerBegin(object);
    void *result=reinterpret_cast<void*(*)(void*)>(originals[Consume])(object);
    if(r) {
        if(result)__atomic_add_fetch(&r->progress,uint64_t(1),__ATOMIC_ACQ_REL);
        __atomic_add_fetch(&r->activity,uint64_t(1),__ATOMIC_ACQ_REL);
        __atomic_sub_fetch(&r->readers,1U,__ATOMIC_RELEASE);
    }
    return result;
}
#define POWER_WRAPPER(name,id) \
static int32_t name(void *o,uint64_t state) {Mutation m;beginMutation(m,o,false); \
    const auto result=reinterpret_cast<int32_t(*)(void*,uint64_t)>(originals[id])(o,state);endMutation(m);return result;}
POWER_WRAPPER(lower,Lower) POWER_WRAPPER(raise,Raise)
POWER_WRAPPER(pciLower,PCILower) POWER_WRAPPER(pciRaise,PCIRaise)
#undef POWER_WRAPPER
#define SIMPLE_WRAPPER(name,id,ring) \
static int32_t name(void *o) {Mutation m;beginMutation(m,o,ring); \
    const auto result=reinterpret_cast<int32_t(*)(void*)>(originals[id])(o);endMutation(m);return result;}
SIMPLE_WRAPPER(reset,Reset,false) SIMPLE_WRAPPER(stop,Stop,false)
SIMPLE_WRAPPER(init,Init,true) SIMPLE_WRAPPER(discard,Discard,true)
SIMPLE_WRAPPER(pciStop,PCIStop,false)
#undef SIMPLE_WRAPPER
#define FREE_WRAPPER(name,id,ring) \
static void name(void *o) {Mutation m;beginMutation(m,o,ring,!ring); \
    reinterpret_cast<void(*)(void*)>(originals[id])(o);endMutation(m);}
FREE_WRAPPER(freeController,Free,false) FREE_WRAPPER(freeRing,RingFree,true)
FREE_WRAPPER(pciFree,PCIFree,false)
#undef FREE_WRAPPER
static bool controllerRoutesValid(Context &c) {
    auto reject=[&](const char *reason)->bool {c.bindingStatus=reason;return false;};
    if(!tableTarget(c.service,manualSlot,manual)||
        !tableTarget(c.service,layout.controllerFilterSlot,filter)||
        !tableTarget(c.service,layout.controllerActionSlot,dispatch))return reject("unsupported native dispatch override");
    const Hook base[]={Lower,Raise,Reset,Stop,Free};
    const Hook alt[]={PCILower,PCIRaise,Reset,PCIStop,PCIFree};
    for(unsigned i=0;i<5;++i)if(!tableTarget(c.service,slots[base[i]],targets[base[i]],targets[alt[i]]))
        return reject("uncovered controller lifecycle override");
    for(unsigned i=0;i<Roles;++i)if(!tableTarget(c.service,layout.filterSlots[i],primaryFilters[i])||
        !tableTarget(c.service,layout.actionSlots[i],primaryActions[i]))return reject("unsupported role filter/action override");
    return true;
}
static bool controllerValid(Context &c) {
    auto reject=[&](const char *reason)->bool {c.bindingStatus=reason;return false;};
    if(!c.service||!c.loop||c.service->isInactive()||c.service->getWorkLoop()!=c.loop||!c.loop->inGate())
        return reject("controller inactive or workloop changed");
    if(!controllerRoutesValid(c))return false;
    const size_t fields[]={layout.controllerSource,
        layout.controllerRings[0],layout.controllerRings[1],layout.controllerRings[2]};
    for(size_t off:fields)if(!objectFits(c.service,off,8))return reject("controller field outside class size");
    if(!objectFits(c.service,layout.controllerAccessible,4)||!objectFits(c.service,layout.controllerFlags,4))
        return reject("controller state outside class size");
    if(field<uint32_t>(c.service,layout.controllerAccessible)!=1)return reject("controller registers not accessible");
    const auto sourceToken=field<uintptr_t>(c.service,layout.controllerSource);
    if(!plausible(sourceToken))return reject("interrupt source not ready");
    auto *source=OSDynamicCast(IOFilterInterruptEventSource,reinterpret_cast<OSObject*>(sourceToken));
    const bool valid=source&&source->getWorkLoop()==c.loop&&source->isEnabled()&&
        reinterpret_cast<uintptr_t>(source->getAction())==dispatch&&
        reinterpret_cast<uintptr_t>(source->getFilterAction())==filter;
    return valid||reject("interrupt source disabled or dispatch relationship changed");
}
static bool ringValid(Context &c,Ring &r,unsigned role) {
    if(!r.object||field<uintptr_t>(c.service,layout.controllerRings[role])!=reinterpret_cast<uintptr_t>(r.object)||
        field<uintptr_t>(r.object,layout.ringController)!=c.objectToken||
        field<uint32_t>(r.object,layout.ringIndex)!=r.id||
        field<uintptr_t>(r.object,layout.ringBuffer)!=r.driverBuffer||
        field<uintptr_t>(r.object,layout.ringDescriptor)!=reinterpret_cast<uintptr_t>(r.descriptor))return false;
    return r.map&&r.map->getLength()==layout.ringBytes&&r.descriptor->getLength()==layout.ringBytes;
}
static Observation observe(Context &c,Ring &r,unsigned role) {
    Observation o;
    const uint64_t activity=__atomic_load_n(&r.activity,__ATOMIC_ACQUIRE);
    const uint64_t progress=__atomic_load_n(&r.progress,__ATOMIC_ACQUIRE);
    if(__atomic_load_n(&r.readers,__ATOMIC_ACQUIRE)||!ringValid(c,r,role))return o;
    const uint32_t state=field<uint32_t>(r.object,layout.ringConsumer);
    o.head.epoch=c.bindingEpoch;o.head.ring=reinterpret_cast<uintptr_t>(r.object);
    o.head.consumerIndex=state&0x7fffffffU;o.head.consumerCycle=state>>31;o.head.consumerSerial=progress;
    if(o.head.consumerIndex>=layout.ringEntries)return o;
    const uintptr_t cell=r.sampleBuffer+uintptr_t(o.head.consumerIndex)*16;
    const auto control=field<uint32_t>(reinterpret_cast<void*>(cell),12);
    __asm__ volatile("lfence" ::: "memory");
    for(unsigned i=0;i<4;++i)o.head.words[i]=field<uint32_t>(reinterpret_cast<void*>(cell),i*4);
    __asm__ volatile("lfence" ::: "memory");
    bool equal=true;
    for(unsigned i=0;i<4;++i)equal&=o.head.words[i]==field<uint32_t>(reinterpret_cast<void*>(cell),i*4);
    __asm__ volatile("lfence" ::: "memory");
    o.coherent=equal&&control==o.head.words[3]&&state==field<uint32_t>(r.object,layout.ringConsumer)&&
        progress==__atomic_load_n(&r.progress,__ATOMIC_ACQUIRE)&&activity==__atomic_load_n(&r.activity,__ATOMIC_ACQUIRE)&&
        !__atomic_load_n(&r.readers,__ATOMIC_ACQUIRE)&&ringValid(c,r,role);
    o.valid=o.coherent&&(control&1U)==o.head.consumerCycle;
    return o;
}
struct Refs {OSObject *objects[Roles] {};IOMemoryMap *maps[Roles] {};IOBufferMemoryDescriptor *descriptors[Roles] {};};
static Refs detachLocked(Context &c) {
    Refs refs;
    for(unsigned i=0;i<Roles;++i) {
        auto &r=c.rings[i];
        __atomic_store_n(&r.token,uintptr_t(0),__ATOMIC_RELEASE);
        refs.objects[i]=r.object;refs.maps[i]=r.map;refs.descriptors[i]=r.descriptor;
        r.object=nullptr;r.map=nullptr;r.descriptor=nullptr;r.sampleBuffer=r.driverBuffer=0;r.policy.cancel();
    }
    c.bindingEpoch=0;return refs;
}
static void releaseRefs(Refs &refs) {
    for(unsigned i=0;i<Roles;++i) {
        if(refs.maps[i])refs.maps[i]->release();
        if(refs.descriptors[i])refs.descriptors[i]->release();
        if(refs.objects[i])refs.objects[i]->release();
    }
}
static bool bind(Context &c,uint64_t epoch) {
    // The caller owns the controller gate. No manager lock across mapping or
    // reference release; lifecycle mutations invalidate the binding epoch.
    IOLockLock(managerLock);
    bool valid=isReady()&&isRunning()&&c.policy.bindingCurrent(epoch)&&controllerValid(c);
    uintptr_t tokens[Roles] {};
    if(valid)for(unsigned i=0;i<Roles;++i) {
        tokens[i]=field<uintptr_t>(c.service,layout.controllerRings[i]);
        // This recognized native dispatcher unconditionally visits all three
        // roles; a missing role is not safe to call through this implementation.
        if(!tokens[i]){c.bindingStatus="native dispatcher requires all three role rings";valid=false;break;}
        auto *o=reinterpret_cast<OSObject*>(tokens[i]);
        const size_t fields[]={layout.ringController,layout.ringIndex,layout.ringConsumer,layout.ringBuffer,layout.ringDescriptor};
        if(!plausible(tokens[i])){c.bindingStatus="invalid interrupter pointer";valid=false;break;}
        for(size_t off:fields)if(!objectFits(o,off,8))valid=false;
        if(!valid||!o->metaCast("AppleUSBXHCIInterrupter")||
            !tableTarget(o,slots[Init],targets[Init])||!tableTarget(o,slots[Discard],targets[Discard])||
            !tableTarget(o,slots[RingFree],targets[RingFree])||!tableTarget(o,slots[Consume],targets[Consume])||
            field<uintptr_t>(o,layout.ringController)!=c.objectToken) {c.bindingStatus="unsupported interrupter structure/lifecycle override";valid=false;break;}
        for(unsigned j=0;j<i;++j)if(tokens[j]==tokens[i])valid=false;
        const auto d=field<uintptr_t>(o,layout.ringDescriptor);
        if(!plausible(d)){c.bindingStatus="event ring descriptor unavailable";valid=false;break;}
        auto *descriptor=OSDynamicCast(IOBufferMemoryDescriptor,reinterpret_cast<OSObject*>(d));
        const auto buffer=field<uintptr_t>(o,layout.ringBuffer);
        if(!descriptor||descriptor->getLength()!=layout.ringBytes||
            reinterpret_cast<uintptr_t>(descriptor->getBytesNoCopy())!=buffer||!plausible(buffer)) {c.bindingStatus="event ring buffer/descriptor relationship mismatch";valid=false;break;}
        auto &r=c.rings[i];r.object=o;r.object->retain();r.descriptor=descriptor;descriptor->retain();
        r.driverBuffer=buffer;r.id=field<uint32_t>(o,layout.ringIndex);
        for(unsigned j=0;j<i;++j)if(c.rings[j].object&&c.rings[j].id==r.id)valid=false;
    }
    IOLockUnlock(managerLock);
    bool any=false;
    bool mapFailed=false;
    if(valid)for(auto &r:c.rings)if(r.object) {
        any=true;r.map=r.descriptor->map(kIOMapUnique|kIOMapReadOnly);
        if(!r.map||r.map->getLength()!=layout.ringBytes||!r.map->getVirtualAddress()) {mapFailed=true;valid=false;break;}
        r.sampleBuffer=uintptr_t(r.map->getVirtualAddress());
    }
    IOLockLock(managerLock);
    if(mapFailed)c.bindingStatus="read-only event ring mapping failed";
    valid=valid&&any&&c.policy.bindingCurrent(epoch)&&controllerValid(c);
    if(valid)for(unsigned i=0;i<Roles;++i)if(c.rings[i].object)valid&=ringValid(c,c.rings[i],i);
    if(valid) {
        c.bindingEpoch=epoch;c.dirty=false;++c.rebinds;c.bindFailure=0;c.bindingStatus="active";
        for(auto &r:c.rings)if(r.object) {
            r.policy.start(Mode::Watchdog,StallUs);
            __atomic_store_n(&r.token,reinterpret_cast<uintptr_t>(r.object),__ATOMIC_RELEASE);
        }
        valid=c.policy.bindingSucceeded(epoch,nowUs());
    } else {c.bindFailure=1;c.policy.bindingFailed(epoch,nowUs());}
    Refs cleanup;
    if(!valid)cleanup=detachLocked(c);
    IOLockUnlock(managerLock);
    releaseRefs(cleanup);return valid;
}
static Context *contextForTimer(IOTimerEventSource *source) {
    for(auto *c=head;c;c=c->next)if(c->timer==source&&!c->retiring&&!c->retired)return c;
    return nullptr;
}
static void controllerTimer(OSObject*,IOTimerEventSource *source) {
    const uint64_t begin=nowUs();
    IOLockLock(managerLock);
    Context *c=contextForTimer(source);
    if(!c||!isRunning()){IOLockUnlock(managerLock);return;}
    if(!c->loop||!c->loop->inGate()){c->policy.block(ControllerBlock::InvalidState);IOLockUnlock(managerLock);return;}
    ++c->polls;
    if(c->lastPollUs&&begin>=c->lastPollUs) {
        const auto gap=begin-c->lastPollUs;c->intervalTotalUs+=gap;
        if(!c->minIntervalUs||gap<c->minIntervalUs)c->minIntervalUs=gap;
        if(gap>c->maxIntervalUs)c->maxIntervalUs=gap;
    }
    c->lastPollUs=begin;
    Refs cleanup;
    if(c->dirty&&!c->reservation.active())cleanup=detachLocked(*c);
    const uint64_t epoch=isReady()?c->policy.beginBinding(begin):0;
    IOLockUnlock(managerLock);
    releaseRefs(cleanup);
    if(epoch)bind(*c,epoch);
    IOLockLock(managerLock);
    if(c->policy.activeFor(c->bindingEpoch)) {
        bool valid=controllerValid(*c);
        for(unsigned i=0;i<Roles;++i)if(c->rings[i].object)valid&=ringValid(*c,c->rings[i],i);
        if(!valid) {c->policy.beginMutation();c->policy.endMutation(nowUs());invalidate(*c);c->bindFailure=2;}
        else if(!ml_at_interrupt_context()&&ml_get_interrupts_enabled()) {
            int selected=-1;Candidate candidate;
            for(unsigned i=0;i<Roles;++i)if(c->rings[i].object) {
                Candidate possible;
                if(c->rings[i].policy.observe(observe(*c,c->rings[i],i),nowUs(),possible)&&selected<0) {selected=int(i);candidate=possible;}
                if(!c->rings[i].policy.active()) {
                    c->policy.block(c->rings[i].policy.stopReason()==StopReason::NoProgress?
                        ControllerBlock::NoProgress:ControllerBlock::InvalidState);invalidate(*c);selected=-1;break;
                }
            }
            if(selected>=0&&c->policy.activeFor(c->bindingEpoch)) {
                auto &r=c->rings[selected];
                const auto final=observe(*c,r,unsigned(selected));
                if(r.policy.commit(candidate,final,nowUs())) {
                    uintptr_t tokens[Roles] {};for(unsigned i=0;i<Roles;++i)tokens[i]=reinterpret_cast<uintptr_t>(c->rings[i].object);
                    const auto owner=reinterpret_cast<uintptr_t>(IOThreadSelf());
                    const auto savedEpoch=c->bindingEpoch;
                    if(c->reservation.begin(owner,c->objectToken,tokens)) {
                        IOLockUnlock(managerLock);
                        // Invoke only the verified original path on its own gate.
                        // The native code performs its ordinary register updates.
                        reinterpret_cast<void(*)(void*)>(manual)(c->service);
                        IOLockLock(managerLock);
                        c->reservation.finish(owner);IOLockWakeup(managerLock,&c->reservation,false);
                        ++c->wakeCalls;c->lastWakeUs=nowUs();
                        if(c->policy.activeFor(savedEpoch))r.policy.reportWakeResult(true);
                    } else {r.policy.reportWakeResult(false);c->policy.block(ControllerBlock::WakeFailed);invalidate(*c);}
                }
            }
        }
    }
    const uint64_t end=nowUs();
    if(end>=begin&&end-begin>c->maxCallbackUs)c->maxCallbackUs=end-begin;
    const auto phase=c->policy.phase();
    const uint32_t delay=phase==ControllerPhase::Active?PollUs:
        (phase==ControllerPhase::Blocked||phase==ControllerPhase::Disabled?100000:1000);
    const bool rearm=isRunning()&&!c->retiring&&!c->retired;
    IOLockUnlock(managerLock);
    if(rearm)source->setTimeoutUS(delay);
}
static void number(OSDictionary *d,const char *key,uint64_t v,unsigned bits=64) {
    auto *o=OSNumber::withNumber(v,bits);if(o){d->setObject(key,o);o->release();}
}
static void string(OSDictionary *d,const char *key,const char *value) {
    auto *o=OSString::withCString(value);if(o){d->setObject(key,o);o->release();}
}
static void publishStatus() {
    if(!managerLock)return;
    IOLockLock(managerLock);
    if(!status){IOLockUnlock(managerLock);return;}
    auto *array=OSArray::withCapacity(4);
    if(array)for(auto *c=head;c;c=c->next) {
        auto *row=OSDictionary::withCapacity(24);if(!row)continue;
        number(row,"ControllerRegistryID",c->registryID);string(row,"Name",c->name);string(row,"Class",c->className);
        const char *phases[]={"empty","disabled","discovering","binding","active","suspended","terminated","blocked"};
        string(row,"Phase",phases[unsigned(c->policy.phase())]);
        const char *blocks[]={"none","explicit stop","no progress after wake","wake failed","invalid state","clock regressed"};
        number(row,"StopReason",unsigned(c->policy.stopReason()),32);string(row,"StopReasonName",blocks[unsigned(c->policy.stopReason())]);
        number(row,"BindFailure",c->bindFailure,32);string(row,"BindingStatus",c->bindingStatus);
        number(row,"Epoch",c->policy.epoch());number(row,"WakeCalls",c->wakeCalls);number(row,"Rebinds",c->rebinds);
        number(row,"PollCount",c->polls);number(row,"LastPollUs",c->lastPollUs);
        number(row,"MinimumPollIntervalUs",c->minIntervalUs);number(row,"MaximumPollIntervalUs",c->maxIntervalUs);
        number(row,"PollIntervalTotalUs",c->intervalTotalUs);number(row,"MaximumCallbackUs",c->maxCallbackUs);
        number(row,"LastWakeUs",c->lastWakeUs);
        auto *rings=OSArray::withCapacity(Roles);
        if(rings) {
            for(unsigned i=0;i<Roles;++i) {
                auto &r=c->rings[i];auto *rr=OSDictionary::withCapacity(10);if(!rr)continue;
                number(rr,"Role",i,32);number(rr,"InterrupterIndex",r.id,32);number(rr,"ConsumerProgress",__atomic_load_n(&r.progress,__ATOMIC_ACQUIRE));
                number(rr,"WakeCommitted",r.policy.stats().wakesCommitted);number(rr,"ProgressAfterWake",r.policy.stats().progressAfterWake);
                number(rr,"NoProgressStops",r.policy.stats().noProgressStops);number(rr,"RaceResets",r.policy.stats().raceResets);
                number(rr,"StopReason",unsigned(r.policy.stopReason()),32);
                rr->setObject("Bound",r.object?kOSBooleanTrue:kOSBooleanFalse);rings->setObject(rr);rr->release();
            }
            row->setObject("Rings",rings);rings->release();
        }
        array->setObject(row);row->release();
    }
    status->setProperty("WakeupVersion","0.2.0");status->setProperty("WakeupRuntimeReady",isReady());
    status->setProperty("RuntimeStatus",__atomic_load_n(&runtimeError,__ATOMIC_ACQUIRE));status->setProperty("RegistrationOK",registrationOK);
    status->setProperty("WakeupDefaultEnabled",defaultEnabled);status->setProperty("WakeupPollUs",PollUs,32);
    status->setProperty("WakeupStallUs",StallUs,32);status->setProperty("UnlimitedDuration",true);
    status->setProperty("RoutedHookMask",__atomic_load_n(&routedMask,__ATOMIC_ACQUIRE),64);status->setProperty("UnloadSupported",false);
    status->setProperty("OwnsOriginalWorkloopTimers",true);status->setProperty("VersionWhitelist",false);
    if(array){status->setProperty("Controllers",array);array->release();}
    IOLockUnlock(managerLock);
}
static bool published(void*,void*,IOService *service,IONotifier*) {
    if(!service||!managerLock||!isRunning())return false;
    auto *c=new Context;if(!c)return false;
    c->registryID=service->getRegistryEntryID();c->objectToken=reinterpret_cast<uintptr_t>(service);
    c->service=service;service->retain();
    strlcpy(c->name,service->getName(),sizeof c->name);strlcpy(c->className,service->getMetaClass()->getClassName(),sizeof c->className);
    IOLockLock(managerLock);
    if(find(c->registryID)||!isRunning()){IOLockUnlock(managerLock);service->release();delete c;return false;}
    c->policy.published(c->registryID,c->objectToken,nowUs());c->policy.configure(defaultEnabled,nowUs());
    for(auto *m=mutations;m;m=m->next)if(m->controller==c->objectToken)c->policy.beginMutation();
    c->next=head;__atomic_store_n(&head,c,__ATOMIC_RELEASE);
    IOLockUnlock(managerLock);return true;
}
static IOReturn cleanupGated(OSObject*,void *arg,void*,void*,void*) {
    auto *c=static_cast<Context*>(arg);IOLockLock(managerLock);
    Refs refs=detachLocked(*c);IOLockUnlock(managerLock);releaseRefs(refs);return kIOReturnSuccess;
}
static void retire(Context &c) {
    IOLockLock(managerLock);
    if(c.retiring||c.retired){IOLockUnlock(managerLock);return;}
    c.retiring=true;c.policy.terminated();invalidate(c);
    while(c.reservation.mustDrainAll(reinterpret_cast<uintptr_t>(IOThreadSelf())))
        IOLockSleep(managerLock,&c.reservation,THREAD_UNINT);
    auto *timer=c.timer;auto *loop=c.loop;auto *service=c.service;
    IOLockUnlock(managerLock);
    if(timer){timer->cancelTimeout();if(loop)loop->removeEventSource(timer);timer->release();}
    if(loop)loop->runAction(cleanupGated,service,&c);
    else cleanupGated(nullptr,&c,nullptr,nullptr,nullptr);
    IOLockLock(managerLock);c.timer=nullptr;c.loop=nullptr;c.service=nullptr;c.retired=true;IOLockUnlock(managerLock);
    if(loop)loop->release();if(service)service->release();
    // Context/counter tombstones remain module-lifetime for lock-free IRQ readers.
}
static void detachTimerForRebind(Context &c) {
    // Only the management callback calls this. It never waits for the original
    // controller gate with managerLock held, and never holds two such gates.
    IOLockLock(managerLock);
    c.retiring=true;c.policy.beginMutation();invalidate(c);
    while(c.reservation.mustDrainAll(reinterpret_cast<uintptr_t>(IOThreadSelf())))
        IOLockSleep(managerLock,&c.reservation,THREAD_UNINT);
    auto *oldTimer=c.timer;auto *oldLoop=c.loop;
    IOLockUnlock(managerLock);
    if(oldTimer){oldTimer->cancelTimeout();if(oldLoop)oldLoop->removeEventSource(oldTimer);}
    if(oldLoop)oldLoop->runAction(cleanupGated,c.service,&c);
    IOLockLock(managerLock);
    c.timer=nullptr;c.loop=nullptr;c.retiring=false;c.bindingStatus="workloop changed; waiting to rebind";
    c.policy.endMutation(nowUs());
    IOLockUnlock(managerLock);
    if(oldTimer)oldTimer->release();if(oldLoop)oldLoop->release();
}
static void managementAction(OSObject*,IOTimerEventSource *source) {
    if(source!=managementTimer||!isRunning())return;
    for(auto *c=__atomic_load_n(&head,__ATOMIC_ACQUIRE);c;c=c->next) {
        if(c->retired||c->retiring)continue;
        if(c->service->isInactive()){retire(*c);continue;}
        IOLockLock(managerLock);
        if(!isReady()||c->policy.mutationDepth()||c->retiring||c->retired) {IOLockUnlock(managerLock);continue;}
        if(!controllerRoutesValid(*c)) {
            c->policy.block(ControllerBlock::InvalidState);invalidate(*c);c->bindFailure=3;
            IOLockUnlock(managerLock);continue;
        }
        // Every supported lifecycle entry takes this same lock before changing
        // workloop ownership. Retain the candidate before allowing it to enter.
        auto *candidateLoop=c->service->getWorkLoop();if(candidateLoop)candidateLoop->retain();
        IOLockUnlock(managerLock);
        if(c->loop&&candidateLoop!=c->loop)detachTimerForRebind(*c);
        if(!c->timer) {
            auto *loop=candidateLoop;if(!loop)continue;
            auto *newTimer=IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityLow,status,controllerTimer);
            if(!newTimer){loop->release();continue;}
            IOLockLock(managerLock);c->loop=loop;c->timer=newTimer;IOLockUnlock(managerLock);
            if(loop->addEventSource(newTimer)!=kIOReturnSuccess) {
                IOLockLock(managerLock);c->loop=nullptr;c->timer=nullptr;IOLockUnlock(managerLock);
                newTimer->release();loop->release();continue;
            }
            newTimer->setTimeoutUS(PollUs);
        } else if(candidateLoop)candidateLoop->release();
    }
    if(isRunning())source->setTimeoutMS(100);
}
static bool initialize(IOService *service) {
    status=service;
    managementLoop=IOWorkLoop::workLoop();
    managementTimer=managementLoop?IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityLow,service,managementAction):nullptr;
    if(!managementTimer||managementLoop->addEventSource(managementTimer)!=kIOReturnSuccess)return false;
    managementAdded=true;setRunning(true);
    publishNotifier=IOService::addMatchingNotification(gIOPublishNotification,IOService::serviceMatching("AppleUSBXHCI"),published,nullptr);
    if(!publishNotifier){setRunning(false);return false;}
    managementTimer->setTimeoutUS(1);publishStatus();return true;
}
static void shutdown() {
    IOLockLock(controlLock);setRunning(false);
    if(publishNotifier){publishNotifier->remove();publishNotifier=nullptr;}
    if(managementTimer){managementTimer->cancelTimeout();if(managementAdded)managementLoop->removeEventSource(managementTimer);}
    managementAdded=false;
    for(auto *c=head;c;c=c->next)retire(*c);
    if(managementTimer){managementTimer->release();managementTimer=nullptr;}
    if(managementLoop){managementLoop->release();managementLoop=nullptr;}
    IOLockLock(managerLock);status=nullptr;IOLockUnlock(managerLock);IOLockUnlock(controlLock);
}
static IOReturn control(OSDictionary *d) {
    auto *cmd=OSDynamicCast(OSString,d->getObject("WakeupCommand"));if(!cmd)return kIOReturnBadArgument;
    auto *id=OSDynamicCast(OSNumber,d->getObject("ControllerRegistryID"));
    const uint64_t selected=id?id->unsigned64BitValue():0;
    if(id&&!selected)return kIOReturnBadArgument;
    if(cmd->isEqualTo("status")){publishStatus();return kIOReturnSuccess;}
    const bool start=cmd->isEqualTo("start");
    if(!start&&!cmd->isEqualTo("stop"))return kIOReturnUnsupported;
    IOLockLock(controlLock);IOLockLock(managerLock);
    if(selected&&!find(selected)){IOLockUnlock(managerLock);IOLockUnlock(controlLock);return kIOReturnNotFound;}
    if(selected&&find(selected)->retired){IOLockUnlock(managerLock);IOLockUnlock(controlLock);return kIOReturnNotReady;}
    if(start)for(auto *c=head;c;c=c->next)if((!selected||c->registryID==selected)&&!c->retired&&
        (c->policy.mutationDepth()||c->reservation.active())) {
        IOLockUnlock(managerLock);IOLockUnlock(controlLock);return kIOReturnBusy;
    }
    if(!selected)defaultEnabled=start;
    for(auto *c=head;c;c=c->next)if((!selected||c->registryID==selected)&&!c->retired&&(!start||!c->retiring)) {
        if(start) {
            c->policy=ControllerPolicy();c->policy.published(c->registryID,c->objectToken,nowUs());c->policy.configure(true,nowUs());
        } else {
            c->policy.block(ControllerBlock::ExplicitStop);
            while(c->reservation.mustDrainAll(reinterpret_cast<uintptr_t>(IOThreadSelf())))
                IOLockSleep(managerLock,&c->reservation,THREAD_UNINT);
        }
        invalidate(*c);
    }
    IOLockUnlock(managerLock);IOLockUnlock(controlLock);publishStatus();return kIOReturnSuccess;
}
static uintptr_t solve(KernelPatcher &p,size_t id,const char *name) {
    const auto value=p.solveSymbol(id,name);p.clearError();return uintptr_t(value);
}
static bool slotFor(uintptr_t table,uintptr_t target,size_t &slot) {
    if(!target||!table||!readable(table,16))return false;
    size_t available=images[0].available(table+16,false);
    if(!available)available=images[1].available(table+16,false);
    if(available>0x1000)available=0x1000;
    return GenericXhci::findPrimaryVtableSlot(reinterpret_cast<const uintptr_t*>(table+16),available,target,slot);
}
static bool safePrefix(uintptr_t function,unsigned module) {
    if(!images[module].contains(function,64,true))return false;
    size_t span=0;
    return GenericXhci::safeHookSpan(reinterpret_cast<const uint8_t*>(function),64,14,span);
}
static const char *paths[2][1]={
    {"/System/Library/Extensions/IOUSBHostFamily.kext/Contents/PlugIns/AppleUSBXHCI.kext"},
    {"/System/Library/Extensions/IOUSBHostFamily.kext/Contents/PlugIns/AppleUSBXHCIPCI.kext"}
};
static KernelPatcher::KextInfo modules[]={
    {"com.apple.driver.usb.AppleUSBXHCI",paths[0],1,{true}},
    {"com.apple.driver.usb.AppleUSBXHCIPCI",paths[1],1,{true}}
};
static bool prepare(KernelPatcher &p) {
    auto view=[&](const char *symbol,size_t limit,bool untilReturn=false)->GenericXhci::FunctionView {
        const auto address=solve(p,moduleIDs[0],symbol);
        size_t size=images[0].available(address,true);if(size>limit)size=limit;
        if(!size)return {nullptr,0,0};
        if(untilReturn) {
            // No live __LINKEDIT dereference. This tested linear bound is
            // conservative, rather than a claim to recover arbitrary CFGs.
            size_t span=0;
            if(!GenericXhci::firstReturnSpan(reinterpret_cast<const uint8_t*>(address),size,span))return {nullptr,0,0};
            size=span;
        }
        return {reinterpret_cast<const uint8_t*>(address),size,address};
    };
    GenericXhci::Inputs in {};
    in.primaryFilter=view("__ZN12AppleUSBXHCI22primaryFilterInterruptEP28IOFilterInterruptEventSource",24);
    in.ioFilter=view("__ZN12AppleUSBXHCI17ioFilterInterruptEP28IOFilterInterruptEventSource",24);
    in.isoFilter=view("__ZN12AppleUSBXHCI26isochronousFilterInterruptEP28IOFilterInterruptEventSource",108);
    in.filter=view("__ZN12AppleUSBXHCI14filterOccurredEP28IOFilterInterruptEventSource",112);
    in.dispatch=view("__ZN12AppleUSBXHCI17interruptOccurredEP22IOInterruptEventSourcei",120);
    in.manual=view("__ZN12AppleUSBXHCI25manuallyProcessInterruptsEv",76);
    in.next=view(symbols[Consume],138);in.initRegisters=view(symbols[Init],512,true);
    in.initWithParameters=view("__ZN23AppleUSBXHCIInterrupter18initWithParametersEP12AppleUSBXHCIjP8IOMapper",1024,true);
    in.ringFree=view(symbols[RingFree],80);
    in.expectedCAS=reinterpret_cast<uintptr_t>(&OSCompareAndSwap);
    in.expectedAtomicOr=reinterpret_cast<uintptr_t>(&OSBitOrAtomic);
    GenericXhci::Error error {};
    if(!GenericXhci::decode(in,layout,&error)){errorStatus(GenericXhci::errorName(error));return false;}
    manual=in.manual.address;filter=in.filter.address;dispatch=in.dispatch.address;
    primaryFilters[0]=in.primaryFilter.address;primaryFilters[1]=in.ioFilter.address;primaryFilters[2]=in.isoFilter.address;
    const char *actions[]={"__ZN12AppleUSBXHCI16primaryInterruptEP22IOInterruptEventSourcei",
        "__ZN12AppleUSBXHCI11ioInterruptEP22IOInterruptEventSourcei","__ZN12AppleUSBXHCI20isochronousInterruptEP22IOInterruptEventSourcei"};
    const auto controllerTable=solve(p,moduleIDs[0],"__ZTV12AppleUSBXHCI");
    const auto ringTable=solve(p,moduleIDs[0],"__ZTV23AppleUSBXHCIInterrupter");
    if(!slotFor(controllerTable,manual,manualSlot)){errorStatus("manual-vtable-slot");return false;}
    for(unsigned i=0;i<Roles;++i) {
        primaryActions[i]=solve(p,moduleIDs[0],actions[i]);
        if(!primaryActions[i]||!readable(controllerTable+16,layout.actionSlots[i]+8)||
            field<uintptr_t>(reinterpret_cast<void*>(controllerTable+16),layout.filterSlots[i])!=primaryFilters[i]||
            field<uintptr_t>(reinterpret_cast<void*>(controllerTable+16),layout.actionSlots[i])!=primaryActions[i]) {
            errorStatus("role-vtable-slots");return false;
        }
    }
    for(unsigned i=0;i<HookCount;++i) {
        const unsigned module=i>=PCILower?1:0;
        targets[i]=solve(p,moduleIDs[module],symbols[i]);
        if(!targets[i]||!safePrefix(targets[i],module)){errorStatus("unsafe-route-prefix");return false;}
        if(i<PCILower) {
            const bool ring=i==Consume||i==Init||i==Discard||i==RingFree;
            if(!slotFor(ring?ringTable:controllerTable,targets[i],slots[i])){errorStatus("lifecycle-vtable-slot");return false;}
        }
    }
    const mach_vm_address_t destinations[HookCount]={
        reinterpret_cast<uintptr_t>(consume),reinterpret_cast<uintptr_t>(lower),reinterpret_cast<uintptr_t>(raise),
        reinterpret_cast<uintptr_t>(reset),reinterpret_cast<uintptr_t>(stop),reinterpret_cast<uintptr_t>(freeController),
        reinterpret_cast<uintptr_t>(init),reinterpret_cast<uintptr_t>(discard),reinterpret_cast<uintptr_t>(freeRing),
        reinterpret_cast<uintptr_t>(pciLower),reinterpret_cast<uintptr_t>(pciRaise),reinterpret_cast<uintptr_t>(pciStop),reinterpret_cast<uintptr_t>(pciFree)
    };
    KernelPatcher::RouteRequest routes[]={
        {symbols[0],destinations[0],originals[0]},{symbols[1],destinations[1],originals[1]},
        {symbols[2],destinations[2],originals[2]},{symbols[3],destinations[3],originals[3]},
        {symbols[4],destinations[4],originals[4]},{symbols[5],destinations[5],originals[5]},
        {symbols[6],destinations[6],originals[6]},{symbols[7],destinations[7],originals[7]},
        {symbols[8],destinations[8],originals[8]},{symbols[9],destinations[9],originals[9]},
        {symbols[10],destinations[10],originals[10]},{symbols[11],destinations[11],originals[11]},
        {symbols[12],destinations[12],originals[12]}
    };
    for(unsigned module=0;module<2;++module) {
        const unsigned first=module?PCILower:Consume,last=module?HookCount:PCILower;
        const bool ok=p.routeMultiple(moduleIDs[module],routes+first,last-first,0,0,true,false);p.clearError();
        for(unsigned i=first;i<last;++i)if(originals[i])__atomic_fetch_or(&routedMask,uint64_t(1)<<i,__ATOMIC_RELEASE);
        if(!ok){errorStatus("partial-routing; compensation disabled");return false;}
    }
    errorStatus("ready");return true;
}
static void loaded(void*,KernelPatcher &p,size_t id,mach_vm_address_t address,size_t size) {
    unsigned selected=2;
    for(unsigned i=0;i<2;++i)if(modules[i].loadIndex!=KernelPatcher::KextInfo::Unloaded&&modules[i].loadIndex==id)selected=i;
    if(selected==2||seen[selected])return;
    seen[selected]=true;moduleIDs[selected]=id;
    if(!images[selected].parse(uintptr_t(address),size)){errorStatus("invalid-module-segments");return;}
    if(seen[0]&&seen[1]&&images[0].valid&&images[1].valid&&!attempted){attempted=true;__atomic_store_n(&runtimeReady,prepare(p),__ATOMIC_RELEASE);}
}
static void initPlugin() {
    defaultEnabled=!checkKernelArgument("-xhciwakeupmanual");
    managerLock=IOLockAlloc();controlLock=IOLockAlloc();
    if(!managerLock||!controlLock){errorStatus("lock-allocation");return;}
    registrationOK=lilu.onKextLoad(modules,2,loaded,nullptr)==LiluAPI::Error::NoError;
}
} // namespace Wake

class EXPORT USBXHCIWakeupService : public IOService {
    OSDeclareDefaultStructors(USBXHCIWakeupService)
public:
    IOService *probe(IOService *p,SInt32 *score) override {return ADDPR(startSuccess)?IOService::probe(p,score):nullptr;}
    bool start(IOService *p) override {
        if(!IOService::start(p)||!Wake::managerLock||!Wake::controlLock)return false;
        setName("USBXHCIWakeup");
        if(!Wake::initialize(this)){Wake::shutdown();return false;}
        registerService();return true;
    }
    void stop(IOService *p) override {Wake::shutdown();IOService::stop(p);}
    IOReturn setProperties(OSObject *object) override {
        if(IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)return kIOReturnNotPrivileged;
        auto *d=OSDynamicCast(OSDictionary,object);return d?Wake::control(d):kIOReturnBadArgument;
    }
};
OSDefineMetaClassAndStructors(USBXHCIWakeupService,IOService)
static const char *disableArguments[]={"-xhciwakeupoff"};
PluginConfiguration ADDPR(config)={
    "USBXHCIWakeup",parseModuleVersion("0.2.0"),LiluAPI::AllowNormal|LiluAPI::AllowInstallerRecovery,
    disableArguments,1,nullptr,0,nullptr,0,KernelVersion::BigSur,static_cast<KernelVersion>(0x7fff),Wake::initPlugin
};
extern "C" kern_return_t ADDPR(kern_start)(kmod_info_t*,void*);
extern "C" kern_return_t ADDPR(kern_stop)(kmod_info_t*,void*);
extern "C" kern_return_t _start(kmod_info_t*,void*);
extern "C" kern_return_t _stop(kmod_info_t*,void*);
KMOD_EXPLICIT_DECL(as.local.USBXHCIWakeup,"0.2.0",_start,_stop)
extern "C" {
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain=ADDPR(kern_start);
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain=ADDPR(kern_stop);
__attribute__((visibility("hidden"))) int _kext_apple_cc=__APPLE_CC__;
}

