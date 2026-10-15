#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build/objects build/USBXHCIWakeup.kext/Contents/MacOS
kernel_sdk=${XHCI_KERNEL_SDK:?Set XHCI_KERNEL_SDK to a MacKernelSDK checkout}
lilu_root=${XHCI_LILU_SOURCE:?Set XHCI_LILU_SOURCE to a Lilu checkout}
lilu_headers=$lilu_root/Lilu
test -f "$kernel_sdk/Headers/IOKit/IOService.h"
test -f "$lilu_headers/Headers/plugin_start.hpp"
test -f "$lilu_root/hde/hde64.c"
compile_cpp() {
  clang++ -target x86_64-apple-macos11.0 -mkernel -std=c++14 -O2 \
    -fno-exceptions -fno-rtti -fno-builtin -fno-stack-protector -fno-common \
    -mno-red-zone -fno-asynchronous-unwind-tables -nostdinc -nostdinc++ \
    -DKERNEL -DKERNEL_PRIVATE -DLILU_KEXTPATCH_SUPPORT -DLILU_CUSTOM_IOKIT_INIT \
    -DPRODUCT_NAME=USBXHCIWakeup -DMODULE_VERSION=0.2.0 -DKDEBUG_LEVEL=1 \
    -isystem "$kernel_sdk/Headers" -isystem "$lilu_headers" -I "$lilu_root/hde" -I src \
    -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations \
    -Wno-inconsistent-missing-override -c "$1" -o "$2"
}
compile_cpp src/USBXHCIWakeup.cpp build/objects/USBXHCIWakeup.o
compile_cpp "$lilu_headers/Library/plugin_start.cpp" build/objects/plugin_start.o
clang -target x86_64-apple-macos11.0 -mkernel -std=c11 -O2 \
  -fno-builtin -fno-stack-protector -fno-common -mno-red-zone \
  -fno-asynchronous-unwind-tables -nostdinc -DKERNEL -DKERNEL_PRIVATE \
  -Dhde64_disasm=xhciWakeDisasm -Dhde64_table=xhciWakeHdeTable \
  -isystem "$kernel_sdk/Headers" \
  -Wall -Wextra -Werror -Wno-implicit-int-conversion \
  -c "$lilu_root/hde/hde64.c" -o build/objects/hde64.o
ld -arch x86_64 -kext -undefined dynamic_lookup -no_fixup_chains \
  -o build/USBXHCIWakeup.kext/Contents/MacOS/USBXHCIWakeup \
  build/objects/USBXHCIWakeup.o build/objects/plugin_start.o build/objects/hde64.o \
  "$kernel_sdk/Library/x86_64/libkmod.a"
python3 - <<'PY'
from pathlib import Path
import plistlib
info={'CFBundleIdentifier':'as.local.USBXHCIWakeup','CFBundleExecutable':'USBXHCIWakeup',
 'CFBundleName':'USBXHCIWakeup','CFBundlePackageType':'KEXT','CFBundleVersion':'0.2.0',
 'CFBundleShortVersionString':'0.2.0','OSBundleCompatibleVersion':'0.2.0','OSBundleRequired':'Root',
 'OSBundleLibraries':{'as.vit9696.Lilu':'1.7.3','com.apple.kpi.bsd':'20.0.0','com.apple.kpi.iokit':'20.0.0',
  'com.apple.kpi.libkern':'20.0.0','com.apple.kpi.mach':'20.0.0','com.apple.kpi.unsupported':'20.0.0'},
 'IOKitPersonalities':{'USBXHCIWakeup':{'CFBundleIdentifier':'as.local.USBXHCIWakeup',
  'IOClass':'USBXHCIWakeupService','IOProviderClass':'IOResources','IOResourceMatch':'IOKit'}}}
Path('build/USBXHCIWakeup.kext/Contents/Info.plist').write_bytes(plistlib.dumps(info))
PY
file build/USBXHCIWakeup.kext/Contents/MacOS/USBXHCIWakeup
echo 'Built only; not signed, loaded, installed, or hardware-validated.'
