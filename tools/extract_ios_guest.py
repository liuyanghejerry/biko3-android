#!/usr/bin/env python3
"""Recover the embedded guest image from a published Biko3 IPA.

The unsigned development IPA carries the rewritten guest in three Mach-O
sections of the app executable:

  __TEXT,__dmmainx0       [0, round16K(TEXT_END))
  __DMmainW,__img         [round16K(TEXT_END), PATCH_OFF)   (RW in the app)
  __TEXT,__dmmainx1       [PATCH_OFF, round16K(file length))

Concatenating them reproduces the runtime image the loader expects; the tail of
the last section is zero padding, which the loader rounds away anyway. The
IPA's dm/ resources carry main.fixups.tsv, main.gates.tsv and the other
rewriter metadata, which are copied next to the image.

This is the same input the iOS build embeds, so the Android port uses exactly
the reviewed image. Nothing here decrypts anything: the IPA is a public release
asset.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zipfile
from pathlib import Path

SECTIONS = (("__TEXT", "__dmmainx0"), ("__DMmainW", "__img"), ("__TEXT", "__dmmainx1"))
METADATA = ("main.fixups.tsv", "main.gates.tsv", "main.sites.tsv", "main.cuts.tsv",
            "main.obs.tsv", "Biko3Data.manifest.json")


def macho_sections(data: bytes) -> dict[tuple[str, str], dict]:
    magic, cpu, subtype, filetype, commands, size, flags, reserved = struct.unpack_from("<8I", data)
    if magic != 0xFEEDFACF or cpu != 0x0100000C or filetype != 2 or 32 + size > len(data):
        raise ValueError("expected a thin arm64 Mach-O executable")
    offset = 32
    sections: dict[tuple[str, str], dict] = {}
    for _ in range(commands):
        command, length = struct.unpack_from("<II", data, offset)
        if length < 8 or offset + length > 32 + size:
            raise ValueError("truncated Mach-O load command")
        if command == 0x19:
            segment = struct.unpack_from("<II16s4Q4I", data, offset)
            segment_name = segment[2].rstrip(b"\0").decode()
            count = segment[9]
            for index in range(count):
                section = struct.unpack_from("<16s16sQQ8I", data, offset + 72 + index * 80)
                sections[(segment_name, section[0].rstrip(b"\0").decode())] = {
                    "addr": section[2], "size": section[3], "offset": section[4],
                }
        offset += length
    return sections


def extract(ipa: Path, out: Path) -> dict:
    with zipfile.ZipFile(ipa) as archive:
        executable_name = next((name for name in archive.namelist()
                                if name.startswith("Payload/") and name.endswith("/Biko3")), None)
        if not executable_name:
            raise ValueError("IPA has no Payload/*.app/Biko3 executable")
        executable = archive.read(executable_name)
        sections = macho_sections(executable)
        missing = [key for key in SECTIONS if key not in sections]
        if missing:
            raise ValueError("missing embedded guest sections: " + ", ".join("/".join(key) for key in missing))
        image = bytearray()
        for key in SECTIONS:
            section = sections[key]
            image += executable[section["offset"]:section["offset"] + section["size"]]
        if image[0x10:0x14] != b"NRO0":
            raise ValueError("reconstructed image does not start with the NRO header")
        out.mkdir(parents=True, exist_ok=True)
        (out / "main.patched.bin").write_bytes(image)
        copied = []
        for name in METADATA:
            entry = next((n for n in archive.namelist() if n.endswith("/dm/" + name)), None)
            if entry:
                (out / name).write_bytes(archive.read(entry))
                copied.append(name)
    return {
        "ipa": str(ipa),
        "exported_executable": executable_name,
        "image_bytes": len(image),
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "nro_build_id": image[0x40:0x60].hex(),
        "sections": {"/".join(key): sections[key]["size"] for key in SECTIONS},
        "metadata": copied,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ipa", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--report", type=Path, default=None)
    args = parser.parse_args(argv)
    if args.out.exists() and any(args.out.iterdir()):
        raise SystemExit(f"refusing to write into a non-empty directory: {args.out}")
    report = extract(args.ipa, args.out)
    text = json.dumps(report, indent=2) + "\n"
    if args.report:
        args.report.write_text(text, encoding="utf8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
