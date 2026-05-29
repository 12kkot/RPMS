#!/usr/bin/env python3
"""
SGA TUI — Minecraft MCPR Scanner with Textual interface.
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from SGA import (
    STATE_ID_MAP, CHUNK_PACKET_IDS, WORLD_MIN_Y,
    read_varint, read_varint_bytes, skip_nbt,
    read_palette, iter_packets, PRESETS,
    parse_chunk,
)

from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical, ScrollableContainer
from textual.widgets import (
    Header, Footer, Input, RadioSet, RadioButton,
    Button, RichLog, Static, Label, Rule
)
from textual.binding import Binding
from textual.screen import Screen

import zipfile, json, struct, io
from collections import defaultdict
from rich.table import Table as RichTable
from rich.text import Text


class ScanWorker:
    """Runs scan logic, yields progress/result events."""

    def __init__(self, mcpr_file, targets):
        self.mcpr_file = mcpr_file
        self.targets = targets

    def run(self, progress_cb, result_cb, done_cb):
        try:
            with zipfile.ZipFile(self.mcpr_file, "r") as zf:
                metadata = json.loads(zf.read("metaData.json"))
                data = zf.read("recording.tmcpr")
        except Exception as e:
            progress_cb(f"Error opening file: {e}")
            done_cb({})
            return

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
                    results = parse_chunk(payload, self.targets)
                    for r in results:
                        pos = (r["x"], r["y"], r["z"])
                        if pos not in found[r["block"]]:
                            found[r["block"]].append(pos)
                except:
                    pass

                if packets % 5000 == 0:
                    progress_cb(f"Packets: {packets}  Chunks: {chunks}  Found: {sum(len(v) for v in found.values())}")
        except Exception as e:
            progress_cb(f"Error: {e}")

        progress_cb(f"Done. {packets} packets, {chunks} chunks.")
        done_cb(dict(found))


class MainScreen(Screen):

    BINDINGS = [
        Binding("ctrl+q", "quit", "Quit", priority=True),
    ]

    TITLE = "SGA Scanner"

    def compose(self):
        yield Header(show_clock=False)
        yield Vertical(
            Input(
                placeholder=r"Path to .mcpr file...",
                id="file-input",
            ),
            RadioSet(
                RadioButton("Trial Chamber", id="trial_chamber"),
                RadioButton("Deep Dark", id="deep_dark"),
                RadioButton("Ancient City", id="ancient_city"),
                id="preset-radio",
            ),
            Horizontal(
                Button("Scan", id="scan-btn"),
                Button("Clear Results", id="clear-btn"),
                id="button-row",
            ),
            Rule(),
            Label("Results", id="results-header"),
            ScrollableContainer(
                RichLog(id="results-log", highlight=True, markup=True),
                id="results-area",
            ),
            Static("Status: Ready", id="status-bar"),
            id="main-layout",
        )
        yield Footer()

    def on_mount(self):
        self.query_one("#file-input", Input).focus()

    def on_button_pressed(self, event: Button.Pressed):
        if event.button.id == "scan-btn":
            self.start_scan()
        elif event.button.id == "clear-btn":
            self.clear_results()

    def clear_results(self):
        self.query_one("#results-log", RichLog).clear()
        self.query_one("#status-bar", Static).update("Status: Ready")

    def start_scan(self):
        file_input = self.query_one("#file-input", Input).value.strip()
        if not file_input:
            self.query_one("#status-bar", Static).update("Status: [red]Please enter a file path[/]")
            return

        if not os.path.isfile(file_input):
            self.query_one("#status-bar", Static).update(f"Status: [red]File not found: {file_input}[/]")
            return

        radio = self.query_one("#preset-radio", RadioSet)
        targets = set()
        for btn in radio.query(RadioButton):
            if btn.value and btn.id in PRESETS:
                targets |= PRESETS[btn.id]["blocks"]
                break

        if not targets:
            self.query_one("#status-bar", Static).update("Status: [red]No preset selected[/]")
            return

        log = self.query_one("#results-log", RichLog)
        log.clear()
        log.write("[bold green]Starting scan...[/]")
        log.write(f"File: {file_input}")
        log.write(f"Targets: {', '.join(sorted(targets))}")
        log.write("")

        status = self.query_one("#status-bar", Static)
        status.update("Status: [yellow]Scanning...[/]")

        worker = ScanWorker(file_input, targets)
        self.run_worker(
            self._scan_task(worker, log, status),
            name="scan-worker",
            thread=True,
        )

    async def _scan_task(self, worker, log, status):
        def progress(msg):
            self.app.call_from_thread(status.update, f"Status: [yellow]{msg}[/]")

        def result_cb(found):
            self.app.call_from_thread(self._show_results, found, log, status)

        worker.run(progress, lambda r: None, result_cb)

    def _show_results(self, found, log, status):
        if not found:
            log.write("[red]No matching blocks found.[/]")
            status.update("Status: [red]No results[/]")
            return

        total = 0
        for block, coords in sorted(found.items()):
            log.write(f"\n[bold cyan]{block}[/]  ([white]{len(coords)}[/])")
            table = RichTable(show_header=True, header_style="bold")
            table.add_column("#", style="dim")
            table.add_column("X")
            table.add_column("Y")
            table.add_column("Z")
            for i, (x, y, z) in enumerate(coords[:500], 1):
                table.add_row(str(i), str(x), str(y), str(z))
            log.write(table)
            if len(coords) > 500:
                log.write(f"  ... and {len(coords) - 500} more")
            total += len(coords)

        log.write(f"\n[bold green]TOTAL FOUND: {total}[/]")
        status.update(f"Status: [green]Done — {total} blocks found[/]")


class SGAApp(App):
    TITLE = "SGA Scanner"
    SCREENS = {"main": MainScreen}

    CSS = """
    $accent: #666666;
    $primary: #666666;
    $secondary: #555555;
    $success: #555555;
    $warning: #555555;
    $error: #666666;

    Screen {
        background: #151515;
        color: #cccccc;
    }
    #main-layout {
        margin: 0 1;
    }
    #file-input {
        margin: 0 0 1 0;
        background: #1a1a1a;
        color: #cccccc;
        border: solid #444444;
    }
    #file-input:focus {
        border: solid #666666;
    }
    #preset-radio {
        margin: 0 0 1 0;
        background: #1a1a1a;
        border: solid #444444;
    }
    #preset-radio RadioButton {
        padding: 0 2 0 0;
        color: #cccccc;
    }
    #preset-radio RadioButton.-on {
        color: #cccccc;
    }
    RadioSet:focus {
        border: solid #666666;
    }
    #button-row {
        margin: 0 0 1 0;
    }
    Button {
        background: #2a2a2a;
        color: #cccccc;
        border: solid #444444;
    }
    Button:hover {
        background: #3a3a3a;
    }
    Button:focus {
        background: #444444;
        border: solid #666666;
    }
    #results-area {
        height: 1fr;
        min-height: 3;
    }
    #results-header {
        margin: 1 0 0 0;
        text-style: bold;
        color: #888888;
    }
    #status-bar {
        background: #1a1a1a;
        padding: 0 1;
        color: #aaaaaa;
    }
    Rule {
        margin: 1 0;
        color: #444444;
        background: #444444;
        height: 1;
    }
    Header {
        background: #1a1a1a;
        color: #cccccc;
        dock: top;
    }
    Header .header--title {
        color: #cccccc;
    }
    Footer {
        background: #1a1a1a;
        color: #888888;
        dock: bottom;
    }
    Footer > FooterItem {
        color: #888888;
    }
    Footer > FooterItem:hover {
        background: #333333;
    }
    .-textual-focused {
        background-tint: #333333;
    }
    Scrollbar {
        background: #1a1a1a;
    }
    ScrollbarThumb {
        background: #444444;
    }
    ScrollbarThumb:hover {
        background: #555555;
    }
    RichLog {
        background: #1a1a1a;
        color: #cccccc;
    }
    """

    def on_mount(self):
        self.dark = True
        self.push_screen("main")


if __name__ == "__main__":
    app = SGAApp()
    app.run()
