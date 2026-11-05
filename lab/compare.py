#!/usr/bin/env python3
"""Shader lab: compares an NVN shader (code.bin + control.bin from SaltyNX, built with Nintendo's
offline glslc) with the same GLSL compiled by UAM (deko3d's open compiler, .dksh).

Writes for each stage:
  <out>/<stage>.report.txt   parsed headers / control fields
  <out>/<stage>.nvn.words    NVN instructions as 32-bit hex words (for envydis -w)
  <out>/<stage>.uam.words    UAM instructions as 32-bit hex words

Usage: compare.py <nvn_dir> <uam_dir> <out_dir>
  nvn_dir: has {vert,frag}.code.bin and {vert,frag}.control.bin
  uam_dir: has {vert,frag}.dksh
"""
import struct
import sys
from pathlib import Path

NVN_SPH_OFF = 0x30   # SPH (shader program header, 0x50 bytes) starts here in NVN code
SPH_SIZE = 0x50


def u32(d, off):
    return struct.unpack_from("<I", d, off)[0]


def hexdump_nonzero(d, base=0):
    lines = []
    for off in range(0, len(d), 16):
        ch = d[off:off + 16]
        if any(ch):
            lines.append("%04x: %s" % (base + off, ch.hex(" ")))
    return "\n".join(lines)


def words(code):
    return "\n".join("0x%08x" % w for w in struct.unpack("<%dI" % (len(code) // 4), code))


def parse_nvn(code, control):
    r = {}
    r["code_magic"] = hex(u32(code, 0))
    r["control_magic"] = hex(u32(control, 0))
    # Fields found by comparing SaltyNX vert/frag control sections (see README of the lab).
    r["ctl_0x6f8_code_size_incl_sph"] = hex(u32(control, 0x6F8))
    r["ctl_0x6fc_constbuf1_size"] = hex(u32(control, 0x6FC))
    r["ctl_0x700_constbuf1_off"] = hex(u32(control, 0x700))
    r["ctl_0x704_total_size"] = hex(u32(control, 0x704))
    r["ctl_0x708_program_off"] = hex(u32(control, 0x708))
    r["ctl_0x70c_num_gprs"] = u32(control, 0x70C)
    code_size = u32(control, 0x6F8)
    sph = code[NVN_SPH_OFF:NVN_SPH_OFF + SPH_SIZE]
    insns = code[NVN_SPH_OFF + SPH_SIZE:NVN_SPH_OFF + code_size]
    cb1 = code[u32(control, 0x700):u32(control, 0x700) + u32(control, 0x6FC)]
    return r, sph, insns, cb1


def parse_dksh(d):
    magic, header_sz, control_sz, code_sz, programs_off, num_programs = struct.unpack_from("<6I", d, 0)
    r = {"magic": hex(magic), "header_sz": header_sz, "control_sz": hex(control_sz),
         "code_sz": hex(code_sz), "programs_off": hex(programs_off), "num_programs": num_programs}
    p = programs_off
    ptype, entry, gprs, cb1_off, cb1_sz, scratch = struct.unpack_from("<6I", d, p)
    r.update({"type": ptype, "entrypoint": hex(entry), "num_gprs": gprs,
              "constbuf1_off": hex(cb1_off), "constbuf1_sz": hex(cb1_sz), "per_warp_scratch_sz": scratch})
    code = d[control_sz:control_sz + code_sz]
    sph = code[entry:entry + SPH_SIZE]
    end = cb1_off if cb1_sz else len(code)
    insns = code[entry + SPH_SIZE:end]
    # strip trailing zero padding (keep multiple of 8 bytes)
    while len(insns) >= 8 and not any(insns[-8:]):
        insns = insns[:-8]
    cb1 = code[cb1_off:cb1_off + cb1_sz]
    return r, sph, insns, cb1, code


def main():
    nvn_dir, uam_dir, out_dir = map(Path, sys.argv[1:4])
    out_dir.mkdir(parents=True, exist_ok=True)
    for stage in ("vert", "frag"):
        code = (nvn_dir / f"{stage}.code.bin").read_bytes()
        control = (nvn_dir / f"{stage}.control.bin").read_bytes()
        nr, nsph, ninsns, ncb1 = parse_nvn(code, control)
        rep = [f"=== {stage}: NVN (SaltyNX, glslc) ==="]
        rep += [f"{k}: {v}" for k, v in nr.items()]
        rep.append(f"insn bytes: {len(ninsns)}  constbuf1: {ncb1.hex(' ')}")
        rep.append("SPH: " + nsph.hex(" "))
        rep.append("control (nonzero lines):\n" + hexdump_nonzero(control))
        (out_dir / f"{stage}.nvn.words").write_text(words(ninsns) + "\n")

        dksh_path = uam_dir / f"{stage}.dksh"
        if dksh_path.exists():
            dr, dsph, dinsns, dcb1, dcode = parse_dksh(dksh_path.read_bytes())
            rep.append(f"\n=== {stage}: UAM (.dksh) ===")
            rep += [f"{k}: {v}" for k, v in dr.items()]
            rep.append(f"insn bytes: {len(dinsns)}  constbuf1: {dcb1.hex(' ')}")
            rep.append("SPH: " + dsph.hex(" "))
            rep.append("SPH diff (offset: nvn uam): " + ", ".join(
                "%02x:%02x/%02x" % (i, a, b) for i, (a, b) in enumerate(zip(nsph, dsph)) if a != b))
            rep.append("code prefix 0x00-0x30: " + dcode[:0x30].hex(" "))
            (out_dir / f"{stage}.uam.words").write_text(words(dinsns) + "\n")
        else:
            rep.append(f"\n(no {dksh_path})")
        (out_dir / f"{stage}.report.txt").write_text("\n".join(rep) + "\n")
        print("\n".join(rep))


if __name__ == "__main__":
    main()
