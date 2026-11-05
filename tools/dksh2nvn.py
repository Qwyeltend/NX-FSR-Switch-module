#!/usr/bin/env python3
"""Converts a UAM (deko3d) shader module (.dksh) into NVN shader sections: code.bin + control.bin.

Layout found by comparing SaltyNX's NVN shaders (Nintendo's offline glslc) with UAM output of the same
GLSL (see lab/): both put a 0x30 byte prefix first, then the SPH (shader program header, 0x50 bytes) at
0x30, then the Maxwell instructions, then the compiler constant buffer (c1) aligned to 256 bytes.

NVN code prefix: u32 0x12345678 + zeros. NVN control section: taken from a template (a real control
section from SaltyNX of the same stage) with these fields patched:
  0x6F8 code size including SPH    0x6FC constbuf1 size    0x700 constbuf1 offset
  0x704 total code size            0x708 program offset (0x30)  0x70C number of GPRs

Usage: dksh2nvn.py <in.dksh> <template.control.bin> <out.code.bin> <out.control.bin>
"""
import struct
import sys

NVN_CODE_MAGIC = 0x12345678
NVN_CONTROL_MAGIC = 0x98761234
PROGRAM_OFF = 0x30
SPH_SIZE = 0x50


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def main():
    dksh_path, template_path, out_code, out_control = sys.argv[1:5]
    d = open(dksh_path, "rb").read()
    magic, header_sz, control_sz, code_sz, programs_off, num_programs = struct.unpack_from("<6I", d, 0)
    if magic != 0x48534B44 or num_programs != 1:
        raise SystemExit(f"{dksh_path}: not a single-program DKSH")
    ptype, entry, gprs, cb1_off, cb1_sz, scratch = struct.unpack_from("<6I", d, programs_off)
    if entry != PROGRAM_OFF:
        raise SystemExit(f"{dksh_path}: entrypoint 0x{entry:x}, expected 0x30")
    if scratch != 0:
        raise SystemExit(f"{dksh_path}: shader needs scratch memory ({scratch} bytes per warp), not supported")

    code = bytearray(d[control_sz:control_sz + code_sz])
    code[0:PROGRAM_OFF] = struct.pack("<I", NVN_CODE_MAGIC) + bytes(PROGRAM_OFF - 4)

    # End of instructions: last non-zero 8 byte word before the constant buffer (or end of code).
    insn_end = cb1_off if cb1_sz else len(code)
    while insn_end > PROGRAM_OFF + SPH_SIZE and not any(code[insn_end - 8:insn_end]):
        insn_end -= 8
    insn_end = PROGRAM_OFF + SPH_SIZE + align(insn_end - PROGRAM_OFF - SPH_SIZE, 32)  # whole 4-insn bundles
    code_size_incl_sph = insn_end - PROGRAM_OFF
    if not cb1_sz:
        cb1_off = align(insn_end, 256)
    total = align(max(len(code), cb1_off + cb1_sz), 256)
    code = bytes(code) + bytes(total - len(code))

    control = bytearray(open(template_path, "rb").read())
    if struct.unpack_from("<I", control, 0)[0] != NVN_CONTROL_MAGIC:
        raise SystemExit(f"{template_path}: not an NVN control section")
    struct.pack_into("<6I", control, 0x6F8, code_size_incl_sph, cb1_sz, cb1_off, total, PROGRAM_OFF, gprs)

    open(out_code, "wb").write(code)
    open(out_control, "wb").write(bytes(control))
    print(f"{dksh_path}: type {ptype}, gprs {gprs}, code+sph 0x{code_size_incl_sph:x}, "
          f"cb1 0x{cb1_sz:x}@0x{cb1_off:x}, total 0x{total:x}")

    # Instructions as 32-bit words for envydis (verification step in CI).
    insns = code[PROGRAM_OFF + SPH_SIZE:insn_end]
    with open(out_code + ".words", "w") as f:
        for (w,) in struct.iter_unpack("<I", insns):
            f.write("0x%08x\n" % w)


if __name__ == "__main__":
    main()
