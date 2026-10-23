#!/usr/bin/env python3
"""Retarget the guest's thread-pointer reads for Android.

The nxnative rewriter turns every guest access to the thread slots (x18,
tpidr_el0, tpidrro_el0, the gate entry) into "read the key page offset, then
load [tpidrro_el0 + offset]". That matches Darwin, where the host TSD lives at
TP + 8*key, and works on iOS because the host can point tpidrro_el0 at its own
block.

Android uses a different scheme (android/port/src/dm_tsd.c): the host clones
each thread's TLS block, places the slots a fixed distance below the clone and
moves the thread pointer into it, so the offsets are TP-relative and the guest
must read the real thread pointer. Both instructions are the same width, so the
image layout, the patch area and every address in the rewriter metadata stay
valid; only the register operand changes.

  mrs xN, tpidrro_el0 -> mrs xN, tpidr_el0

A second class of edits covers instructions the x86 emulator's ARM translator
decodes as illegal. They are valid AArch64 and are executed natively on real
devices; the emulator-only rewrites below keep identical semantics for the
values this game produces.

The output is written to a new file; the input is never modified.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

MRS_MASK, MRS_TPIDRRO = 0xFFFFFFE0, 0xD53BD060
MRS_TPIDR = 0xD53BD040

# Instruction classes the x86 emulator's ARM translator (berberis) rejects.
# The first entries retarget scalar double-to-integer conversions to the vector
# form: the translator implements the SIMD conversion but traps on the scalar
# one, and lane 0 computes exactly the same value. Encodings are (base, mask,
# xor) and every match in the image is rewritten.
TRANSLATOR_CLASSES = [
    # fcvtzu dD, dN -> fcvtzu vD.2d, vN.2d
    (0x7EE1B800, 0xFFFFFC00, 0x10000000),
    # fcvtzs dD, dN -> fcvtzs vD.2d, vN.2d
    (0x5EE1B800, 0xFFFFFC00, 0x10000000),
]

# Install(offset, expected word, replacement) for one-off rewrites; the class
# rules above cover the conversion instructions this build uses.
TRANSLATOR_FIXUPS = []


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def patch(data: bytes) -> tuple[bytes, dict]:
    if data[0x10:0x14] != b"NRO0":
        raise ValueError("input does not start with an NRO header")
    if len(data) % 4:
        raise ValueError("input is not a whole number of instructions")
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    mrs = 0
    for index, word in enumerate(words):
        if word & MRS_MASK == MRS_TPIDRRO:
            words[index] = MRS_TPIDR | (word & 0x1F)
            mrs += 1
    if not mrs:
        raise ValueError("no tpidrro_el0 read found in the image")
    classes = 0
    for index, word in enumerate(words):
        for base, mask, xor in TRANSLATOR_CLASSES:
            if word & mask == base:
                words[index] = word ^ xor
                classes += 1
                break
    fixups = 0
    for offset, expected, replacement in TRANSLATOR_FIXUPS:
        index = offset // 4
        if words[index] != expected:
            raise ValueError(f"unexpected instruction at {offset:#x}: {words[index]:#010x}")
        words[index] = replacement
        fixups += 1
    patched = struct.pack(f"<{len(words)}I", *words)
    report = {
        "source_sha256": sha256(data),
        "patched_sha256": sha256(patched),
        "mrs_tpidrro_to_tpidr": mrs,
        "translator_fixups": fixups,
        "translator_class_rewrites": classes,
        "bytes": len(patched),
        "nro_build_id": data[0x40:0x60].hex(),
    }
    return patched, report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--report", type=Path, default=None)
    args = parser.parse_args(argv)
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    patched, report = patch(args.input.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(patched)
    text = json.dumps(report, indent=2) + "\n"
    if args.report:
        args.report.write_text(text, encoding="utf8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
