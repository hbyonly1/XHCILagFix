#include <Headers/plugin_start.hpp>
#include "fix.hpp"
namespace XhciLagFix {
static const char *paths[]={"/System/Library/Extensions/IOUSBHostFamily.kext/Contents/PlugIns/AppleUSBXHCI.kext"};
static KernelPatcher::KextInfo modules[]={{"com.apple.driver.usb.AppleUSBXHCI",paths,1,{true}}};
static bool attempted;
static void loaded(void*,KernelPatcher &patcher,size_t id,mach_vm_address_t address,size_t size) {
    if (attempted || modules[0].loadIndex!=id) return;
    attempted=true;install(patcher,id,address,size);publishStatus();
    const auto s=status();
    SYSLOG("xhcilagfix","Ring 0 active=%d, failure=%s",s.active,s.failure);
}
static void initialize() {
    const auto error=lilu.onKextLoad(modules,1,loaded,nullptr);
    if (error!=LiluAPI::Error::NoError) SYSLOG("xhcilagfix","module callback registration failed: %d",int(error));
}
}
static const char *disableArguments[]={"-xhcilagfixoff","-xhciallrouteoff"};
PluginConfiguration ADDPR(config)={
    "XHCILagFix",parseModuleVersion("0.1.0"),LiluAPI::AllowNormal|LiluAPI::AllowInstallerRecovery,
    disableArguments,2,nullptr,0,nullptr,0,KernelVersion::BigSur,
    static_cast<KernelVersion>(0x7fffffff),XhciLagFix::initialize
};
