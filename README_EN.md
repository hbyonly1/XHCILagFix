<h1 align="center">USBXHCIWakeup</h1>

<p align="center">An experimental USB event compensation patch for delayed AppleUSBXHCI interrupt delivery on macOS</p>

<p align="center"><a href="README.md">简体中文</a> · <a href="README_EN.md">English</a></p>

---

This is an experimental compensation patch for macOS AppleUSBXHCI, targeting cases where USB devices enumerate normally but storage reads and input responses periodically stall. The root mechanism is still unclear. Keep a bootable original EFI and use this project at your own risk.

## Prerequisites

| Item | Test environment |
|---|---|
| Model | ROG Zephyrus G14 (2024) GA403UV |
| CPU / iGPU | AMD Ryzen 9 8945HS; Radeon 780M |
| dGPU | NVIDIA GeForce RTX 4060 Laptop GPU |
| macOS target | x86_64 macOS; mainly tested in Sequoia Recovery / installer environments |
| USB ports | 2× USB 3.2 Gen 2 Type-A (up to 10 Gbps); 1× USB 3.2 Gen 2 Type-C (up to 10 Gbps, DisplayPort); 1× USB4 Type-C (up to 40 Gbps, DisplayPort / PD); plus microSD, HDMI 2.1 and 3.5 mm audio |
| Device mapping | XHC0/XHC1/XHC3/XHC4 use a custom UTBMap; XHC3/XHC4 Type-C ports use Type-C + Switch mapping, while XHC0 keeps external USB3 and internal device ports |

