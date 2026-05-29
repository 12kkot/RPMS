#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
FULL MCPR SCANNER
Minecraft ReplayMod scanner for 1.21.x

Работает с:
- ReplayMod .mcpr
- Minecraft 1.21 - 1.21.11
- Trial Chambers
- Deep Dark
- Ancient City
- кастомными блоками

ИСПОЛЬЗОВАНИЕ:

python SGA.py replay.mcpr --preset trial_chamber
python SGA.py replay.mcpr --preset deep_dark
python SGA.py replay.mcpr --blocks minecraft:beacon
"""

import io
import json
import struct
import zipfile
import argparse
from collections import defaultdict

# =========================================================
# PRESETS
# =========================================================

PRESETS = {
    "trial_chamber": {
        "blocks": {
            "minecraft:trial_spawner",
            "minecraft:vault",
            "minecraft:copper_block",
            "minecraft:cut_copper",
            "minecraft:tuff_bricks",
            "minecraft:chiseled_tuff",
        }
    },

    "deep_dark": {
        "blocks": {
            "minecraft:sculk",
            "minecraft:sculk_sensor",
            "minecraft:sculk_shrieker",
            "minecraft:reinforced_deepslate",
        }
    },

    "ancient_city": {
        "blocks": {
            "minecraft:reinforced_deepslate",
            "minecraft:sculk_shrieker",
        }
    }
}

# =========================================================
# IMPORTANT STATE IDS
# 1.21.x
# =========================================================

STATE_ID_MAP = {}

def add_range(start, end, name):
    for i in range(start, end + 1):
        STATE_ID_MAP[i] = name

# =========================================================
# DEEP DARK
# =========================================================

add_range(24968, 24968, "minecraft:sculk")
add_range(24488, 24583, "minecraft:sculk_sensor")
add_range(25099, 25106, "minecraft:sculk_shrieker")
add_range(29390, 29390, "minecraft:reinforced_deepslate")

# =========================================================
# TRIAL CHAMBER
# =========================================================

add_range(29455, 29466, "minecraft:trial_spawner")
add_range(29467, 29498, "minecraft:vault")

# COPPER
add_range(25107, 25107, "minecraft:copper_block")
add_range(25116, 25116, "minecraft:cut_copper")

# TUFF
add_range(24073, 24073, "minecraft:tuff_bricks")
add_range(24072, 24072, "minecraft:chiseled_tuff")

# =========================================================
# VARINT
# =========================================================

def read_varint(buf):

    result = 0
    shift = 0

    while True:

        b = buf.read(1)

        if not b:
            raise EOFError

        byte = b[0]

        result |= (byte & 0x7F) << shift

        if not (byte & 0x80):
            break

        shift += 7

        if shift > 35:
            raise ValueError("VarInt too long")

    return result


def read_varint_bytes(data, offset):

    result = 0
    shift = 0

    while True:

        byte = data[offset]
        offset += 1

        result |= (byte & 0x7F) << shift

        if not (byte & 0x80):
            break

        shift += 7

    return result, offset

# =========================================================
# NETWORK NBT SKIP
# =========================================================

def skip_nbt(buf):

    tag = buf.read(1)

    if not tag:
        return

    tag = tag[0]

    if tag == 0:
        return

    # NETWORK NBT
    if tag == 10:

        while True:

            t = buf.read(1)

            if not t:
                break

            t = t[0]

            if t == 0:
                break

            name_len = struct.unpack(">H", buf.read(2))[0]

            buf.read(name_len)

            skip_nbt_payload(buf, t)

        return

    # OLD NBT
    name_len = struct.unpack(">H", buf.read(2))[0]

    buf.read(name_len)

    skip_nbt_payload(buf, tag)


def skip_nbt_payload(buf, tag):

    if tag == 1:
        buf.read(1)

    elif tag == 2:
        buf.read(2)

    elif tag == 3:
        buf.read(4)

    elif tag == 4:
        buf.read(8)

    elif tag == 5:
        buf.read(4)

    elif tag == 6:
        buf.read(8)

    elif tag == 7:
        length = struct.unpack(">i", buf.read(4))[0]
        buf.read(length)

    elif tag == 8:
        length = struct.unpack(">H", buf.read(2))[0]
        buf.read(length)

    elif tag == 9:

        inner = buf.read(1)[0]

        length = struct.unpack(">i", buf.read(4))[0]

        for _ in range(length):
            skip_nbt_payload(buf, inner)

    elif tag == 10:

        while True:

            t = buf.read(1)[0]

            if t == 0:
                break

            name_len = struct.unpack(">H", buf.read(2))[0]

            buf.read(name_len)

            skip_nbt_payload(buf, t)

    elif tag == 11:

        length = struct.unpack(">i", buf.read(4))[0]

        buf.read(length * 4)

    elif tag == 12:

        length = struct.unpack(">i", buf.read(4))[0]

        buf.read(length * 8)

# =========================================================
# PALETTE CONTAINER
# =========================================================

def read_palette(buf, is_block=True):

    bits = buf.read(1)

    if not bits:
        raise EOFError

    bits = bits[0]

    size = 4096 if is_block else 64
    indirect_max = 8 if is_block else 3

    # SINGLE VALUE
    if bits == 0:

        value = read_varint(buf)

        return [value] * size

    palette = None

    # INDIRECT
    if bits <= indirect_max:

        palette_len = read_varint(buf)

        palette = []

        for _ in range(palette_len):
            palette.append(read_varint(buf))

    # LONG ARRAY (length calculated from bits in protocol 774+)
    if bits == 0:
        arr_len = 0
    else:
        entries_per_long = 64 // bits
        arr_len = (size + entries_per_long - 1) // entries_per_long

    if arr_len <= 0:
        raw = b''
        longs = ()

    else:
        raw = buf.read(arr_len * 8)

        if len(raw) < arr_len * 8:
            raise EOFError

        longs = struct.unpack(f">{arr_len}Q", raw)

    longs = struct.unpack(f">{arr_len}Q", raw)

    mask = (1 << bits) - 1

    entries_per_long = 64 // bits

    values = []

    for ulong in longs:

        for i in range(entries_per_long):

            if len(values) >= size:
                break

            val = (ulong >> (i * bits)) & mask

            if palette is not None:

                if val < len(palette):
                    values.append(palette[val])
                else:
                    values.append(0)

            else:
                values.append(val)

    while len(values) < size:
        values.append(0)

    return values[:size]

# =========================================================
# PACKET ITERATOR
# =========================================================

def iter_packets(data):

    buf = io.BytesIO(data)

    while True:

        header = buf.read(8)

        if len(header) < 8:
            break

        timestamp, length = struct.unpack(">II", header)

        if length <= 0:
            continue

        payload = buf.read(length)

        if len(payload) < length:
            break

        yield payload

# =========================================================
# WORLD SETTINGS
# =========================================================

WORLD_MIN_Y = -64

# =========================================================
# CHUNK IDS
# =========================================================

# brute force for 1.21.x

CHUNK_PACKET_IDS = set(range(0x20, 0x40))

# =========================================================
# PARSE CHUNK
# =========================================================

def parse_chunk(payload, targets):

    results = []

    buf = io.BytesIO(payload)

    try:

        chunk_x = struct.unpack(">i", buf.read(4))[0]
        chunk_z = struct.unpack(">i", buf.read(4))[0]

    except:
        return results

    # Dimension Type (1.21+)
    try:
        read_varint(buf)
    except:
        return results

    # HEIGHTMAPS (Prefixed Array of Heightmap - protocol 774+)
    # 3 heightmaps: MOTION_BLOCKING(4), MOTION_BLOCKING_NO_LEAVES(5), WORLD_SURFACE(1)
    try:
        for _ in range(3):
            buf.read(1)  # type byte (4, 5, or 1)
            hm_len = read_varint(buf)  # data length in longs
            buf.read(hm_len * 8)  # skip long array
    except:
        return results

    # SECTION DATA SIZE
    try:
        sections_length = read_varint(buf)
    except:
        return results

    if sections_length <= 0:
        return results

    section_data = buf.read(sections_length)

    if not section_data:
        return results

    sec = io.BytesIO(section_data)

    section_index = 0

    while sec.tell() < len(section_data):

        try:

            # block count
            sec.read(2)

            # BLOCK STATES
            blocks = read_palette(sec, True)

            y_base = WORLD_MIN_Y + section_index * 16

            for i, state_id in enumerate(blocks):

                block_name = STATE_ID_MAP.get(state_id)

                if block_name in targets:

                    bx = i & 15
                    bz = (i >> 4) & 15
                    by = (i >> 8) & 15

                    results.append({
                        "block": block_name,
                        "x": chunk_x * 16 + bx,
                        "y": y_base + by,
                        "z": chunk_z * 16 + bz,
                    })

            # BIOMES
            read_palette(sec, False)

            section_index += 1

        except:
            break

    return results

# =========================================================
# MAIN SCAN
# =========================================================

def scan(mcpr_file, targets):

    print(f"\nOpening: {mcpr_file}")

    with zipfile.ZipFile(mcpr_file, "r") as zf:

        metadata = json.loads(
            zf.read("metaData.json")
        )

        version = metadata.get("mcversion")
        protocol = metadata.get("protocol")

        print(f"Minecraft: {version}")
        print(f"Protocol: {protocol}")

        data = zf.read("recording.tmcpr")

    found = defaultdict(list)

    packets = 0
    chunk_packets = 0

    biggest = []

    for packet in iter_packets(data):

        packets += 1

        try:
            packet_id, offset = read_varint_bytes(packet, 0)
        except:
            continue

        size = len(packet)

        # DEBUG BIG PACKETS
        if size > 15000:
            biggest.append((packet_id, size))

        if packet_id not in CHUNK_PACKET_IDS:
            continue

        chunk_packets += 1

        payload = packet[offset:]

        try:

            results = parse_chunk(payload, targets)

            for r in results:

                pos = (r["x"], r["y"], r["z"])

                if pos not in found[r["block"]]:
                    found[r["block"]].append(pos)

        except:
            pass

        if packets % 10000 == 0:

            print(
                f"Packets={packets} "
                f"Chunks={chunk_packets}",
                end="\r"
            )

    print("\n")

    print("=" * 60)
    print("BIG PACKETS")
    print("=" * 60)

    for pid, size in sorted(biggest, key=lambda x: -x[1])[:20]:
        print(f"0x{pid:02X}  {size:,} bytes")

    print()

    total = 0

    for block, coords in found.items():

        print("=" * 60)
        print(block)
        print("=" * 60)

        for x, y, z in coords[:500]:

            print(f"X={x} Y={y} Z={z}")

        print()

        total += len(coords)

    print("=" * 60)
    print(f"TOTAL FOUND: {total}")
    print("=" * 60)

# =========================================================
# CLI
# =========================================================

def main():

    parser = argparse.ArgumentParser()

    parser.add_argument("file")

    parser.add_argument("--preset")

    parser.add_argument(
        "--blocks",
        "--block",
        nargs="*",
        default=[]
    )

    args = parser.parse_args()

    targets = set()

    if args.preset:

        if args.preset in PRESETS:
            targets |= PRESETS[args.preset]["blocks"]
        else:
            print(f'Unknown preset "{args.preset}"')
            print(f"Available presets: {', '.join(PRESETS.keys())}")
            print('Use --blocks to search for specific blocks, e.g.: --blocks minecraft:waxed_copper_block')
            return

    if args.blocks:
        targets |= set(args.blocks)

    if not targets:

        print("No blocks selected")
        return

    print("\nSearching for:")

    for b in sorted(targets):
        print("-", b)

    scan(args.file, targets)

# =========================================================

if __name__ == "__main__":
    main()