#!/usr/bin/env python3
"""Compare backend conformance outputs against a reference backend.

Each output directory is written by a wui_conformance_<api> executable and
holds manifest.txt, summary.txt and one binary PPM per case. Tolerances match
tests/conformance/compare.cpp: fixture cases allow a max RGB channel
difference of 2; every other case allows at most 0.2% of pixels differing by
more than 8 and a max difference of 48. Exits 1 on any failure.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

try:
    import numpy
except ImportError:  # pure-Python fallback, slower but dependency-free
    numpy = None


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    fields: list[bytes] = []
    at = 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        if data[at:at + 1] == b"#":
            at = data.index(b"\n", at) + 1
            continue
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end])
        at = end
    if fields[0] != b"P6" or fields[3] != b"255":
        raise ValueError(f"{path}: not an 8-bit binary PPM")
    width, height = int(fields[1]), int(fields[2])
    pixels = data[at + 1:]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path}: truncated pixel data")
    return width, height, pixels


def difference(reference: bytes, candidate: bytes) -> tuple[int, int]:
    """Returns (max channel difference, pixels whose max difference is > 8)."""
    if reference == candidate:
        return 0, 0
    if numpy is not None:
        a = numpy.frombuffer(reference, dtype=numpy.uint8).reshape(-1, 3).astype(numpy.int16)
        b = numpy.frombuffer(candidate, dtype=numpy.uint8).reshape(-1, 3).astype(numpy.int16)
        per_pixel = numpy.abs(a - b).max(axis=1)
        return int(per_pixel.max()), int((per_pixel > 8).sum())
    worst = over = 0
    for i in range(0, len(reference), 3):
        pixel = max(abs(reference[i + c] - candidate[i + c]) for c in range(3))
        worst = max(worst, pixel)
        over += pixel > 8
    return worst, over


def within_tolerance(name: str, worst: int, fraction: float) -> tuple[bool, str]:
    if name.startswith("fixture/"):
        return worst <= 2, f"fixture max {worst} > 2"
    if fraction > 0.002:
        return False, f"fraction over 8 = {fraction:.4%} > 0.2%"
    return worst <= 48, f"max {worst} > 48"


def file_name(case: str) -> str:
    return case.replace("/", "__") + ".ppm"


def read_manifest(directory: Path) -> list[str]:
    lines = (directory / "manifest.txt").read_text(encoding="utf-8").splitlines()
    return [line.split()[0] for line in lines if line.strip()]


def summary_result(directory: Path) -> str:
    path = directory / "summary.txt"
    if not path.exists():
        return "missing summary.txt"
    lines = path.read_text(encoding="utf-8").splitlines()
    adapter = next((line for line in lines if line.startswith("adapter:")), "adapter: unknown")
    result = next((line for line in reversed(lines) if line.startswith("RESULT")), "RESULT missing")
    return f"{adapter[len('adapter:'):].strip()} | {result}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, action="append", required=True)
    parser.add_argument("--report", type=Path, help="write the Markdown report here")
    args = parser.parse_args()

    failures = 0
    reference_cases = read_manifest(args.reference)
    report = [
        "# Backend conformance comparison",
        "",
        f"Reference: `{args.reference.name}` ({len(reference_cases)} cases) - "
        f"{summary_result(args.reference)}",
        "",
        "| Candidate | Adapter / suite result | Identical | Within tolerance | Failed |",
        "|---|---|---|---|---|",
    ]
    details: list[str] = []
    for candidate in args.candidate:
        identical = tolerated = failed = 0
        suite = summary_result(candidate)
        if "RESULT PASS" not in suite:
            failed += 1
            details.append(f"| {candidate.name} | (suite) | - | - | {suite} |")
        candidate_cases = set(read_manifest(candidate))
        worst_case = ("", -1, 0.0)
        for case in reference_cases:
            if case not in candidate_cases:
                failed += 1
                details.append(f"| {candidate.name} | {case} | - | - | missing |")
                continue
            rw, rh, rp = read_ppm(args.reference / file_name(case))
            cw, ch, cp = read_ppm(candidate / file_name(case))
            if (rw, rh) != (cw, ch):
                failed += 1
                details.append(f"| {candidate.name} | {case} | - | - | size {cw}x{ch} vs {rw}x{rh} |")
                continue
            worst, over = difference(rp, cp)
            fraction = over / (rw * rh) if rw * rh else 0.0
            ok, reason = within_tolerance(case, worst, fraction)
            if worst > worst_case[1]:
                worst_case = (case, worst, fraction)
            if worst == 0:
                identical += 1
            elif ok:
                tolerated += 1
            else:
                failed += 1
                details.append(f"| {candidate.name} | {case} | {worst} | {fraction:.4%} | **FAIL** {reason} |")
        failures += failed
        report.append(f"| {candidate.name} | {suite} | {identical} | {tolerated} | {failed} |")
        if worst_case[0]:
            details.append(f"| {candidate.name} | worst: {worst_case[0]} | {worst_case[1]} | "
                           f"{worst_case[2]:.4%} | summary |")

    report += ["", "## Failures and worst case per candidate", "",
               "| Candidate | Case | Max channel diff | Fraction over 8 | Verdict |",
               "|---|---|---|---|---|"]
    report += details or ["| - | - | - | - | all identical |"]
    report += ["", f"RESULT {'PASS' if failures == 0 else f'FAIL failures={failures}'}", ""]
    text = "\n".join(report)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text, encoding="utf-8")
    sys.stdout.write(text)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
