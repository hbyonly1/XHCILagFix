#pragma once
#include <stdint.h>
#include <stddef.h>
using mach_vm_address_t=uint64_t;
class KernelPatcher {
public:
    // Match the real exported Lilu x86_64 ABI including `from`, not a reduced
    // mock-only layout. Patching and trampolines remain deliberately mocked.
    struct RouteRequest {
        const char *symbol {nullptr};
        mach_vm_address_t from {0};
        const mach_vm_address_t to {0};
        mach_vm_address_t *org {nullptr};
        template<typename T> RouteRequest(const char *s,T t,mach_vm_address_t &o):symbol(s),to(t),org(&o) {}
    };
    mach_vm_address_t solveSymbol(size_t,const char *);
    bool routeMultiple(size_t,RouteRequest*,size_t,mach_vm_address_t,size_t,bool,bool);
    void clearError() {}
};
static_assert(sizeof(KernelPatcher::RouteRequest)==32,"Real x86_64 Lilu RouteRequest ABI");
static_assert(offsetof(KernelPatcher::RouteRequest,from)==8,"Real RouteRequest from offset");
static_assert(offsetof(KernelPatcher::RouteRequest,to)==16,"Real RouteRequest destination offset");
static_assert(offsetof(KernelPatcher::RouteRequest,org)==24,"Real RouteRequest original offset");
