#!/usr/bin/env python3
"""Offline checks only. Never loads a kext, edits EFI, or accesses hardware."""
from pathlib import Path
import subprocess,tempfile,json,hashlib,plistlib,re,struct,os
ROOT=Path(__file__).resolve().parent
KERNEL=Path(os.environ['XHCI_REFERENCE_KERNEL']) if os.environ.get('XHCI_REFERENCE_KERNEL') else None
LILU=Path(os.environ['XHCI_LILU_BINARY']) if os.environ.get('XHCI_LILU_BINARY') else None
def run(args):
    r=subprocess.run(args,cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    if r.returncode:raise RuntimeError(str(args)+'\n'+r.stdout)
    return r.stdout
def symbols(path):
    result = {}
    for line in run(['nm', '-arch', 'x86_64', '-n', str(path)]).splitlines():
        match = re.match(r'([0-9a-f]{16})\s+([a-zA-Z])\s+(\S+)$', line)
        if match:
            result[match[3]] = (int(match[1], 16), match[2])
    return result


def image_reader(path):
    data = Path(path).read_bytes()
    assert struct.unpack_from('<I', data)[0] == 0xfeedfacf
    segments, cursor = [], 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, cursor)
        if command == 0x19:
            segments.append(struct.unpack_from('<QQQQ', data, cursor + 24))
        cursor += size

    def read(address, length):
        for base, _, fileoff, filesize in segments:
            if base <= address and address + length <= base + filesize:
                offset = fileoff + address - base
                return data[offset:offset + length]
        raise ValueError(f'address {address:#x} lacks mapped file bytes in {path}')
    return read


def vtable_check(binary, kernel_symbols):
    plugin_symbols = symbols(binary)
    plugin_read, kernel_read = image_reader(binary), image_reader(KERNEL)
    symbol_at_plugin, symbol_at_kernel = {}, {}
    for name, (address, _) in plugin_symbols.items():
        symbol_at_plugin.setdefault(address, []).append(name)
    for name, (address, _) in kernel_symbols.items():
        symbol_at_kernel.setdefault(address, []).append(name)
    external = {}
    for line in run(['otool', '-rV', str(binary)]).splitlines():
        match = re.match(r'([0-9a-f]{8})\s+False\s+\?\( 3\)\s+True\s+UNSIGND\s+False\s+(\S+)', line)
        if match:
            external[int(match[1], 16)] = match[2]
    plugin_base = plugin_symbols['__ZTV10XHCILagFix'][0]
    kernel_base = kernel_symbols['__ZTV9IOService'][0]
    overrides = {'5probe': 'IOService5probe', '5start': 'IOService5start', '4stop': 'IOService4stop',
                 '13setProperties': 'IORegistryEntry13setProperties', '12getMetaClass': 'IOService12getMetaClass'}
    checks = []
    for offset in range(16, 0x860, 8):
        pvalue = struct.unpack('<Q', plugin_read(plugin_base + offset, 8))[0]
        kvalue = struct.unpack('<Q', kernel_read(kernel_base + offset, 8))[0]
        psymbol = external.get(plugin_base + offset) or (symbol_at_plugin.get(pvalue) or [None])[0]
        ksymbols = symbol_at_kernel.get(kvalue) or []
        valid = psymbol in ksymbols
        if 'XHCILagFix' in str(psymbol):
            if any(marker in psymbol for marker in ('D0Ev', 'D1Ev', 'D2Ev')):
                valid = any('IOServiceD' in name for name in ksymbols)
            else:
                valid = any(marker in psymbol and any(expected in name for name in ksymbols)
                            for marker, expected in overrides.items())
        assert valid, {'offset': hex(offset - 16), 'plugin': psymbol, 'kernel': ksymbols}
        checks.append({'vptr_offset': hex(offset - 16), 'plugin_symbol': psymbol, 'kernel_symbols': ksymbols})
    return {'matching_nonpadding_entries': len(checks), 'checks': checks}



