#!/usr/bin/env python3
"""
ESP32-C5 ITS-G5 / 802.11p Python client.

Connects to the board over a serial port, displays health (heartbeat)
information, saves every received 802.11 frame to a file, and
replays ITS payloads on demand.

Usage:
    # Monitor a board and record received frames
    uv run python -m client --port COM6

    # Replay ITS payloads through one board
    uv run python -m client --port COM6 --send-pcap frames.pcap --interval 100

    # Specify a custom output path
    uv run python -m client --port COM6 --pcap captures.pcap
"""

from __future__ import annotations

import argparse
import time
import sys
import threading
from datetime import datetime
from decimal import Decimal
from pathlib import Path

try:
    from scapy.data import DLT_IEEE802_11
    from scapy.utils import PcapNgWriter, rdpcap
except ImportError:
    print("Error: Could not import Scapy.")
    print("Please run: uv add scapy")
    sys.exit(1)

import serial

from .protocol import SerialProtocol
from . import interface_pb2 as pb


# ---------------------------------------------------------------------------
# Writer
# ---------------------------------------------------------------------------
def _unique_output_path(output_path: Path) -> Path:
    if not output_path.exists():
        return output_path
    counter = 1
    while True:
        candidate = output_path.with_name(
            f"{output_path.stem}_{counter}{output_path.suffix}"
        )
        if not candidate.exists():
            return candidate
        counter += 1


class ItsPcapNgWriter:
    """Writes received 802.11 frames to a capture file.

    The actual pcapng file is only created when the first frame is written,
    so no empty capture files are left behind.
    """

    def __init__(self, output_path: Path):
        self.output_path = _unique_output_path(output_path)
        self.frames_written = 0
        self._writer: PcapNgWriter | None = None

    def write(self, frame_data: bytes, timestamp_us: int) -> None:
        if self._writer is None:
            self._writer = PcapNgWriter(str(self.output_path))
            self._writer.linktype = DLT_IEEE802_11
            self._writer.write_header(None)
            print(f"Recording frames to {self.output_path}")
        timestamp_sec = Decimal(timestamp_us) / Decimal(1_000_000)
        self._writer.write_packet(frame_data, sec=timestamp_sec)
        self.frames_written += 1
        self._writer.flush()

    def close(self) -> None:
        if self._writer is not None:
            self._writer.close()
        print(f"\nCapture: {self.frames_written} frames written", end="")
        if self.frames_written > 0:
            print(f" to {self.output_path}")
        else:
            print()


# ---------------------------------------------------------------------------
# Reader / frame extractor
# ---------------------------------------------------------------------------
def extract_80211_frames(pcap_path: str) -> list[bytes]:
    """Read a capture file and return the complete 802.11 frames as raw bytes.

    The frames are sent as-is to the board, which only overrides the
    802.11 Sequence Control field (bytes 22-23) before transmitting.
    """
    pkts = rdpcap(pcap_path)
    frames: list[bytes] = []

    for i, pkt in enumerate(pkts):
        raw = bytes(pkt)
        if not raw:
            continue
        frames.append(raw)
        print(f"  Packet {i}: {len(raw)} B complete 802.11 frame")

    return frames


# ---------------------------------------------------------------------------
# Callbacks
# ---------------------------------------------------------------------------
def _make_heartbeat_callback() -> object:
    """Return a callback that prints heartbeat info and stores the last one."""
    lock = threading.Lock()
    last_hb = None

    def on_heartbeat(hb: pb.Heartbeat) -> None:
        nonlocal last_hb
        mac = ":".join(f"{b:02X}" for b in hb.src_mac)
        ver = hb.version
        hours, remainder = divmod(hb.uptime_s, 3600)
        minutes, seconds = divmod(remainder, 60)
        uptime_str = f"{int(hours)}h {int(minutes)}m {int(seconds)}s"
        with lock:
            last_hb = hb
        print(
            f"HB [#{hb.hb_seq}] v{ver}  uptime={uptime_str}  "
            f"heap={hb.free_heap / 1024:.0f}K  "
            f"min_heap={hb.min_free_heap / 1024:.0f}K  "
            f"TX={hb.tx_count}  RX={hb.rx_count}  dropped={hb.rx_dropped}  "
            f"MAC={mac}"
        )

    return on_heartbeat


def _make_frame_callback(pcap_writer: ItsPcapNgWriter) -> object:
    """Return a callback that writes received frames to the capture file."""

    def on_received_frame(rf: pb.ReceivedFrame) -> None:
        mac = ":".join(f"{b:02X}" for b in rf.src_mac)
        print(f"RX frame: len={len(rf.frame.frame_data)}  rssi={rf.rssi}dBm  src={mac}")
        pcap_writer.write(rf.frame.frame_data, rf.timestamp_us)

    return on_received_frame


