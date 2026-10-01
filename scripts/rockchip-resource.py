#!/usr/bin/env python3
"""Inspect and rebuild Rockchip RSCE resource images.

The tool keeps the original 512-byte header and entry metadata, while
recomputing the SHA-1 digest, sector position and byte size for each payload.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping

SECTOR_SIZE = 512
HEADER_MAGIC = b"RSCE"
ENTRY_MAGIC = b"ENTR"
ENTRY_NAME_OFFSET = 4
ENTRY_NAME_SIZE = 220
ENTRY_SHA1_OFFSET = 0xE0
ENTRY_SHA1_SIZE = 20
ENTRY_FIELDS_OFFSET = 0x100


@dataclass(frozen=True)
class ResourceEntry:
    name: str
    data: bytes
    sha1: bytes
    sector: int
    size: int
    raw_entry: bytes


@dataclass(frozen=True)
class ResourceImage:
    header: bytes
    entries: tuple[ResourceEntry, ...]


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def parse_resource(blob: bytes) -> ResourceImage:
    if len(blob) < SECTOR_SIZE or blob[:4] != HEADER_MAGIC:
        raise ValueError("not a Rockchip RSCE resource image")

    entry_count = struct.unpack_from("<I", blob, 0x0C)[0]
    metadata_size = (entry_count + 1) * SECTOR_SIZE
    if entry_count == 0 or metadata_size > len(blob):
        raise ValueError(f"invalid resource entry count: {entry_count}")

    entries: list[ResourceEntry] = []
    for index in range(entry_count):
        offset = (index + 1) * SECTOR_SIZE
        raw_entry = blob[offset : offset + SECTOR_SIZE]
        if raw_entry[:4] != ENTRY_MAGIC:
            raise ValueError(f"entry {index} has invalid magic")

        raw_name = raw_entry[ENTRY_NAME_OFFSET : ENTRY_NAME_OFFSET + ENTRY_NAME_SIZE]
        name = raw_name.split(b"\0", 1)[0].decode("utf-8")
        sha1 = raw_entry[ENTRY_SHA1_OFFSET : ENTRY_SHA1_OFFSET + ENTRY_SHA1_SIZE]
        hash_size, sector, size = struct.unpack_from("<III", raw_entry, ENTRY_FIELDS_OFFSET)
        if hash_size != ENTRY_SHA1_SIZE:
            raise ValueError(f"entry {name!r} uses unsupported hash size {hash_size}")

        data_offset = sector * SECTOR_SIZE
        data_end = data_offset + size
        if sector < entry_count + 1 or data_end > len(blob):
            raise ValueError(f"entry {name!r} payload is outside the image")
        data = blob[data_offset:data_end]
        actual_sha1 = hashlib.sha1(data).digest()
        if actual_sha1 != sha1:
            raise ValueError(f"entry {name!r} SHA-1 mismatch")

        entries.append(ResourceEntry(name, data, sha1, sector, size, raw_entry))

    return ResourceImage(blob[:SECTOR_SIZE], tuple(entries))


def rebuild_resource(image: ResourceImage, replacements: Mapping[str, bytes]) -> bytes:
    known_names = {entry.name for entry in image.entries}
    unknown_names = set(replacements) - known_names
    if unknown_names:
        raise ValueError(f"replacement entries do not exist: {sorted(unknown_names)}")

    metadata_size = (len(image.entries) + 1) * SECTOR_SIZE
    metadata = bytearray(metadata_size)
    metadata[:SECTOR_SIZE] = image.header
    payload = bytearray()
    next_sector = len(image.entries) + 1

    for index, entry in enumerate(image.entries):
        data = replacements.get(entry.name, entry.data)
        raw_entry = bytearray(entry.raw_entry)
        raw_entry[ENTRY_SHA1_OFFSET : ENTRY_SHA1_OFFSET + ENTRY_SHA1_SIZE] = hashlib.sha1(data).digest()
        struct.pack_into(
            "<III",
            raw_entry,
            ENTRY_FIELDS_OFFSET,
            ENTRY_SHA1_SIZE,
            next_sector,
            len(data),
        )
        entry_offset = (index + 1) * SECTOR_SIZE
        metadata[entry_offset : entry_offset + SECTOR_SIZE] = raw_entry

        payload.extend(data)
        padded_size = align_up(len(data), SECTOR_SIZE)
        payload.extend(b"\0" * (padded_size - len(data)))
        next_sector += padded_size // SECTOR_SIZE

    rebuilt = bytes(metadata + payload)
    parse_resource(rebuilt)
    return rebuilt


def describe(image: ResourceImage) -> dict[str, object]:
    return {
        "magic": image.header[:4].decode("ascii"),
        "version_bytes": image.header[8:12].hex(),
        "entry_count": len(image.entries),
        "entries": [
            {
                "name": entry.name,
                "sha1": entry.sha1.hex(),
                "sector": entry.sector,
                "offset": entry.sector * SECTOR_SIZE,
                "size": entry.size,
            }
            for entry in image.entries
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="source RSCE resource image")
    parser.add_argument("--output", type=Path, help="write rebuilt resource image")
    parser.add_argument(
        "--replace",
        action="append",
        default=[],
        metavar="NAME=FILE",
        help="replace one named payload; may be specified more than once",
    )
    parser.add_argument("--manifest", type=Path, help="write JSON manifest for the output")
    args = parser.parse_args()

    source_blob = args.source.read_bytes()
    image = parse_resource(source_blob)
    replacements: dict[str, bytes] = {}
    for item in args.replace:
        if "=" not in item:
            parser.error(f"invalid replacement {item!r}; expected NAME=FILE")
        name, file_name = item.split("=", 1)
        if name in replacements:
            parser.error(f"duplicate replacement for {name!r}")
        replacements[name] = Path(file_name).read_bytes()

    if args.output:
        rebuilt = rebuild_resource(image, replacements)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(rebuilt)
        result = parse_resource(rebuilt)
    elif replacements:
        parser.error("--replace requires --output")
    else:
        result = image

    manifest = describe(result)
    manifest["image_size"] = (
        args.output.stat().st_size if args.output else args.source.stat().st_size
    )
    manifest["image_sha256"] = hashlib.sha256(
        args.output.read_bytes() if args.output else source_blob
    ).hexdigest()
    rendered = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    print(rendered, end="")
    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