def main():
    outputs={'build':run(['sh','build.sh'])}
    with tempfile.TemporaryDirectory(prefix='xhcilagfix-offline-') as temp:
        for arch in ('arm64','x86_64'):
            flags=['clang++','-arch',arch,'-mmacosx-version-min=11.0','-std=c++14','-O2','-Wall','-Wextra','-Werror','-I','tests/stubs','-I','src']
            binary=str(Path(temp)/('queue-'+arch))
            run(flags+['tests/test_route0.cpp','-o',binary]);outputs['queue_'+arch]=run([binary])
            binary=str(Path(temp)/('fix-'+arch))
            run(flags+['src/fix.cpp','tests/test_fix.cpp','-o',binary])
            for scenario in ('success','missing','bounds','partial'):
                outputs['fix_'+arch+'_'+scenario]=run([binary,scenario])
    binary=ROOT/'build/XHCILagFix.kext/Contents/MacOS/XHCILagFix'
    undefined=[l.split()[-1] for l in run(['nm','-u',str(binary)]).splitlines() if l.strip()]
    missing=None;vtable=None
    if KERNEL and LILU:
        kernel=symbols(KERNEL);lilu=symbols(LILU)
        missing=[n for n in undefined if n not in kernel and n not in lilu];assert not missing,missing
        vtable=vtable_check(binary,kernel)
    assert not any(any(term in n for term in ('kernel_debug','kdebug','IOPCIDevice','IOMemoryMap','rdmsr','wrmsr','IOTimerEventSource','IOWorkLoop')) for n in undefined)
    disassembly=run(['otool','-tvV',str(binary)]);assert not re.search(r'%(?:[xyz]mm\d+|mm[0-7])\b',disassembly)
    info=plistlib.loads((binary.parent.parent/'Info.plist').read_bytes())
    assert info['CFBundleIdentifier']=='as.local.XHCILagFix' and info['CFBundleVersion']=='0.1.0'
    assert 'com.apple.iokit.IOPCIFamily' not in info['OSBundleLibraries']
    add=plistlib.loads((ROOT/'OpenCore/Kernel-Add.plist').read_bytes())['Kernel']['Add'][0]
    assert add['MinKernel']==add['MaxKernel']=='' and add['Enabled']
    plugin=(ROOT/'src/plugin.cpp').read_text();assert 'static_cast<KernelVersion>(0x7fffffff)' in plugin
    assert 'AllowInstallerRecovery' in plugin
    fix=(ROOT/'src/fix.cpp').read_text();assert 'matchingUuid' not in fix and 'sameBytes' not in fix
    counts={str(p.relative_to(ROOT)):len(p.read_text().splitlines()) for p in (ROOT/'src').iterdir() if p.is_file()}
    sources=[p for directory in ('src','tests','licenses') for p in (ROOT/directory).rglob('*') if p.is_file()]
    sources += [ROOT/'build.sh',ROOT/'validate.py',ROOT/'README.md',ROOT/'OpenCore/Kernel-Add.plist',binary,binary.parent.parent/'Info.plist']
    report={'passed':True,'version':'0.1.0','core_hooks':3,'source_lines_including_comments_and_blanks':sum(counts.values()),'source_line_counts':counts,
      'architectures_tested':['arm64 userspace fixtures','x86_64 userspace fixtures'],
      'queue_cases_per_architecture':37,'actual_fix_cpp_scenarios_per_architecture':4,
      'kernel_version_gate':{'minimum_darwin':20,'maximum_darwin':2147483647,'darwin_25_macos_26_allowed':True},
      'opencore_kernel_bounds_empty':True,'runtime_uuid_or_fingerprint_gate':False,'request_counters':False,'periodic_timer':False,
      'macos_26_hardware_tested':False,'standalone_hardware_boot_tested':False,'efi_changed':False,
      'undefined_symbols':undefined,'missing_symbols':missing,'reference_kernel_vtable':vtable,
      'reference_binary_audit_performed':bool(KERNEL and LILU),
      'dependencies':{n:run(['git','-C',os.environ.get(env,str(ROOT/'deps'/n)),'rev-parse','HEAD']).strip() for n,env in (('Lilu','XHCI_LILU_SOURCE'),('MacKernelSDK','XHCI_KERNEL_SDK'))},
      'sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},'outputs':outputs}
    (ROOT/'build/validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('passed','core_hooks','source_lines_including_comments_and_blanks','kernel_version_gate','standalone_hardware_boot_tested','efi_changed')},indent=2))
if __name__=='__main__':main()
