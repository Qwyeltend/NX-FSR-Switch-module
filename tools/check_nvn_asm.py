#!/usr/bin/env python3
"""Checks disassembled (envydis) shader code for bindings that NVN would not understand.

NVN layout (found in lab/): user UBO binding 0 = constbuf c3, compiler constants = c1,
fragment texture binding 0 = handle index 0x8.
Fails the build if the shader reads c2 (UAM's binding 0, means a wrong binding in GLSL),
c0 (driver constbuf, layout differs between deko3d and NVN), or uses another texture index.

Usage: check_nvn_asm.py <stage: vert|frag> <file.asm> [<file.asm> ...]
"""
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def main():
    stage = sys.argv[1]
    ok = True
    for path in sys.argv[2:]:
        text = ANSI.sub("", open(path).read())
        banks = sorted(set(re.findall(r"\bc(\d+)\[", text)))
        tex_lines = [l for l in text.splitlines() if re.search(r"\b(tex|texs|tld|tlds|tld4|tld4s|txq|txd)\b", l)]
        print(f"{path}: constbufs used {banks}, texture instructions {len(tex_lines)}")
        for bank in banks:
            if bank not in ("1", "3"):
                print(f"  ERROR: constbuf c{bank} is not valid for NVN here")
                ok = False
        for line in tex_lines:
            print("   ", " ".join(line.split()[2:]))
            if stage != "frag":
                print("  ERROR: texture use outside fragment stage is not mapped")
                ok = False
            elif not re.search(r"\b0x8\b", line):
                print("  ERROR: texture index is not 0x8 (NVN fragment binding 0)")
                ok = False
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
