"""MAVLink transport shared by the gimbal and camera components.

One ``MavlinkLink`` per port (serial, or UDP for PX4 SITL). Its reader thread
frames raw bytes itself so the 200 Hz ``HIL_ACTUATOR_CONTROLS`` stream PX4 puts
on every fast link while HIL is on is dropped by message ID before pymavlink
decodes it, then dispatches the rest to the registered components by
``target_system``/``target_component`` (0 = broadcast). Each component packs
with its own pymavlink encoder, so it has its own sysid/compid and sequence
numbers; writes are serialised by a lock.

URLs:
  serial:/dev/ttyUSB0:921600   (or just /dev/ttyUSB0:921600)
  udpin:0.0.0.0:13280          bind; reply to whoever sent last (PX4 SITL)
  udpout:127.0.0.1:13030       send to a fixed peer; replies come back to us
"""

from __future__ import annotations

import logging
import os
import socket
import threading
import time
from typing import Iterable, Protocol

os.environ.setdefault("MAVLINK20", "1")
from pymavlink.dialects.v20 import common as mavlink  # noqa: E402

log = logging.getLogger(__name__)

MAV_COMP_ID_ALL = 0


class Transport(Protocol):
    def read(self, timeout: float) -> bytes: ...
    def write(self, data: bytes) -> None: ...
    def close(self) -> None: ...


class SerialTransport:
    def __init__(self, device: str, baud: int):
        import serial  # pyserial

        self.port = serial.Serial(device, baudrate=baud, timeout=0.05, write_timeout=0.5)
        self.desc = f"serial:{device}:{baud}"

    def read(self, timeout: float) -> bytes:
        n = self.port.in_waiting
        return self.port.read(n if n > 0 else 1)

    def write(self, data: bytes) -> None:
        self.port.write(data)

    def close(self) -> None:
        self.port.close()


