#!/usr/bin/env python3
"""Verify a build-directory reconfigure cannot reuse another config's prefix."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def failure_output(result: subprocess.CompletedProcess[str]) -> str:
    return result.stdout + result.stderr


def stage_debug_prefix(source: Path) -> None:
    prefix = source / "out" / "deps" / "Debug"
    for relative in (
        "include/freetype2/ft2build.h",
        "include/harfbuzz/hb.h",
        "include/utf8proc.h",
        "lib/libfreetype.a",
        "lib/libfreetyped.a",
        "lib/libharfbuzz.a",
        "lib/libutf8proc.a",
        "lib/freetype.lib",
        "lib/freetyped.lib",
        "lib/harfbuzz.lib",
        "lib/utf8proc.lib",
    ):
        path = prefix / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--make-program")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="wui-prefix-reconfigure-") as temporary:
        copied_source = Path(temporary) / "source"
        shutil.copytree(args.source, copied_source, ignore=shutil.ignore_patterns(".git", "out", "third_party", "assets", "__pycache__"))
        stage_debug_prefix(copied_source)
        build = Path(temporary) / "build"
        common = [args.cmake, "-S", str(copied_source), "-B", str(build), "-G", args.generator,
                  "-DWUI_BUILD_TEXT=ON", "-DWUI_BUILD_TESTS=OFF"]
        if args.make_program:
            common.append(f"-DCMAKE_MAKE_PROGRAM={args.make_program}")

        debug = run([*common, "-DCMAKE_BUILD_TYPE=Debug"])
        if debug.returncode != 0:
            raise RuntimeError(f"Debug configuration unexpectedly failed:\n{failure_output(debug)}")

        release = run([*common, "-DCMAKE_BUILD_TYPE=Release"])
        if release.returncode == 0:
            raise RuntimeError("Release reconfigure unexpectedly reused the Debug dependency prefix")
        output = failure_output(release)
        if "Prepared text dependencies are missing from" not in output or "out/deps/Release" not in output:
            raise RuntimeError(f"Release configuration failed for the wrong reason:\n{output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
