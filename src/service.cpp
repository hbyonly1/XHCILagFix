#include <Headers/plugin_start.hpp>
#include <IOKit/IOService.h>
#include <mach/kmod.h>
#include "fix.hpp"
namespace {
IOService *statusService;
}
namespace XhciLagFix {
void publishStatus() {
    if (!statusService) return;
    const auto s=status();
    statusService->setProperty("Version","0.1.0");statusService->setProperty("Mode","Ring 0");
    statusService->setProperty("Active",s.active);statusService->setProperty("Failure",s.failure);
}
}
class EXPORT XHCILagFix : public IOService {
    OSDeclareDefaultStructors(XHCILagFix)
public:
    IOService *probe(IOService *provider,SInt32 *score) override {
        return ADDPR(startSuccess) ? IOService::probe(provider,score):nullptr;
    }
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        statusService=this;XhciLagFix::publishStatus();registerService();return true;
    }
    void stop(IOService *provider) override {
        statusService=nullptr;IOService::stop(provider);
    }
};
OSDefineMetaClassAndStructors(XHCILagFix,IOService)
extern "C" kern_return_t ADDPR(kern_start)(kmod_info_t*,void*);
extern "C" kern_return_t ADDPR(kern_stop)(kmod_info_t*,void*);
extern "C" kern_return_t _start(kmod_info_t*,void*);
extern "C" kern_return_t _stop(kmod_info_t*,void*);
KMOD_EXPLICIT_DECL(as.local.XHCILagFix,"0.1.0",_start,_stop)
extern "C" {
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain=ADDPR(kern_start);
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain=ADDPR(kern_stop);
__attribute__((visibility("hidden"))) int _kext_apple_cc=__APPLE_CC__;
}
