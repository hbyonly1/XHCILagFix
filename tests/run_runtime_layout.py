#!/usr/bin/env python3
"""Compile the real C++ decoder and test it against locally supplied Mach-O images.

No Apple image bytes are redistributed. The fixtures are created in a temporary
directory from the two locally extracted KCs, then deleted when the run ends.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

FUNCTIONS = (
    '__ZN12AppleUSBXHCI22primaryFilterInterruptEP28IOFilterInterruptEventSource',
    '__ZN12AppleUSBXHCI17ioFilterInterruptEP28IOFilterInterruptEventSource',
    '__ZN12AppleUSBXHCI26isochronousFilterInterruptEP28IOFilterInterruptEventSource',
    '__ZN12AppleUSBXHCI14filterOccurredEP28IOFilterInterruptEventSource',
    '__ZN12AppleUSBXHCI17interruptOccurredEP22IOInterruptEventSourcei',
    '__ZN12AppleUSBXHCI25manuallyProcessInterruptsEv',
    '__ZN23AppleUSBXHCIInterrupter20getNextEventInternalEv',
    '__ZN23AppleUSBXHCIInterrupter13initRegistersEv',
    '__ZN23AppleUSBXHCIInterrupter18initWithParametersEP12AppleUSBXHCIjP8IOMapper',
    '__ZN23AppleUSBXHCIInterrupter4freeEv',
)


class Image:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        if self.data[:4] != b'\xcf\xfa\xed\xfe':
            raise ValueError(f'Not a little-endian Mach-O 64 image: {path}')
        self.segments, self.sections, self.symbols = [], {}, {}
        cursor, section_index, symtab = 32, 1, None
        for _ in range(struct.unpack_from('<I', self.data, 16)[0]):
            cmd, size = struct.unpack_from('<II', self.data, cursor)
            if cmd == 0x19:
                self.segments.append(struct.unpack_from('<QQQQ', self.data, cursor + 24))
                count = struct.unpack_from('<I', self.data, cursor + 64)[0]
                for j in range(count):
                    start = cursor + 72 + 80 * j
                    name = self.data[start:start + 16].rstrip(b'\0')
                    address, length = struct.unpack_from('<QQ', self.data, start + 32)
                    self.sections[section_index] = (name, address, length)
                    section_index += 1
            elif cmd == 2:
                symtab = struct.unpack_from('<IIII', self.data, cursor + 8)
            cursor += size
        if symtab is None:
            raise ValueError(f'No symbol table: {path}')
        symoff, count, stroff, strsize = symtab
        strings = self.data[stroff:stroff + strsize]
        for j in range(count):
            string_index, kind, section, _, value = struct.unpack_from('<IBBHQ', self.data, symoff + 16 * j)
            if kind & 0xe0 or not value or string_index >= len(strings):
                continue
            name = strings[string_index:strings.index(b'\0', string_index)].decode()
            self.symbols[name] = (value, section)

    def read(self, address, count):
        for va, _, offset, size in self.segments:
            if va <= address and address + count <= va + size:
                return self.data[offset + address - va:offset + address - va + count]
        raise ValueError(f'Unmapped bytes: {address:#x}+{count:#x}')

    def function(self, name):
        address, section = self.symbols[name]
        section_name, base, length = self.sections[section]
        if section_name != b'__text':
            raise ValueError(f'Symbol is not executable __text: {name}')
        end = min([base + length] + [a for a, s in self.symbols.values() if s == section and a > address])
        return address, self.read(address, end - address)


def fixture(folder, target):
    image = Image(folder / 'com.apple.driver.usb.AppleUSBXHCI.macho')
    kernel = Image(folder / 'com.apple.kernel.macho')
    output = bytearray(b'XHCLYT01')
    output += struct.pack('<QQ', kernel.symbols['_OSCompareAndSwap'][0], kernel.symbols['_OSBitOrAtomic'][0])
    evidence = []
    for name in FUNCTIONS:
        address, data = image.function(name)
        output += struct.pack('<QI', address, len(data)) + data
        evidence.append({'symbol': name, 'address': hex(address), 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    target.write_bytes(output)
    return {'folder': str(folder), 'functions': evidence}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image_folders', nargs='*', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    folders = args.image_folders
    if not folders:
        raise SystemExit('pass one or more extracted driver image directories')
    here = Path(__file__).resolve().parent
    evidence = []
    with tempfile.TemporaryDirectory(prefix='xhci-layout-test-') as temp:
        temp = Path(temp)
        executable = temp / 'test-runtime-layout'
        subprocess.run(['clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror', '-O2', '-fsanitize=address,undefined', str(here / 'test_runtime_layout.cpp'), '-o', str(executable)], check=True)
        for index, folder in enumerate(folders):
            path = temp / f'fixture-{index}.bin'
            item = fixture(folder, path)
            result = subprocess.run([str(executable), str(path)], text=True, capture_output=True)
            if result.returncode:
                raise RuntimeError(f'{folder}: {result.stdout}{result.stderr}')
            item['test_output'] = result.stdout.strip()
            print(folder.name + ': ' + result.stdout.strip())
            evidence.append(item)
    if args.report:
        args.report.write_text(json.dumps({'decoder': 'src/runtime_layout.hpp', 'sanitizers': ['address', 'undefined'], 'hardware_executed': False, 'images': evidence}, indent=2) + '\n')


if __name__ == '__main__':
    main()