See the [official ASUS GA403 specifications](https://rog.asus.com/us/laptops/rog-zephyrus/rog-zephyrus-g14-2024/spec/). The official page describes the product model; actual device relationships come from this machine's IORegistry and UTBMap.

### Enabled Kexts and parameters

| Component | Version / role |
|---|---|
| Lilu | 1.7.3; base for other plugins |
| GenericUSBXHCI | 1.3.0b2, the Ryzen-compatible GUX-RyzenXHCIFix branch; used for early xHCI probing |
| USBToolBox + UTBMap | USBToolBox 1.2.0 and UTBMap 1.1; UTBMap is customized for this machine |
| USBXHCIWakeup | 0.2.0; this project's fallback event-wakeup path |
| AMDRyzenCPUPowerManagement | 17.0.0 |
| SMCAMDProcessor | 17.0.0 |
| VoodooI2C / VoodooI2CHID | Touchpad and I2C input path |
| VoodooPS2Controller | Keyboard, mouse and compatible input path |
| AsusSMC, WhateverGreen, AppleALC, AMFIPass, RestrictEvents | Platform, graphics, audio and system compatibility |

The key boot arguments in this EFI are:

```text
-v keepsyms=1 debug=0x100 -wegnoegpu -vi2c-force-polling
amfi=0x80 ipc_control_port_options=0 alcid=11
```

`XhciPortLimit=false` is also retained. Firmware IOMMU is disabled.

## Symptoms

On the ROG Zephyrus G14 GA403UV:

- After reboot, the keyboard could randomly develop input delays and duplicate characters; a 2.4 GHz wireless mouse could barely move or click; a USB-C Ethernet adapter could enumerate but had extremely high latency; only the touchpad remained usable;
- Offline installer reads were extremely slow and took more than three hours to enter the system; another user reported almost the same duration;
- Device enumeration and protocols looked normal, IORegistry showed the controller and devices, and the link could report USB 3 at 5 Gbps, while actual requests still stalled for long periods.

The symptoms resemble several AMD OS X community reports. The first concerns a Ryzen 7 6800H / Radeon RX 6850M XT laptop with very slow keyboard/mouse input and almost unusable USB storage during Sonoma installation; users in that discussion suggested it was very likely related to USB4 controller compatibility. The second concerns a Ryzen 9 8945HS Minisforum UM890 Pro and reports extremely slow wireless keyboard/mouse input in Recovery. It also has USB4. These reports suggest a possible AMD Ryzen, USB4/XHCI and macOS-installer combination, but the relationship with USB4 is still unconfirmed:

- [R7 6800H / RX 6850M USB installation issue](https://forum.amd-osx.com/threads/r7-6800h-rx6850m-laptops-having-usb-issues-while-installing-sonoma.4726/)
- [Minisforum UM890 Pro / Ryzen 9 8945HS slow wireless input](https://forum.amd-osx.com/threads/minisforum-um890-pro-amd-ryzen-9-8945hs-w-radeon-780m-graphics.5900/)

## Investigation and conclusion

Through repeated logs, IORegistry inspection, driver disassembly and limited event sampling, the problem was narrowed from “USB devices are slow” to the interrupt-notification path for one completion event. The logs confirmed BOT storage, but the BOT CBW, data and CSW phases were not the cause of the stall.

The boundaries of one read can be represented as:

```text
Block request / SCSI READ
        ↓
BOT: CBW → Data → CSW
        ↓
xHCI Transfer TRB / Event Ring
        ↓
MSI or another interrupt notification
        ↓
XNU interrupt entry
        ↓
AppleUSBXHCI Filter / Action / event consumption
```

The most important timeline was:

```text
USB Transfer Event already written to the Event Ring
        ↓ about 40–50 ms (the main source of all the delay!!)
XNU hardware interrupt entry
        ↓ microseconds
AppleUSBXHCI Filter / Action / event consumption
```

The completion event already existed, and the native driver handled it quickly. The main gap was between the event becoming valid and the XNU interrupt path starting. It still cannot be determined whether the delay occurs before xHCI generates MSI, during PCIe/SoC delivery, between the Local APIC and CPU acceptance, or because another shared interrupt later wakes the other Event Ring activity.

### Relation to QEMU Issue #2705

[QEMU Issue #2705](https://gitlab.com/qemu-project/qemu/-/issues/2705) discusses macOS guests where incorrect xHCI interrupt mapping prevents non-zero Event Ring events from being delivered in time through MSI/MSI-X or pin-based interrupts. The proposed fixes include disabling unsupported Interrupters or routing events to Ring 0 when mapping is unsupported.

The local machine and that issue share a symptom: USB device delays caused by interrupt anomalies. Later investigation confirmed that this machine's three Interrupters (Rings 0, 1 and 2) correctly share CPU Vector 116 through one MSI message according to the xHCI mapping rule, and Ring 1 uses Target=1. The mapping itself is therefore correct. The actual finding was that Ring 1's Transfer Event was already valid but arrived at the XNU interrupt entry about 40–50 ms late, while Filter, Action and event consumption took only microseconds. The correct description is delayed interrupt notification or delivery for Ring 1 completion events, rather than an Interrupter mapping error; the exact MSI generation and delivery stage was not directly observed.

After repeated logs, disassembly and reboots, the cause could only be localized roughly to this boundary. Further sampling still lacked an available timestamp for the actual MSI write transaction, and continued communication with AI produced little new progress. The investigation stops here. This patch is a symptom workaround, not a root fix for the hardware mechanism.

## Current workaround

Because only the touchpad remained usable, and inspired by its `-vi2c-force-polling` approach, I also adopted polling to bypass the interrupt-notification problem.

The patch keeps the normal MSI path and adds a fallback wake path:

1. Check the real Event Ring on each controller's original workloop;
2. Confirm that a valid event remains unconsumed;
3. Request a check every **100 µs**, triggering compensation after **200 µs** of stable dwell;
4. Call AppleUSBXHCI's own `manuallyProcessInterrupts()` on the original workloop;
5. Let the native driver consume events and perform normal ERDP/EHB updates.

The 100 µs check interval and 200 µs dwell interval were chosen from current testing, and this setting worked well in those tests. The patch does not change Interrupter Target, MSI/MSI-X, IMAN, IMOD or interrupt configuration, and does not move the Ring consumer. Unknown private ABIs, object relationships or lifecycle overrides fail closed.

## Unknown impact

The following are still unproven:

- CPU usage and power cost of the 100 µs check interval on every machine;
- Stability during sleep, wake, hot-plug and multi-controller workloads;
- Whether manual compensation can trigger an undiscovered driver lock interaction;
- Whether long runs or abnormal power loss can expose a data-consistency problem.

Repeated MD5 values for large-file reads show that USB reads are stable and repeatable. The native driver still consumes the Event Ring; the patch does not fabricate events or rewrite MSI configuration.

## Current test feedback

According to tests on this machine only:

- USB read speed returned to normal practical USB 3.0 behavior; offline installer image loading fell from more than three hours to around three minutes, and the system was successfully installed and entered;
- 2.4 GHz wireless mouse and keyboard responsiveness returned; USB-C Ethernet became usable and ping latency was normal;
- USB-A and USB-C ports appeared to work normally;
- The USB 3.0 SG Flash boot disk, which previously exited abnormally, can now be read.

## Scope

- macOS 11 or later, x86_64, AppleUSBXHCI;
- Lilu 1.7.3 or a compatible version, loaded before this Kext;
- not guaranteed for every AMD/Intel controller or macOS build; unknown private ABIs are safely rejected;
- 100 µs is a requested polling interval, not a hard real-time guarantee; CPU usage, sleep/wake, hot-plug and multi-controller workloads need further testing.

The patch starts automatically. Use `-xhciwakeupoff` to disable it or restore the original EFI.

## Applying the patch

1. Back up the current EFI and copy `Kexts/USBXHCIWakeup.kext` to `EFI/OC/Kexts/`.
2. Add the entry from `OpenCore/Kernel-Add.plist` to `config.plist → Kernel → Add`, keep it after Lilu, and enable it.

## Repository contents

- `Kexts/USBXHCIWakeup.kext`: built x86_64 Kext;
- `OpenCore/Kernel-Add.plist`: Kernel → Add fragment;
- `collector/`: read-only status export and start/stop tool;
- `src/`, `tests/`, `build.sh`: source and offline validation material;
- `licenses/`: Lilu, MacKernelSDK and HDE64 license texts;
- `validation-summary.json`: build and offline validation summary.

The collector is not required to enable the patch. It only reports per-controller state, wake calls and progress counters.

## Validation boundary

`manuallyProcessInterrupts()` follows the native completion path, so the native driver may update hardware state registers. The patch does not directly rewrite those registers. Lock interactions in extreme native-callback lifecycle interleavings have not been exhaustively verified; keep a bootable original EFI and rollback option.

