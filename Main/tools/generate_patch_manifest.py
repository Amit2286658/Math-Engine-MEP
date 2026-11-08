#!/usr/bin/env python3
"""Scans Main/patches/*.math, extracts [module] & <[dep] metadata, topologically sorts patches, and writes Main/patches/manifest.json."""
import hashlib
import json
import re
from pathlib import Path

PATCHES_DIR = Path(__file__).resolve().parent.parent / "patches"
MOD_RE = re.compile(r"^\s*\[([A-Za-z_][A-Za-z0-9_]*)\]\s*(?:#.*)?$", re.MULTILINE)
DEP_RE = re.compile(r"^\s*<\[([A-Za-z_][A-Za-z0-9_]*)\]\s*(?:#.*)?$", re.MULTILINE)


def sha256_hex(path: Path) -> str:
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def parse_patch_meta(path: Path):
    text = path.read_text(encoding="utf-8")
    mods = [m for m in MOD_RE.findall(text) if m != "for"]
    deps = []
    for d in DEP_RE.findall(text):
        if d not in deps:
            deps.append(d)
    if not mods:
        mods = [path.stem]
    return mods, deps


def topo_sort_patches(items):
    mod_to_idx = {}
    for idx, it in enumerate(items):
        for m in it["modules"]:
            mod_to_idx[m] = idx

    n = len(items)
    adj = [[] for _ in range(n)]
    in_deg = [0] * n
    for u, it in enumerate(items):
        for d in it["deps"]:
            v = mod_to_idx.get(d)
            if v is not None and v != u and u not in adj[v]:
                adj[v].append(u)
                in_deg[u] += 1

    scheduled = [False] * n
    order = []
    while len(order) < n:
        pick = next((i for i in range(n) if not scheduled[i] and in_deg[i] == 0), None)
        if pick is None:
            raise RuntimeError("Circular dependency detected among patches/*.math")
        scheduled[pick] = True
        order.append(items[pick])
        for w in adj[pick]:
            in_deg[w] -= 1
    return order


def main() -> None:
    PATCHES_DIR.mkdir(parents=True, exist_ok=True)
    raw_items = []
    for math_file in sorted(PATCHES_DIR.glob("*.math")):
        mods, deps = parse_patch_meta(math_file)
        raw_items.append(
            {
                "file": math_file.name,
                "modules": mods,
                "deps": deps,
                "sha256": sha256_hex(math_file),
                "size": math_file.stat().st_size,
            }
        )
    entries = topo_sort_patches(raw_items)
    manifest = {"version": 1, "patches": entries}
    out_path = PATCHES_DIR / "manifest.json"
    out_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"[patches] Wrote {len(entries)} topologically sorted patch(es) to {out_path}")


if __name__ == "__main__":
    main()
