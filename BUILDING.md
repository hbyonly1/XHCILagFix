# Building

The release directory contains a prebuilt Kext under `Kexts/USBXHCIWakeup.kext`.
To rebuild it, provide compatible checkouts without adding them to the repository:

```sh
export XHCI_KERNEL_SDK=/path/to/MacKernelSDK
export XHCI_LILU_SOURCE=/path/to/Lilu
cp ../research/usb-xhci-wakeup-v0.2.0/build.sh ./build.sh
sh ./build.sh
```

The source uses private AppleUSBXHCI methods and is deliberately fail-closed. A
successful compilation does not establish compatibility with a new macOS build.
Use the supplied offline tests only with extracted, permitted driver images.

The bundled binary was built and checked on the local development machine; it
was not signed, loaded, or installed by the packaging step.
