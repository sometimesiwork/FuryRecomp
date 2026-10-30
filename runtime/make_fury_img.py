#!/usr/bin/env python3
"""
Generate fury.img from original FURY.EXE

fury.img is the raw memory image of the MZ executable loaded at segment 0x0207
with MZ relocations applied, as expected by the static recompiled runtime.
"""

import os
import sys
import struct

def find_fury_exe():
    """Look for FURY.EXE in common locations."""
    candidates = [
        "FURY.EXE",
        "fury.exe",
        "../FURY.EXE",
        "../fury.exe",
        "../../FURY.EXE",
        "../../fury.exe",
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    return None

def make_fury_img(exe_path, out_path="fury.img", load_seg=0x0207):
    with open(exe_path, "rb") as f:
        data = f.read()
    
    if len(data) < 0x40 or data[0:2] != b"MZ":
        raise SystemExit(f"{exe_path} is not a valid MZ executable")
    
    # MZ header
    last_page_bytes = data[0x02] | (data[0x03] << 8)
    pages = data[0x04] | (data[0x05] << 8)
    relocation_count = data[0x06] | (data[0x07] << 8)
    header_paragraphs = data[0x08] | (data[0x09] << 8)
    relocation_offset = data[0x18] | (data[0x19] << 8)
    
    # Calculate image size
    if last_page_bytes == 0:
        last_page_bytes = 512
    image_size = (pages - 1) * 512 + last_page_bytes
    
    image_start = header_paragraphs * 16
    if image_start + image_size > len(data):
        image_size = max(0, len(data) - image_start)
    
    # Copy image
    image = bytearray(data[image_start:image_start + image_size])
    
    # Apply relocations
    rel = relocation_offset
    applied = 0
    for i in range(relocation_count):
        if rel + 4 > len(data):
            break
        off = data[rel] | (data[rel + 1] << 8)
        seg = data[rel + 2] | (data[rel + 3] << 8)
        rel += 4
        target = seg * 16 + off
        if target + 1 < len(image):
            val = image[target] | (image[target + 1] << 8)
            val = (val + load_seg) & 0xFFFF
            image[target] = val & 0xFF
            image[target + 1] = (val >> 8) & 0xFF
            applied += 1
    
    with open(out_path, "wb") as f:
        f.write(image)
    
    print(f"Generated {out_path} ({len(image)} bytes)")
    print(f"Source: {exe_path}")
    print(f"Load segment: 0x{load_seg:04X}")
    print(f"Relocations applied: {applied}")
    return out_path

def main():
    exe = find_fury_exe()
    if not exe:
        print("Error: Could not find FURY.EXE", file=sys.stderr)
        print("Place FURY.EXE in this directory or parent directories.", file=sys.stderr)
        sys.exit(1)
    
    out = "fury.img"
    if len(sys.argv) > 1:
        out = sys.argv[1]
    
    make_fury_img(exe, out)
    
    # Also copy to runtime/ if it exists
    runtime_dir = "runtime"
    if os.path.isdir(runtime_dir):
        runtime_out = os.path.join(runtime_dir, "fury.img")
        make_fury_img(exe, runtime_out)
        print(f"Also copied to {runtime_out}")

if __name__ == "__main__":
    main()
