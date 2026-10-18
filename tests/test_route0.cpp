// Offline safety/coverage fixtures. No Kext loading or hardware access.
#include "route0.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

struct TRB {uint32_t word[4];};
enum EndpointType {Control,Isochronous,Bulk,Interrupt};
namespace R=XhciLagFix::AllRoute0;

struct TD {
    uintptr_t array;
    uint32_t capacity, reserved;
    uint64_t dma;
    uintptr_t next;
};
static_assert(offsetof(TD,array)==R::TDArray,"array ABI");
static_assert(offsetof(TD,capacity)==R::TDCapacity,"capacity ABI");
static_assert(offsetof(TD,next)==R::TDNext,"queue ABI");
struct alignas(16) Node { TD td{}; alignas(16) std::array<TRB,16> trbs{}; };
static unsigned cases;

template<class T> static void put(void *p,size_t offset,T value) {
    std::memcpy(static_cast<uint8_t*>(p)+offset,&value,sizeof value);
}
struct Fixture {
    alignas(16) std::array<uint8_t,0x100> request{};
    alignas(16) std::array<uintptr_t,2> queue{};
    std::vector<Node> nodes;
    explicit Fixture(size_t count=1,EndpointType endpoint=Bulk):nodes(count) {
        const bool iso=endpoint==Isochronous;
        for (size_t i=0;i<count;i++) {
            auto &node=nodes[i];
            node.td.array=reinterpret_cast<uintptr_t>(node.trbs.data());
            node.td.capacity=16;
            node.td.dma=0x12345000+i*0x100;
            node.td.next=i+1<count ? reinterpret_cast<uintptr_t>(&nodes[i+1].td):0;
            // Include Normal, Event Data, No Op and a terminal Link. Iso also
            // includes its own TRB type. Every other slot is unused/zero.
            size_t linkSlot=3;
            if (endpoint==Control) {
                // Setup/Data/Status are the control-only TRB types; a long
                // data stage may also contain Normal. No Op Transfer models the
                // final extra descriptor added by SparseRequest::prepare.
                set(i,0,2,1,8,0x41);
                set(i,1,3,1,0x12345,0x231);
                set(i,2,1,1,0x53,0x11);
                set(i,3,4,1,0,0x21);
                set(i,4,8,1,0,0x21);
                linkSlot=5;
            } else {
                set(i,0,iso?5:1,iso?2:1,0x0012345,0x231);
                set(i,1,7,iso?2:1,0,0x21);
                set(i,2,iso?1:8,iso?2:1,iso?0x53:0,0x11);
            }
            set(i,linkSlot,6,iso?2:1,0,0x10); // Tail Link Cycle remains zero.
            node.trbs[linkSlot].word[0]=i+1<count ? uint32_t(0x12345000+(i+1)*0x100):0;
            node.trbs[linkSlot].word[1]=0;
        }
        queue[0]=count ? reinterpret_cast<uintptr_t>(&nodes[0].td):0;
        queue[1]=count ? reinterpret_cast<uintptr_t>(&nodes[count-1].td.next):reinterpret_cast<uintptr_t>(queue.data());
        put(request.data(),R::RequestQueue,reinterpret_cast<uintptr_t>(queue.data()));
    }
    void set(size_t n,size_t slot,uint32_t type,uint32_t target,uint32_t low,uint32_t flags) {
        nodes[n].trbs[slot]={{uint32_t(0x87654000+n*0x100+slot*0x10),0xabcdef12,
                              (target<<22)|low,(type<<10)|flags}};
    }
    bool route() { return R::rewritePreparedQueue(request.data()); }
};

static void unchanged(const Fixture &f,const std::vector<Node> &before) {
    assert(f.nodes.size()==before.size());
    for (size_t i=0;i<before.size();i++) {
        assert(!std::memcmp(&f.nodes[i],&before[i],sizeof(Node)));
    }
}
static void routed(const Fixture &f,const std::vector<Node> &before,bool result) {
    assert(result);
    for (size_t i=0;i<before.size();i++) {
        assert(!std::memcmp(&f.nodes[i].td,&before[i].td,sizeof(TD)));
        for (size_t t=0;t<16;t++) {
            const auto &old=before[i].trbs[t],&now=f.nodes[i].trbs[t];
            assert(old.word[0]==now.word[0] && old.word[1]==now.word[1] && old.word[3]==now.word[3]);
            assert(now.word[2]==(old.word[2]&~R::TargetMask));
        }
    }
}
template<class Mutate> static void rejected(const char *name,Mutate mutate) {
    Fixture f(2);mutate(f);const auto before=f.nodes;
    const auto out=f.route();
    assert(!out);
    unchanged(f,before);++cases;
    std::printf("PASS reject without partial mutation: %s\n",name);
}

