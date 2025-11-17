
file build/kernel.elf
target remote :1234
set mem inaccessible-by-default off
set disassembly-flavor intel
hbreak _start
c