# XHCILagFix (Ring 0)

Fixes severe USB port latency on some x86_64 macOS platforms, manifested as a mouse that stutters and can barely move, repeated keyboard input, and extremely slow USB storage reads. The patch hooks into AppleUSBXHCI through Lilu and redirects the completion-event targets of all four USB transfer types to **Interrupter 0 / Event Ring 0**.

This repository was previously named USBXHCIWakeup. The new version completely replaces the old polling-based compensation implementation with the standalone **XHCILagFix.kext 0.1.0**: instead of waiting for events to accumulate and then manually waking the driver, it changes the target ring that the hardware will use before requests are submitted.

## Test Status

- **Tested on macOS 15 Sequoia**: AMD Ryzen 9 8945HS / ROG Zephyrus G14 GA403UV. The previously severe USB input latency has improved, and the devices have become usable again.
- **Tested on macOS 26 Tahoe**.

In theory, all newer systems should be supported. Older systems have not been tested, and it is not yet known when Apple made major changes to XHCI.

## Problem Mechanism

Repeated log captures showed that a USB Transfer Event was already valid in Ring 1, but the corresponding XNU interrupt entry often did not execute until about 50 ms later, with some samples taking longer. After entering the XNU interrupt handler, AppleUSBXHCI's Filter, Action, and event consumption usually took only microseconds.

```text
USB transfer completed; the event in Ring 1 is already valid
                 ↓
       Tens of milliseconds of waiting (the culprit!!)
                 ↓
         XNU hardware interrupt entry
                 ↓
       Filter / Action / event consumption
             Usually microseconds
```

Therefore, this project addresses the notification path after completion events are generated. A valid Event does not establish that an MSI write transaction has been sent, and the existing logs alone cannot assign responsibility to MSI, APIC, or the CPU.

## My Hypothesis: Enabling Multiple Interrupters Does Not Mean Interrupter Mapping Is Usable

