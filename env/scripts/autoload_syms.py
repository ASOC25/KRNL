"""
autoload_syms.py — GDB Python script for KRNL debugging.

Automatically computes and issues add-symbol-file commands for user-space
binaries by reading .text section VMAs from the on-disk ELF files and
combining them with the runtime load bases known from the OS design:

  - ld.so  → always mapped at DYNAMIC_LINKER_BASE_ADDRESS (0x40000000)
  - bash   → ET_EXEC, non-PIE: text VMA in ELF == load address
  - others → discovered by walking the ld.so r_debug link map

Usage: sourced automatically from debug.gdb. Symbols are loaded the first
time GDB stops at a frame named 'main'. You can also run 'autoload-syms'
manually at any time after the process has started.
"""

import gdb
import os
import struct
import subprocess

SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
KRNL_ROOT    = os.path.normpath(os.path.join(SCRIPT_DIR, '..', '..'))
SYSROOT      = os.path.join(KRNL_ROOT, 'sysroot')
LD_BASE      = 0x40000000  # DYNAMIC_LINKER_BASE_ADDRESS from loader.h

_loaded = False  # prevent duplicate loading


def _readelf_text_vma(binary_path):
    """Return the .text section VMA from the on-disk binary, or None."""
    try:
        out = subprocess.check_output(
            ['readelf', '-S', '--wide', binary_path],
            stderr=subprocess.DEVNULL
        ).decode(errors='replace')
        for line in out.splitlines():
            parts = line.split()
            for i, p in enumerate(parts):
                if p == '.text' and i + 2 < len(parts):
                    try:
                        return int(parts[i + 2], 16)
                    except ValueError:
                        pass
    except Exception:
        pass
    return None


def _elf_type(binary_path):
    """Return 'EXEC', 'DYN', or None."""
    try:
        out = subprocess.check_output(
            ['readelf', '-h', binary_path],
            stderr=subprocess.DEVNULL
        ).decode(errors='replace')
        for line in out.splitlines():
            if 'Type:' in line:
                if 'EXEC' in line:
                    return 'EXEC'
                if 'DYN' in line:
                    return 'DYN'
    except Exception:
        pass
    return None


def _add_sym(sym_path, text_addr):
    if not os.path.exists(sym_path):
        print(f'[autoload-syms] Missing: {sym_path}')
        return
    cmd = f'add-symbol-file {sym_path} 0x{text_addr:x}'
    print(f'[autoload-syms] {cmd}')
    try:
        gdb.execute(cmd, to_string=True)
    except Exception as e:
        print(f'[autoload-syms] Warning: {e}')


def _read_u64(addr):
    try:
        return struct.unpack('<Q', bytes(
            gdb.selected_inferior().read_memory(addr, 8)))[0]
    except Exception:
        return None


def _read_cstr(addr, max_len=512):
    try:
        raw = bytes(gdb.selected_inferior().read_memory(addr, max_len))
        end = raw.find(b'\x00')
        return raw[:end].decode('latin-1') if end >= 0 else raw.decode('latin-1')
    except Exception:
        return ''


def _walk_link_map():
    """Walk ld.so's r_debug link map and load symbols for each shared lib."""
    try:
        # After ld.so symbols are loaded, GDB can evaluate _r_debug directly.
        r_debug = gdb.parse_and_eval('_r_debug')
        r_map   = r_debug['r_map']
        visited = set()
        while r_map and int(r_map) not in visited:
            visited.add(int(r_map))
            l_addr = int(r_map['l_addr'])
            l_name_ptr = int(r_map['l_name'])
            r_map = r_map['l_next']

            if not l_name_ptr or not l_addr:
                continue
            name = _read_cstr(l_name_ptr)
            if not name:
                continue

            # Resolve path relative to sysroot
            for candidate in (
                os.path.join(SYSROOT, name.lstrip('/')),
                name,
            ):
                if os.path.exists(candidate):
                    text_off = _readelf_text_vma(candidate)
                    if text_off is not None:
                        _add_sym(candidate + '.sym', l_addr + text_off)
                    break

    except Exception as e:
        # Fall back to raw memory offsets if type info is unavailable.
        try:
            sym = gdb.lookup_global_symbol('_r_debug')
            if sym is None:
                raise RuntimeError('_r_debug symbol not found')
            # r_debug layout: int r_version (4B) + pad (4B) + struct link_map *r_map (8B)
            r_debug_addr = int(sym.value().address)
            r_map_addr   = _read_u64(r_debug_addr + 8)
            visited = set()
            while r_map_addr and r_map_addr not in visited:
                visited.add(r_map_addr)
                l_addr     = _read_u64(r_map_addr)
                l_name_ptr = _read_u64(r_map_addr + 8)
                l_next     = _read_u64(r_map_addr + 24)
                if l_name_ptr and l_addr:
                    name = _read_cstr(l_name_ptr)
                    if name:
                        for candidate in (
                            os.path.join(SYSROOT, name.lstrip('/')),
                            name,
                        ):
                            if os.path.exists(candidate):
                                text_off = _readelf_text_vma(candidate)
                                if text_off is not None:
                                    _add_sym(candidate + '.sym', l_addr + text_off)
                                break
                r_map_addr = l_next
        except Exception as e2:
            print(f'[autoload-syms] Could not walk link map: {e2}')


def autoload_symbols():
    global _loaded
    _loaded = True

    # ld.so: PIE shared lib always loaded at DYNAMIC_LINKER_BASE_ADDRESS
    ld_bin = os.path.join(SYSROOT, 'usr/lib/ld.so')
    if os.path.exists(ld_bin):
        text_off = _readelf_text_vma(ld_bin)
        if text_off is not None:
            _add_sym(ld_bin + '.sym', LD_BASE + text_off)

    # Main executables: ET_EXEC binaries have fixed text VMAs
    for rel_path in ('usr/bin/bash', 'usr/bin/init', 'bin/sh'):
        bin_path = os.path.join(SYSROOT, rel_path)
        if not os.path.exists(bin_path):
            continue
        if _elf_type(bin_path) == 'EXEC':
            text_vma = _readelf_text_vma(bin_path)
            if text_vma is not None:
                _add_sym(bin_path + '.sym', text_vma)

    # Shared libraries: walk the ld.so link map
    _walk_link_map()


class AutoloadSymsCmd(gdb.Command):
    """Load symbol files for user-space binaries.

    Computes .text load addresses from on-disk ELF binaries using readelf,
    combining them with runtime load bases (fixed for ld.so, from the link
    map for shared libraries). Safe to run multiple times.
    """

    def __init__(self):
        super().__init__('autoload-syms', gdb.COMMAND_SUPPORT)

    def invoke(self, arg, from_tty):
        global _loaded
        _loaded = False  # allow re-run if called manually
        autoload_symbols()


AutoloadSymsCmd()


def _on_stop(event):
    global _loaded
    if _loaded:
        return
    try:
        frame = gdb.selected_frame()
        if frame and frame.name() == 'main':
            autoload_symbols()
    except Exception:
        pass


gdb.events.stop.connect(_on_stop)
print('[autoload-syms] Ready. Symbols auto-load at main, or run "autoload-syms" manually.')