# ---------------------------------------------------------------------------
# Send loop: replay complete 802.11 frames from a capture file
# ---------------------------------------------------------------------------
def _replay_loop(proto: SerialProtocol, frames: list[bytes], interval_ms: int) -> None:
    """Send each complete 802.11 frame to the board with the given interval.
    The board only overrides the Sequence Control field (bytes 22-23)."""
    total = len(frames)
    print(f"\nReplaying {total} frame(s) with {interval_ms} ms interval ...")
    for idx, frame in enumerate(frames):
        if len(frame) > 2304:
            print(f"  Frame {idx} too large ({len(frame)} B, max 2304), skipping")
            continue
        if len(frame) < 26:
            print(f"  Frame {idx} too short ({len(frame)} B, need >=26), skipping")
            continue
        proto.send_to_board(frame)
        print(f"  [{idx + 1}/{total}] Sent {len(frame)} B 802.11 frame")
        if idx + 1 < total and interval_ms > 0:
            time.sleep(interval_ms / 1000.0)
    print("  All frames sent.")


# ---------------------------------------------------------------------------
# Log callback
# ---------------------------------------------------------------------------
def _on_log(text: str) -> None:
    for line in text.strip().split("\n"):
        line = line.strip()
        if line:
            print(f"{line}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main() -> None:
    parser = argparse.ArgumentParser(
        description="ESP32-C5 ITS-G5 / 802.11p Python client"
    )
    parser.add_argument(
        "--port",
        "-p",
        default="COM6",
        help="Serial port (default: COM6)",
    )
    parser.add_argument(
        "--baud",
        "-b",
        type=int,
        default=115200,
        help="Baud rate (default: 115200)",
    )
    parser.add_argument(
        "--pcap",
        type=str,
        default=None,
        help="Output path for received frames (default: auto-generated)",
    )
    parser.add_argument(
        "--send-pcap",
        type=str,
        default=None,
        help="File with ITS frames to replay (strips 802.11 + LLC/SNAP)",
    )
    parser.add_argument(
        "--interval",
        type=int,
        default=1000,
        help="Delay between replayed frames in ms (default: 1000)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=10,
        help="Serial read timeout in seconds (default: 10)",
    )
    parser.add_argument(
        "--request-hb",
        action="store_true",
        help="Request a single heartbeat from the board and exit",
    )
    args = parser.parse_args()

    # Load frames from send-pcap if provided
    send_frames: list[bytes] = []
    if args.send_pcap:
        sp = Path(args.send_pcap)
        if not sp.exists():
            print(f"Send pcap not found: {sp}", file=sys.stderr)
            sys.exit(1)
        print(f"Reading frames from {sp} ...")
        send_frames = extract_80211_frames(str(sp))
        if not send_frames:
            print("No 802.11 frames found in capture file", file=sys.stderr)
            sys.exit(1)

    # Output pcap path for captured frames
    if args.pcap:
        pcap_path = Path(args.pcap)
    else:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        pcap_path = Path(f"cits_capture_{timestamp}.pcapng")

    pcap_writer = ItsPcapNgWriter(pcap_path)

    # Connect
    print(f"Connecting to {args.port} at {args.baud} baud...")
    proto = SerialProtocol(
        port=args.port,
        baud=args.baud,
        timeout=args.timeout,
        on_heartbeat=_make_heartbeat_callback(),
        on_received_frame=_make_frame_callback(pcap_writer),
        on_log=_on_log,
    )

    try:
        proto.start()
        print("Connected.")

        if args.request_hb:
            # One-shot heartbeat request
            print("Requesting heartbeat...")
            time.sleep(1)  # let reader thread settle
            proto.request_heartbeat()
            time.sleep(2)  # wait for response
            print("Done.")
        elif send_frames:
            # Replay mode: send all frames and exit
            time.sleep(2)  # brief settle time
            _replay_loop(proto, send_frames, args.interval)
            print("\nReplay finished. Waiting for incoming frames ...")
            while True:
                time.sleep(1)
        else:
            # Monitor-only mode: stay open to receive
            print("Monitoring. Press Ctrl+C to stop.")
            while True:
                time.sleep(1)

    except serial.SerialException as e:
        print(f"Serial error: {e}", file=sys.stderr)
        sys.exit(1)
    except serial.SerialException as e:
        print(f"Serial error: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        proto.stop()
        pcap_writer.close()


if __name__ == "__main__":
    main()
