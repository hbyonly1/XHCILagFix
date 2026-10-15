#!/usr/bin/env python3
"""Verify the shared trampoline preflight on the actual thirteen routed methods."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from run_runtime_layout import Image

HOOKS = {
    'com.apple.driver.usb.AppleUSBXHCI': (
        '__ZN23AppleUSBXHCIInterrupter20getNextEventInternalEv',
        '__ZN12AppleUSBXHCI20lowerOnePowerStateToEm',
        '__ZN12AppleUSBXHCI20raiseOnePowerStateToEm',
        '__ZN12AppleUSBXHCI5resetEv',
        '__ZN12AppleUSBXHCI19stopThreadCallGatedEv',
        '__ZN12AppleUSBXHCI4freeEv',
        '__ZN23AppleUSBXHCIInterrupter13initRegistersEv',
        '__ZN23AppleUSBXHCIInterrupter16discardRegistersEv',
        '__ZN23AppleUSBXHCIInterrupter4freeEv',
    ),
    'com.apple.driver.usb.AppleUSBXHCIPCI': (
        '__ZN15AppleUSBXHCIPCI20lowerOnePowerStateToEm',
        '__ZN15AppleUSBXHCIPCI20raiseOnePowerStateToEm',
        '__ZN15AppleUSBXHCIPCI19stopThreadCallGatedEv',
        '__ZN15AppleUSBXHCIPCI4freeEv',
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image_folders', nargs='*', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    folders = args.image_folders
    if not folders:
        raise SystemExit('pass one or more extracted driver image directories')
    here = Path(__file__).resolve().parent
    hde = Path(os.environ['XHCI_LILU_SOURCE']) / 'hde'
    reports = []
    with tempfile.TemporaryDirectory(prefix='xhci-hooks-test-') as directory:
        directory = Path(directory)
        obj = directory / 'hde.o'
        executable = directory / 'test-hook-spans'
        flags = ['-O2', '-fsanitize=address,undefined']
        # HDE is a portable byte decoder; enable its x86-64 table on an ARM host
        # too. The tested x86 instructions are data, never executed.
        subprocess.run(['clang', *flags, '-fno-sanitize=alignment', '-D_M_X64=1', '-Dhde64_disasm=xhciWakeDisasm', '-Dhde64_table=xhciWakeHdeTable', '-c', str(hde / 'hde64.c'), '-o', str(obj)], check=True)
        subprocess.run(['clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror', *flags, '-I', str(hde), str(here / 'test_hook_spans.cpp'), str(obj), '-o', str(executable)], check=True)
        for index, folder in enumerate(folders):
            output = bytearray(b'XHCHOOK1') + struct.pack('<I', sum(map(len, HOOKS.values())))
            evidence = []
            for module, names in HOOKS.items():
                image = Image(folder / (module + '.macho'))
                for name in names:
                    address, data = image.function(name)
                    name_bytes = name.encode()
                    output += struct.pack('<H', len(name_bytes)) + name_bytes + struct.pack('<I', len(data)) + data
                    evidence.append({'module': module, 'symbol': name, 'address': hex(address), 'function_size': len(data)})
            fixture = directory / f'fixture-{index}.bin'
            fixture.write_bytes(output)
            result = subprocess.run([str(executable), str(fixture)], text=True, capture_output=True)
            if result.returncode:
                raise RuntimeError(f'{folder}: {result.stdout}{result.stderr}')
            print(folder.name + ': ' + result.stdout.splitlines()[-1])
            reports.append({'folder': str(folder), 'functions': evidence, 'test_output': result.stdout.strip()})
    if args.report:
        args.report.write_text(json.dumps({'preflight': 'src/hook_span.hpp', 'route_sizes': [5, 6, 14], 'sanitizers': ['address', 'undefined'], 'hde_alignment_check': 'disabled only for upstream HDE intentional unaligned loads; enabled for C++ helper', 'hardware_executed': False, 'images': reports}, indent=2) + '\n')


if __name__ == '__main__':
    main()
