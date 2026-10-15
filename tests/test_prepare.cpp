#include "../src/runtime_layout.hpp"
#include "../src/hook_span.hpp"
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace GenericXhci;
namespace {
unsigned checks=0;
void require(bool value,const std::string &why) {++checks;if(!value){std::cerr<<"FAIL: "<<why<<'\n';std::exit(1);}}
uint64_t number(std::ifstream &stream,size_t length) {
    uint64_t value=0;for(size_t i=0;i<length;++i){char c=0;stream.get(c);value|=uint64_t(uint8_t(c))<<(8*i);}
    require(bool(stream),"fixture number complete");return value;
}
struct Fixture {
    uintptr_t cas {},atomicOr {};
    std::array<uintptr_t,10> addresses {};
    std::array<bool,10> untilReturn {};
    std::array<std::vector<uint8_t>,10> bytes {};
    bool inputs(Inputs &result,bool bounded=true) const {
        std::array<FunctionView,10> views {};
        for(size_t i=0;i<10;++i) {
            size_t span=bytes[i].size();
            if(bounded&&untilReturn[i]&&!firstReturnSpan(bytes[i].data(),span,span))return false;
            views[i]={bytes[i].data(),span,addresses[i]};
        }
        result={views[0],views[1],views[2],views[3],views[4],views[5],views[6],views[7],views[8],views[9],cas,atomicOr};return true;
    }
};
bool accepted(const Fixture &f,bool bounded=true) {
    Inputs in {};Layout out {};return f.inputs(in,bounded)&&decode(in,out);
}
void fakeSuffix(Fixture f) {
    // These windows still contain the original, perfectly matching fragments.
    // Such a fragment must stop being evidence once a RET precedes it.
    f.bytes[7][0]=0xc3;
    require(accepted(f,false),"synthetic unbounded initializer still contains matching fragments");
    require(!accepted(f),"initializer fragment after first RET rejected");
    f.bytes[7][0]=0x55;f.bytes[8][39]=0xc3;
    require(accepted(f,false),"synthetic unbounded allocation still contains matching fragments");
    require(!accepted(f),"allocation fragment after first RET rejected");
}
void synthetic() {
    size_t span=99;
    const uint8_t immediateRet[]={0x48,0xb8,0xc3,0,0,0,0,0,0,0,0xc3,0x90};
    require(firstReturnSpan(immediateRet,sizeof(immediateRet),span)&&span==11,"RET byte inside immediate is not a boundary");
    const uint8_t retPop[]={0x90,0xc2,0x10,0,0x90};
    require(firstReturnSpan(retPop,sizeof(retPop),span)&&span==4,"RET imm16 includes its complete instruction");
    require(!firstReturnSpan(retPop,3,span)&&span==0,"truncated RET operand rejected");
    const uint8_t noReturn[]={0x90,0x90,0x90};
    require(!firstReturnSpan(noReturn,sizeof(noReturn),span)&&span==0,"missing return rejected");
    require(!firstReturnSpan(nullptr,3,span),"null scan rejected");
    const uint8_t tailOutside[]={0xe9,0x7f,0,0,0,0xc3};
    require(!firstReturnSpan(tailOutside,sizeof(tailOutside),span),"branch outside bounded window rejected");
    const uint8_t branchAfterReturn[]={0x75,2,0xc3,0x90,0xc3};
    require(!firstReturnSpan(branchAfterReturn,sizeof(branchAfterReturn),span),"first return does not hide a later branch target");
    const uint8_t indirectTail[]={0xff,0xe0,0x90,0xc3};
    require(!firstReturnSpan(indirectTail,sizeof(indirectTail),span),"indirect tail cannot masquerade as linear function");
    const uintptr_t adjacent[]={11,22,0,11,22};
    require(findPrimaryVtableSlot(adjacent,sizeof(adjacent),11,span)&&span==0,"matching neighbor does not duplicate primary slot");
    require(findPrimaryVtableSlot(adjacent,sizeof(adjacent),22,span)&&span==8,"primary slot offsets are address-point relative");
    const uintptr_t suffixOnly[]={11,0,22};
    require(!findPrimaryVtableSlot(suffixOnly,sizeof(suffixOnly),22,span),"match in neighbor cannot satisfy primary vtable");
    const uintptr_t duplicate[]={11,11,0};
    require(!findPrimaryVtableSlot(duplicate,sizeof(duplicate),11,span),"duplicate inside primary table rejected");
    require(!findPrimaryVtableSlot(adjacent,7,11,span),"short table bound rejected");
    require(!findPrimaryVtableSlot(nullptr,8,11,span),"missing table rejected");
    require(!findPrimaryVtableSlot(adjacent,sizeof(adjacent),0,span),"zero metadata cannot be a target");
    require(!findPrimaryVtableSlot(adjacent,size_t(-1),11,span),"unbounded table input rejected");
}
}
int main(int argc,char **argv) {
    require(argc==2,"one prepared fixture required");std::ifstream stream(argv[1],std::ios::binary);
    char magic[8]{};stream.read(magic,8);require(std::string(magic,8)=="XHCPRP01","prepare fixture format");
    Fixture f;f.cas=number(stream,8);f.atomicOr=number(stream,8);
    for(size_t i=0;i<10;++i) {
        f.addresses[i]=number(stream,8);const size_t count=number(stream,4);f.untilReturn[i]=number(stream,1);
        require(count>0&&count<=0x10000,"core view bound");f.bytes[i].resize(count);
        stream.read(reinterpret_cast<char *>(f.bytes[i].data()),count);require(bool(stream),"core view complete");
    }
    Inputs in {};Layout out {};Error error=Error::None;
    require(f.inputs(in),"actual core first-return scans succeed");
    require(decode(in,out,&error),std::string("actual core view limits decode: ")+errorName(error));
    std::cout<<"actual core init span="<<in.initRegisters.size<<" allocation span="<<in.initWithParameters.size<<'\n';
    require(in.initRegisters.size==433&&in.initWithParameters.size==300,"instruction-aligned real init and allocation RET boundaries");
    const size_t tables=number(stream,4);require(tables==2,"two actual primary vtables");
    for(size_t i=0;i<tables;++i) {
        const size_t count=number(stream,4);require(count>0&&count<=512,"table mapped bound");
        std::vector<uintptr_t> entries(count);for(auto &value:entries)value=number(stream,8);
        const size_t targets=number(stream,4);require(targets>0&&targets<=16,"target count bound");
        for(size_t j=0;j<targets;++j) {
            const auto target=number(stream,8);size_t slot=0;
            require(findPrimaryVtableSlot(entries.data(),entries.size()*sizeof(uintptr_t),target,slot),"real primary lifecycle/manual slot uniquely found");
            require(entries[slot/sizeof(uintptr_t)]==target,"slot points at requested actual method");
        }
    }
    require(stream.peek()==std::char_traits<char>::eof(),"fixture ends cleanly");fakeSuffix(f);synthetic();
    std::cout<<"PASS ("<<checks<<" assertions; actual core view limits, RET suffix rejection, primary vtable boundaries)\n";
}
