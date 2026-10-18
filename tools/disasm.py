#!/usr/bin/env python3
"""Disassemble part of a linked image (out/boot-linked.bin) with the PS4 functions it calls named.

Guest offsets are the ones the loader prints (faults, GOW3_IMPORT_STATS callers). Calls through
the PLT and through GOT slots are annotated with the imported function (src/import_names.inc).
Needs llvm-objdump (MSYS2 CLANG64 has it), or set OBJDUMP.

    python tools/disasm.py lbp3-data/out/boot-linked.bin 0x19209 [--before 0x80] [--after 0x40]
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAME_LINE = re.compile(r'^\{"([A-Za-z0-9+-]{11}#\w+)","([^"]+)"\},$')


def load(path):
    data = Path(path).read_bytes()
    magic, size, entry, ns, nr, ni, flags = struct.unpack_from('<8s6Q', data, 0)
    if magic not in (b'G3PROBE5', b'G3PROBE6'):
        raise ValueError(f'{path}: expected a G3PROBE5/6 image from link_modules.py')
    pos = 56 + 8 + 32
    modules, = struct.unpack_from('<Q', data, pos)
    pos += 8 + modules * (128 if magic == b'G3PROBE6' else 56)
    bindings, = struct.unpack_from('<Q', data, pos)
    pos += 8 + bindings * 24 + ns * 24
    names = [data[pos + i * 128:pos + (i + 1) * 128].split(b'\0', 1)[0].decode() for i in range(ni)]
    pos += ni * 128
    got = {}
    for i in range(nr):
        target, kind, value, addend = struct.unpack_from('<QQqq', data, pos + i * 32)
        if kind == 1:
            got[target] = names[value]
    pos += nr * 32
    return data[pos:pos + size], got


def elf_with(code, address):
    """A minimal ELF64 file holding `code` as .text at `address`, for llvm-objdump."""
    strings = b'\0.text\0.shstrtab\0'
    text, table = 64, 64 + len(code)
    sections = (table + len(strings) + 7) & ~7
    header = struct.pack('<16sHHIQQQIHHHHHH', b'\x7fELF\x02\x01\x01'.ljust(16, b'\0'), 2, 62, 1, address, 0,
                         sections, 0, 64, 0, 0, 64, 3, 2)
    entries = (bytes(64)
               + struct.pack('<IIQQQQIIQQ', 1, 1, 6, address, text, len(code), 0, 0, 16, 0)
               + struct.pack('<IIQQQQIIQQ', 7, 3, 0, 0, table, len(strings), 0, 0, 1, 0))
    return (header + code + strings).ljust(sections, b'\0') + entries


def symbol_names():
    known = {}
    for line in (ROOT / 'src' / 'import_names.inc').read_text(encoding='utf-8').splitlines():
        if m := NAME_LINE.match(line):
            known[m.group(1)] = m.group(2)
    return known


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', type=Path)
    parser.add_argument('offset', type=lambda v: int(v, 0))
    parser.add_argument('--before', type=lambda v: int(v, 0), default=0x80)
    parser.add_argument('--after', type=lambda v: int(v, 0), default=0x40)
    args = parser.parse_args()
    objdump = (os.environ.get('OBJDUMP') or shutil.which('llvm-objdump')
               or r'C:\msys64\clang64\bin\llvm-objdump.exe')
    image, got = load(args.image)
    known = symbol_names()

    def describe(address):
        if address in got:
            name = got[address]
        elif image[address:address + 2] == b'\xff\x25':  # PLT stub: jmp [rip+disp32]
            slot = address + 6 + struct.unpack_from('<i', image, address + 2)[0]
            name = got.get(slot)
        else:
            return None
        if not name:
            return None
        key = name.replace('#libScePosix', '#libkernel')
        return f'{known.get(key, "?")} ({name})'

    start = max(0, args.offset - args.before)
    end = min(len(image), args.offset + args.after)
    with tempfile.TemporaryDirectory() as tmp:
        chunk = Path(tmp) / 'chunk.elf'
        chunk.write_bytes(elf_with(image[start:end], start))
        out = subprocess.run([objdump, '-d', str(chunk)], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):', line)
        if not m:
            continue
        note = ''
        for target in re.findall(r'(?:call|jmp)\w*\s+(?:\*0x[0-9a-f]+\(%rip\)\s+# )?0x([0-9a-f]+)', line):
            if found := describe(int(target, 16)):
                note = f'    ; {found}'
        marker = '=>' if int(m.group(1), 16) <= args.offset < int(m.group(1), 16) + 16 and \
            int(m.group(1), 16) == args.offset else '  '
        print(f'{marker}{line}{note}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'disasm failed: {error}')
