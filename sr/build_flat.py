#!/usr/bin/env python3
"""Build the flat binary of the recompiled game.

Runs the source rewriter, assembles with NASM and widens out-of-range short
jumps until the assembler is happy.  Also emits the guest -> flat symbol table
used by the runtime to resolve interrupt vectors and indirect code pointers.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NASM = os.environ.get("NASM") or os.path.join(os.environ.get("TEMP", ""),
                                               "opencode", "nasm",
                                               "nasm-2.16.03", "nasm.exe")
PY = sys.executable

LABEL_RE = re.compile(r"^(?:loc_([0-9A-Fa-f]+)|_?(program_start)):$")


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=HERE, capture_output=True, text=True, **kw)


def write_symbols(flat_src):
    """Emit syms.inc: guest linear address -> label, sorted by address."""
    seen = {}
    with open(flat_src, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            s = line.strip()
            m = LABEL_RE.match(s)
            if not m:
                continue
            if m.group(1):
                addr = int(m.group(1), 16)
            else:
                addr = 0x2170          # program_start == entry point
            seen.setdefault(s[:-1], addr)
    rows = sorted((addr, name) for name, addr in seen.items())
    with open(os.path.join(HERE, "syms.inc"), "w", encoding="utf-8",
              newline="\n") as fh:
        for addr, name in rows:
            fh.write(f"dd 0x{addr:08x}, {name}\n")
    print(f"symbols: {len(rows)}")


def main():
    src = os.path.join(HERE, "seg01.inc")
    flat = os.path.join(HERE, "seg01_flat.inc")
    top = os.path.join(HERE, "fury-flat.asm")
    out = os.path.join(HERE, "fury-flat.bin")
    err = os.path.join(HERE, "nasm.err")

    r = run([PY, os.path.join(HERE, "flatify.py"), src, flat])
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return 1
    write_symbols(flat)

    for _ in range(40):
        r = subprocess.run([NASM, "-f", "bin", "-o", "fury-flat.bin",
                            "fury-flat.asm"],
                           cwd=HERE, capture_output=True, text=True)
        with open(err, "w", encoding="utf-8") as fh:
            fh.write(r.stderr)
        if r.returncode == 0:
            print(f"BUILD OK: {out} ({os.path.getsize(out)} bytes)")
            return 0
        short = r.stderr.count("short jump is out of range")
        if short == 0:
            print(r.stderr[:4000])
            return 1
        print(f"widening {short} short jumps")
        r2 = run([PY, os.path.join(HERE, "repair_short_jumps.py"), err])
        sys.stdout.write(r2.stdout)
        if r2.returncode != 0:
            sys.stdout.write(r2.stderr)
            return 1

    print("too many rounds")
    return 1


if __name__ == "__main__":
    sys.exit(main())
