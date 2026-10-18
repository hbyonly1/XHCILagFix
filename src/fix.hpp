#pragma once
#include <Headers/kern_api.hpp>
namespace XhciLagFix {
struct Status { bool active;const char *failure; };
void install(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size);
Status status();
void publishStatus();
}
