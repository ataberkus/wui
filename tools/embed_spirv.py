#!/usr/bin/env python3
"""Embed a validated little-endian SPIR-V binary as C++ uint32_t words."""

import struct
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: embed_spirv.py INPUT_SPV OUTPUT_HPP SYMBOL")
    source, destination, symbol = sys.argv[1:]
    if not symbol.isidentifier():
        raise SystemExit("SPIR-V symbol must be a valid identifier")
    data = Path(source).read_bytes()
    if len(data) < 4 or len(data) % 4:
        raise SystemExit("SPIR-V byte length must be a nonzero multiple of four")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0x07230203:
        raise SystemExit("SPIR-V magic or little-endian word order is invalid")
    rows = ["    " + ", ".join(f"0x{word:08x}u" for word in words[i:i + 8]) + ","
            for i in range(0, len(words), 8)]
    output = ("#pragma once\n#include <cstdint>\n\nnamespace wui::vulkan::detail {\n"
              f"alignas(4) inline constexpr std::uint32_t {symbol}[] = {{\n"
              + "\n".join(rows) + "\n};\n} // namespace wui::vulkan::detail\n")
    Path(destination).write_text(output, encoding="utf-8")


if __name__ == "__main__":
    main()
