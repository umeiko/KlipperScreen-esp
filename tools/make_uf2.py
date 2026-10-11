#!/usr/bin/env python3
"""Convert a raw application binary to UF2 without external dependencies.

Wio Terminal's UF2 bootloader maps the SAMD51 application at 0x4000 and
identifies the MCU with the official SAMD51 family ID (0x55114460).
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
UF2_BLOCK_SIZE = 512
UF2_PAYLOAD_SIZE = 256


def convert(binary: bytes, base_address: int, family_id: int) -> bytes:
    block_count = (len(binary) + UF2_PAYLOAD_SIZE - 1) // UF2_PAYLOAD_SIZE
    blocks: list[bytes] = []

    for block_no in range(block_count):
        start = block_no * UF2_PAYLOAD_SIZE
        payload = binary[start : start + UF2_PAYLOAD_SIZE]
        payload = payload.ljust(UF2_PAYLOAD_SIZE, b"\x00")
        header = struct.pack(
            "<IIIIIIII",
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            UF2_FLAG_FAMILY_ID_PRESENT,
            base_address + start,
            UF2_PAYLOAD_SIZE,
            block_no,
            block_count,
            family_id,
        )
        padding = b"\x00" * (UF2_BLOCK_SIZE - len(header) - len(payload) - 4)
        blocks.append(header + payload + padding + struct.pack("<I", UF2_MAGIC_END))

    return b"".join(blocks)


def parse_int(value: str) -> int:
    return int(value, 0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="raw application .bin")
    parser.add_argument("output", type=Path, help="output .uf2")
    parser.add_argument("--base", type=parse_int, default=0x4000)
    parser.add_argument("--family", type=parse_int, default=0x55114460)
    args = parser.parse_args()

    image = args.input.read_bytes()
    if not image:
        parser.error("input binary is empty")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    uf2 = convert(image, args.base, args.family)
    args.output.write_bytes(uf2)
    print(
        f"UF2: {len(image)} bytes -> {len(uf2)} bytes, "
        f"base=0x{args.base:x}, family=0x{args.family:08x}"
    )


if __name__ == "__main__":
    main()