class UdpTransport:
    """udpin: bind and answer the last peer; udpout: fixed peer."""

    def __init__(self, mode: str, host: str, port: int, local_port: int = 0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.mode = mode
        self.peer: tuple[str, int] | None = None
        if mode == "udpin":
            self.sock.bind((host, port))
        else:
            self.sock.bind(("0.0.0.0", local_port))
            self.peer = (host, port)
        self.desc = f"{mode}:{host}:{port}"

    @property
    def local_port(self) -> int:
        return self.sock.getsockname()[1]

    def read(self, timeout: float) -> bytes:
        self.sock.settimeout(timeout)
        try:
            data, addr = self.sock.recvfrom(65535)
        except (TimeoutError, socket.timeout):
            return b""
        if self.mode == "udpin":
            self.peer = addr
        return data

    def write(self, data: bytes) -> None:
        if self.peer is not None:
            try:
                self.sock.sendto(data, self.peer)
            except OSError as e:
                log.debug("udp send failed: %s", e)

    def close(self) -> None:
        self.sock.close()


def open_transport(url: str) -> Transport:
    if url.startswith(("udpin:", "udp:", "udpout:")):
        mode, rest = url.split(":", 1)
        mode = "udpin" if mode == "udp" else mode
        host, _, port = rest.rpartition(":")
        return UdpTransport(mode, host or "0.0.0.0", int(port))
    dev = url[len("serial:") :] if url.startswith("serial:") else url
    baud = 921600
    head, _, tail = dev.rpartition(":")
    if head and tail.isdigit():
        dev, baud = head, int(tail)
    return SerialTransport(dev, baud)


class Framer:
    """Split a byte stream into MAVLink v1/v2 frames without decoding them."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.garbage = 0

    def feed(self, data: bytes) -> Iterable[tuple[int, bytes]]:
        self.buf += data
        out = []
        b = self.buf
        i = 0
        n = len(b)
        while i < n:
            stx = b[i]
            if stx == 0xFD:
                if n - i < 10:
                    break
                plen = b[i + 1]
                flen = 12 + plen + (13 if b[i + 2] & 0x01 else 0)
                if n - i < flen:
                    break
                msgid = b[i + 7] | (b[i + 8] << 8) | (b[i + 9] << 16)
                out.append((msgid, bytes(b[i : i + flen])))
                i += flen
            elif stx == 0xFE:
                if n - i < 6:
                    break
                flen = 8 + b[i + 1]
                if n - i < flen:
                    break
                out.append((b[i + 5], bytes(b[i : i + flen])))
                i += flen
            else:
                i += 1
                self.garbage += 1
        del self.buf[:i]
        return out


class Component:
    """Base for a MAVLink component living on a link."""

    def __init__(self, sysid: int, compid: int):
        self.sysid = sysid
        self.compid = compid
        self.encoder = mavlink.MAVLink(None, srcSystem=sysid, srcComponent=compid)
        self.link: MavlinkLink | None = None
        self._t0 = time.monotonic()

    def time_boot_ms(self) -> int:
        return int((time.monotonic() - self._t0) * 1000) & 0xFFFFFFFF

    def accepts(self, msg) -> bool:
        ts = getattr(msg, "target_system", None)
        tc = getattr(msg, "target_component", None)
        if ts is not None and ts not in (0, self.sysid):
            return False
        if tc is not None and tc not in (MAV_COMP_ID_ALL, self.compid):
            return False
        return True

    def send(self, msg) -> None:
        if self.link is not None:
            self.link.send(self, msg)

    def handle(self, msg) -> None:  # pragma: no cover - interface
        pass


class MavlinkLink:
    def __init__(self, name: str, transport: Transport, drop_msg_ids: Iterable[int] = (93,)):
        self.name = name
        self.transport = transport
        self.drop = set(drop_msg_ids)
        self.components: list[Component] = []
        self._wlock = threading.Lock()
        self._parser = mavlink.MAVLink(None)
        self._parser.robust_parsing = True
        self._framer = Framer()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name=f"mav-{name}", daemon=True)
        self.dropped = 0
        self.received = 0
        self.bad = 0
        self.last_rx = 0.0

    def register(self, comp: Component) -> None:
        comp.link = self
        self.components.append(comp)

    def send(self, comp: Component, msg) -> None:
        with self._wlock:
            buf = msg.pack(comp.encoder)
            comp.encoder.seq = (comp.encoder.seq + 1) % 256
            try:
                self.transport.write(buf)
            except Exception as e:  # serial timeouts etc.
                log.warning("link %s: write failed: %s", self.name, e)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        try:
            self.transport.close()
        except Exception:
            pass

    def _own(self, sysid: int, compid: int) -> bool:
        return any(c.sysid == sysid and c.compid == compid for c in self.components)

    def feed(self, data: bytes) -> None:
        """Process received bytes (the reader thread; tests may call it directly)."""
        for msgid, frame in self._framer.feed(data):
            if msgid in self.drop:
                self.dropped += 1
                continue
            try:
                msgs = self._parser.parse_buffer(frame) or []
            except Exception as e:
                self.bad += 1
                log.debug("link %s: parse error %s", self.name, e)
                continue
            for msg in msgs:
                if msg.get_type() == "BAD_DATA":
                    self.bad += 1
                    continue
                self.received += 1
                self.last_rx = time.monotonic()
                if self._own(msg.get_srcSystem(), msg.get_srcComponent()):
                    continue
                for c in self.components:
                    if c.accepts(msg):
                        try:
                            c.handle(msg)
                        except Exception:
                            log.exception("link %s: %s failed on %s", self.name, type(c).__name__, msg.get_type())

    def _run(self) -> None:
        log.info("link %s: open on %s", self.name, getattr(self.transport, "desc", "?"))
        while not self._stop.is_set():
            try:
                data = self.transport.read(0.1)
            except Exception as e:
                log.error("link %s: read failed: %s", self.name, e)
                time.sleep(0.5)
                continue
            if data:
                self.feed(data)


def payload_bytes(msg, size: int) -> bytes:
    """A decoded message's raw payload, zero-padded (MAVLink 2 trims trailing zeros).

    Taken from the frame itself: pymavlink's get_payload() assumes the
    MAVLink 1 header length on MAVLink 2 frames."""
    buf = bytes(msg.get_msgbuf() or b"")
    if buf[:1] == b"\xfd":
        p = buf[10 : 10 + buf[1]]
    elif buf[:1] == b"\xfe":
        p = buf[6 : 6 + buf[1]]
    else:
        p = b""
    return p.ljust(size, b"\0")[:size]


def encode_str(s: str, n: int) -> bytes:
    return s.encode("utf-8")[:n]


def u8_array(s: str, n: int = 32) -> list[int]:
    return list(s.encode("utf-8")[:n].ljust(n, b"\0"))


def version_u32(v: str | int) -> int:
    """'major.minor.patch.dev' -> MAVLink firmware_version (dev<<24|patch<<16|minor<<8|major)."""
    if isinstance(v, int):
        return v
    parts = [int(x) for x in str(v).split(".")] + [0, 0, 0, 0]
    major, minor, patch, dev = parts[:4]
    return ((dev & 0xFF) << 24) | ((patch & 0xFF) << 16) | ((minor & 0xFF) << 8) | (major & 0xFF)
