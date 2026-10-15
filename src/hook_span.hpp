#pragma once
#include <stddef.h>
#include <stdint.h>
#include <hde64.h>

// Lilu's trampoline copies instructions without fixing RIP-relative operands.
// Use a private HDE symbol and the same preflight in the Kext and offline tests.
extern "C" unsigned int xhciWakeDisasm(const void *code,hde64s *instruction);

namespace GenericXhci {
enum class HookSpanError : uint8_t {None,MissingInput,Instruction,Relative,Terminator,Truncated};
inline const char *hookSpanErrorName(HookSpanError error) {
    switch(error) {
        case HookSpanError::None:return "none";
        case HookSpanError::MissingInput:return "missing-input";
        case HookSpanError::Instruction:return "instruction";
        case HookSpanError::Relative:return "relative";
        case HookSpanError::Terminator:return "terminator";
        case HookSpanError::Truncated:return "truncated";
    }
    return "unknown";
}
inline bool safeHookSpan(const uint8_t *bytes,size_t available,size_t minimum,size_t &span,HookSpanError *error=nullptr) {
    span=0;if(error)*error=HookSpanError::None;
    auto fail=[&](HookSpanError reason)->bool {if(error)*error=reason;span=0;return false;};
    if(!bytes||!available||(minimum!=5&&minimum!=6&&minimum!=14))return fail(HookSpanError::MissingInput);
    size_t offset=0;
    while(offset<minimum) {
        if(offset>=available)return fail(HookSpanError::Truncated);
        // HDE requires 32 readable bytes even for a one-byte instruction. Never
        // let it read across a function boundary or beyond a mapped text range.
        uint8_t window[32] {};
        const size_t remaining=available-offset;
        const size_t count=remaining<sizeof(window)?remaining:sizeof(window);
        for(size_t i=0;i<count;++i)window[i]=bytes[offset+i];
        hde64s instruction {};
        const unsigned int length=xhciWakeDisasm(window,&instruction);
        if(!length||length>15||length!=instruction.len||(instruction.flags&F_ERROR))return fail(HookSpanError::Instruction);
        if(length>remaining)return fail(HookSpanError::Truncated);
        if((instruction.flags&F_RELATIVE)||
           ((instruction.flags&F_MODRM)&&instruction.modrm_mod==0&&instruction.modrm_rm==5&&!instruction.p_67)||
           (instruction.opcode==0xc7&&instruction.modrm==0xf8))return fail(HookSpanError::Relative);
        const uint8_t op=instruction.opcode;
        if((op>=0xc2&&op<=0xc3)||(op>=0xca&&op<=0xcf)||op==0xf4||op==0xfa||op==0xfb||
           (op==0xff&&instruction.modrm_reg>=2&&instruction.modrm_reg<=5)||
           (op==0x0f&&(instruction.opcode2==0x05||instruction.opcode2==0x07||instruction.opcode2==0x0b||
                         instruction.opcode2==0x34||instruction.opcode2==0x35)))return fail(HookSpanError::Terminator);
        offset+=length;
    }
    span=offset;return true;
}

// A conservative linear first-RET boundary, not complete control-flow recovery
// or a proven function extent. It prevents matching a suffix after a decoded
// return. A changed implementation with an early return, tail-call or different
// branch shape can be rejected. No instruction or branch target is executed.
inline bool firstReturnSpan(const uint8_t *bytes,size_t available,size_t &span) {
    span=0;
    if(!bytes||!available||available>0x10000)return false;
    size_t offset=0,farthestBranch=0;
    while(offset<available) {
        uint8_t window[32] {};
        const size_t remaining=available-offset;
        const size_t count=remaining<sizeof(window)?remaining:sizeof(window);
        for(size_t i=0;i<count;++i)window[i]=bytes[offset+i];
        hde64s instruction {};
        const auto length=xhciWakeDisasm(window,&instruction);
        if(!length||length>15||length!=instruction.len||length>remaining||(instruction.flags&F_ERROR))return false;
        offset+=length;
        const uint8_t op=instruction.opcode;
        if((instruction.flags&F_RELATIVE)&&op!=0xe8) {
            int64_t displacement=0;
            if(instruction.flags&F_IMM8)displacement=instruction.imm.imm8<0x80?instruction.imm.imm8:int64_t(instruction.imm.imm8)-0x100;
            else if(instruction.flags&F_IMM32)displacement=instruction.imm.imm32<0x80000000U?instruction.imm.imm32:int64_t(instruction.imm.imm32)-0x100000000LL;
            else return false;
            const int64_t target=int64_t(offset)+displacement;
            if(target<0||uint64_t(target)>=available)return false;
            if(size_t(target)>farthestBranch)farthestBranch=size_t(target);
        }
        if((op==0xff&&instruction.modrm_reg>=4&&instruction.modrm_reg<=5)||op==0xcc||op==0xcd||op==0xce||
           op==0xcf||op==0xf4||(op==0x0f&&(instruction.opcode2==0x0b||instruction.opcode2==0x07||instruction.opcode2==0x35)))return false;
        if(op==0xc3||op==0xc2){if(farthestBranch>=offset)return false;span=offset;return true;}
    }
    return false;
}

// An Itanium primary vtable ends before the zero metadata of the following
// table group. A matching inherited method in a neighboring table is unrelated.
inline bool findPrimaryVtableSlot(const uintptr_t *addressPoint,size_t availableBytes,uintptr_t target,size_t &slot) {
    if(!addressPoint||!target||availableBytes<sizeof(uintptr_t)||availableBytes>0x10000)return false;
    bool found=false;
    for(size_t offset=0;offset<=availableBytes-sizeof(uintptr_t);offset+=sizeof(uintptr_t)) {
        const uintptr_t entry=addressPoint[offset/sizeof(uintptr_t)];
        if(!entry)break;
        if(entry==target){if(found)return false;found=true;slot=offset;}
    }
    return found;
}
} // namespace GenericXhci
