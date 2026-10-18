#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build/objects build/XHCILagFix.kext/Contents/MacOS
kernel_sdk=${XHCI_KERNEL_SDK:-$PWD/deps/MacKernelSDK}
lilu_root=${XHCI_LILU_SOURCE:-$PWD/deps/Lilu}
lilu_headers=$lilu_root/Lilu
test -f "$kernel_sdk/Headers/IOKit/IOService.h"
test -f "$lilu_headers/Headers/plugin_start.hpp"
compile() {
    clang++ -target x86_64-apple-macos11.0 -mkernel -std=c++14 -O2 \
        -fno-exceptions -fno-rtti -fno-builtin -fno-stack-protector -fno-common \
        -mno-red-zone -fno-asynchronous-unwind-tables -nostdinc -nostdinc++ \
        -DKERNEL -DKERNEL_PRIVATE -DLILU_KEXTPATCH_SUPPORT -DLILU_CUSTOM_IOKIT_INIT \
        -DPRODUCT_NAME=XHCILagFix -DMODULE_VERSION=0.1.0 -DKDEBUG_LEVEL=1 \
        -isystem "$kernel_sdk/Headers" -isystem "$lilu_headers" -I src \
        -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations \
        -Wno-inconsistent-missing-override -c "$1" -o "$2"
}
compile src/plugin.cpp build/objects/plugin.o
compile src/fix.cpp build/objects/fix.o
compile src/service.cpp build/objects/service.o
compile "$lilu_headers/Library/plugin_start.cpp" build/objects/plugin_start.o
ld -arch x86_64 -kext -undefined dynamic_lookup -no_fixup_chains \
    -o build/XHCILagFix.kext/Contents/MacOS/XHCILagFix \
    build/objects/plugin.o build/objects/fix.o build/objects/service.o build/objects/plugin_start.o \
    "$kernel_sdk/Library/x86_64/libkmod.a"
python3 - <<'PY'
from pathlib import Path
import plistlib
info={
 'CFBundleIdentifier':'as.local.XHCILagFix','CFBundleExecutable':'XHCILagFix',
 'CFBundleName':'XHCILagFix','CFBundlePackageType':'KEXT',
 'CFBundleVersion':'0.1.0','CFBundleShortVersionString':'0.1.0','OSBundleCompatibleVersion':'0.1.0',
 'OSBundleRequired':'Root',
 'OSBundleLibraries':{'as.vit9696.Lilu':'1.7.3','com.apple.kpi.bsd':'20.0.0','com.apple.kpi.iokit':'20.0.0',
                     'com.apple.kpi.libkern':'20.0.0','com.apple.kpi.mach':'20.0.0','com.apple.kpi.unsupported':'20.0.0'},
 'IOKitPersonalities':{'XHCILagFix':{'CFBundleIdentifier':'as.local.XHCILagFix',
                                    'IOClass':'XHCILagFix','IOProviderClass':'IOResources','IOResourceMatch':'IOKit'}}}
Path('build/XHCILagFix.kext/Contents/Info.plist').write_bytes(plistlib.dumps(info))
PY
file build/XHCILagFix.kext/Contents/MacOS/XHCILagFix
printf '%s\n' 'Built standalone Ring 0 patch; not loaded or hardware-validated.'
