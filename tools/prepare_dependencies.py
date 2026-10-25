#!/usr/bin/env python3
"""Explicitly stage and build the dependencies locked in third_party/lock.json."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT / "third_party" / "lock.json"


def run(command: list[object]) -> None:
    subprocess.run([str(part) for part in command], cwd=ROOT, check=True)


def output(command: list[object]) -> str:
    return subprocess.check_output([str(part) for part in command], cwd=ROOT, text=True).strip()


def load_lock() -> dict:
    with LOCK_PATH.open(encoding="utf-8") as lock_file:
        return json.load(lock_file)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def selected_dependencies(lock: dict, names: list[str] | None) -> list[dict]:
    dependencies = lock["dependencies"]
    if names is None:
        return dependencies
    found = {dependency["name"] for dependency in dependencies}
    missing = sorted(set(names) - found)
    if missing:
        raise RuntimeError(f"Unknown locked dependency: {', '.join(missing)}")
    return [dependency for dependency in dependencies if dependency["name"] in names]


def checkout_dirty(destination: Path) -> bool:
    # Compare content rather than status's stat/EOL dirtiness. HarfBuzz's
    # generators emit LF even in an autocrlf Windows checkout.
    dirty = bool(output(["git", "-C", destination, "ls-files", "--others", "--exclude-standard"]))
    for extra in ([], ["--cached"]):
        compared = subprocess.run(
            ["git", "-C", str(destination), "diff", "--quiet", "--no-ext-diff", *extra],
            cwd=ROOT,
        )
        if compared.returncode not in (0, 1):
            compared.check_returncode()
        dirty |= compared.returncode == 1
    return dirty


def ensure_checkout(record: dict, fetch: bool) -> None:
    destination = ROOT / record["destination"]
    revision = record["revision"]
    if destination.exists():
        if not (destination / ".git").is_dir():
            raise RuntimeError(f"Existing dependency is not a git checkout: {destination}")
        if checkout_dirty(destination):
            raise RuntimeError(f"Dependency checkout is dirty: {destination}")
        resolved = output(["git", "-C", destination, "rev-parse", "HEAD"])
        if resolved != revision:
            raise RuntimeError(f"Dependency is pinned differently: {destination} is {resolved}, expected {revision}")
        return
    if not fetch:
        raise RuntimeError(f"Locked dependency is not staged: {destination}. Re-run with --fetch.")

    destination.parent.mkdir(parents=True, exist_ok=True)
    repo_url = record["repository"]
    subprocess.run(["git", "clone", "--no-checkout", repo_url, str(destination)], check=True)
    subprocess.run(["git", "-C", str(destination), "fetch", "origin", revision], check=True)
    subprocess.run(["git", "-C", str(destination), "checkout", "--detach", "FETCH_HEAD"], check=True)
    resolved = subprocess.check_output(
        ["git", "-C", str(destination), "rev-parse", "HEAD"], text=True
    ).strip()
    if len(revision) == 40 and resolved != revision:
        raise RuntimeError("Dependency revision mismatch")


def prepare_fonts(lock: dict) -> None:
    fonts = lock["fonts"]
    entries = fonts["files"]
    regular = [entry for entry in entries if entry["archive_path"].endswith("/Inter-Regular.ttf")]
    semibold = [entry for entry in entries if entry["archive_path"].endswith("/Inter-SemiBold.ttf")]
    if len(regular) != 1 or len(semibold) != 1:
        raise RuntimeError("Inter archive lock must select exactly one Regular and one SemiBold static TTF")

    with tempfile.TemporaryDirectory(prefix="wui-inter-") as temporary:
        archive = Path(temporary) / "Inter-4.1.zip"
        urllib.request.urlretrieve(fonts["archive"], archive)
        with zipfile.ZipFile(archive) as bundle:
            names = bundle.namelist()
            regular_matches = [name for name in names if name.endswith("/Inter-Regular.ttf")]
            semibold_matches = [name for name in names if name.endswith("/Inter-SemiBold.ttf")]
            if len(regular_matches) != 1 or len(semibold_matches) != 1:
                raise RuntimeError("Inter release archive must contain exactly one Regular and one SemiBold static TTF")
            for entry in entries:
                archive_path = entry["archive_path"]
                if archive_path not in names:
                    raise RuntimeError(f"Inter release archive missing {archive_path}")
                destination = ROOT / entry["destination"]
                destination.parent.mkdir(parents=True, exist_ok=True)
                staged = Path(temporary) / Path(archive_path).name
                with bundle.open(archive_path) as source, staged.open("wb") as target:
                    shutil.copyfileobj(source, target)
                actual = sha256(staged)
                if actual != entry["sha256"]:
                    raise RuntimeError(f"Inter asset hash mismatch for {archive_path}: {actual}")
                shutil.copyfile(staged, destination)


def verify_fonts(lock: dict) -> list[str]:
    errors: list[str] = []
    for entry in lock["fonts"]["files"]:
        destination = ROOT / entry["destination"]
        if not destination.is_file():
            errors.append(f"Missing font asset: {destination}")
        elif sha256(destination) != entry["sha256"]:
            errors.append(f"Font hash mismatch: {destination}")
    return errors


def write_meson_metadata(build: Path, source: Path, config: str) -> None:
    build.mkdir(parents=True, exist_ok=True)
    (build / ".wui-build.json").write_text(
        json.dumps({"source": str(source.resolve()), "config": config}, indent=2) + "\n",
        encoding="utf-8",
    )


def ensure_meson_metadata(build: Path, source: Path, config: str) -> bool:
    metadata = build / ".wui-build.json"
    if not metadata.exists():
        if build.exists():
            raise RuntimeError(f"Existing Meson build directory lacks WUI source/configuration metadata: {build}")
        return False
    data = json.loads(metadata.read_text(encoding="utf-8"))
    if data != {"source": str(source.resolve()), "config": config}:
        raise RuntimeError(f"Meson build directory has a different source or configuration: {build}")
    return True


def build_dependencies(lock: dict, config: str, names: list[str] | None) -> None:
    records = {record["name"]: record for record in selected_dependencies(lock, names)}
    required = ("freetype", "harfbuzz", "utf8proc")
    missing = [name for name in required if name not in records]
    if missing:
        raise RuntimeError(f"--build requires all build dependencies; missing selection: {', '.join(missing)}")
    for record in records.values():
        ensure_checkout(record, fetch=False)

    prefix = ROOT / "out" / "deps" / config

    # Release dependencies are optimized for size: they are linked statically
    # into every consumer executable, and text shaping/rasterization is not a
    # hot loop (runs and glyphs are cached by wui_text).
    cmake_build_type = "MinSizeRel" if config == "Release" else config
    hb_build_options = (["--buildtype", "minsize", "-Ddebug=false", "-Db_ndebug=true"]
        if config == "Release" else ["--buildtype", config.lower()])
    # HarfBuzz features wui_text cannot reach: it shapes horizontally with the
    # "ot" shaper through hb_ot_font funcs and calls none of these APIs. HB_LEAN
    # is not used: its HB_NO_HINTING, HB_NO_VAR, HB_NO_GETENV, HB_NO_METRICS
    # and HB_NO_BITMAP change shaping results for some fonts.
    hb_cpp_args = ["-D" + name for name in (
        "HB_DISABLE_DEPRECATED", "HB_NO_BUFFER_MESSAGE", "HB_NO_BUFFER_SERIALIZE",
        "HB_NO_BUFFER_VERIFY", "HB_NO_ERRNO", "HB_NO_FACE_COLLECT_UNICODES",
        "HB_NO_FALLBACK_SHAPE", "HB_NO_LAYOUT_COLLECT_GLYPHS", "HB_NO_LAYOUT_FEATURE_PARAMS",
        "HB_NO_LAYOUT_RARELY_USED", "HB_NO_LAYOUT_UNUSED", "HB_NO_MATH", "HB_NO_META",
        "HB_NO_MMAP", "HB_NO_NAME", "HB_NO_OPEN", "HB_NO_OT_FETCH", "HB_NO_OT_FONT_GLYPH_NAMES",
        "HB_NO_SETLOCALE", "HB_NO_VERTICAL")]
    # /Gw puts each global in its own COMDAT so /OPT:REF drops unused tables.
    cmake_size_options = ["-DCMAKE_C_FLAGS_MINSIZEREL=/O1 /Ob1 /DNDEBUG /Gw"]
    if os.name == "nt" and config == "Release":
        hb_build_options.append("-Dc_args=/Gw")
        hb_cpp_args.append("/Gw")
    elif os.name == "nt":
        # Debug consumers still get the debug CRT, symbols and FreeType tracing;
        # only the third-party code itself is optimized.
        cmake_size_options = ["-DCMAKE_C_FLAGS_DEBUG=/Zi /O1 /Ob1"]
        hb_build_options.append("-Doptimization=s")
    else:
        cmake_size_options = []
    hb_build_options.append("-Dcpp_args=" + " ".join(hb_cpp_args))

    def cmake_dependency(name: str, options: list[str], clean_first: bool = False) -> None:
        build = ROOT / "out" / "deps-build" / config / name
        run(["cmake", "-S", ROOT / "third_party" / name, "-B", build, "-G", "Ninja",
             f"-DCMAKE_BUILD_TYPE={cmake_build_type}", f"-DCMAKE_INSTALL_PREFIX={prefix}",
             "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL",
             "-DBUILD_SHARED_LIBS=OFF", *cmake_size_options, *options])
        run(["cmake", "--build", build, "--parallel", "4", *(["--clean-first"] if clean_first else [])])
        run(["cmake", "--install", build])

    # The build tree's include directory precedes the source tree's, so this
    # replaces FreeType's default module list (see the header for the rationale).
    # Trees compiled before the override tracked the source header, so rebuild them.
    ft_modules = ROOT / "out" / "deps-build" / config / "freetype" / "include" / "freetype" / "config" / "ftmodule.h"
    ft_modules_source = ROOT / "cmake" / "freetype_ftmodule.h"
    ft_modules_changed = not ft_modules.is_file() or ft_modules.read_bytes() != ft_modules_source.read_bytes()
    ft_modules.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ft_modules_source, ft_modules)
    cmake_dependency("freetype", ["-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_ZLIB=ON",
        "-DFT_DISABLE_BZIP2=ON", "-DFT_DISABLE_PNG=ON", "-DFT_DISABLE_BROTLI=ON"],
        clean_first=ft_modules_changed)
    hb_build = ROOT / "out" / "deps-build" / config / "harfbuzz"
    hb_source = ROOT / "third_party" / "harfbuzz"
    hb_flags = ["-Dauto_features=disabled", "-Dtests=disabled", "-Dutilities=disabled",
        "-Ddocs=disabled", "-Dsubset=disabled", "-Draster=disabled", "-Dvector=disabled",
        "-Dgpu=disabled", "-Dgpu_demo=disabled", "-Dfreetype=disabled"]
    if os.name == "nt":
        hb_flags.append("-Db_vscrt=mdd" if config == "Debug" else "-Db_vscrt=md")
    if ensure_meson_metadata(hb_build, hb_source, config):
        run(["meson", "setup", "--reconfigure", hb_build, hb_source, "--prefix", prefix,
             *hb_build_options, "--default-library", "static",
             "--wrap-mode=nofallback", *hb_flags])
    else:
        run(["meson", "setup", hb_build, hb_source, "--prefix", prefix,
             *hb_build_options, "--default-library", "static",
             "--wrap-mode=nofallback", *hb_flags])
        write_meson_metadata(hb_build, hb_source, config)
    run(["meson", "compile", "-C", hb_build, "-j", "4"])
    run(["meson", "install", "-C", hb_build])
    cmake_dependency("utf8proc", ["-DUTF8PROC_ENABLE_TESTING=OFF"])


def verify(lock: dict) -> int:
    errors: list[str] = []
    for record in lock["dependencies"]:
        destination = ROOT / record["destination"]
        if not (destination / ".git").is_dir():
            errors.append(f"Missing locked checkout: {destination}")
            continue
        if checkout_dirty(destination):
            errors.append(f"Dirty checkout: {destination}")
            continue
        if output(["git", "-C", destination, "rev-parse", "HEAD"]) != record["revision"]:
            errors.append(f"Revision mismatch: {destination}")
    errors.extend(verify_fonts(lock))
    if errors:
        print("Dependency verification failed:", file=sys.stderr)
        print("\n".join(f"- {error}" for error in errors), file=sys.stderr)
        return 1
    print("Locked dependencies and Inter assets verified without network access.")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="Explicitly fetch locked source checkouts and Inter assets.")
    parser.add_argument("--build", action="store_true", help="Build FreeType, HarfBuzz, and utf8proc after verification.")
    parser.add_argument("--verify", action="store_true", help="Verify staged revisions and asset hashes without network access.")
    parser.add_argument("--config", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--only", action="append", choices=("freetype", "harfbuzz", "utf8proc", "doctest"),
                        help="Limit --fetch to a named git dependency; repeatable.")
    args = parser.parse_args()
    if not (args.fetch or args.build or args.verify):
        parser.error("choose at least one of --fetch, --build, or --verify")
    if args.only and (args.build or args.verify):
        parser.error("--only is only valid with --fetch")
    return args


def main() -> int:
    args = parse_args()
    lock = load_lock()
    if args.fetch:
        for record in selected_dependencies(lock, args.only):
            ensure_checkout(record, fetch=True)
        if args.only is None:
            prepare_fonts(lock)
    if args.build:
        build_dependencies(lock, args.config, None)
    if args.verify:
        return verify(lock)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError, OSError, zipfile.BadZipFile) as error:
        print(f"Dependency preparation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
