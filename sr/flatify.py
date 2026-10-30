#!/usr/bin/env python3
"""Rewrite the recompiler's 16-bit assembly into flat 32-bit code.

The original program is a real mode DOS executable: it addresses memory with
16-bit offsets and segment registers.  The runtime runs it as a 32-bit flat
program, so every memory operand has to carry the base of its segment:

    [si]            ->  [<base> + esi]

DS and ES are loaded at run time from far pointers, so their bases live in
variables and a scratch register is used for each access; CS is static (code
never changes segment except through far calls, which the recompiler resolves
to absolute addresses) and SS is the runtime's stack buffer.

The output is assembled with ``nasm -f bin`` and ``org 0x10000``: the guest
memory is mapped at that host address, so a label is both the guest linear
address and the host address, and a paragraph is ``label >> 4``.
"""

import os
import re
import sys

# --- layout ----------------------------------------------------------------

ORG = 0x08000000
ORG_PARA = ORG >> 4
CALL_GATE = 0x080F0080

# Host address the flat code itself is loaded at.  The guest image (data,
# tables, strings, code used as data) is loaded at ORG + <guest address>, so
# every data reference keeps the address it had in the original executable.
# The recompiled code lives elsewhere: it is longer than the original code, so
# it cannot share the original layout.
CODEBASE = 0x20000000

# Guest linear addresses of the paragraphs the executable relocates into.
SEG_STARTS = [0x2070, 0x11D70, 0x155A0, 0x7BA10, 0x7FDA0, 0x8E260]

# Host address of the guest stack segment.  The runtime maps it twice (two
# adjacent 64 KiB views of one page-file section) so a 16-bit stack pointer
# wraps inside the segment.
STACK = 0x03000000

# "push cs" + near call is eight bytes in the recompiled code but six in
# real mode, so BP-frame arguments sit two bytes higher.
FRAME_DELTA = 2

# Variables and helper pointers, outside the guest memory image.
DS_BASE = 0x080F0000
DS_SEL = 0x080F0004
ES_BASE = 0x080F0008
ES_SEL = 0x080F000C
SS_SEL = 0x080F0010
TMP_A = 0x080F0014
TMP_V = 0x080F0018
TMP_T = 0x080F001C
IF_FLAG = 0x080F0020

HELPERS = {
    "x86_int": 0x080F0100,
    "x86_out_dx_al": 0x080F0104,
    "x86_out_dx_ax": 0x080F0108,
    "x86_in_al_dx": 0x080F010C,
    "x86_out_imm_al": 0x080F0110,
    "x86_in_al_imm": 0x080F0114,
    "SR_CheckTimer": 0x080F0118,
    "x86_code_ptr": 0x080F011C,
}

# The VGA helpers.  Every access through ES goes through them, so the runtime
# can apply the map mask, write mode, ALU and bit mask of the adapter.
VGA_PTR = {
    "store8": 0x080F0130, "store16": 0x080F0134, "store32": 0x080F0138,
    "load8": 0x080F013C, "load16": 0x080F0140, "load32": 0x080F0144,
    "stos8": 0x080F0148, "stos16": 0x080F014C,
    "movsb": 0x080F0150, "movsw": 0x080F0154,
}
VGA_ARG0 = 0x080F0158
VGA_ARG1 = 0x080F015C
VGA_ARG2 = 0x080F0160
VGA_RES = 0x080F0164

R16 = {
    "al", "ah", "ax", "bl", "bh", "bx", "cl", "ch", "cx",
    "dl", "dh", "dx", "si", "di", "bp", "sp",
}
R32 = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"}
REG16 = {"ax", "bx", "cx", "dx", "si", "di", "bp", "sp"}

R16_TO_R32 = {
    "ax": "eax", "bx": "ebx", "cx": "ecx", "dx": "edx",
    "si": "esi", "di": "edi", "bp": "ebp", "sp": "esp",
    "al": "eax", "ah": "eax", "bl": "ebx", "bh": "ebx",
    "cl": "ecx", "ch": "ecx", "dl": "edx", "dh": "edx",
}
R32_TO_R16 = {
    "eax": "ax", "ebx": "bx", "ecx": "cx", "edx": "dx",
    "esi": "si", "edi": "di", "ebp": "bp", "esp": "sp",
}
SREG = {"cs", "ds", "es", "ss", "fs", "gs"}
ADDR_REGS = {"bx", "bp", "si", "di"}

