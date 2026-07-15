"""
Serial protocol for communicating with the ESP32-C5 ITS-G5 board.

Frame format (bidirectional):
  0xBE 0xEF  [uint32 LE length]  [protobuf bytes]

Board -> PC : FromBoard (heartbeat | received_frame)
PC   -> Board: ToBoard   (payload to transmit)
"""

from __future__ import annotations

import struct
import threading
from queue import Queue
from typing import Callable, Optional

import serial

from . import interface_pb2 as pb

MAGIC_1 = 0xBE
MAGIC_2 = 0xEF
MAX_FRAME_SIZE = 4096


class SerialProtocol:
    """Reads framed protobuf messages from a serial port and provides a
    thread-safe send method."""

    def __init__(
        self,
        port: str,
        baud: int = 115200,
        timeout: float = 0.1,
        on_heartbeat: Optional[Callable[[pb.Heartbeat], None]] = None,
        on_received_frame: Optional[Callable[[pb.ReceivedFrame], None]] = None,
        on_log: Optional[Callable[[str], None]] = None,
    ):
        self.port = port
        self.baud = baud
        self._ser = serial.Serial(port, baud, timeout=timeout)
        self._lock = threading.Lock()
        self._running = False
        self._reader_thread: Optional[threading.Thread] = None
        self._buf = bytearray()

        self.on_heartbeat = on_heartbeat
        self.on_received_frame = on_received_frame
        self.on_log = on_log

    # -- public API ------------------------------------------------------

    def start(self) -> None:
        """Start the background reader thread."""
        if self._running:
            return
        self._running = True
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()

    def stop(self) -> None:
        """Stop the reader thread and close the serial port."""
        self._running = False
        if self._reader_thread:
            self._reader_thread.join(timeout=2)
        with self._lock:
            self._ser.close()

    def send_to_board(self, frame_bytes: bytes) -> None:
        """Send a complete 802.11 frame to the board for transmission."""
        msg = pb.ToBoard()
        msg.frame.frame_data = frame_bytes
        self._send_protobuf(msg)

    def request_heartbeat(self) -> None:
        """Request an immediate Heartbeat from the board (no waiting for period)."""
        msg = pb.ToBoard()
        msg.request_hb = True
        self._send_protobuf(msg)

    # -- internal --------------------------------------------------------

    def _send_protobuf(self, msg) -> None:
        """Encode *msg* and write it framed to the serial port."""
        data = msg.SerializeToString()
        frame = bytearray()
        frame.append(MAGIC_1)
        frame.append(MAGIC_2)
        frame.extend(struct.pack("<I", len(data)))
        frame.extend(data)
        with self._lock:
            self._ser.write(frame)

    def _reader_loop(self) -> None:
        """Background loop: read raw bytes, find frames, dispatch."""
        while self._running:
            try:
                chunk = self._ser.read(1024)
            except serial.SerialException:
                break
            if not chunk:
                continue
            self._buf.extend(chunk)
            self._process_buffer()

    def _process_buffer(self) -> None:
        """Parse as many complete frames as possible from the buffer."""
        while True:
            # Find magic marker
            try:
                idx = self._buf.index(MAGIC_1)
            except ValueError:
                self._buf.clear()
                return

            if idx > 0:
                # Emit text before the marker as a log line
                text = self._buf[:idx].decode("utf-8", errors="replace")
                if text.strip() and self.on_log:
                    self.on_log(text)
                self._buf = self._buf[idx:]

            if len(self._buf) < 6:  # magic(2) + length(4)
                return

            if self._buf[1] != MAGIC_2:
                # False sync - skip first byte and retry
                self._buf.pop(0)
                continue

            # Read length
            length = struct.unpack_from("<I", self._buf, 2)[0]
            if length == 0 or length > MAX_FRAME_SIZE:
                # Invalid length, skip the two magic bytes
                self._buf = self._buf[2:]
                continue

            frame_end = 6 + length
            if len(self._buf) < frame_end:
                return  # wait for more data

            pb_data = self._buf[6:frame_end]
            self._buf = self._buf[frame_end:]

            self._dispatch(pb_data)

    def _dispatch(self, pb_data: bytes) -> None:
        """Decode a FromBoard protobuf and call the appropriate callback."""
        msg = pb.FromBoard()
        msg.ParseFromString(pb_data)

        which = msg.WhichOneof("msg")
        if which == "heartbeat" and self.on_heartbeat:
            self.on_heartbeat(msg.heartbeat)
        elif which == "received_frame" and self.on_received_frame:
            self.on_received_frame(msg.received_frame)
