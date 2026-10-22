#!/usr/bin/env python3
"""Build visual-review contact sheets from conformance outputs.

For every candidate directory, picks its worst case against the reference
(largest max channel difference, then largest share of pixels over 8) plus
any extra cases named with --case, and writes one PNG per candidate:
reference | candidate | difference (x8, red where over 8), cropped around
the differing region. Needs numpy and Pillow (development tool only).
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy
from PIL import Image, ImageDraw


def load(directory: Path, case: str) -> numpy.ndarray:
    return numpy.asarray(Image.open(directory / (case.replace("/", "__") + ".ppm")).convert("RGB"))


def cases(directory: Path) -> list[str]:
    lines = (directory / "manifest.txt").read_text(encoding="utf-8").splitlines()
    return [line.split()[0] for line in lines if line.strip()]


def score(reference: numpy.ndarray, candidate: numpy.ndarray) -> tuple[int, float]:
    if reference.shape != candidate.shape:
        return 256, 1.0
    diff = numpy.abs(reference.astype(int) - candidate.astype(int)).max(axis=2)
    return int(diff.max()), float((diff > 8).mean())


def panel(reference: numpy.ndarray, candidate: numpy.ndarray, title: str, zoom: int) -> Image.Image:
    diff = numpy.abs(reference.astype(int) - candidate.astype(int)).max(axis=2)
    ys, xs = numpy.nonzero(diff)
    height, width = diff.shape
    if len(ys):
        cx, cy = int(xs.mean()), int(ys.mean())
        span_x = min(width, max(160, int(xs.max() - xs.min()) + 32))
        span_y = min(height, max(120, int(ys.max() - ys.min()) + 32))
    else:
        cx, cy, span_x, span_y = width // 2, height // 2, width, height
    span_x, span_y = min(span_x, 480), min(span_y, 320)
    x0 = max(0, min(width - span_x, cx - span_x // 2))
    y0 = max(0, min(height - span_y, cy - span_y // 2))
    crop = (slice(y0, y0 + span_y), slice(x0, x0 + span_x))
    amplified = numpy.clip(diff[crop] * 8, 0, 255).astype(numpy.uint8)
    visual = numpy.stack([amplified] * 3, axis=2)
    visual[diff[crop] > 8] = (255, 0, 0)
    strip = numpy.hstack([reference[crop], candidate[crop], visual]).astype(numpy.uint8)
    image = Image.fromarray(strip).resize((strip.shape[1] * zoom, strip.shape[0] * zoom), Image.NEAREST)
    sheet = Image.new("RGB", (image.width, image.height + 22), (255, 255, 255))
    sheet.paste(image, (0, 22))
    ImageDraw.Draw(sheet).text((4, 4), f"{title}  crop x={x0} y={y0} {span_x}x{span_y}  "
                               "[reference | candidate | diff x8, red > 8]", fill=(0, 0, 0))
    return sheet


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, action="append", required=True)
    parser.add_argument("--case", action="append", default=[], help="extra case to include for every candidate")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--zoom", type=int, default=2)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for candidate in args.candidate:
        ranked = []
        for case in cases(args.reference):
            worst, fraction = score(load(args.reference, case), load(candidate, case))
            ranked.append((worst, fraction, case))
        ranked.sort(reverse=True)
        chosen = [ranked[0][2]] + [case for case in args.case if case != ranked[0][2]]
        panels = []
        for case in chosen:
            reference, image = load(args.reference, case), load(candidate, case)
            worst, fraction = score(reference, image)
            panels.append(panel(reference, image,
                                f"{candidate.name}: {case}  max={worst} over8={fraction:.4%}", args.zoom))
        sheet = Image.new("RGB", (max(p.width for p in panels), sum(p.height + 8 for p in panels)), (255, 255, 255))
        y = 0
        for item in panels:
            sheet.paste(item, (0, y))
            y += item.height + 8
        path = args.out / f"{candidate.name}.png"
        sheet.save(path)
        print(f"{path}: worst {ranked[0][2]} max={ranked[0][0]} over8={ranked[0][1]:.4%}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
