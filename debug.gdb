
file build/kernel.elf
target remote :1234
set mem inaccessible-by-default off
set disassembly-flavor intel
set remotetimeout 999
hbreak boot_startup
hbreak interrupt_handler
hbreak syscall_handler
c