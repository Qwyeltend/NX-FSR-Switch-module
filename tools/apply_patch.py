#!/usr/bin/env python3
"""Adds fsrSwitch to a SaltyNX source tree (tag 2.1.4).

Every edit is an exact text replacement that must match exactly once, so a changed SaltyNX version
fails loudly here instead of silently building something wrong.

Usage: apply_patch.py <saltynx_dir> <repo_dir>
"""
import shutil
import sys
from pathlib import Path


def replace_once(path, old, new):
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected 1 match, found {count}:\n{old}")
    path.write_text(text.replace(old, new))


def main():
    saltynx, repo = Path(sys.argv[1]), Path(sys.argv[2])
    core = saltynx / "saltysd_core"
    api = core / "source" / "api"

    for name in ("FsrNVN.hpp", "FsrNVN.cpp"):
        shutil.copy(repo / "core" / name, api / name)
    shutil.copy(repo / "common" / "fsr_shared.h", api / "fsr_shared.h")

    nvn = api / "NVN.cpp"
    replace_once(nvn, '#include "LogoNVN.hpp"\n', '#include "LogoNVN.hpp"\n#include "FsrNVN.hpp"\n')

    # Shader pool early, like the logo.
    replace_once(nvn,
        "if (ret && nvnDevice == mainDevice && !Logo::done) LogoNVN::PrepareEarly(nvnDevice, Logo::Resolve);\n",
        "if (ret && nvnDevice == mainDevice && !Logo::done) LogoNVN::PrepareEarly(nvnDevice, Logo::Resolve);\n"
        "\t\tif (ret && nvnDevice == mainDevice) FsrNVN::PrepareEarly(nvnDevice, nvnDeviceBuilder, Logo::Resolve);\n")

    # Effect right before present (before the logo, so the logo is drawn on top of the processed frame).
    replace_once(nvn,
        "\t\t\tif (!Logo::done) Logo::Draw(queue, TripleBuffer::windowTextures[TripleBuffer::windowIndex], crop);\n",
        "\t\t\tFsrNVN::Process(mainDevice, queue, TripleBuffer::windowTextures[TripleBuffer::windowIndex], crop, Logo::Resolve);\n"
        "\t\t\tif (!Logo::done) Logo::Draw(queue, TripleBuffer::windowTextures[TripleBuffer::windowIndex], crop);\n")
    replace_once(nvn,
        "\t\t\tif (!Logo::done && index >= 0 && index < presentedTextureCount) Logo::Draw(queue, presentedTextures[index], crop);\n",
        "\t\t\tif (index >= 0 && index < presentedTextureCount) FsrNVN::Process(mainDevice, queue, presentedTextures[index], crop, Logo::Resolve);\n"
        "\t\t\tif (!Logo::done && index >= 0 && index < presentedTextureCount) Logo::Draw(queue, presentedTextures[index], crop);\n")

    # Window textures must be known even when the logo is off (game fills the window builder directly).
    replace_once(nvn,
        "if (presentedTextureCount == 0 && (!Logo::done || TripleBuffer::requested) && Logo::RecoverFromBuilder(windowBuilder)",
        "if (presentedTextureCount == 0 && (!Logo::done || TripleBuffer::requested || FsrNVN::Wanted()) && Logo::RecoverFromBuilder(windowBuilder)")

    main_c = core / "source" / "main.c"
    replace_once(main_c,
        "uintptr_t SaltySDCore_GetReservedMemory(size_t* size)\n",
        "// fsrSwitch: reserve more heap after what was reserved before (TripleBuffer keeps the start of the area).\n"
        "uintptr_t SaltySDCore_ReserveMemoryAppend(size_t size)\n"
        "{\n"
        "\tif (!size) return 0;\n"
        "\tconst size_t aligned = ((size+0x1FFFFF) & ~0x1FFFFF);\n"
        "\tvoid* addr = NULL;\n"
        "\tResult rc = svcSetHeapSize(&addr, getHeapAddon() + aligned);\n"
        "\tif (R_FAILED(rc)) {\n"
        "\t\tSaltySDCore_printf(MODULE_NAME \": fsrSwitch: failed to reserve 0x%lx bytes of heap: 0x%lx\\n\", (unsigned long)aligned, (unsigned long)rc);\n"
        "\t\treturn 0;\n"
        "\t}\n"
        "\tconst size_t offset = reserved_area_size;\n"
        "\treserved_area_size += aligned;\n"
        "\tg_heapAddr = (uintptr_t)addr;\n"
        "\tg_heapSize = getHeapAddon();\n"
        "\tif (!reserved_area_addr) reserved_area_addr = g_heapAddr + getElfAreaAligned();\n"
        "\treturn reserved_area_addr + offset;\n"
        "}\n\n"
        "void FsrSwitch_Init(SharedMemory* sharedMemory);\n\n"
        "uintptr_t SaltySDCore_GetReservedMemory(size_t* size)\n")
    replace_once(main_c,
        "\t\t\tNX_FPS(&_sharedmemory, &sharedOperationMode);\n",
        "\t\t\tNX_FPS(&_sharedmemory, &sharedOperationMode);\n"
        "\t\t\t#if !defined(SWITCH32) && !defined(OUNCE32)\n"
        "\t\t\tFsrSwitch_Init(&_sharedmemory);\n"
        "\t\t\t#endif\n")
    print("fsrSwitch patch applied")


if __name__ == "__main__":
    main()
