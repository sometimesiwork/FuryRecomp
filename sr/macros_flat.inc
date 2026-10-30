; Runtime helper pointers and recompiler action macros for the flat build.

%define DS_BASE          0x080F0000
%define DS_SEL           0x080F0004
%define ES_BASE          0x080F0008
%define ES_SEL           0x080F000C
%define SS_SEL           0x080F0010
%define TMP_A            0x080F0014
%define TMP_V            0x080F0018
%define TMP_T            0x080F001C
%define IF_FLAG          0x080F0020
%define SS_BASE          0x080F0024

%define PTR_X86_INT      0x080F0100
%define PTR_OUT_DX_AL    0x080F0104
%define PTR_OUT_DX_AX    0x080F0108
%define PTR_IN_AL_DX     0x080F010C
%define PTR_OUT_IMM_AL   0x080F0110
%define PTR_IN_AL_IMM    0x080F0114
%define PTR_CHECK_TIMER  0x080F0118
%define VID_BASE_CUR      0x080F0040
%define PTR_TRACE         0x080F0120
%define PTR_CODE_PTR     0x080F011C

%define VGA_ARG0         0x080F0158
%define VGA_ARG1         0x080F015C
%define VGA_ARG2         0x080F0160
%define VGA_RES          0x080F0164
%define PTR_FLAT_RET     0x080F016C
%define TICK_FLAG        0x080F0170
%define KBD_FLAG         0x080F0174

; The timer/keyboard hook only has to run when something is pending.  At every
; backward branch a full helper call (pushad, host stack switch, C call) would
; dominate the run time, so test the two flags inline first.
%macro CHECK_TIMER_HOOK 0
    pushfd
    cmp dword [TICK_FLAG], 0
    jne %%run
    cmp dword [KBD_FLAG], 0
    je %%skip
%%run:
    call dword [PTR_CHECK_TIMER]
%%skip:
    popfd
%endmacro

%macro ACTION_UNCONDITIONAL_SHORT_JUMP_BACKWARD 0
    CHECK_TIMER_HOOK
%endmacro

%macro ACTION_UNCONDITIONAL_SHORT_JUMP_FORWARD 0
%endmacro

%macro ACTION_UNCONDITIONAL_JUMP 0
    CHECK_TIMER_HOOK
%endmacro

%macro ACTION_LOOP_BACKWARD 0
    CHECK_TIMER_HOOK
%endmacro

%macro ACTION_LOOP_FORWARD 0
%endmacro

%macro ACTION_CONDITIONAL_JUMP_BACKWARD 0
    CHECK_TIMER_HOOK
%endmacro

%macro ACTION_CONDITIONAL_JUMP_FORWARD 0
%endmacro

%macro ACTION_CALL 0
    CHECK_TIMER_HOOK
%endmacro
