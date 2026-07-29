"""
autoload_syms.py — GDB Python script for KRNL debugging.

Loads symbols for bash and mlibc at their actual runtime addresses.

Kernel load layout (from loader.c):
  - ET_EXEC (bash, init, ...): base=0, so ELF VMAs are absolute load addresses
  - ld.so:  always mapped at DYNAMIC_LINKER_BASE_ADDRESS (0x40000000)
  - libc.so and other shared libs: loaded by ld.so; base discovered via
    the r_debug link map that ld.so fills in before calling main()

Section relocation strategy:
  - ET_EXEC:  add-symbol-file FILE -o 0
              (ELF VMAs already are absolute addresses, no adjustment needed)
  - ET_DYN:   add-symbol-file FILE -o LOAD_BASE
              (-o offsets every section, so .data/.bss land correctly)

Usage: sourced automatically from debug.gdb. Symbols are loaded the first
time GDB stops at a frame named 'main'. Also available as 'autoload-syms'.
"""

import gdb
import os
import struct
import subprocess

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
KRNL_ROOT  = os.path.normpath(os.path.join(SCRIPT_DIR, '..', '..'))
SYSROOT    = os.path.join(KRNL_ROOT, 'sysroot')
LD_BASE    = 0x40000000  # DYNAMIC_LINKER_BASE_ADDRESS from loader.h

_loaded = False


# ── ELF introspection helpers ────────────────────────────────────────────────

def _elf_type(binary_path):
    """Return 'EXEC', 'DYN', or None."""
    try:
        out = subprocess.check_output(
            ['readelf', '-h', binary_path], stderr=subprocess.DEVNULL
        ).decode(errors='replace')
        for line in out.splitlines():
            if 'Type:' in line:
                if 'EXEC' in line: return 'EXEC'
                if 'DYN'  in line: return 'DYN'
    except Exception:
        pass
    return None


# ── GDB add-symbol-file wrappers ─────────────────────────────────────────────

def _add_sym_exec(sym_path):
    """ET_EXEC: ELF VMAs are absolute — no offset needed."""
    if not os.path.exists(sym_path):
        print(f'[autoload-syms] Missing: {sym_path}')
        return
    cmd = f'add-symbol-file {sym_path} -o 0'
    print(f'[autoload-syms] {cmd}')
    try:
        gdb.execute(cmd, to_string=True)
    except Exception as e:
        print(f'[autoload-syms] Warning: {e}')


def _add_sym_pie(sym_path, load_base):
    """ET_DYN / shared lib: offset every section by load_base."""
    if not os.path.exists(sym_path):
        print(f'[autoload-syms] Missing: {sym_path}')
        return
    cmd = f'add-symbol-file {sym_path} -o 0x{load_base:x}'
    print(f'[autoload-syms] {cmd}')
    try:
        gdb.execute(cmd, to_string=True)
    except Exception as e:
        print(f'[autoload-syms] Warning: {e}')


# ── Memory reading helpers ───────────────────────────────────────────────────

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


# ── Link-map walker (for shared libs loaded by ld.so) ────────────────────────

def _walk_link_map():
    """Walk ld.so's r_debug link map; use load_base from each entry as -o offset."""
    def _process_entry(l_addr, l_name_ptr):
        if not l_name_ptr or not l_addr:
            return
        name = _read_cstr(l_name_ptr)
        if not name:
            return
        for candidate in (
            os.path.join(SYSROOT, name.lstrip('/')),
            name,
        ):
            if os.path.exists(candidate):
                _add_sym_pie(candidate + '.sym', l_addr)
                break

    # Primary path: use GDB type info after ld.so symbols are loaded
    try:
        r_debug = gdb.parse_and_eval('_r_debug')
        r_map   = r_debug['r_map']
        visited = set()
        while r_map and int(r_map) not in visited:
            visited.add(int(r_map))
            l_addr     = int(r_map['l_addr'])
            l_name_ptr = int(r_map['l_name'])
            r_map = r_map['l_next']
            _process_entry(l_addr, l_name_ptr)
        return
    except Exception:
        pass

    # Fallback: raw memory walk using known r_debug layout:
    #   int r_version (4 B) + pad (4 B) + struct link_map *r_map (8 B)
    try:
        sym = gdb.lookup_global_symbol('_r_debug')
        if sym is None:
            raise RuntimeError('_r_debug not found')
        r_debug_addr = int(sym.value().address)
        r_map_addr   = _read_u64(r_debug_addr + 8)
        visited = set()
        while r_map_addr and r_map_addr not in visited:
            visited.add(r_map_addr)
            l_addr     = _read_u64(r_map_addr)       # l_addr  @ +0
            l_name_ptr = _read_u64(r_map_addr + 8)   # l_name  @ +8
            l_next     = _read_u64(r_map_addr + 24)  # l_next  @ +24
            _process_entry(l_addr, l_name_ptr)
            r_map_addr = l_next
    except Exception as e:
        print(f'[autoload-syms] Could not walk link map: {e}')


# ── Sysroot scan helper ──────────────────────────────────────────────────────

def _find_sym_files(root):
    for dirpath, _, filenames in os.walk(root):
        for fname in filenames:
            if fname.endswith('.sym'):
                yield os.path.join(dirpath, fname)


# ── Main entry point ─────────────────────────────────────────────────────────

def autoload_symbols():
    global _loaded
    _loaded = True

    # ld.so: ET_DYN always at DYNAMIC_LINKER_BASE_ADDRESS
    ld_sym = os.path.join(SYSROOT, 'usr/lib/ld.so.sym')
    if os.path.exists(ld_sym):
        _add_sym_pie(ld_sym, LD_BASE)

    # All ET_EXEC binaries in sysroot: ELF VMAs are absolute (base=0 in loader)
    for sym_path in sorted(_find_sym_files(SYSROOT)):
        bin_path = sym_path[:-4]
        if not os.path.exists(bin_path):
            continue
        if _elf_type(bin_path) == 'EXEC':
            _add_sym_exec(sym_path)

    # Shared libs (libc.so, etc.): runtime base from ld.so link map
    _walk_link_map()


# ── GDB command & event hook ─────────────────────────────────────────────────

class AutoloadSymsCmd(gdb.Command):
    """Load symbol files for user-space binaries.

    Correct -o offsets are applied per binary type:
      ET_EXEC  → -o 0   (absolute VMAs from kernel loader)
      ET_DYN   → -o LOAD_BASE  (base from ld.so link map)
    Safe to run multiple times.
    """
    def __init__(self):
        super().__init__('autoload-syms', gdb.COMMAND_SUPPORT)

    def invoke(self, arg, from_tty):
        global _loaded
        _loaded = False
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
