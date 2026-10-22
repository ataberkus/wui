#!/usr/bin/env python3
"""Install this build tree, relocate the prefix and consume it as a package.

Cases (see the E plan's Review Focus):
  relocated          install to prefix-a, copy to prefix-b, delete prefix-a, then
                     configure/build/run tests/package/consumer against prefix-b;
                     the package must hold no build, source or install-time paths.
  missing-config     the other build type fails with a clear message.
  missing-component  a component that was not built fails with a clear message.
  static-runtime     a /MT consumer fails with a clear message.
  no-vulkan-sdk      with vulkan installed, a consumer asking only for core+d3d11
                     configures and builds without the Vulkan SDK in the environment.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def run(command: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)


class Driver:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.consumer = args.source / "tests" / "package" / "consumer"
        self.failures = 0

    def report(self, name: str, ok: bool, detail: str = "") -> None:
        print(f"{'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail and not ok else ''}")
        if not ok:
            self.failures += 1

    def configure(self, name: str, prefix: Path, config: str, components: str,
                  extra: list[str] | None = None, env: dict[str, str] | None = None):
        build = self.args.work / f"consumer-{name}"
        shutil.rmtree(build, ignore_errors=True)
        command = [self.args.cmake, "-S", str(self.consumer), "-B", str(build), "-G", self.args.generator,
                   f"-DCMAKE_BUILD_TYPE={config}", f"-DCMAKE_PREFIX_PATH={prefix.as_posix()}",
                   f"-DWUI_TEST_COMPONENTS={components}"]
        if self.args.make_program:
            command.append(f"-DCMAKE_MAKE_PROGRAM={self.args.make_program}")
        return build, run(command + (extra or []), env)

    def expect_configure_failure(self, name: str, prefix: Path, config: str, components: str,
                                 message: str, extra: list[str] | None = None) -> None:
        _, result = self.configure(name, prefix, config, components, extra)
        output = result.stdout + result.stderr
        # CMake wraps long messages, so compare with whitespace collapsed.
        found = " ".join(message.split()) in " ".join(output.split())
        self.report(name, result.returncode != 0 and found,
                    f"exit {result.returncode}, expected message {message!r}; output tail: {output[-600:]}")

    def main(self) -> int:
        args = self.args
        shutil.rmtree(args.work, ignore_errors=True)
        args.work.mkdir(parents=True)
        prefix_a, prefix_b = args.work / "prefix-a", args.work / "prefix-b"
        result = run([args.cmake, "--install", str(args.build_dir), "--prefix", str(prefix_a)])
        if result.returncode != 0 or not prefix_a.is_dir():
            print("FAIL install: nothing installed" if result.returncode == 0 else "FAIL install:",
                  result.stdout[-800:], result.stderr[-800:])
            return 1
        shutil.copytree(prefix_a, prefix_b)
        shutil.rmtree(prefix_a)

        # No absolute paths from the build, the source or the original prefix.
        forbidden = {str(p).lower() for p in (args.build_dir, args.source, prefix_a)}
        forbidden |= {s.replace("\\", "/") for s in forbidden} | {"out/deps", "out\\deps"}
        leaks = []
        for path in (prefix_b / "lib" / "cmake" / "wui").glob("*.cmake"):
            text = path.read_text(encoding="utf-8").lower()
            leaks += [f"{path.name}: {needle}" for needle in forbidden if needle in text]
        self.report("relocated-paths", not leaks and (prefix_b / "lib" / "cmake" / "wui").is_dir(),
                    "; ".join(leaks) or "no lib/cmake/wui")

        # CTest passes WUI_PACKAGE_COMPONENTS as a CMake list; the consumer expects spaces.
        components = " ".join(args.components.replace(";", " ").split())
        build, result = self.configure("relocated", prefix_b, args.config, components)
        if result.returncode == 0:
            result = run([args.cmake, "--build", str(build)])
        if result.returncode == 0:
            exe = build / "consumer.exe"
            result = run([str(exe), "--fonts", str(prefix_b / "share" / "wui" / "fonts")])
            ok = result.returncode == 0 and f"wui {args.version} ok" in result.stdout
        else:
            ok = False
        self.report("relocated", ok, (result.stdout + result.stderr)[-1200:])

        for name in ("LICENSE", "THIRD_PARTY_NOTICES.txt"):
            self.report(f"installed-{name}", (prefix_b / "share" / "doc" / "wui" / name).is_file())

        other = "Release" if args.config == "Debug" else "Debug"
        self.expect_configure_failure("missing-config", prefix_b, other, "core",
                                      f"wui was not installed for configuration {other}")
        missing = "vulkan" if "vulkan" not in components.split() else "bogus"
        self.expect_configure_failure("missing-component", prefix_b, args.config, f"core {missing}",
                                      f"wui component '{missing}' is not installed")
        self.expect_configure_failure("static-runtime", prefix_b, args.config, "core", "DLL runtime",
                                      ["-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"])

        if {"vulkan", "d3d11"} <= set(components.split()):
            env = dict(os.environ)
            sdk = env.pop("VULKAN_SDK", "")
            env.pop("VK_LAYER_PATH", None)
            if sdk:
                keep = [p for p in env.get("PATH", "").split(os.pathsep)
                        if not os.path.normcase(p).startswith(os.path.normcase(sdk))]
                env["PATH"] = os.pathsep.join(keep)
            build, result = self.configure("no-vulkan-sdk", prefix_b, args.config, "core d3d11", env=env)
            if result.returncode == 0:
                result = run([args.cmake, "--build", str(build)], env)
            self.report("no-vulkan-sdk", result.returncode == 0, (result.stdout + result.stderr)[-1200:])

        print(f"{'PASS' if not self.failures else 'FAIL'} package consumer ({self.failures} failures)")
        return 1 if self.failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--components", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--make-program", default="")
    return Driver(parser.parse_args()).main()


if __name__ == "__main__":
    sys.exit(main())
