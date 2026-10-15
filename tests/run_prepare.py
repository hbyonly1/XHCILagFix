#!/usr/bin/env python3
"""Audit the live core's actual prepare symbols, view limits and vtable boundary.

Limits and symbol names are read from USBXHCIWakeup.cpp, then the real shared
C++ helpers decode saved image windows of those exact lengths. This catches
errors that testing runtime_layout.hpp with full functions cannot reveal.
"""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from run_runtime_layout import FUNCTIONS, Image

FIELDS = ('primaryFilter', 'ioFilter', 'isoFilter', 'filter', 'dispatch', 'manual',
          'next', 'initRegisters', 'initWithParameters', 'ringFree')


def core_contract(source):
    text = source.read_text()
    hooks = [item.strip() for item in re.search(r'enum Hook\s*:\s*unsigned\s*\{([^}]+)\}', text, re.S)[1].split(',')]
    symbols = re.findall(r'"([^"]+)"', re.search(r'static const char \*symbols\[HookCount\]\s*=\s*\{([^}]+)\}', text, re.S)[1])
    names = dict(zip(hooks, symbols))
    rows = re.findall(r'in\.(\w+)\s*=\s*view\(("[^"]+"|symbols\[\w+\]),\s*(\d+)(?:,\s*(true|false))?\);', text)
    views = {}
    for field, symbol, limit, until_return in rows:
        if symbol.startswith('"'):
            symbol = symbol.strip('"')
        else:
            symbol = names[symbol[len('symbols['):-1]]
        views[field] = {'symbol': symbol, 'limit': int(limit), 'until_return': until_return == 'true'}
    if set(views) != set(FIELDS):
        raise RuntimeError('Cannot identify all ten actual core prepare views')
    for field, expected in zip(FIELDS, FUNCTIONS):
        if views[field]['symbol'] != expected:
            raise RuntimeError(f'core {field}: actual symbol is wrong: {views[field]["symbol"]}')
    if not all(views[name]['until_return'] for name in ('initRegisters', 'initWithParameters')):
        raise RuntimeError('Core initialization views must stop before scanning the next function')
    if 'GenericXhci::firstReturnSpan' not in text:
        raise RuntimeError('Core prepare must use the tested shared firstReturnSpan helper')
    if 'GenericXhci::findPrimaryVtableSlot' not in text:
        raise RuntimeError('Core slotFor must use the tested shared first-zero vtable helper')
    return [views[field] for field in FIELDS]


def pointer(raw):
    if not raw or raw >= 0xffffff8000000000:
        return raw
    # The extracted BootKC retains the x86 kernel-cache chained encoding.
    return (raw & 0x3fffffff) + 0xffffff8000100000


def make_fixture(folder, views, destination):
    image = Image(folder / 'com.apple.driver.usb.AppleUSBXHCI.macho')
    kernel = Image(folder / 'com.apple.kernel.macho')
    output = bytearray(b'XHCPRP01') + struct.pack('<QQ', kernel.symbols['_OSCompareAndSwap'][0], kernel.symbols['_OSBitOrAtomic'][0])
    for item in views:
        address, section = image.symbols[item['symbol']]
        _, section_start, section_size = image.sections[section]
        count = min(item['limit'], section_start + section_size - address)
        output += struct.pack('<QIB', address, count, item['until_return']) + image.read(address, count)
    tables = (
        ('__ZTV12AppleUSBXHCI', (FUNCTIONS[5],
            '__ZN12AppleUSBXHCI20lowerOnePowerStateToEm', '__ZN12AppleUSBXHCI20raiseOnePowerStateToEm',
            '__ZN12AppleUSBXHCI5resetEv', '__ZN12AppleUSBXHCI19stopThreadCallGatedEv', '__ZN12AppleUSBXHCI4freeEv')),
        ('__ZTV23AppleUSBXHCIInterrupter', (FUNCTIONS[6], FUNCTIONS[7],
            '__ZN23AppleUSBXHCIInterrupter16discardRegistersEv', FUNCTIONS[9])),
    )
    evidence = []
    output += struct.pack('<I', len(tables))
    for table, targets in tables:
        address = image.symbols[table][0] + 16
        entries = [pointer(value) for value in struct.unpack('<512Q', image.read(address, 0x1000))]
        output += struct.pack('<I', len(entries)) + struct.pack('<512Q', *entries)
        output += struct.pack('<I', len(targets))
        for name in targets:
            output += struct.pack('<Q', image.symbols[name][0])
        boundary = next((index for index, value in enumerate(entries) if not value), len(entries))
        evidence.append({'symbol': table, 'primary_entries': boundary,
                         'targets': [{'symbol': name, 'inside_primary': sum(value == image.symbols[name][0] for value in entries[:boundary]),
                                      'after_boundary': sum(value == image.symbols[name][0] for value in entries[boundary:])} for name in targets]})
    destination.write_bytes(output)
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image_folders', nargs='*', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    views = core_contract(here.parent / 'src/USBXHCIWakeup.cpp')
    folders = args.image_folders
    if not folders:
        raise SystemExit('pass one or more extracted driver image directories')
    hde = Path(os.environ['XHCI_LILU_SOURCE']) / 'hde'
    report = []
    with tempfile.TemporaryDirectory(prefix='xhci-prepare-test-') as directory:
        directory = Path(directory)
        obj, executable = directory / 'hde.o', directory / 'test-prepare'
        flags = ['-O2', '-fsanitize=address,undefined']
        # Upstream HDE intentionally performs x86 unaligned scalar loads. Keep
        # ASan and other UB checks for it; alignment checks stay enabled for our
        # C++ helpers, where no such accesses are needed.
        subprocess.run(['clang', *flags, '-fno-sanitize=alignment', '-D_M_X64=1', '-Dhde64_disasm=xhciWakeDisasm', '-Dhde64_table=xhciWakeHdeTable', '-c', str(hde / 'hde64.c'), '-o', str(obj)], check=True)
        subprocess.run(['clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror', *flags, '-I', str(hde), str(here / 'test_prepare.cpp'), str(obj), '-o', str(executable)], check=True)
        for index, folder in enumerate(folders):
            fixture = directory / f'fixture-{index}.bin'
            tables = make_fixture(folder, views, fixture)
            result = subprocess.run([str(executable), str(fixture)], text=True, capture_output=True)
            if result.returncode:
                raise RuntimeError(f'{folder}: {result.stdout}{result.stderr}')
            print(folder.name + ': ' + result.stdout.splitlines()[-1])
            report.append({'folder': str(folder), 'vtables': tables, 'test_output': result.stdout.strip()})
    if args.report:
        args.report.write_text(json.dumps({'core_views': views, 'hardware_executed': False, 'sanitizers': ['address', 'undefined'], 'hde_alignment_check': 'disabled only for upstream HDE intentional unaligned loads; enabled for C++ helpers', 'images': report}, indent=2) + '\n')


if __name__ == '__main__':
    main()
