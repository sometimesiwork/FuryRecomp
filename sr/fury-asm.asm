%include "x86inc.inc"
%include "misc.inc"
%include "extern.inc"
%include "macros.inc"
global stack_start
global _stack_start
global program_start
global _program_start

%ifidn __OUTPUT_FORMAT__, elf32
section .note.GNU-stack noalloc noexec nowrite progbits
%endif

%ifidn __OUTPUT_FORMAT__, elf32
section cseg01 progbits alloc exec nowrite align=16
%else
section cseg01 code align=16
%endif
%include "seg01.inc"
%ifdef DEBUG
%ifidn __OUTPUT_FORMAT__, elf32
%include "code_linux.inc"
%else
%include "code_win32.inc"
%endif
%endif