int main() {
    for(auto type:{Control,Isochronous,Bulk,Interrupt}) for(size_t segments:{size_t(1),size_t(3),size_t(R::MaxTDs)}) {
        Fixture f(segments,type);const auto before=f.nodes;
        const auto reqBefore=f.request;const auto queueBefore=f.queue;
        const auto out=f.route();routed(f,before,out);
        assert(f.request==reqBefore && f.queue==queueBefore);
        // Already Target 0 is idempotent.
        const auto zero=f.nodes;const auto twice=f.route();
        assert(twice);unchanged(f,zero);++cases;
    }
    // An invalid request pointer must fail before any dereference.
    assert(!R::rewritePreparedQueue(reinterpret_cast<void*>(1)));++cases;
    // Unknown layouts in the second segment must not alter the valid first one.
    rejected("zero capacity in later TD",[](Fixture &f){f.nodes[1].td.capacity=0;});
    rejected("over-capacity later TD",[](Fixture &f){f.nodes[1].td.capacity=R::MaximumCapacity+1;});
    rejected("unexpected reserved TRB",[](Fixture &f){f.set(1,0,9,1,0x123,1);});
    rejected("command TRB in transfer queue",[](Fixture &f){f.set(1,0,23,1,0x123,1);});
    rejected("missing final Link",[](Fixture &f){f.nodes[1].trbs[3]={};});
    rejected("valid entry following terminal Link",[](Fixture &f){f.set(1,4,1,1,0x123,1);});
    rejected("duplicate Link",[](Fixture &f){f.set(1,4,6,1,0,0);});
    rejected("nonzero supposedly unused TRB",[](Fixture &f){f.nodes[1].trbs[15].word[0]=123;});
    rejected("zero TRB type but live control bits",[](Fixture &f){f.nodes[1].trbs[15].word[3]=1;});
    rejected("misaligned later TRB array",[](Fixture &f){f.nodes[1].td.array++;});
    rejected("null later TRB array",[](Fixture &f){f.nodes[1].td.array=0;});
    rejected("misaligned next TD",[](Fixture &f){f.nodes[0].td.next++;});
    rejected("self cycle",[](Fixture &f){f.nodes[1].td.next=reinterpret_cast<uintptr_t>(&f.nodes[1].td);});
    rejected("backward queue cycle",[](Fixture &f){f.nodes[1].td.next=reinterpret_cast<uintptr_t>(&f.nodes[0].td);});
    rejected("null queue",[](Fixture &f){put<uintptr_t>(f.request.data(),R::RequestQueue,0);});
    rejected("misaligned queue",[](Fixture &f){put(f.request.data(),R::RequestQueue,reinterpret_cast<uintptr_t>(f.queue.data())+1);});
    rejected("empty queue head",[](Fixture &f){f.queue[0]=0;});
    {
        Fixture f(R::MaxTDs+1);const auto before=f.nodes;
        auto out=f.route();assert(!out);unchanged(f,before);++cases;
    }
    {
        Fixture f;put<uintptr_t>(f.request.data(),R::RequestPreviousTD,0x1234);
        const auto before=f.nodes;auto out=f.route();
        assert(!out);unchanged(f,before);++cases;
    }
    {
        auto out=R::rewritePreparedQueue(nullptr);
        assert(!out);++cases;
    }
    // Mixed 0/1 targets are valid.
    {
        Fixture f;f.nodes[0].trbs[0].word[2]&=~R::TargetMask;const auto before=f.nodes;
        auto out=f.route();routed(f,before,out);++cases;
    }
    // Actual TD capacity is metadata, not a fixed 16-slot activation gate.
    for (uint32_t capacity : {8U,32U,256U}) {
        alignas(16) std::array<uint8_t,0x100> request{};
        alignas(16) std::array<TRB,256> array{};
        TD td{reinterpret_cast<uintptr_t>(array.data()),capacity,0,0,0};
        alignas(16) uintptr_t queue=reinterpret_cast<uintptr_t>(&td);
        put(request.data(),R::RequestQueue,reinterpret_cast<uintptr_t>(&queue));
        const uint32_t types[]={2,3,4,5,1,7,8,6};
        for(unsigned i=0;i<8;i++) {
            put(&array[i],8,uint32_t((2U<<22)|123));
            put(&array[i],12,uint32_t(types[i]<<10));
        }
        const auto out=R::rewritePreparedQueue(request.data());
        assert(out);
        for(unsigned i=0;i<8;i++) {
            assert(XhciLagFix::loadField<uint32_t>(&array[i],8)==123);
            assert(XhciLagFix::loadField<uint32_t>(&array[i],12)==types[i]<<10);
        }
        ++cases;
    }
    std::printf("PASS route0: %u cases, all four endpoint classes including Control/Sparse tail and Interrupt Normal, complete final/multiple TDs, unrelated-bit/pointer/Cycle preservation, idempotence, invalid-pointer rejection before reads, malformed chain all-or-none and bounded cycles. No hardware access.\n",cases);
}
