#!/usr/bin/env python3
"""Read sorted 'nm --defined-only -n' output from stdin, emit kernel_syms.c."""
import sys

syms = []
for line in sys.stdin:
    parts = line.strip().split(None, 2)
    if len(parts) < 3:
        continue
    type_char = parts[1]
    if type_char not in ('T', 't', 'W', 'w'):
        continue
    addr = int(parts[0], 16)
    name = parts[2]
    syms.append((addr, name))

print('#include <krnl/debug/kernel_syms.h>')
print()
print('typedef struct { uint64_t addr; const char *name; } ksym_t;')
print()
print('static ksym_t ksyms[] = {')
for addr, name in syms:
    escaped = name.replace('\\', '\\\\').replace('"', '\\"')
    print(f'    {{0x{addr:016x}ULL, "{escaped}"}},')
print('    {0, 0}')
print('};')
print(f'static unsigned int ksym_count = {len(syms)};')
print()
print('const char * kernel_resolve_symbol(uint64_t addr) {')
print('    if (!ksym_count || addr < ksyms[0].addr) return NULL;')
print('    unsigned int lo = 0, hi = ksym_count;')
print('    while (lo + 1 < hi) {')
print('        unsigned int mid = (lo + hi) / 2;')
print('        if (ksyms[mid].addr <= addr) lo = mid;')
print('        else hi = mid;')
print('    }')
print('    return ksyms[lo].name;')
print('}')
