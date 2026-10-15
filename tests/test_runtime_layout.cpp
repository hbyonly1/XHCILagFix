#include "../src/runtime_layout.hpp"
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace GenericXhci;
namespace {
enum Function { Primary, IO, Iso, Filter, Dispatch, Manual, Next, Init, Allocation, Free, FunctionCount };
struct Fixture {
    uintptr_t cas {},atomicOr {};
    std::array<uintptr_t,FunctionCount> address {};
    std::array<std::vector<uint8_t>,FunctionCount> bytes {};
    Inputs inputs() const {
        std::array<FunctionView,FunctionCount> views {};
        for(size_t i=0;i<FunctionCount;++i)views[i]={bytes[i].data(),bytes[i].size(),address[i]};
        return {views[0],views[1],views[2],views[3],views[4],views[5],views[6],views[7],views[8],views[9],cas,atomicOr};
    }
};
unsigned checks=0;
void require(bool value,const std::string &what) {
    ++checks;
    if(!value){std::cerr<<"FAIL: "<<what<<'\n';std::exit(1);}
}
uint64_t readNumber(std::ifstream &stream,size_t length) {
    uint64_t value=0;
    for(size_t i=0;i<length;++i){char byte=0;stream.get(byte);value|=uint64_t(uint8_t(byte))<<(8*i);}
    require(bool(stream),"fixture contains complete number");return value;
}
Fixture readFixture(const char *path) {
    Fixture f;std::ifstream stream(path,std::ios::binary);char magic[8]{};stream.read(magic,8);
    require(std::string(magic,8)=="XHCLYT01","fixture format");
    f.cas=readNumber(stream,8);f.atomicOr=readNumber(stream,8);
    for(size_t i=0;i<FunctionCount;++i) {
        f.address[i]=readNumber(stream,8);const size_t count=readNumber(stream,4);
        require(count>0&&count<0x10000,"fixture function size");f.bytes[i].resize(count);
        stream.read(reinterpret_cast<char *>(f.bytes[i].data()),count);require(bool(stream),"fixture complete function");
    }
    require(stream.peek()==std::char_traits<char>::eof(),"fixture has no trailing bytes");return f;
}
void put(Fixture &f,Function function,size_t offset,uint32_t value,size_t length=4) {
    require(offset<=f.bytes[function].size()&&length<=f.bytes[function].size()-offset,"mutation is in range");
    for(size_t i=0;i<length;++i)f.bytes[function][offset+i]=uint8_t(value>>(i*8));
}
size_t fragment(const Fixture &f,Function function,const char *hex) {
    const FunctionView view {f.bytes[function].data(),f.bytes[function].size(),f.address[function]};
    size_t at=0;require(Detail::uniqueFragment(view,hex,nullptr,0,at),"reference fragment present");return at;
}
bool accepted(const Fixture &f,Layout *result=nullptr) {
    Layout out {};Error e=Error::None;const bool ok=decode(f.inputs(),out,&e);
    if(result)*result=out;return ok;
}
void reject(const Fixture &f,const std::string &why) {require(!accepted(f),"reject "+why);}
void mutateReject(const Fixture &base,Function function,size_t offset,uint32_t value,size_t length,const char *why) {
    Fixture changed=base;put(changed,function,offset,value,length);reject(changed,why);
}
void baseline(const Fixture &f) {
    Layout out {};Error error=Error::None;
    require(decode(f.inputs(),out,&error),std::string("actual image accepted: ")+errorName(error));
    require(out.controllerRings[0]==0xe10&&out.controllerRings[1]==0xe18&&out.controllerRings[2]==0xe20,"three controller ring offsets");
    require(out.controllerAccessible==0x2b0&&out.controllerSource==0xe28&&out.controllerFlags==0xe70,"controller readiness, source and flags");
    require(out.controllerFilterSlot==0xad0&&out.controllerActionSlot==0xad8&&out.clearSlot==0x160,"native dispatch and ring clear slots");
    require(out.filterSlots[0]==0xbc0&&out.filterSlots[1]==0xbb0&&out.filterSlots[2]==0xba0,"filter role ordering");
    require(out.actionSlots[0]==0xbc8&&out.actionSlots[1]==0xbb8&&out.actionSlots[2]==0xba8,"action role ordering");
    require(out.ringController==0x10&&out.ringIndex==0xc&&out.ringConsumer==0x60&&out.ringBuffer==0x30,"ring fields");
    require(out.ringDescriptor==0x20&&out.ringDMA==0x28&&out.ringBytesSlot==0x2e0,"ring memory descriptor fields");
    require(out.ringEntries==1024&&out.ringBytes==16384&&out.ringInitialConsumer==0x80000000,"ring geometry and cycle");
}
void negative(const Fixture &f) {
    const struct Mutation {Function function;size_t offset;uint32_t value;size_t length;const char *why;} mutations[] = {
        {Primary,0,0x90,1,"changed tiny prologue"},{Primary,7,0xe18,4,"aliased controller roles"},
        {Primary,16,0x168,4,"mismatched clear slot"},{Iso,61,0x2b4,4,"mismatched accessibility field"},
        {Iso,95,0xe28,4,"mismatched iso repeat"},{Filter,64,0xba0,4,"aliased role vtable slots"},
        {Dispatch,64,0xbac,4,"mismatched action pair"},{Manual,48,0xe30,4,"mismatched source repeat"},
        {Next,47,0x64,1,"mismatched consumer CAS field"},{Next,23,0x1e,1,"changed cycle extraction"},
        {Next,32,5,1,"changed TRB stride"},{Next,58,0xffffffffU,4,"capacity overflow"},
        {Next,58,1000,4,"non-power-of-two ring"},{Next,58,8191,4,"oversized ring"},
        {Next,58,7,4,"undersized ring"},{Next,76,0,4,"wrong CAS call target"},
        {Manual,22,0,4,"wrong atomic OR call target"},{Free,75,0x28,1,"free releases wrong descriptor"},
        {Primary,7,0x10008,4,"out-of-bounds controller field"},{Primary,7,0xe11,4,"unaligned controller pointer"},
    };
    for(const auto &m:mutations)mutateReject(f,m.function,m.offset,m.value,m.length,m.why);
    auto input=f.inputs();Layout out {};Error error;
    input.expectedCAS=0;require(!decode(input,out,&error)&&error==Error::AtomicTarget,"missing atomic symbol rejected");
    input=f.inputs();input.manual.address=UINTPTR_MAX-10;require(!decode(input,out,&error),"relocation end overflow rejected");
    input=f.inputs();input.next.address=1;require(!decode(input,out,&error),"relocation subtraction underflow rejected");
    input=f.inputs();input.primaryFilter.bytes=nullptr;require(!decode(input,out,&error),"missing function rejected");
    input=f.inputs();input.primaryFilter.size=0x10001;require(!decode(input,out,&error),"unbounded module view rejected");
    const size_t requiredPrefix[]={24,24,108,112,115,75,138};
    for(size_t function=0;function<7;++function)
        for(size_t length=0;length<requiredPrefix[function];++length) {
            Fixture truncated=f;truncated.bytes[function].resize(length);reject(truncated,"truncated required prefix");
        }
    for(size_t function=7;function<FunctionCount;++function) {
        Fixture truncated=f;truncated.bytes[function].resize(1);reject(truncated,"truncated lifecycle function");
    }
    const size_t init=fragment(f,Init,"488b7b20488b07ff90e0020000be004000004889c7");
    Fixture altered=f;put(altered,Init,init+14,8192);reject(altered,"initialization disagrees with capacity");
    altered=f;altered.bytes[Init].insert(altered.bytes[Init].end(),f.bytes[Init].begin()+init,f.bytes[Init].begin()+init+65);
    reject(altered,"ambiguous duplicated initialization block");
    const size_t erst=fragment(f,Init,"488b7b18488b07ff90e0020000488b4b28488908c7400800040000c7400c00000000");
    altered=f;put(altered,Init,erst+23,512);reject(altered,"ERST capacity disagrees with consumer");
    altered=f;put(altered,Init,erst+16,0x30,1);reject(altered,"ERST points to wrong DMA field");
    altered=f;put(altered,Allocation,16,0x10,1);reject(altered,"constructor initializes wrong ring index");
    altered=f;put(altered,Allocation,38,0x18,1);reject(altered,"constructor initializes wrong controller link");
    altered=f;put(altered,Manual,29,0xe18);put(altered,Manual,48,0xe18);reject(altered,"event source aliases a ring field");
    altered=f;put(altered,Manual,12,0x2b0);reject(altered,"state flags alias accessibility field");
    altered=f;put(altered,Manual,41,0xba0);put(altered,Manual,58,0xba8);reject(altered,"manual dispatcher recursively aliases ring role");
    altered=f;put(altered,Next,105,0x14,1);put(altered,Init,init+52,0x14,1);put(altered,Allocation,16,0x14,1);
    reject(altered,"32-bit ring index overlaps 64-bit controller pointer");
    altered=f;put(altered,Next,11,0xe0,1);put(altered,Next,47,0xe0,1);put(altered,Init,init+45,0xe0,1);
    reject(altered,"negative signed disp8 is not a positive member offset");
    input=f.inputs();input.primaryFilter.bytes=nullptr;out.ringEntries=0xdead;
    require(!decode(input,out,&error)&&out.ringEntries==0xdead,"failure leaves result unchanged");
}
void synthetic(const Fixture &f) {
    Fixture changed=f;
    put(changed,Primary,7,0x1110);put(changed,IO,7,0x1118);put(changed,Iso,78,0x1120);put(changed,Iso,95,0x1120);
    put(changed,Iso,61,0x3c4);put(changed,Filter,29,0x3c4);put(changed,Dispatch,41,0x3c4);
    put(changed,Manual,12,0x1174);put(changed,Manual,29,0x1128);put(changed,Manual,48,0x1128);
    Layout out {};require(accepted(changed,&out),"coherently moved controller fields accepted");
    require(out.controllerRings[0]==0x1110&&out.controllerRings[1]==0x1118&&out.controllerRings[2]==0x1120&&
        out.controllerFlags==0x1174&&out.controllerAccessible==0x3c4,"moved fields actually extracted");
    changed=f;put(changed,Primary,16,0x180);put(changed,IO,16,0x180);put(changed,Iso,87,0x180);put(changed,Iso,104,0x188);
    for(const auto &pair:std::array<std::array<size_t,2>,3>{{{{49,64}},{{64,82}},{{79,92}}}}) {
        const auto old=Detail::little(f.bytes[Filter].data()+pair[0],4);
        put(changed,Filter,pair[0],old+0x20);put(changed,Dispatch,pair[1],old+0x28);
    }
    put(changed,Manual,41,0xaf0);put(changed,Manual,58,0xaf8);
    require(accepted(changed,&out),"coherently moved virtual slots accepted");
    require(out.clearSlot==0x180&&out.filterSlots[2]==0xbc0&&out.controllerFilterSlot==0xaf0,"moved virtual slots extracted");
    // Relocate entire functions and both referenced kernel functions together.
    changed=f;for(auto &address:changed.address)address+=0x20000;changed.cas+=0x20000;changed.atomicOr+=0x20000;
    require(accepted(changed),"load slide accepted without UUID or fixed address");

    const size_t init=fragment(f,Init,"488b7b20488b07ff90e0020000be004000004889c7");
    const size_t erst=fragment(f,Init,"488b7b18488b07ff90e0020000488b4b28488908c7400800040000c7400c00000000");
    const size_t allocation=fragment(f,Allocation,"ba00400000b900400000488b7dd04489ee4d89f1");
    changed=f;
    for(const size_t position: {size_t(11),size_t(47)})put(changed,Next,position,0x70,1);
    put(changed,Next,28,0x40,1);put(changed,Next,95,0x20,1);put(changed,Next,105,0x1c,1);put(changed,Next,115,0x38,1);
    put(changed,Init,init+3,0x30,1);put(changed,Init,init+29,0x30,1);put(changed,Init,init+42,0x40,1);
    put(changed,Init,init+45,0x70,1);put(changed,Init,init+52,0x1c,1);put(changed,Init,init+56,0x20,1);
    put(changed,Init,erst+3,0x28,1);put(changed,Init,erst+16,0x38,1);
    put(changed,Allocation,16,0x1c,1);put(changed,Allocation,38,0x20,1);
    put(changed,Allocation,allocation+28,0x30,1);put(changed,Allocation,allocation+45,0x38,1);
    for(const size_t position: {size_t(12),size_t(27)})put(changed,Free,position,0x20,1);
    for(const size_t position: {size_t(35),size_t(52)})put(changed,Free,position,0x28,1);
    put(changed,Free,56,0x38,1);put(changed,Free,60,0x30,1);put(changed,Free,75,0x30,1);
    require(accepted(changed,&out),"coherently moved ring members accepted");
    require(out.ringController==0x20&&out.ringConsumer==0x70&&out.ringDescriptor==0x30&&out.ringDMA==0x38,"moved ring members extracted");
    changed=f;put(changed,Next,58,511);put(changed,Init,init+14,8192);put(changed,Init,erst+23,512);
    put(changed,Allocation,allocation+1,8192);put(changed,Allocation,allocation+6,8192);
    require(accepted(changed,&out)&&out.ringEntries==512&&out.ringBytes==8192,"coherent alternative ring geometry accepted");
    changed=f;changed.cas=f.address[Next]+80+0x1000;changed.atomicOr=f.address[Manual]+26+0x2000;
    put(changed,Next,76,0x1000);put(changed,Manual,22,0x2000);
    require(accepted(changed),"positive rel32 targets accepted as well as negative ones");
}
}
int main(int argc,char **argv) {
    require(argc==2,"one generated fixture path required");const Fixture f=readFixture(argv[1]);
    baseline(f);negative(f);synthetic(f);
    std::cout<<"PASS ("<<checks<<" assertions; real decoder, negative mutations, synthetic layout changes)\n";
}
