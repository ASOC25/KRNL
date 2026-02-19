
file build/kernel.elf
target remote :1234
set mem inaccessible-by-default off
set disassembly-flavor intel
set remotetimeout 999
add-symbol-file /home/tretorn/KRNL/sysroot/usr/lib/ld.so.sym 0x40000548
define hook-stop
  x/10ig $rip
end
hbreak boot_startup
hbreak exception
hbreak main
hbreak allocmatch
c