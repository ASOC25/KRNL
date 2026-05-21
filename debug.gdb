
file build/kernel.elf
target remote :1234
set mem inaccessible-by-default off
set disassembly-flavor intel
set remotetimeout 999

source env/scripts/autoload_syms.py

define hook-stop
  x/10ig $rip
end
hbreak boot_startup
hbreak exception
hbreak main
hbreak allocmatch
c
