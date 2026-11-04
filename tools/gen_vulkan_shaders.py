#!/usr/bin/env python3
"""Embed the Vulkan UI shaders into native/ps4/vulkan_ui_shaders.h.

Compile the shaders first (glslangValidator is part of the Vulkan SDK):

    cd native/ps4/shaders
    glslangValidator -V --target-env vulkan1.0 ui.vert -o ui.vert.spv
    glslangValidator -V --target-env vulkan1.0 ui.frag -o ui.frag.spv
    python3 ../../../tools/gen_vulkan_shaders.py

The SPIR-V files are build artifacts and are not committed; the generated
header is, so builds do not need glslangValidator.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHADERS = ROOT / "native" / "ps4" / "shaders"
HEADER = ROOT / "native" / "ps4" / "vulkan_ui_shaders.h"

LINES = [
    "// Generated from native/ps4/shaders/ui.vert and ui.frag with:",
    "//   glslangValidator -V --target-env vulkan1.0 ui.vert -o ui.vert.spv",
    "//   glslangValidator -V --target-env vulkan1.0 ui.frag -o ui.frag.spv",
    "// Regenerate this header with tools/gen_vulkan_shaders.py after editing",
    "// either shader.",
    "#pragma once",
    "#include <cstdint>",
    "",
]


def main() -> None:
    for name, filename in (("kUiVertSpv", "ui.vert.spv"), ("kUiFragSpv", "ui.frag.spv")):
        data = (SHADERS / filename).read_bytes()
        if len(data) % 4:
            raise SystemExit(f"{filename} is not a multiple of four bytes")
        words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
        LINES.append(f"static const std::uint32_t {name}[] = {{")
        for i in range(0, len(words), 6):
            LINES.append("    " + ", ".join(f"0x{word:08x}u" for word in words[i:i + 6]) + ",")
        LINES.append("};")
        LINES.append(f"static const std::uint32_t {name}Size = sizeof({name});")
        LINES.append("")
    HEADER.write_text("\n".join(LINES) + "\n", encoding="utf-8")
    print(f"wrote {HEADER.relative_to(ROOT)} ({HEADER.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
