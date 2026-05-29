#!/usr/bin/env python3
import sys, os, zipfile, json, struct, io
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from SGA import (
    STATE_ID_MAP, CHUNK_PACKET_IDS, WORLD_MIN_Y,
    read_varint, read_varint_bytes, skip_nbt,
    read_palette, iter_packets, PRESETS,
    parse_chunk,
)

import questionary
from rich.console import Console
from rich.table import Table
from rich.text import Text
from rich import print as rprint

console = Console()
PRESET_NAMES = {"Trial Chamber": "trial_chamber", "Deep Dark": "deep_dark", "Ancient City": "ancient_city"}


def run_scan(mcpr_file, targets):
    try:
        with zipfile.ZipFile(mcpr_file, "r") as zf:
            data = zf.read("recording.tmcpr")
    except Exception as e:
        console.print(f"[red]Error opening file: {e}[/]")
        return {}

    found = defaultdict(list)
    packets = 0
    chunks = 0

    try:
        for packet in iter_packets(data):
            packets += 1
            try:
                packet_id, offset = read_varint_bytes(packet, 0)
            except:
                continue
            if packet_id not in CHUNK_PACKET_IDS:
                continue
            chunks += 1
            payload = packet[offset:]
            try:
                results = parse_chunk(payload, targets)
                for r in results:
                    pos = (r["x"], r["y"], r["z"])
                    if pos not in found[r["block"]]:
                        found[r["block"]].append(pos)
            except:
                pass
            if packets % 5000 == 0:
                console.print(f"  packets: {packets}  chunks: {chunks}  found: {sum(len(v) for v in found.values())}")
    except Exception as e:
        console.print(f"[red]Error: {e}[/]")

    console.print(f"  done. {packets} packets, {chunks} chunks.")
    return dict(found)


def show_results(found):
    if not found:
        console.print("[red]No matching blocks found.[/]")
        return

    total = 0
    for block, coords in sorted(found.items()):
        table = Table(title=f"{block}  ({len(coords)})")
        table.add_column("#", style="dim")
        table.add_column("X")
        table.add_column("Y")
        table.add_column("Z")
        for i, (x, y, z) in enumerate(coords[:500], 1):
            table.add_row(str(i), str(x), str(y), str(z))
        console.print(table)
        if len(coords) > 500:
            console.print(f"  ... and {len(coords) - 500} more\n")
        total += len(coords)

    console.print(f"\n[green]TOTAL FOUND: {total}[/]")


def main():
    console.clear()

    file_path = questionary.text("MCPR file:").ask()
    if not file_path:
        return

    if not os.path.isfile(file_path):
        console.print(f"[red]File not found: {file_path}[/]")
        return

    preset_label = questionary.select("Preset:", choices=list(PRESET_NAMES.keys())).ask()
    if not preset_label:
        return

    preset_key = PRESET_NAMES[preset_label]
    targets = PRESETS[preset_key]["blocks"]

    console.print(f"\nFile: {file_path}")
    console.print(f"Preset: {preset_label}")
    console.print(f"Targets: {', '.join(sorted(targets))}\n")

    found = run_scan(file_path, targets)
    show_results(found)

    questionary.press_any_key_to_continue("Press any key to exit...").ask()


if __name__ == "__main__":
    main()
