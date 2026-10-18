#pragma once
#include <stdint.h>
#include <stddef.h>
namespace XhciLagFix {
template<typename T> inline T loadField(const void *object,size_t offset) {
    return *reinterpret_cast<const volatile T*>(reinterpret_cast<uintptr_t>(object)+offset);
}
}