IMPLICIT = {
    "mul": {"eax", "edx"}, "imul": {"eax", "edx"},
    "div": {"eax", "edx"}, "idiv": {"eax", "edx"},
    "in": {"eax", "edx"}, "out": {"eax", "edx"},
    "loop": {"ecx"}, "loope": {"ecx"}, "loopne": {"ecx"},
    "pushad": {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"},
    "popad": {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"},
    "stosb": {"eax", "edi", "ecx"}, "stosw": {"eax", "edi", "ecx"},
    "lodsb": {"eax", "esi", "ecx"}, "lodsw": {"eax", "esi", "ecx"},
    "movsb": {"eax", "esi", "edi", "ecx"}, "movsw": {"eax", "esi", "edi", "ecx"},
    "scasb": {"eax", "edi", "ecx"}, "cmpsb": {"esi", "edi", "ecx"},
}

SCRATCH_ORDER = ["ecx", "edx", "eax", "ebx", "esi", "edi", "ebp"]


class Unsupported(Exception):
    pass


def split_ops(text):
    ops, depth, cur = [], 0, ""
    for ch in text:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            ops.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        ops.append(cur.strip())
    return ops


def split_mnemonic(line):
    parts = line.split(None, 1)
    return parts[0].lower(), split_ops(parts[1]) if len(parts) > 1 else []


def regs_in(text):
    out = set()
    for tok in re.findall(r"[A-Za-z][A-Za-z0-9]*", text):
        t = tok.lower()
        if t in R16 or t in R32:
            out.add(R16_TO_R32.get(t, t))
    return out


def parse_mem(op):
    m = re.match(r"^(?:(byte|word|dword)\s+)?\[(.*)\]$", op.strip(), re.I)
    if not m:
        return None
    return (m.group(1).lower() if m.group(1) else ""), m.group(2)


def es_mem(op):
    """Return (size, inner) for an explicit [es:...] operand."""
    m = re.match(r"^(?:(byte|word|dword)\s+)?\[es:(.*)\]$", op.strip(), re.I)
    if not m:
        return None
    return (m.group(1).lower() if m.group(1) else ""), m.group(2)


def reg_size(op):
    r = op.strip().lower()
    if r in ("al", "ah", "bl", "bh", "cl", "ch", "dl", "dh"):
        return 1
    if r in R16:
        return 2
    if r in R32:
        return 4
    return 0


def is_immediate(op):
    return re.match(r"^[-+]?(0x[0-9a-fA-F]+|\d+)$", op.strip()) is not None


def parse_addr(inner):
    m = re.match(r"^(cs|ds|es|ss|fs|gs):(.*)$", inner.strip(), re.I)
    seg = None
    if m:
        seg, inner = m.group(1).lower(), m.group(2)
    regs, disp = [], 0
    for term in re.findall(r"[+-]?[^+-]+", inner.strip()):
        t = term.strip()
        sign = 1
        if t.startswith("+"):
            t = t[1:].strip()
        elif t.startswith("-"):
            sign, t = -1, t[1:].strip()
        if not t:
            continue
        low = t.lower()
        if low in ADDR_REGS:
            regs.append(low)
        elif low in ("sp", "esp", "ebp"):
            return None
        else:
            disp += sign * int(t, 0)
    return seg, regs, disp


def fmt_mem(size, regs, disp):
    parts = list(regs)
    if not parts:
        return (size + " " if size else "") + "[" + hex(disp & 0xFFFFFFFF) + "]"
    if disp > 0:
        parts.append(hex(disp))
    elif disp < 0:
        parts.append("-" + hex(-disp))
    body = "+".join(parts)
    return (size + " " if size else "") + "[" + body + "]"


def choose_scratch(used, mnem, extra=()):
    implicit = set(IMPLICIT.get(mnem, set()))
    for t in extra:
        implicit |= regs_in(t)
    for r in SCRATCH_ORDER:
        if r not in used and r not in implicit:
            return r
    return None


def seg_start(label_addr):
    base = 0
    for s in SEG_STARTS:
        if s <= label_addr:
            base = s
    return base


def cs_base(label_addr):
    return ORG + seg_start(label_addr)


def cs_paragraph(label_addr):
    """Guest paragraph of the code segment containing the instruction."""
    return seg_start(label_addr) >> 4


class Rewriter:
    def __init__(self):
        self.line = ""
        self.label_addr = 0
        self.errors = []
        self.uid = 0
        self.bp_delta = 0

    # -- address computation ------------------------------------------------

    # A far call in the recompiled program pushes a 32-bit return address and a
    # 32-bit CS, i.e. four bytes more than the real-mode frame.  Code that
    # reaches its arguments through "mov bp, sp" therefore finds them four
    # bytes higher; locals (negative displacements) are unaffected.
    #
    # However, most routines here use "mov bp, sp" *without* pushing bp, so
    # their arguments sit exactly at [bp+0..] and no adjustment is wanted.
    # The delta only applies to a classic "push bp; mov bp, sp" prologue,
    # where [bp+0] is the saved bp and the first argument is at [bp+4].
    def frame_disp(self, seg, reg32, disp):
        if (seg == "ss" and len(reg32) == 1 and reg32[0] == "ebp"
                and disp > 0 and self.bp_delta):
            return disp + self.bp_delta
        return disp

    def addr_to_scratch(self, out, scratch, seg, reg32, disp):
        """Leave the complete host address of a memory operand in scratch."""
        if seg == "ss":
            disp = self.frame_disp(seg, reg32, disp)
            if reg32:
                out.append(f"lea {scratch}, {fmt_mem('', reg32, STACK + disp)}")
            else:
                out.append(f"mov {scratch}, {hex((STACK + disp) & 0xFFFFFFFF)}")
        elif seg == "cs":
            base = cs_base(self.label_addr)
            if reg32:
                out.append(f"lea {scratch}, {fmt_mem('', reg32, base + disp)}")
            else:
                out.append(f"mov {scratch}, {hex((base + disp) & 0xFFFFFFFF)}")
        else:
            out.append(f"mov {scratch}, [{seg.upper()}_BASE]")
            if len(reg32) == 2:
                out.append(f"lea {scratch}, [{scratch}+{reg32[0]}]")
                out.append(f"lea {scratch}, {fmt_mem('', [reg32[1]], disp)}".replace(
                    "[", f"[{scratch}+", 1))
            elif len(reg32) == 1:
                out.append(f"lea {scratch}, {fmt_mem('', [reg32[0]], disp)}".replace(
                    "[", f"[{scratch}+", 1))
            else:
                out.append(f"lea {scratch}, [{scratch}+{hex(disp & 0xFFFFFFFF)}]")

    def operand_expr(self, out, size, inner, used, mnem):
        """Rewrite a memory operand, returning (text, scratch-to-pop)."""
        parsed = parse_addr(inner)
        if parsed is None:
            return None
        seg, regs, disp = parsed
        if seg is None:
            seg = "ss" if "bp" in regs else "ds"
        reg32 = [R16_TO_R32[r] for r in regs]
        if reg32 == ["ebp"]:
            disp = self.frame_disp("ss", reg32, disp) if seg == "ss" else disp
        if seg == "ss":
            return fmt_mem(size, reg32, STACK + disp), None
        if seg == "cs":
            return fmt_mem(size, reg32, cs_base(self.label_addr) + disp), None
        scratch = choose_scratch(used, mnem)
        if scratch is None:
            raise Unsupported(f"no scratch register: {self.line}")
        out.append(f"push {scratch}")
        out.append(f"mov {scratch}, [{seg.upper()}_BASE]")
        if len(reg32) == 2:
            out.append(f"lea {scratch}, [{scratch}+{reg32[0]}]")
            rest = [reg32[1]]
        else:
            rest = reg32
        return fmt_mem(size, [scratch] + rest, disp), scratch

    def mem_address(self, out, inner, used, mnem):
        """Compute the complete host address of a memory operand.

        Returns (scratch, pushed): the address is in ``scratch`` and the
        caller has to pop it when ``pushed`` is true.
        """
        parsed = parse_addr(inner)
        if parsed is None:
            raise Unsupported(f"host stack operand: {self.line}")
        seg, regs, disp = parsed
        if seg is None:
            seg = "ss" if "bp" in regs else "ds"
        reg32 = [R16_TO_R32[r] for r in regs]
        scratch = choose_scratch(used, mnem)
        if scratch is None:
            raise Unsupported(f"no scratch register: {self.line}")
        out.append(f"push {scratch}")
        self.addr_to_scratch(out, scratch, seg, reg32, disp)
        return scratch, True

    # -- instruction rewrite ------------------------------------------------

    WRITES_DST = {
        "mov", "add", "sub", "adc", "sbb", "and", "or", "xor", "inc", "dec",
        "neg", "not", "shl", "shr", "sal", "sar", "rol", "ror", "rcl", "rcr",
        "lea", "pop", "imul", "les", "lds",
    }

    def extend16(self, out, line):
        """Zero the upper half of every 16-bit register the instruction wrote."""
        if "[esp" in line or "[ebp" in line:
            return out
        mnem, ops = split_mnemonic(line)
        if not ops and mnem not in ("cbw", "cwd", "lodsw"):
            return out
        extra = []
        if mnem in ("mul", "imul", "div", "idiv") and len(ops) == 1:
            return out + ["movzx eax, ax", "movzx edx, dx"]
        if mnem == "cbw":
            return out + ["movzx eax, ax"]
        if mnem == "cwd":
            return out + ["movzx edx, dx"]
        if mnem == "lodsw":
            return out + ["movzx eax, ax"]
        if mnem == "xchg" and len(ops) == 2:
            for op in ops:
                d = op.lower()
                if d in REG16 and d != "sp":
                    extra.append(f"movzx {R16_TO_R32[d]}, {d}")
            return out + extra
        if mnem in self.WRITES_DST:
            d = ops[0].lower()
            if d in REG16 and d != "sp":
                extra.append(f"movzx {R16_TO_R32[d]}, {d}")
        return out + extra

    def rewrite(self, line, label_addr):
        return self.extend16(self._rewrite(line, label_addr), line)

    def _rewrite(self, line, label_addr):
        self.line = line
        self.label_addr = label_addr
        # The recompiler builds interrupt frames on the host stack itself.
        if "[esp" in line or "[ebp" in line:
            return [line]
        mnem, ops = split_mnemonic(line)
        low = mnem

        if low == "call" and len(ops) == 1 and ops[0] in HELPERS:
            return [f"call dword [{hex(HELPERS[ops[0]])}]"]
        if low in ("cli", "sti"):
            return [f"mov byte [{hex(IF_FLAG)}], {0 if low == 'cli' else 1}"]
        if low == "rep":
            return self.rewrite_string(line.split(None, 1)[1].strip(), rep=True)
        if low in ("lodsb", "lodsw", "stosb", "stosw", "movsb", "movsw", "xlatb"):
            return self.rewrite_string(line, rep=False)
        if low == "retf":
            # A far call leaves a 4-byte CS slot under its 32-bit return
            # address; a far return pops the address and discards that slot.
            return ["ret 0x4"]
        if low == "mov" and len(ops) == 2 and (
                ops[0].lower() in SREG or ops[1].lower() in SREG):
            return self.rewrite_seg_mov(ops)
        if low in ("push", "pop") and len(ops) == 1 and ops[0].lower() in SREG:
            return self.rewrite_seg_push(low, ops[0].lower())
        if low == "les":
            return self.rewrite_les(ops)
        if low == "jmp" and re.match(r"word\s+near\s+\[", ops[0], re.I):
            return self.rewrite_indirect_jmp(ops[0])
        if low == "call" and ops and ops[0].lower() in R16:
            return self.rewrite_indirect_call(ops[0].lower())
        if low in ("push", "pop") and len(ops) == 1 and "[" in ops[0]:
            return self.rewrite_push_pop_mem(low, ops[0])
        if low == "loop" and len(ops) == 1:
            # Keep the original encoding; repair_short_jumps.py gives the few
            # loops that no longer reach their target a near relay.
            return [f"loop {ops[0]}"]
        if low == "mov" and len(ops) == 2:
            m = es_mem(ops[0])
            if m is not None:
                return self.es_store(m, ops[1])
            m = es_mem(ops[1])
            if m is not None:
                return self.es_load(m, ops[0])
        if low in ("or", "and", "add", "sub", "xor", "adc", "sbb") and len(ops) == 2:
            m = es_mem(ops[0])
            if m is not None:
                return self.es_rmw(low, m, ops[1])
        if low in ("neg", "not", "inc", "dec") and len(ops) == 1:
            m = es_mem(ops[0])
            if m is not None:
                return self.es_unary(low, m)
        if low in ("test", "cmp") and len(ops) == 2:
            m = es_mem(ops[0])
            if m is not None:
                return self.es_compare(low, m, ops[1])
        if any(op.lower() == "sp" for op in ops):
            return self.rewrite_sp(mnem, ops)
        if low == "lea":
            return self.rewrite_lea(ops)
        if any("[" in op for op in ops):
            return self.rewrite_generic(mnem, ops)
        return [line]

    # -- segment registers --------------------------------------------------

    # ES accesses go through the VGA helpers, which apply the plane selection
    # per store.  DS/SS are still remapped at segment load: the game also
    # writes video memory through DS in a few routines.
    def vid_fix(self, out, reg, sreg):
        if sreg == "es":
            return
        self.uid += 1
        lbl = f"vidfix{self.uid}"
        out.append("pushfd")
        out.append(f"cmp {reg}, 0x080a0000")
        out.append(f"jne short {lbl}")
        out.append(f"mov {reg}, [VID_BASE_CUR]")
        out.append(f"{lbl}:")
        out.append("popfd")

    def rewrite_seg_mov(self, ops):
        dst, src = ops
        d, s = dst.lower(), src.lower()
        if d in SREG:
            mem = None if s in SREG or s in R16 else parse_mem(src)
            if s in SREG:
                out = [
                    "push eax",
                    f"mov ax, [{s.upper()}_SEL]",
                    f"mov word [{d.upper()}_SEL], ax",
                    "movzx eax, ax",
                    f"lea eax, [eax+{ORG_PARA:#x}]",
                    "lea eax, [eax*8]",
                    "lea eax, [eax*2]",
                ]
                self.vid_fix(out, "eax", d)
                out += [f"mov [{d.upper()}_BASE], eax", "pop eax"]
                return out
            if mem is not None:
                size, inner = mem
                out = []
                used = regs_in(dst) | regs_in(src)
                scratch, _ = self.mem_address(out, inner, used, "mov")
                out.append(f"movzx {scratch}, word [{scratch}]")
                out.append(f"mov word [{d.upper()}_SEL], {R32_TO_R16[scratch]}")
                out.append(f"lea {scratch}, [{scratch}+{ORG_PARA:#x}]")
                out.append(f"lea {scratch}, [{scratch}*8]")
                out.append(f"lea {scratch}, [{scratch}*2]")
                self.vid_fix(out, scratch, d)
                out.append(f"mov [{d.upper()}_BASE], {scratch}")
                out.append(f"pop {scratch}")
                return out
            if s in R16:
                out = [
                    "push ecx",
                    f"mov word [{d.upper()}_SEL], {s}",
                    f"movzx ecx, {s}",
                    f"lea ecx, [ecx+{ORG_PARA:#x}]",
                    "lea ecx, [ecx*8]",
                    "lea ecx, [ecx*2]",
                ]
                self.vid_fix(out, "ecx", d)
                out += [f"mov [{d.upper()}_BASE], ecx", "pop ecx"]
                return out
            raise Unsupported(f"segment load: {self.line}")

        if d in R16:
            if s == "cs":
                return [f"mov {dst}, {hex(cs_paragraph(self.label_addr))}"]
            return [f"mov {dst}, [{s.upper()}_SEL]"]
        if d == "eax":
            return [f"mov eax, [{s.upper()}_SEL]"]

        mem = parse_mem(dst)
        if mem is None:
            raise Unsupported(f"segment move: {self.line}")
        size, inner = mem
        used = regs_in(dst) | regs_in(src)
        out = []
        scratch, pushed = self.mem_address(out, inner, used, "mov")
        if s == "cs":
            out.append(f"mov word [{scratch}], {hex(cs_paragraph(self.label_addr))}")
        else:
            s2 = choose_scratch(used | {scratch}, "mov")
            out.append(f"push {s2}")
            out.append(f"mov {s2}, [{s.upper()}_SEL]")
            out.append(f"mov [{scratch}], {R32_TO_R16[s2]}")
            out.append(f"pop {s2}")
        out.append(f"pop {scratch}")
        return out

    def rewrite_seg_push(self, mnem, sreg):
        if mnem == "push":
            if sreg == "cs":
                # two bytes: the guest stack layout has to stay intact when the
                # value is later popped with "pop ds"/"pop es"
                return [f"push word {hex(cs_paragraph(self.label_addr))}"]
            return [f"push word [{sreg.upper()}_SEL]"]
        if sreg == "cs":
            raise Unsupported(f"pop cs: {self.line}")
        return [
            f"pop word [{sreg.upper()}_SEL]",
            "push ecx",
            f"movzx ecx, word [{sreg.upper()}_SEL]",
            f"lea ecx, [ecx+{ORG_PARA:#x}]",
            "lea ecx, [ecx*8]",
            "lea ecx, [ecx*2]",
        ] + (lambda o: (self.vid_fix(o, "ecx", sreg), o.append(f"mov [{sreg.upper()}_BASE], ecx"), o.append("pop ecx"), o)[3])([])

    def rewrite_les(self, ops):
        dst, src = ops
        if dst.lower() not in R16:
            raise Unsupported(f"les: {self.line}")
        mem = parse_mem(src)
        if mem is None:
            raise Unsupported(f"les: {self.line}")
        _size, inner = mem
        used = regs_in(dst) | regs_in(src)
        out = []
        scratch, _ = self.mem_address(out, inner, used, "mov")
        out.append(f"mov {dst}, [{scratch}]")
        out.append(f"movzx {scratch}, word [{scratch}+2]")
        out.append(f"mov word [ES_SEL], {R32_TO_R16[scratch]}")
        out.append(f"lea {scratch}, [{scratch}+{ORG_PARA:#x}]")
        out.append(f"lea {scratch}, [{scratch}*8]")
        out.append(f"lea {scratch}, [{scratch}*2]")
        out.append("mov [ES_BASE], " + scratch)
        out.append(f"pop {scratch}")
        return out

    # -- control transfers --------------------------------------------------

    def rewrite_indirect_jmp(self, op):
        op = re.sub(r"^(word\s+)?(near\s+)?", "", op.strip(), flags=re.I)
        mem = parse_mem(op)
        if mem is None:
            raise Unsupported(f"indirect jmp: {self.line}")
        _size, inner = mem
        out = []
        scratch, _ = self.mem_address(out, inner, set(), "mov")
        out.append(f"movzx {scratch}, word [{scratch}]")
        out.append(f"add {scratch}, {hex(seg_start(self.label_addr))}")
        out.append(f"push {scratch}")
        out.append(f"call dword [{hex(HELPERS['x86_code_ptr'])}]")
        out.append("mov [TMP_T], eax")
        out.append(f"pop {scratch}")
        out.append("jmp dword [TMP_T]")
        return out

    def rewrite_indirect_call(self, reg):
        base = seg_start(self.label_addr)
        return [
            "push eax",
            f"movzx eax, {reg}",
            f"add eax, {hex(base)}",
            "push eax",
            f"call dword [{hex(HELPERS['x86_code_ptr'])}]",
            "mov [TMP_T], eax",
            "pop eax",
            "call dword [TMP_T]",
        ]

    def rewrite_push_pop_mem(self, mnem, op):
        mem = parse_mem(op)
        _size, inner = mem
        if mnem == "push":
            out = []
            scratch, _ = self.mem_address(out, inner, set(), "mov")
            out.append(f"movzx {scratch}, word [{scratch}]")
            out.append(f"mov [TMP_V], {scratch}")
            out.append(f"pop {scratch}")
            out.append("push word [TMP_V]")
            return out
        out = []
        s1, _ = self.mem_address(out, inner, set(), "mov")
        out.append(f"mov [TMP_A], {s1}")
        out.append(f"pop {s1}")
        out.append("pop word [TMP_V]")
        s2 = choose_scratch({s1}, "mov")
        out.append(f"push {s1}")
        out.append(f"mov {s1}, [TMP_A]")
        out.append(f"push {s2}")
        out.append(f"mov {s2}, [TMP_V]")
        out.append(f"mov [{s1}], {R32_TO_R16[s2]}")
        out.append(f"pop {s2}")
        out.append(f"pop {s1}")
        return out

    # -- stack pointer ------------------------------------------------------

    def rewrite_sp(self, mnem, ops):
        if mnem == "mov" and len(ops) == 2 and ops[0].lower() == "sp":
            src = ops[1]
            if src.lower() in R16:
                return [f"lea esp, [{R16_TO_R32[src.lower()]}+{hex(STACK)}]"]
            if re.match(r"^([-+]?0x[0-9a-fA-F]+|\d+)$", src):
                return [f"mov esp, {hex(STACK + int(src, 0))}"]
            raise Unsupported(f"mov sp: {self.line}")
        if mnem in ("add", "sub") and len(ops) == 2 and ops[0].lower() == "sp":
            rhs = R16_TO_R32.get(ops[1].lower(), ops[1])
            return [f"{mnem} esp, {rhs}"]
        if mnem in ("inc", "dec") and len(ops) == 1 and ops[0].lower() == "sp":
            return [f"{mnem} esp"]
        if mnem == "xchg" and any(op.lower() == "sp" for op in ops):
            raise Unsupported(f"xchg sp: {self.line}")
        return [mnem + (" " + ", ".join(ops) if ops else "")]

    def rewrite_lea(self, ops):
        dst, src = ops
        mem = parse_mem(src)
        if mem is None:
            raise Unsupported(f"lea: {self.line}")
        _size, inner = mem
        parsed = parse_addr(inner)
        if parsed is None:
            raise Unsupported(f"lea host stack: {self.line}")
        _seg, regs, disp = parsed
        return [f"lea {dst}, {fmt_mem('', [R16_TO_R32[r] for r in regs], disp)}"]

    # -- ES / VGA accesses --------------------------------------------------

    LOW8 = {"ecx": "cl", "edx": "dl", "eax": "al", "ebx": "bl"}
    LOW16 = {"ecx": "cx", "edx": "dx", "eax": "ax", "ebx": "bx"}
    SIZE_OF = {"": 0, "byte": 1, "word": 2, "dword": 4}
    NAME_OF = {1: "byte", 2: "word", 4: "dword"}
    PTR_STORE = {1: "store8", 2: "store16", 4: "store32"}
    PTR_LOAD = {1: "load8", 2: "load16", 4: "load32"}

    def es_addr(self, out, inner, scratch, used):
        parsed = parse_addr("es:" + inner)
        if parsed is None:
            raise Unsupported(f"es address: {self.line}")
        _seg, regs, disp = parsed
        reg32 = [R16_TO_R32[r] for r in regs]
        self.addr_to_scratch(out, scratch, "es", reg32, disp)

    def es_store(self, mem, value):
        size, inner = mem
        n = self.SIZE_OF.get(size, 0)
        if not n:
            n = reg_size(value) or 1
        if n not in (1, 2, 4):
            raise Unsupported(f"es store size: {self.line}")
        used = regs_in(value) | set(parse_addr("es:" + inner)[1])
        scratch = choose_scratch(used, "mov")
        if scratch is None:
            raise Unsupported(f"no scratch: {self.line}")
        out = [f"push {scratch}"]
        self.es_addr(out, inner, scratch, used)
        out.append(f"mov [VGA_ARG0], {scratch}")
        if reg_size(value) or value.lower() in R32:
            out.append(f"mov [VGA_ARG1], {value}")
        else:
            out.append(f"mov dword [VGA_ARG1], {value}")
        out.append(f"call dword [{hex(VGA_PTR[self.PTR_STORE[n]])}]")
        out.append(f"pop {scratch}")
        return out

    def es_load(self, mem, dst):
        size, inner = mem
        n = self.SIZE_OF.get(size, 0)
        if not n:
            n = reg_size(dst) or 1
        used = regs_in(dst) | set(parse_addr("es:" + inner)[1])
        scratch = choose_scratch(used, "mov")
        if scratch is None:
            raise Unsupported(f"no scratch: {self.line}")
        out = [f"push {scratch}"]
        self.es_addr(out, inner, scratch, used)
        out.append(f"mov [VGA_ARG0], {scratch}")
        out.append(f"call dword [{hex(VGA_PTR[self.PTR_LOAD[n]])}]")
        out.append(f"mov {dst}, {self.NAME_OF[n]} [VGA_RES]")
        out.append(f"pop {scratch}")
        return out

    def es_rmw_scratch(self, used):
        for r in ("ecx", "edx", "eax", "ebx"):
            if r not in used:
                return r
        return None

    def es_rmw(self, mnem, mem, src):
        size, inner = mem
        n = self.SIZE_OF.get(size, 0) or reg_size(src) or 1
        used = regs_in(src) | set(parse_addr("es:" + inner)[1])
        scratch = self.es_rmw_scratch(used)
        if scratch is None:
            raise Unsupported(f"no scratch: {self.line}")
        lo = (self.LOW8 if n == 1 else self.LOW16)[scratch]
        out = [f"push {scratch}"]
        self.es_addr(out, inner, scratch, used)
        out.append(f"mov [VGA_ARG0], {scratch}")
        out.append(f"call dword [{hex(VGA_PTR[self.PTR_LOAD[n]])}]")
        out.append(f"mov {lo}, {self.NAME_OF[n]} [VGA_RES]")
        out.append(f"{mnem} {lo}, {src}")
        out.append(f"mov [VGA_ARG1], {lo}")
        out.append(f"call dword [{hex(VGA_PTR[self.PTR_STORE[n]])}]")
        out.append(f"pop {scratch}")
        return out

    def es_unary(self, mnem, mem):
        size, inner = mem
        n = self.SIZE_OF.get(size, 0) or 1
        used = set(parse_addr("es:" + inner)[1])
        scratch = self.es_rmw_scratch(used)
        if scratch is None:
            raise Unsupported(f"no scratch: {self.line}")
        lo = (self.LOW8 if n == 1 else self.LOW16)[scratch]
        out = [f"push {scratch}"]
        self.es_addr(out, inner, scratch, used)
        out.append(f"mov [VGA_ARG0], {scratch}")
        out.append(f"call dword [{hex(VGA_PTR[self.PTR_LOAD[n]])}]")
        out.append(f"mov {lo}, {self.NAME_OF[n]} [VGA_RES]")
        out.append(f"{mnem} {lo}")
        out.append(f"mov [VGA_ARG1], {lo}")
        out.append(f"call dword [{hex(VGA_PTR[self.PTR_STORE[n]])}]")
        out.append(f"pop {scratch}")
        return out

    def es_compare(self, mnem, mem, src):
        size, inner = mem
        n = self.SIZE_OF.get(size, 0) or reg_size(src) or 1
        used = regs_in(src) | set(parse_addr("es:" + inner)[1])
        scratch = self.es_rmw_scratch(used)
        if scratch is None:
            raise Unsupported(f"no scratch: {self.line}")
        lo = (self.LOW8 if n == 1 else self.LOW16)[scratch]
        out = [f"push {scratch}"]
        self.es_addr(out, inner, scratch, used)
        out.append(f"mov [VGA_ARG0], {scratch}")
        out.append(f"call dword [{hex(VGA_PTR[self.PTR_LOAD[n]])}]")
        out.append(f"mov {lo}, {self.NAME_OF[n]} [VGA_RES]")
        out.append(f"{mnem} {lo}, {src}")
        out.append(f"pop {scratch}")
        return out

    def rewrite_generic(self, mnem, ops):
        used = regs_in(mnem + " " + ", ".join(ops))
        out, new_ops, scratch = [], [], None
        for op in ops:
            mem = parse_mem(op)
            if mem is None:
                new_ops.append(op)
                continue
            size, inner = mem
            res = self.operand_expr(out, size, inner, used, mnem)
            if res is None:
                new_ops.append(op)
                continue
            text, sc = res
            new_ops.append(text)
            if sc is not None:
                scratch = sc
        out.append(mnem + " " + ", ".join(new_ops))
        if scratch is not None:
            out.append(f"pop {scratch}")
        return out

    # -- string instructions ------------------------------------------------

    def rewrite_string(self, text, rep):
        mnem = text.split(None, 1)[0].lower()
        if mnem in ("lodsb", "lodsw"):
            if rep:
                raise Unsupported(f"rep lods: {self.line}")
            size = "al" if mnem == "lodsb" else "ax"
            step = 1 if mnem == "lodsb" else 2
            return [
                "push ecx",
                "mov ecx, [DS_BASE]",
                f"mov {size}, [ecx+esi]",
                "pop ecx",
                f"lea esi, [esi+{step}]",
            ]
        if mnem in ("stosb", "stosw"):
            step = 1 if mnem == "stosb" else 2
            size = "al" if step == 1 else "ax"
            ptr = VGA_PTR["stos8" if step == 1 else "stos16"]
            if not rep:
                return [
                    "push ecx",
                    "mov ecx, [ES_BASE]",
                    "lea ecx, [ecx+edi]",
                    f"mov [VGA_ARG0], ecx",
                    f"mov [VGA_ARG1], {size}",
                    "mov dword [VGA_ARG2], 1",
                    f"call dword [{hex(ptr)}]",
                    "pop ecx",
                    f"lea edi, [edi+{step}]",
                ]
            add = f"lea edi, [edi+ecx]" if step == 1 else "lea edi, [edi+ecx*2]"
            return [
                "pushfd",
                "push eax",
                "push edi",
                "push ecx",
                "mov eax, edi",
                "mov edi, [ES_BASE]",
                "lea edi, [edi+eax]",
                "mov eax, [esp+8]",
                "mov [VGA_ARG0], edi",
                "mov [VGA_ARG1], eax",
                "mov [VGA_ARG2], ecx",
                f"call dword [{hex(ptr)}]",
                "mov edi, [esp+4]",
                add,
                "and edi, 0xFFFF",
                "mov [esp+4], edi",
                "mov dword [esp+0], 0",
                "pop ecx",
                "pop edi",
                "pop eax",
                "popfd",
            ]
        if mnem in ("movsb", "movsw"):
            step = 1 if mnem == "movsb" else 2
            ptr = VGA_PTR["movsb" if step == 1 else "movsw"]
            if not rep:
                return [
                    "pushfd",
                    "push eax",
                    "push esi",
                    "push edi",
                    "mov eax, esi",
                    "mov esi, [DS_BASE]",
                    "lea esi, [esi+eax]",
                    "mov eax, edi",
                    "mov edi, [ES_BASE]",
                    "lea edi, [edi+eax]",
                    "mov [VGA_ARG0], edi",
                    "mov dword [VGA_ARG1], 1",
                    "mov [VGA_ARG2], esi",
                    f"call dword [{hex(ptr)}]",
                    "pop edi",
                    "pop esi",
                    "pop eax",
                    f"lea esi, [esi+{step}]",
                    f"lea edi, [edi+{step}]",
                    "popfd",
                ]
            return [
                "pushfd",
                "push eax",
                "push esi",
                "push edi",
                "push ecx",
                "mov eax, esi",
                "mov esi, [DS_BASE]",
                "lea esi, [esi+eax]",
                "mov eax, edi",
                "mov edi, [ES_BASE]",
                "lea edi, [edi+eax]",
                "mov [VGA_ARG0], edi",
                "mov [VGA_ARG1], ecx",
                "mov [VGA_ARG2], esi",
                f"call dword [{hex(ptr)}]",
                "mov eax, ecx",
                f"lea eax, [eax*{step}]",
                "add [esp+8], eax",
                "add [esp+4], eax",
                "and dword [esp+4], 0xFFFF",
                "mov dword [esp+0], 0",
                "pop ecx",
                "pop edi",
                "pop esi",
                "pop eax",
                "popfd",
            ]
        if mnem == "xlatb":
            return [
                "push ecx",
                "mov ecx, [DS_BASE]",
                "lea ecx, [ecx+ebx]",
                "mov al, [ecx+eax]",
                "pop ecx",
            ]
        raise Unsupported(f"string op: {self.line}")


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "seg01.inc"
    dst = sys.argv[2] if len(sys.argv) > 2 else "seg01_flat.inc"
    lines = open(src, encoding="utf-8", errors="replace").read().splitlines()

    trace = {int(x, 16) for x in
             os.environ.get("FURY_TRACE", "").replace("0x", "").split(",")
             if x.strip()}
    rw = Rewriter()
    out, errors = [], []
    label_addr = 0
    prev_line = ""
    for no, raw in enumerate(lines, 1):
        s = raw.strip()
        if not s:
            out.append("")
            continue
        if s.endswith(":"):
            m = re.match(r"loc_([0-9A-Fa-f]+)$", s[:-1])
            if m:
                label_addr = int(m.group(1), 16)
            out.append(s)
            if label_addr in trace:
                out.append(f"push dword {label_addr:#x}")
                out.append("call dword [0x080F0120]")
            continue
        if s.startswith(("db ", "align", "%", "section", "global")):
            out.append(s)
            continue
        if s.startswith("ACTION_"):
            out.append(s)
            continue
        if s == "mov bp, sp":
            # A saved register right below bp (push bp / push ds) means the
            # arguments live above the return address and must be shifted by
            # the wider flat return address.  Without such a push the pushed
            # words are the frame itself and need no adjustment.
            rw.bp_delta = FRAME_DELTA if prev_line in ("push bp", "push ds") else 0
        try:
            rewritten = rw.rewrite(s, label_addr)
        except Unsupported as exc:
            errors.append((no, str(exc)))
            rewritten = [s]
        prev_line = s
        # "push cs" + near call is the original program's same-segment far
        # call: the callee returns with retf, which the recompiler emits as a
        # return that discards a 4-byte CS slot. A plain 2-byte push would
        # leave the stack two bytes off after every such call, so widen the
        # slot to a dword here. ("push cs" + "pop ds" stays a word: the pop
        # consumes exactly two bytes.)
        if s == "push cs":
            for j in range(no + 1, len(lines)):
                t = lines[j].strip()
                if not t or t.startswith("ACTION_"):
                    continue
                if t.startswith("call "):
                    rewritten = ["push dword " + rewritten[0].split()[-1]]
                break
        # `add sp, 2` just before a non-local exit discards the near return
        # address.  The original return address is two bytes, the flat one is
        # four, so widen this adjustment too.  A plain argument cleanup has no
        # branch in the next few instructions and stays as it is.
        if s.replace(" ", "") in ("addsp,0x2", "addsp,2"):
            jumpish = False
            for j in range(no, min(no + 5, len(lines))):
                t = lines[j].strip()
                if not t or t.startswith("ACTION_") or t.endswith(":"):
                    continue
                if t == "ret" or t == "retf" or t.startswith("jmp "):
                    jumpish = True
                    break
                if t.startswith("call "):
                    break
            if jumpish:
                rewritten = ["add esp, 4"]
        out.extend(rewritten)

    # The recompiler writes segment immediates as (label >> 4); NASM cannot
    # shift a label, but the paragraph value is known here.
    # SR may enter an instruction in the middle of a paragraph through a
    # vector while a far call names the paragraph base (offset zero). Alias
    # that base to the exact first emitted instruction so both addresses resolve.
    labels = {int(m.group(1), 16) for line in out
              if (m := re.match(r"^loc_([0-9A-Fa-f]+):$", line))}
    referenced = {int(value, 16) for line in out
                  for value in re.findall(r"loc_([0-9A-Fa-f]+)", line)
                  if not re.match(r"^loc_" + value + r":$", line)}
    aliases = {}
    for address in referenced:
        if (address & 0xF) == 0 and address not in labels:
            alias = next((candidate for candidate in labels
                          if address < candidate < address + 0x10), None)
            if alias is not None:
                aliases[address] = alias
    if aliases:
        alias_pattern = re.compile(r"loc_([0-9A-Fa-f]+)")
        out = [alias_pattern.sub(
            lambda m: f"loc_{aliases.get(int(m.group(1), 16), int(m.group(1), 16)):X}",
            ln) for ln in out]

    para = re.compile(r"\(loc_([0-9A-Fa-f]+)\s*>>\s*4\)")
    out = [para.sub(lambda m: hex(int(m.group(1), 16) >> 4), ln) for ln in out]

    # Let NASM choose the reach of every jmp/jcc.  SR emits the original short
    # encodings, but the recompiled instructions are longer, so many of them no
    # longer fit; leaving the choice to NASM removes a whole repair round.
    short = re.compile(r"^(\s*(?:jmp|j[a-z]+)\s+)short\s+")
    out = [short.sub(lambda m: m.group(1), ln) for ln in out]

    # Far returns keep the flat convention (a 32-bit return address and a
    # 4-byte CS slot), but a few paths push a real-mode IP:CS pair instead.
    # flat_retf tells the two apart and translates the real-mode pair.
    ret4 = re.compile(r"^(\s*)ret 0x4$")
    out = [ret4.sub(lambda m: m.group(1) + "call dword [0x080F016C]", ln)
           for ln in out]
    with open(dst, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(out) + "\n")
    print(f"{src}: {len(lines)} lines -> {dst}: {len(out)} lines")
    if errors:
        print(f"{len(errors)} unsupported instructions:")
        for no, msg in errors[:40]:
            print(f"  {no}: {msg}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

