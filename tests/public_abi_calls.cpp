#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
extern "C" {
IOWorkLoop* serviceLoop(IOService *s){return s->getWorkLoop();}
bool loopInGate(IOWorkLoop *s){return s->inGate();}
bool sourceEnabled(IOFilterInterruptEventSource *s){return s->isEnabled();}
IOWorkLoop* sourceLoop(IOFilterInterruptEventSource *s){return s->getWorkLoop();}
IOEventSource::Action sourceAction(IOFilterInterruptEventSource *s){return s->getAction();}
IOFilterInterruptEventSource::Filter sourceFilter(IOFilterInterruptEventSource *s){return s->getFilterAction();}
IOByteCount descriptorLength(IOBufferMemoryDescriptor *s){return s->getLength();}
void* descriptorBytes(IOBufferMemoryDescriptor *s){return s->getBytesNoCopy();}
IOByteCount mapLength(IOMemoryMap *s){return s->getLength();}
IOVirtualAddress mapAddress(IOMemoryMap *s){return s->getVirtualAddress();}
IOReturn timerTimeout(IOTimerEventSource *s){return s->setTimeoutUS(100);}
void timerCancel(IOTimerEventSource *s){s->cancelTimeout();}
}
