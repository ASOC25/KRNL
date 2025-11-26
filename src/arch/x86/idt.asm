section .text

global _load_idt

_load_idt:
    lidt  [rdi]
    ret