This approach was inspired by QEMU's xHCI fault and its fix. [QEMU's MSI mapping patch](https://lists.nongnu.org/archive/html/qemu-devel/2024-12/msg03236.html) corrected the modulo mapping from secondary Interrupters to enabled MSI messages, resolving the problem when used with the macOS driver. This fix indicates a compatibility problem between multiple Interrupters and a small number of MSI messages.

[The xHCI 1.2b specification, §4.17.1 Interrupter Mapping](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=263), states:

> If the Number of Interrupters (MaxIntrs) field is greater than 1, then Interrupter Mapping shall be supported.

And:

> If Interrupter Mapping is not supported, the Interrupter Target field shall be ignored by the xHC and all Events targeted at Interrupter 0.

My view is almost the same as the original author's: **On affected platform and system combinations, MaxIntrs > 1 alone does not establish that Interrupter Mapping and the notification paths of secondary Interrupters are actually usable.**

`shall be supported` is a specification requirement for the implementation, while the actual implementation should be assessed according to the hardware's real behavior. In particular, a distinction must be made between:

- **Which Ring an event is mapped to**: Transfer Events have been observed being written to Ring 1 on this machine, so the ring memory cannot be said to be completely unusable.
- **Whether that Interrupter can notify the CPU promptly**: This is the suspicious stage indicated by the long wait.

On this basis, the conclusion is that **the notification paths of secondary Interrupters 1/2 cannot be used** (the exact mechanism remains unclear). There is not yet enough evidence to determine independently whether Ring 2 is faulty. Based on Linux and Windows enabling only Ring 0, the goal of the fix is to bypass Ring 1 and Ring 2.

A single MSI message is not the cause of the error either: the specification allows multiple Interrupters to map to fewer MSI messages, but **the prerequisite is that the mapping mechanism works**. QEMU's modulo fix changes the selection of the notification message; the events remain in their original Ring. Because we cannot observe the internal behavior of real hardware, and could not modify it even if we observed it, this patch instead changes the transfer Target directly, causing these events to go to Ring 0.

## Why Not Simply Change MaxIntrs to 1

In the AppleUSBXHCI version audited through disassembly, the driver requires at least three Interrupters and always initializes three groups with the following roles:

| Interrupter / Ring | Original driver role |
| --- | --- |
| 0 | Primary events such as command completion, port status changes, and MFINDEX Wrap |
| 1 | Transfer events for non-isochronous endpoints such as Control, Bulk, and Interrupt |
| 2 | Transfer events for Isochronous endpoints |

Therefore, simply faking MaxIntrs=1 would conflict with this driver's startup requirements. Our solution is to **preserve the driver's process of creating three Interrupters while setting all transfer targets that can be rewritten to 0.** Ring 1/2 do not need to be disabled, and neither the controller's reported MaxIntrs nor the PCI MSI configuration needs to be modified.

## Specific Changes: Three Hooks

### 1. Regular TransferRing::enqueueRequest

Covers Control, Bulk, and Interrupt requests. Before the original enqueueRequest executes, all prepare subclasses have finished constructing their TDs, but the new request has not yet been linked and published to the controller by the original function.

Traverse the request's complete TD queue and clear the Interrupter Target of valid TRBs:

```cpp
trb.dw2 &= ~0xffc00000U; // DW2[31:22] = Interrupter Target = 0
```

DMA addresses, the lower 22 bits of DW2, DW3, IOC, BEI, Cycle, and queue topology are preserved. Supported types such as Normal, Setup/Data/Status, Link, Event Data, and No Op Transfer are all processed uniformly before submission.

### 2. IsochronousTransferRing::enqueueRequest

The same processing is applied to isochronous requests, covering Isoch TRBs and associated descriptors. Thus, all four USB transfer types fall within this version's rewrite scope, without distinguishing keyboards and mice, USB drives, audio devices, VID/PID, or ports.

The enqueue entry points were chosen instead of addTD because the final TD may be enqueued directly, bypassing addTD, and Control subclasses may continue appending descriptors after the base prepare returns. Processing the complete queue at both enqueue entry points covers these cases in the audited paths.

### 3. CommandRing::setAddress

Some error events that lack a usable target TRB use the fallback Target in the Slot Context. Rewriting transfer TRBs alone does not cover this selection path.

Before Address Device is submitted, the patch creates a private 32-byte copy of the Slot input, sets the copy's Target to 0, and passes it to the original function. The original driver synchronously copies it into its own DMA Input Context, so the stack copy is not directly used as DMA memory for asynchronous hardware access. The original function's cleanup of the source DW3 is preserved, and the source DW2 is not overwritten directly.

The other audited Configure Endpoint paths do not update the Slot Target, and the EP0 Evaluate path does not set A0, which is required to update the Slot, so no redundant Hooks are added for them. Additional Slot Evaluate A0 paths introduced in the future or by third parties are outside the current coverage.

## Generality

- Locates entry points using the runtime symbols of the loaded AppleUSBXHCI.
- Does not check a system build, UUID, or entry-byte whitelist, and does not restrict operation to a particular AMD PCI ID, device, or port.
- Listens only for AppleUSBXHCI and does not require an IOUSBHostFamily UUID match; request rewriting also does not depend on successful endpoint descriptor identification.
- Lilu creates trampolines at the actual entry points. The plugin's version range includes Darwin 25, used by macOS 26, and MinKernel/MaxKernel are left empty in the OpenCore entry. A Lilu version that supports the corresponding system is required; this project's minimum dependency version is 1.7.3.
- Retains checks for null pointers, unpublished requests, queue structure, and TRB types; rewriting occurs only after the entire queue has been validated. Reads the actual TD capacity, allowing at most 256 slots per TD and 256 TDs per request, to avoid out-of-bounds or partial rewrites.

## Installation

1. Place `Kexts/XHCILagFix.kext` in `EFI/OC/Kexts`.
2. Enable the plugin in OpenCore's `Kernel/Add`, after Lilu. See [OpenCore/Kernel-Add.plist](OpenCore/Kernel-Add.plist) for a reference entry; do not use it to replace your entire configuration.
3. Disable the old USBXHCIWakeup, GenericUSBXHCI, or other plugins that fix similar issues, to avoid duplicate rewriting.
4. Reboot for it to take effect.

To disable it, use the boot argument `-xhcilagfixoff`; `-xhciallrouteoff` is also supported. This plugin does not support runtime unloading.

## Checking Whether It Is Active

```sh
ioreg -r -c XHCILagFix -l -w0
```

| Field | Meaning |
| --- | --- |
| Version | Standalone plugin version, currently 0.1.0 |
| Active | All three core Hooks have been installed and rewriting is enabled |
| Failure | Description of a symbol-resolution or trampoline-installation failure; empty on success |

This shows only the loading and Hook installation status. The status is published only when the service starts and when driver loading completes; `Active=Yes` means that the patch is enabled, while the actual effect should be judged by device behavior.

## Project Contents and Validation

```text
src/                    Standalone patch source, 221 lines including comments and blank lines
Kexts/XHCILagFix.kext/   Prebuilt plugin
OpenCore/               Reference Kernel/Add entry
tests/                  Offline queue and original-function forwarding tests
build.sh                Build script
validate.py             Offline validation script
BUILDING.md             Build instructions
validation-summary.json Validation summary
```

Offline tests cover queue rewriting for all four transfer types, different TD capacities, preservation of other bit fields, original-function arguments and return values, private Slot copies, missing symbols, and forwarding after partial installation.

See [BUILDING.md](BUILDING.md) for build instructions.
