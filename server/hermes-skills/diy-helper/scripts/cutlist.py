#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Nest panel parts onto sheet goods and total up lumber, from a JSON parts list on stdin.

Input (inches):
{
  "sheet": {"length": 96, "width": 48},      # optional, default 4x8
  "kerf": 0.125,                              # optional, saw blade width
  "parts": [
    {"name": "Side", "qty": 2, "length": 84, "width": 23.25, "material": "3/4 plywood"},
    {"name": "Face frame stile", "qty": 2, "length": 84, "width": 1.5, "material": "1x2 poplar", "linear": true}
  ]
}
Parts with "linear": true are boards bought by length (total feet, rounded up to 8 ft sticks);
everything else is nested onto sheets per material. Grain runs along the sheet length; parts are
rotated only if they don't fit otherwise. Prints JSON: sheets per material with a strip layout,
lumber totals, and any parts too big for a sheet.
"""

import json
import math
import sys
from collections import defaultdict


def nest(parts, L, W, kerf):
    """Strip packing: rows across the sheet width, parts placed along the length, first-fit decreasing."""
    pieces = []
    for p in parts:
        for i in range(int(p["qty"])):
            l, w = float(p["length"]), float(p["width"])
            if l > L or w > W:
                l, w = w, l   # try rotated
            pieces.append({"name": p["name"], "l": l, "w": w, "fits": l <= L and w <= W})
    too_big = [p["name"] for p in pieces if not p["fits"]]
    pieces = sorted((p for p in pieces if p["fits"]), key=lambda p: (-p["w"], -p["l"]))
    sheets = []   # each: list of strips {width, used_len, parts}
    for p in pieces:
        placed = False
        for s in sheets:
            for strip in s["strips"]:
                if p["w"] <= strip["width"] and strip["used"] + p["l"] <= L:
                    strip["used"] += p["l"] + kerf; strip["parts"].append(f'{p["name"]} {p["l"]:g}x{p["w"]:g}')
                    placed = True; break
            if not placed and s["used_w"] + p["w"] <= W:
                s["strips"].append({"width": p["w"], "used": p["l"] + kerf, "parts": [f'{p["name"]} {p["l"]:g}x{p["w"]:g}']})
                s["used_w"] += p["w"] + kerf; placed = True
            if placed:
                break
        if not placed:
            sheets.append({"used_w": p["w"] + kerf, "strips": [{"width": p["w"], "used": p["l"] + kerf,
                                                               "parts": [f'{p["name"]} {p["l"]:g}x{p["w"]:g}']}]})
    area = sum(p["l"] * p["w"] for p in pieces)
    return {"sheets": len(sheets), "yield_pct": round(100 * area / (len(sheets) * L * W)) if sheets else 0,
            "layout": [[{"rip_width": s["width"], "parts": s["parts"]} for s in sh["strips"]] for sh in sheets],
            "too_big": too_big}


def main():
    spec = json.load(sys.stdin)
    L, W = spec.get("sheet", {}).get("length", 96), spec.get("sheet", {}).get("width", 48)
    kerf = spec.get("kerf", 0.125)
    by_mat, lumber = defaultdict(list), defaultdict(float)
    for p in spec["parts"]:
        if p.get("linear"):
            lumber[p["material"]] += p["qty"] * (p["length"] + kerf) / 12
        else:
            by_mat[p["material"]].append(p)
    out = {"sheet_goods": {m: nest(ps, L, W, kerf) for m, ps in by_mat.items()},
           "lumber": {m: {"feet": round(ft, 1), "8ft_boards": math.ceil(ft * 1.1 / 8)} for m, ft in lumber.items()},
           "note": "lumber includes 10% waste; sheet yield is the share of each sheet actually used"}
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
