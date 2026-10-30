bits 32
org 0x20000000

; Header read by the runtime:
;   +0  address of the guest entry point
;   +4  address of the guest -> flat symbol table
;   +8  number of symbols
dd program_start
dd syms
dd symcount

%include "macros_flat.inc"
%include "seg01_flat.inc"

align 4
syms:
%include "syms.inc"
symcount equ ($ - syms) / 8
