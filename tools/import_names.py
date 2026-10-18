#!/usr/bin/env python3
"""Add the names of a game's imports to src/import_names.inc.

The runtime binds an import through its symbol name (runtime_lookup), so every NID the game
imports needs a line in src/import_names.inc. This reads the import table of a linked image
(out/boot-linked.bin, made by scripts/link_modules.py) and names each NID from a public symbol
list: shadPS4's src/core/aerolib/aerolib.inl, lines of the form STUB("NID", name). The lines
already in the file are kept; new ones are added in import order. libScePosix imports are
spelled #libkernel, as runtime.c looks them up.

    python tools/import_names.py out/boot-linked.bin path/to/aerolib.inl
"""
import argparse
import re
import struct
import sys
from pathlib import Path

INC = Path(__file__).resolve().parent.parent / 'src' / 'import_names.inc'
LINE = re.compile(r'^\{"([A-Za-z0-9+-]{11}#\w+)","([^"]+)"\},$')


def linked_imports(path):
    """Import names (NID#library) of a G3PROBE5/6 image, in table order."""
    data = Path(path).read_bytes()
    magic, size, entry, ns, nr, ni, flags = struct.unpack_from('<8s6Q', data, 0)
    if magic not in (b'G3PROBE5', b'G3PROBE6'):
        raise ValueError(f'{path}: expected a G3PROBE5/6 image from link_modules.py')
    pos = 56 + 8 + 32
    modules, = struct.unpack_from('<Q', data, pos)
    pos += 8 + modules * (128 if magic == b'G3PROBE6' else 56)
    bindings, = struct.unpack_from('<Q', data, pos)
    pos += 8 + bindings * 24 + ns * 24
    return [data[pos + i * 128:pos + (i + 1) * 128].split(b'\0', 1)[0].decode() for i in range(ni)]


def symbol_list(path):
    names = {}
    for match in re.finditer(r'STUB\("([A-Za-z0-9+-]{11})",\s*([^)\s]+)\)', Path(path).read_text(encoding='utf-8')):
        names.setdefault(match.group(1), match.group(2))
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', type=Path)
    parser.add_argument('symbols', type=Path)
    args = parser.parse_args()
    lines = INC.read_text(encoding='utf-8').splitlines()
    known = {m.group(1) for line in lines if (m := LINE.match(line))}
    symbols = symbol_list(args.symbols)
    added, unnamed = [], []
    for name in linked_imports(args.image):
        nid, _, library = name.partition('#')
        if not library or name.startswith('libc:'):
            continue
        key = f'{nid}#{"libkernel" if library == "libScePosix" else library}'
        if key in known:
            continue
        known.add(key)
        if nid in symbols:
            added.append(f'{{"{key}","{symbols[nid]}"}},')
        else:
            unnamed.append(key)
    INC.write_text('\n'.join(lines + added) + '\n', encoding='utf-8', newline='\n')
    print(f'{len(added)} names added to {INC.name}; {len(unnamed)} imports have no public name')
    for key in unnamed:
        print(f'  unnamed: {key}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        sys.exit(f'import_names failed: {error}')
