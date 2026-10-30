;;
;; Runtime helper stubs for the flat recompiled program.
;;
;; The recompiled code calls these through a pointer table filled in by the
;; runtime.  A stub saves the complete guest register state, switches to the
;; real process stack for the C call and restores everything, so the generated
;; code can treat a helper as an ordinary instruction.
;;
;; Frame built by pushfd/pushad (32-bit pushad order):
;;
;;      [esp +  0] EDI          [esp + 16] EBX
;;      [esp +  4] ESI          [esp + 20] EDX
;;      [esp +  8] EBP          [esp + 24] ECX
;;      [esp + 12] ESP (saved)  [esp + 28] EAX
;;      [esp + 32] EFLAGS       [esp + 36] return address
;;      [esp + 40] argument pushed by the generated code
;;
bits 32
section .text

%define TMP_T          0x080F001C
%define HOST_ESP       0x080F002C
%define TMP_FRAME      0x080F0034
%define CALLER         0x080F0030
%define IN_HANDLER     0x080F0028
%define VGA_ARG0       0x080F0158
%define VGA_ARG1       0x080F015C
%define VGA_ARG2       0x080F0160
%define VGA_RES        0x080F0164

global x86_int
global x86_out_dx_al
global x86_out_dx_ax
global x86_in_al_dx
global x86_out_imm_al
global x86_in_al_imm
global SR_CheckTimer
global x86_code_ptr
global x86_trace
global vga_store8
global vga_store16
global vga_store32
global vga_load8
global vga_load16
global vga_load32
global vga_stos8
global vga_stos16
global vga_movsb
global vga_movsw

extern x86_int_impl
extern out_dx_al_impl
extern out_dx_ax_impl
extern in_al_dx_impl
extern out_imm_al_impl
extern in_al_imm_impl
extern timer_check_impl
extern x86_code_ptr_impl
extern x86_trace_impl
extern vga_store8_impl
extern vga_store16_impl
extern vga_store32_impl
extern vga_load8_impl
extern vga_load16_impl
extern vga_load32_impl
extern vga_stos8_impl
extern vga_stos16_impl
extern vga_movsb_impl
extern vga_movsw_impl

; ---- int n ---------------------------------------------------------------
; void x86_int_impl(uint32_t vector, uint32_t *frame)
align 16
x86_int:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov edx, [eax+36]           ; caller (generated code)
    mov [CALLER], edx
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push eax                    ; frame
    push dword [eax+40]         ; vector
    call x86_int_impl
    ; Guest instructions in the interrupt handler may clobber EAX after it
    ; has also been switched to the host stack above. Restore the frame pointer
    ; from its fixed runtime slot, not from the guest register image.
    mov eax, [TMP_FRAME]
    mov esp, eax
    popad
    popfd
    retn 4

; ---- out dx, al ----------------------------------------------------------
align 16
x86_out_dx_al:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [eax+28]         ; value = AX
    push dword [eax+20]         ; port = DX
    call out_dx_al_impl
    mov esp, [TMP_FRAME]
    popad
    popfd
    ret

; ---- out dx, ax ----------------------------------------------------------
align 16
x86_out_dx_ax:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [eax+28]         ; value = AX
    push dword [eax+20]         ; port = DX
    call out_dx_ax_impl
    mov esp, [TMP_FRAME]
    popad
    popfd
    ret

; ---- in al, dx -----------------------------------------------------------
align 16
x86_in_al_dx:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [eax+20]         ; port = DX
    call in_al_dx_impl
    mov ecx, [TMP_FRAME]
    mov [ecx+28], al
    mov esp, ecx
    popad
    popfd
    ret

; ---- out imm8, al --------------------------------------------------------
align 16
x86_out_imm_al:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [eax+28]         ; value = AL
    push dword [eax+40]         ; port
    call out_imm_al_impl
    mov esp, [TMP_FRAME]
    popad
    popfd
    retn 4

; ---- in al, imm8 ---------------------------------------------------------
align 16
x86_in_al_imm:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [eax+40]         ; port
    call in_al_imm_impl
    mov ecx, [TMP_FRAME]
    mov [ecx+28], al
    mov esp, ecx
    popad
    popfd
    retn 4

; ---- guest offset -> flat code pointer ----------------------------------
align 16
x86_code_ptr:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [eax+40]         ; guest linear address
    call x86_code_ptr_impl
    mov ecx, [TMP_FRAME]
    mov [ecx+28], eax
    mov esp, ecx
    popad
    popfd
    retn 4

; ---- debug trace ---------------------------------------------------------
; void x86_trace_impl(uint32_t id, uint32_t *frame)
align 16
x86_trace:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push eax
    push dword [eax+40]         ; id pushed by the generated code
    call x86_trace_impl
    mov esp, [TMP_FRAME]
    popad
    popfd
    retn 4

