#include "../src/hook_span.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace GenericXhci;
namespace {
unsigned checks=0;
void require(bool value,const std::string &description) {
    ++checks;if(!value){std::cerr<<"FAIL: "<<description<<'\n';std::exit(1);}
}
uint64_t readNumber(std::ifstream &stream,size_t length) {
    uint64_t result=0;
    for(size_t i=0;i<length;++i){char c=0;stream.get(c);result|=uint64_t(uint8_t(c))<<(8*i);}
    require(bool(stream),"fixture number complete");return result;
}
void expectRejected(const std::vector<uint8_t> &bytes,const char *why) {
    for(const size_t minimum:{size_t(5),size_t(6),size_t(14)}) {
        size_t span=0xdead;HookSpanError error=HookSpanError::None;
        require(!safeHookSpan(bytes.data(),bytes.size(),minimum,span,&error)&&span==0&&error!=HookSpanError::None,why);
    }
}
void synthetic() {
    const std::vector<std::vector<uint8_t>> unsafe = {
        {0x48,0x8b,0x05,0,0,0,0,0x90,0x90,0x90,0x90,0x90,0x90,0x90}, // RIP-relative memory
        {0xe8,0,0,0,0,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}, // relative call
        {0xe9,0,0,0,0,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}, // relative branch
        {0x75,0,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}, // conditional branch
        {0xff,0xe0,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}, // indirect branch
        {0xff,0xd0,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}, // indirect call
        {0xc3,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}, // early return
        {0xcc,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}, // trap
        {0x0f,0x0b,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}, // UD2
        {0xc7,0xf8,0,0,0,0,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}, // XBEGIN
        {0x48,0x8b}, // truncated instruction; HDE must only see a padded stack buffer
        std::vector<uint8_t>(32,0x66), // invalid overlong instruction
    };
    for(const auto &bytes:unsafe)expectRejected(bytes,"unsafe movable prefix rejected");
    const std::vector<uint8_t> safe {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,0x48,0x8b,0x47,0x10,0x90,0x90};
    for(const size_t minimum:{size_t(5),size_t(6),size_t(14)}) {
        size_t span=0;require(safeHookSpan(safe.data(),safe.size(),minimum,span),"position-independent prefix accepted");
        require(span==(minimum==14?14:8),"whole instruction boundary recovered");
    }
    const std::vector<uint8_t> absolute32 {0x67,0x48,0x8b,0x05,0x34,0x12,0,0,0x90,0x90,0x90,0x90,0x90,0x90};
    size_t span=0;require(safeHookSpan(absolute32.data(),absolute32.size(),14,span),"address-size override is not RIP relative");
    require(!safeHookSpan(nullptr,14,14,span),"null pointer rejected");
    require(!safeHookSpan(safe.data(),safe.size(),13,span),"unsupported patch size rejected");
}
}
int main(int argc,char **argv) {
    require(argc==2,"one generated fixture required");std::ifstream stream(argv[1],std::ios::binary);
    char magic[8]{};stream.read(magic,8);require(std::string(magic,8)=="XHCHOOK1","hook fixture format");
    const size_t count=readNumber(stream,4);require(count==13,"thirteen actual routed functions");
    for(size_t i=0;i<count;++i) {
        const size_t nameLength=readNumber(stream,2);std::string name(nameLength,'\0');stream.read(&name[0],nameLength);
        const size_t size=readNumber(stream,4);require(size>=14&&size<=0x10000,"real function bound");
        std::vector<uint8_t> bytes(size);stream.read(reinterpret_cast<char *>(bytes.data()),size);require(bool(stream),"hook function complete");
        std::cout<<name<<" spans=";
        for(const size_t minimum:{size_t(5),size_t(6),size_t(14)}) {
            size_t span=0;HookSpanError error=HookSpanError::None;
            require(safeHookSpan(bytes.data(),bytes.size(),minimum,span,&error),name+" movable prefix: "+hookSpanErrorName(error));
            require(span>=minimum&&span<=minimum+14,"whole instruction span bounded");
            std::cout<<span<<(minimum==14?'\n':'/');
            // Every shorter view must fail instead of reading beyond the
            // caller's function bound, including cuts inside an instruction.
            for(size_t available=0;available<span;++available) {
                size_t shortSpan=0;
                require(!safeHookSpan(bytes.data(),available,minimum,shortSpan),"truncated real prologue rejected");
            }
        }
    }
    require(stream.peek()==std::char_traits<char>::eof(),"fixture has no trailing data");synthetic();
    std::cout<<"PASS ("<<checks<<" assertions; 13 real hooks at 5/6/14-byte route sizes)\n";
}