; ---- VGA access helpers ---------------------------------------------------
; The generated code leaves the host address in VGA_ARG0 and, for stores, the
; value in VGA_ARG1; count and second address in VGA_ARG2.  Loads leave the
; result in VGA_RES.

%macro VGA_STORE 1
align 16
%1:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [VGA_ARG1]
    push dword [VGA_ARG0]
    call %1_impl
    add esp, 8
    mov esp, [TMP_FRAME]
    popad
    popfd
    ret
%endmacro

%macro VGA_LOAD 2
align 16
%1:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [VGA_ARG0]
    call %1_impl
    add esp, 4
    mov [VGA_RES], eax
    mov esp, [TMP_FRAME]
    popad
    popfd
    ret
%endmacro

VGA_STORE vga_store8
VGA_STORE vga_store16
VGA_STORE vga_store32
VGA_LOAD vga_load8, al
VGA_LOAD vga_load16, ax
VGA_LOAD vga_load32, eax

align 16
vga_stos8:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [VGA_ARG2]
    push dword [VGA_ARG1]
    push dword [VGA_ARG0]
    call vga_stos8_impl
    add esp, 12
    mov esp, [TMP_FRAME]
    popad
    popfd
    ret

align 16
vga_stos16:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [VGA_ARG2]
    push dword [VGA_ARG1]
    push dword [VGA_ARG0]
    call vga_stos16_impl
    add esp, 12
    mov esp, [TMP_FRAME]
    popad
    popfd
    ret

align 16
vga_movsb:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [VGA_ARG1]
    push dword [VGA_ARG0]
    push dword [VGA_ARG2]
    call vga_movsb_impl
    add esp, 12
    mov esp, [TMP_FRAME]
    popad
    popfd
    ret

align 16
vga_movsw:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    push dword [VGA_ARG1]
    push dword [VGA_ARG0]
    push dword [VGA_ARG2]
    call vga_movsw_impl
    add esp, 12
    mov esp, [TMP_FRAME]
    popad
    popfd
    ret

; ---- far return -----------------------------------------------------------
; A far call in the flat program leaves a 32-bit return address and a 4-byte
; CS slot.  A few paths build a real-mode IP:CS frame instead (a callback
; pushed by hand or a chained handler); the dword then decodes as
; (segment << 16) | offset.  Translate that case through the symbol table.
; All registers and the flags are preserved across the helper.
global flat_retf
flat_retf:
    pushfd
    push eax
    push edx
    mov eax, [esp+16]           ; target (below flags, eax, edx, return)
    cmp eax, 0x10000000
    jb .maybe_far
    cmp eax, 0x10100000
    jb .flat
.maybe_far:
    cmp eax, 0x20000000
    jb .far
    cmp eax, 0x20100000
    jb .flat
.far:
    mov edx, eax
    shr edx, 16
    and eax, 0xFFFF
    shl edx, 4
    add eax, edx
    push eax
    call dword [0x080F011C]     ; x86_code_ptr: real-mode address -> flat
    add esp, 4
    mov eax, [0x080F001C]
    mov [0x080F0158], eax
    pop edx
    pop eax
    popfd
    add esp, 8                  ; helper return + the IP:CS dword
    jmp dword [0x080F0158]
.flat:
    mov [0x080F0158], eax
    pop edx
    pop eax
    popfd
    add esp, 12                 ; helper return + address + CS slot
    jmp dword [0x080F0158]

; ---- start the recompiled program ---------------------------------------
; void run_guest(uint32_t entry)
global run_guest
run_guest:
    mov eax, [esp+4]
    mov [0x080F002C], esp        ; process stack for the helper calls
    mov esp, 0x03000100
    ; the guest register file only ever uses the low 16 bits of each
    ; register, but addressing is done with the full 32 bit register, so the
    ; upper halves have to start out clear
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    call eax
    mov esp, [0x080F002C]
    ret

; ---- periodic hook called at backward branches ---------------------------
; The generated code calls this through SR_CheckTimer at every backward
; branch.  When a timer tick is pending (and interrupts are enabled) the guest
; interrupt handler is entered with a real mode style frame on the guest stack.
align 16
SR_CheckTimer:
    pushfd
    pushad
    mov eax, esp
    mov [TMP_FRAME], eax
    mov edx, [eax+36]           ; caller in the generated code
    mov [CALLER], edx
    mov esp, [HOST_ESP]
    and esp, 0FFFFFFF0h
    call timer_check_impl
    mov [TMP_T], eax
    mov esp, [TMP_FRAME]
    popad
    cmp dword [TMP_T], 0
    je .done
    mov dword [IN_HANDLER], 1
    pushfd                      ; flags land at [esp+8] inside the handler
    push dword 0                ; CS slot
    mov dword [0x080F0168], 1   ; V_IN_TIMER: timer driver is running
    call dword [TMP_T]
    mov dword [0x080F0168], 0
    mov dword [IN_HANDLER], 0
.done:
    popfd                       ; guest flags must survive the whole hook
    ret
