#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Drive the UNA Watch tether stub, or PebbleOS's USB developer access, over USB or TCP.

The stub (tools/una/tether/stub) runs from 0x08060000 and answers framed
commands: memory and registers, I2C, GPIO, running blobs loaded into SRAM,
and flashing and booting a PebbleOS image in the slot at 0x08080000.
PebbleOS with its USB developer access on answers the same protocol on the
second of its two serial ports, but for calls and flashing: `flash` resets it
to the stub first. Standard library only. See docs/boards/unawatch/tether.md.

    tether.py info
    tether.py peek 0x46020c00 4
    tether.py run build/experiments/max17262.bin
    tether.py --tcp mac.local:4500 dump 0x20000000 256
"""

import argparse
import glob
import os
import re
import select
import socket
import struct
import sys
import time
import zlib

# Protocol: stub/tether_proto.h; test_tether.py checks these against it.
MAGIC = b"\xa5\x5a"
PROTO_VERSION = 1
HDR_LEN = 6
CRC_LEN = 4
MAX_DATA = 4096
MAX_PAYLOAD = MAX_DATA + 64
REPLY = 0x80

CMD_INFO = 0x01
CMD_READ = 0x02
CMD_WRITE = 0x03
CMD_FILL = 0x04
CMD_CRC32 = 0x05
CMD_CALL = 0x06
CMD_I2C = 0x07
CMD_GPIO = 0x08
CMD_RESET = 0x09
CMD_FLASH_ERASE = 0x0A
CMD_FLASH_WRITE = 0x0B
CMD_BOOT = 0x0C
CMD_NOTE = 0x0D
CMD_CLEAR_FAULT = 0x0E
CMD_BAD_FRAME = 0x7F

OK = 0
ERR_CRC = 1
ERR_CMD = 2
ERR_ARG = 3
ERR_FAULT = 4
ERR_TIMEOUT = 5
ERR_PROTECTED = 6
ERR_NACK = 7
ERR_BUS = 8
ERR_FLASH = 9
ERR_LENGTH = 10
ERR_UNSUPPORTED = 11
STATUS_NAMES = {
    OK: "ok",
    ERR_CRC: "bad CRC",
    ERR_CMD: "unknown command",
    ERR_ARG: "bad argument",
    ERR_FAULT: "fault",
    ERR_TIMEOUT: "timeout",
    ERR_PROTECTED: "protected",
    ERR_NACK: "NACK",
    ERR_BUS: "bus error",
    ERR_FLASH: "flash error",
    ERR_LENGTH: "too long",
    ERR_UNSUPPORTED: "unsupported",
}

CALL_BLOB = 1 << 0
CALL_ANYWHERE = 1 << 1
CALL_KEEP_OUT = 1 << 2
CALL_MAX_ARGS = 12

I2C_UNSAFE = 1 << 0
I2C_400K = 1 << 1
I2C_MAX = 255

GPIO_READ = 0
GPIO_OUT = 1
GPIO_IN = 2
GPIO_OUT_OD = 3
GPIO_ANALOG = 4

RESET_SYSTEM = 0
RESET_POWER_OFF = 1

STUB_FLASH = 0x08060000
SLOT_BASE = 0x08080000
STUB_RAM = 0x20000000
STUB_RAM_SIZE = 0x20000
LOAD_BASE = 0x20020000
LOAD_END = 0x2024E000
BLOB_STACK_TOP = 0x2025E000
FLASH_PROG_ALIGN = 16
FLASH_PAGE = 0x2000

BLOB_MAGIC = 0x424C4254
FAULT_MAGIC = 0x544C4146
FAULT_TIMEOUT = 0x200

USB_VID = 0x1209
USB_PID = 0x0001
# PebbleOS's developer access: the console on interface 0, the tether protocol on interface 2
USB_PID_PEBBLEOS = 0x0002
PEBBLEOS_CONSOLE_IFACE = 0
PEBBLEOS_TETHER_IFACE = 2

FAULT_FIELDS = (
    "magic exc exc_return cfsr hfsr mmfar bfar afsr r0 r1 r2 r3 r12 lr pc xpsr sp "
    "r4 r5 r6 r7 r8 r9 r10 r11 uptime_ms count"
).split()
FAULT_FMT = "<" + "I" * len(FAULT_FIELDS)
FAULT_SIZE = struct.calcsize(FAULT_FMT)

INFO_FMT = "<4sHH32s" + "III" + "3I" + "IIII" + "BBBB" + "I" + "III" + "III" + "I" + "II" + "I" + "II" + "I"
INFO_FIELDS = (
    "magic proto_version info_size version reset_csr sysclk_hz hclk_hz uid0 uid1 uid2 flash_kb "
    "idcode optr uptime_ms usb_speed usb_state buttons usb_error flags load_base load_end "
    "blob_stack_top out_buf out_size out_len api slot_base slot_end max_data cmd_count err_count "
    "bkp3r"
).split()
INFO_SIZE = struct.calcsize(INFO_FMT) + 2 * FAULT_SIZE

CALL_RESULT_FMT = "<6I"
CALL_RESULT_SIZE = struct.calcsize(CALL_RESULT_FMT)

BLOB_HEADER_FMT = "<6I24s"
BLOB_HEADER_SIZE = struct.calcsize(BLOB_HEADER_FMT)

INFO_FLAGS = ["pll", "hse", "lse", "display", "boot-fault", "slot-valid", "usb-fs-forced", "flash-ok",
              "pebbleos"]
INFO_PEBBLEOS = 1 << 8
USB_STATES = ["off", "waiting", "reset", "configured", "open", "suspended", "error"]
RESET_FLAGS = {25: "OBL", 26: "PIN", 27: "BOR", 28: "SFT", 29: "IWDG", 30: "WWDG", 31: "LPWR"}
EXC_NAMES = {2: "NMI", 3: "HardFault", 4: "MemManage", 5: "BusFault", 6: "UsageFault",
             7: "SecureFault", FAULT_TIMEOUT: "timeout"}
CFSR_BITS = {
    0: "IACCVIOL", 1: "DACCVIOL", 3: "MUNSTKERR", 4: "MSTKERR", 5: "MLSPERR", 7: "MMARVALID",
    8: "IBUSERR", 9: "PRECISERR", 10: "IMPRECISERR", 11: "UNSTKERR", 12: "STKERR", 13: "LSPERR",
    15: "BFARVALID", 16: "UNDEFINSTR", 17: "INVSTATE", 18: "INVPC", 19: "NOCP", 20: "STKOF",
    24: "UNALIGNED", 25: "DIVBYZERO",
}

crc32 = zlib.crc32


class TetherError(Exception):
    def __init__(self, status, message="", fault=None):
        self.status = status
        self.fault = fault
        text = STATUS_NAMES.get(status, f"status {status}")
        if message:
            text = f"{message}: {text}"
        if fault is not None:
            text += "\n" + format_fault(fault)
        super().__init__(text)


def parse_fault(data):
    if len(data) < FAULT_SIZE:
        return None
    fault = dict(zip(FAULT_FIELDS, struct.unpack_from(FAULT_FMT, data)))
    return fault if fault["magic"] == FAULT_MAGIC else None


def format_fault(f):
    exc = f["exc"]
    if exc >= 0x100 and exc != FAULT_TIMEOUT:
        name = f"unexpected IRQ {exc - 0x100}"
    else:
        name = EXC_NAMES.get(exc, f"exception {exc}")
    lines = [f"{name} at pc {f['pc']:#010x}, lr {f['lr']:#010x}, sp {f['sp']:#010x} (#{f['count']}, "
             f"{f['uptime_ms']} ms)"]
    if exc != FAULT_TIMEOUT and exc < 0x100:
        bits = [n for b, n in CFSR_BITS.items() if f["cfsr"] & (1 << b)]
        lines.append(f"  cfsr {f['cfsr']:#010x} {' '.join(bits)}  hfsr {f['hfsr']:#010x}")
        if f["cfsr"] & (1 << 15):
            lines.append(f"  bfar {f['bfar']:#010x}")
        if f["cfsr"] & (1 << 7):
            lines.append(f"  mmfar {f['mmfar']:#010x}")
    regs = " ".join(f"{r} {f[r]:08x}" for r in ("r0", "r1", "r2", "r3", "r12", "xpsr"))
    lines.append("  " + regs)
    lines.append("  " + " ".join(f"{r} {f[r]:08x}" for r in ("r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11")))
    return "\n".join(lines)


def encode_frame(cmd, seq, payload=b""):
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload of {len(payload)} bytes is over {MAX_PAYLOAD}")
    body = struct.pack("<BBH", cmd, seq & 0xFF, len(payload)) + payload
    return MAGIC + body + struct.pack("<I", crc32(body))


# Transports


def _raw_tty(fd):
    import termios

    attrs = termios.tcgetattr(fd)
    attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
                  termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON | termios.IXOFF)
    attrs[1] &= ~termios.OPOST
    attrs[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
    attrs[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    attrs[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    attrs[4] = attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)


class SerialTransport:
    """A CDC-ACM port (or any tty), raw, through termios."""

    def __init__(self, path):
        self.name = path
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            _raw_tty(self.fd)
        except Exception:
            os.close(self.fd)
            raise

    def write(self, data):
        view = memoryview(data)
        deadline = time.monotonic() + 5.0
        while view:
            _, w, _ = select.select([], [self.fd], [], 0.5)
            if w:
                try:
                    n = os.write(self.fd, view)
                except BlockingIOError:
                    n = 0
                view = view[n:]
            if time.monotonic() > deadline:
                raise TimeoutError(f"{self.name}: write timed out")

    def read(self, n, timeout):
        r, _, _ = select.select([self.fd], [], [], max(0.0, timeout))
        if not r:
            return b""
        try:
            data = os.read(self.fd, n)
        except BlockingIOError:
            return b""
        if not data:
            raise ConnectionError(f"{self.name}: device gone")
        return data

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


class TcpTransport:
    """The port of a watch plugged into another machine, through bridge.py or socat."""

    def __init__(self, address):
        host, _, port = address.rpartition(":")
        if not host or not port:
            raise ValueError(f"{address}: expected HOST:PORT")
        self.name = address
        self.sock = socket.create_connection((host.strip("[]"), int(port)), timeout=5.0)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def write(self, data):
        self.sock.settimeout(5.0)
        self.sock.sendall(data)

    def read(self, n, timeout):
        self.sock.settimeout(max(0.001, timeout))
        try:
            data = self.sock.recv(n)
        except socket.timeout:
            return b""
        if not data:
            raise ConnectionError(f"{self.name}: connection closed")
        return data

    def close(self):
        self.sock.close()


class PipeTransport:
    """A pair of file descriptors, for the simulator."""

    def __init__(self, rfd, wfd, name="pipe"):
        self.rfd, self.wfd, self.name = rfd, wfd, name

    def write(self, data):
        view = memoryview(data)
        while view:
            n = os.write(self.wfd, view)
            view = view[n:]

    def read(self, n, timeout):
        r, _, _ = select.select([self.rfd], [], [], max(0.0, timeout))
        if not r:
            return b""
        data = os.read(self.rfd, n)
        if not data:
            raise ConnectionError(f"{self.name}: closed")
        return data

    def close(self):
        pass


def _read(path):
    with open(path) as f:
        return f.read()


def _linux_ports():
    """(path, vid, pid, interface) of each CDC-ACM port, from sysfs."""
    found = []
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):
        dev = os.path.realpath(os.path.join(tty, "device"))
        try:
            iface = int(_read(os.path.join(dev, "bInterfaceNumber")), 16)
        except (OSError, ValueError):
            iface = None
        for d in (os.path.dirname(dev), os.path.dirname(os.path.dirname(dev))):
            try:
                vid = int(_read(os.path.join(d, "idVendor")), 16)
                pid = int(_read(os.path.join(d, "idProduct")), 16)
            except (OSError, ValueError):
                continue
            found.append(("/dev/" + os.path.basename(tty), vid, pid, iface))
            break
    return found


def _macos_ports():
    """(path, vid, pid, interface) of each USB serial port, from ioreg's tree of USB devices."""
    import subprocess

    try:
        out = subprocess.run(["ioreg", "-r", "-l", "-w0", "-c", "IOUSBHostDevice"],
                             capture_output=True, text=True, timeout=5, check=False).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    # Each interface repeats the device's IDs after its bInterfaceNumber, so only a new device
    # node starts afresh. The callout device hangs off a CDC data interface, the one after its
    # control interface, which classify() takes for its function.
    found, vid, pid, iface = [], None, None, None
    for line in out.splitlines():
        if "<class IOUSBHostDevice" in line:
            vid, pid, iface = None, None, None
            continue
        m = re.search(r'"(idVendor|idProduct|bInterfaceNumber|IOCalloutDevice)" = (\S+)', line)
        if not m:
            continue
        key, value = m.group(1), m.group(2).strip('"')
        if key == "idVendor":
            vid = int(value)
        elif key == "idProduct":
            pid = int(value)
        elif key == "bInterfaceNumber":
            iface = int(value)
        elif value.startswith("/dev/cu."):
            found.append((value, vid, pid, iface))
    return found


def classify(vid, pid, iface):
    """What a port is: "stub", "console" or "tether" (PebbleOS's), or None."""
    if vid != USB_VID:
        return None
    if pid == USB_PID:
        return "stub"
    if pid == USB_PID_PEBBLEOS:
        # A CDC function's control interface, or its data interface just after it (macOS)
        control = None if iface is None else iface & ~1
        return {PEBBLEOS_CONSOLE_IFACE: "console", PEBBLEOS_TETHER_IFACE: "tether"}.get(control)
    return None


def find_ports(role="tether"):
    """The ports on this machine for a role: "tether", the stub's port or PebbleOS's tether port,
    whichever is there; or "console", PebbleOS's console. By USB IDs and interface on Linux and
    macOS (ioreg), by name otherwise."""
    wanted = ("stub", "tether") if role == "tether" else ("console",)
    listed = _linux_ports() if sys.platform.startswith("linux") else _macos_ports()
    found = [path for path, vid, pid, iface in listed if classify(vid, pid, iface) in wanted]
    if found or listed:
        return found
    if role == "tether":
        found = sorted(glob.glob("/dev/serial/by-id/*UNA_Watch_tether*"))
        found += sorted(glob.glob("/dev/serial/by-id/*UNA_Watch_PebbleOS*-if02"))
    else:
        found = sorted(glob.glob("/dev/serial/by-id/*UNA_Watch_PebbleOS*-if00"))
    if not found:
        found = _guess_macos_names(role)
    return found


def _guess_macos_names(role):
    """Without ioreg: one usbmodem port is the stub's; two of a device, ending in 1 and 3, are
    PebbleOS's console and tether ports, the interface number plus 1 ending their names."""
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(ports) == 1:
        return ports if role == "tether" else []
    pairs = [p for p in ports if p.endswith("1") and p[:-1] + "3" in ports]
    if len(pairs) == 1:
        return [pairs[0][:-1] + "3"] if role == "tether" else pairs
    return []


def open_transport(port=None, tcp=None):
    port = port or os.environ.get("TETHER_PORT")
    tcp = tcp or os.environ.get("TETHER_TCP")
    if tcp:
        return TcpTransport(tcp)
    if not port:
        ports = find_ports()
        if len(ports) != 1:
            raise SystemExit(
                "tether: no stub or PebbleOS tether port found" if not ports
                else f"tether: several ports, pick one with --port: {' '.join(ports)}")
        port = ports[0]
    return SerialTransport(port)


def is_pebbleos(info):
    return bool(info["flags"] & INFO_PEBBLEOS)


# The client


class Tether:
    def __init__(self, transport, timeout=3.0, verbose=False):
        self.t = transport
        self.timeout = timeout
        self.verbose = verbose
        self.seq = int(time.monotonic() * 1000) & 0xFF
        self.buf = bytearray()
        self.drain()

    def drain(self, quiet=0.05):
        """Drops what a previous session left unread."""
        self.buf.clear()
        while self.t.read(65536, quiet):
            pass

    def _fill(self, n, deadline):
        while len(self.buf) < n:
            left = deadline - time.monotonic()
            if left <= 0:
                return False
            self.buf += self.t.read(65536, left)
        return True

    def _frame(self, deadline):
        """The next well-formed frame: (cmd, seq, payload), or None at the deadline."""
        while True:
            if not self._fill(2, deadline):
                return None
            i = self.buf.find(MAGIC)
            if i < 0:
                del self.buf[: len(self.buf) - 1]
                continue
            del self.buf[:i]
            if not self._fill(HDR_LEN, deadline):
                return None
            cmd, seq, n = struct.unpack_from("<BBH", self.buf, 2)
            if n > MAX_PAYLOAD:
                del self.buf[:2]
                continue
            if not self._fill(HDR_LEN + n + CRC_LEN, deadline):
                return None
            body = bytes(self.buf[2 : HDR_LEN + n])
            (crc,) = struct.unpack_from("<I", self.buf, HDR_LEN + n)
            if crc32(body) != crc:
                del self.buf[:2]
                continue
            del self.buf[: HDR_LEN + n + CRC_LEN]
            return cmd, seq, body[4:]

    def request(self, cmd, payload=b"", timeout=None):
        """Sends a command; returns (status, data) of its reply."""
        self.seq = (self.seq + 1) & 0xFF
        seq = self.seq
        self.t.write(encode_frame(cmd, seq, payload))
        deadline = time.monotonic() + (timeout or self.timeout)
        while True:
            frame = self._frame(deadline)
            if frame is None:
                raise TimeoutError(f"no reply to command {cmd:#04x} from {self.t.name}")
            rcmd, rseq, data = frame
            if len(data) < 4:
                continue
            (status,) = struct.unpack_from("<I", data)
            if rcmd == CMD_BAD_FRAME | REPLY and rseq == seq:
                raise TetherError(status, "the stub got a damaged frame")
            if rcmd == cmd | REPLY and rseq == seq:
                if self.verbose:
                    print(f"< {cmd:#04x} seq {seq} {STATUS_NAMES.get(status, status)} "
                          f"{len(data) - 4} bytes", file=sys.stderr)
                return status, data[4:]

    def check(self, cmd, payload=b"", what="", timeout=None):
        status, data = self.request(cmd, payload, timeout)
        if status != OK:
            raise TetherError(status, what or f"command {cmd:#04x}", parse_fault(data))
        return data

    # Commands

    def info(self):
        data = self.check(CMD_INFO, what="info")
        head = struct.calcsize(INFO_FMT)
        info = dict(zip(INFO_FIELDS, struct.unpack_from(INFO_FMT, data)))
        info["version"] = info["version"].split(b"\0")[0].decode(errors="replace")
        info["boot_fault"] = parse_fault(data[head:])
        info["last_fault"] = parse_fault(data[head + FAULT_SIZE :])
        return info

    def read(self, addr, n, width=0):
        out = bytearray()
        while len(out) < n:
            k = min(MAX_DATA, n - len(out))
            a = addr + len(out)
            out += self.check(CMD_READ, struct.pack("<IIB", a, k, width), f"read {a:#010x}")
        return bytes(out)

    def read32(self, addr):
        return struct.unpack("<I", self.read(addr, 4, 4))[0]

    def write(self, addr, data, width=0):
        step = MAX_DATA - (MAX_DATA % 4)
        for off in range(0, len(data), step):
            chunk = data[off : off + step]
            self.check(CMD_WRITE, struct.pack("<IB3x", addr + off, width) + chunk,
                       f"write {addr + off:#010x}")

    def write32(self, addr, value):
        self.write(addr, struct.pack("<I", value), 4)

    def fill(self, addr, value, count, width=4):
        self.check(CMD_FILL, struct.pack("<IB3xII", addr, width, value, count), f"fill {addr:#010x}",
                   timeout=10.0)

    def crc32(self, addr, n):
        data = self.check(CMD_CRC32, struct.pack("<II", addr, n), f"crc {addr:#010x}", timeout=15.0)
        return struct.unpack("<I", data)[0]

    def call(self, addr, args=(), blob=False, anywhere=False, keep_output=False, timeout_s=5.0):
        """Calls Thumb code at addr. Returns (status, result dict, output bytes, fault)."""
        if len(args) > CALL_MAX_ARGS:
            raise ValueError(f"at most {CALL_MAX_ARGS} arguments")
        flags = ((CALL_BLOB if blob else 0) | (CALL_ANYWHERE if anywhere else 0) |
                 (CALL_KEEP_OUT if keep_output else 0))
        timeout_ms = int(timeout_s * 1000)
        payload = struct.pack("<IIII", addr | 1, flags, timeout_ms, len(args))
        payload += b"".join(struct.pack("<I", a & 0xFFFFFFFF) for a in args)
        status, data = self.request(CMD_CALL, payload, timeout=timeout_s + 3.0 if timeout_ms else 3600)
        if len(data) < CALL_RESULT_SIZE:
            raise TetherError(status, f"call {addr:#010x}")
        res = dict(zip("r0 r1 elapsed_us out_len out_addr faulted".split(),
                       struct.unpack_from(CALL_RESULT_FMT, data)))
        rest = data[CALL_RESULT_SIZE:]
        fault = None
        if res["faulted"]:
            fault = parse_fault(rest[-FAULT_SIZE:])
            rest = rest[:-FAULT_SIZE]
        out = bytes(rest)
        if len(out) < res["out_len"]:
            out += self.read(res["out_addr"] + len(out), res["out_len"] - len(out))
        return status, res, out, fault

    def i2c(self, bus, addr, write=b"", read=0, unsafe=False, fast=False):
        flags = (I2C_UNSAFE if unsafe else 0) | (I2C_400K if fast else 0)
        payload = struct.pack("<BBBxHH", bus, addr, flags, len(write), read) + bytes(write)
        return self.check(CMD_I2C, payload, f"i2c{bus} {addr:#04x}")

    def i2c_scan(self, bus):
        found = []
        for addr in range(0x08, 0x78):
            status, _ = self.request(CMD_I2C, struct.pack("<BBBxHH", bus, addr, 0, 0, 0))
            if status == OK:
                found.append(addr)
            elif status not in (ERR_NACK, ERR_TIMEOUT):
                raise TetherError(status, f"i2c{bus} scan at {addr:#04x}")
        return found

    def gpio(self, port, pin, op=GPIO_READ, arg=0):
        data = self.check(CMD_GPIO, struct.pack("<BBBB", port, pin, op, arg), "gpio")
        regs = struct.unpack_from("<8I", data)
        names = "moder otyper ospeedr pupdr idr odr afrl afrh".split()
        return dict(zip(names, regs)), data[32]

    def reset(self, mode=RESET_SYSTEM):
        self.check(CMD_RESET, struct.pack("<B", mode), "reset")

    def flash_erase(self, addr, n):
        self.check(CMD_FLASH_ERASE, struct.pack("<II", addr, n), f"erase {addr:#010x}", timeout=10.0)

    def flash_write(self, addr, data):
        self.check(CMD_FLASH_WRITE, struct.pack("<I", addr) + data, f"flash {addr:#010x}", timeout=5.0)

    def flash_image(self, image, addr=SLOT_BASE, progress=None):
        """Erases, programs and checks an image in the slot."""
        if addr < SLOT_BASE:
            raise ValueError(f"{addr:#010x} is below the slot at {SLOT_BASE:#010x}")
        image = bytes(image) + b"\xff" * (-len(image) % FLASH_PROG_ALIGN)
        span = len(image) + (-len(image) % FLASH_PAGE)
        step = 32 * FLASH_PAGE
        for off in range(0, span, step):
            self.flash_erase(addr + off, min(step, span - off))
            if progress:
                progress("erase", off + min(step, span - off), span)
        chunk = MAX_DATA
        for off in range(0, len(image), chunk):
            self.flash_write(addr + off, image[off : off + chunk])
            if progress:
                progress("write", off + len(image[off : off + chunk]), len(image))
        got = self.crc32(addr, len(image))
        if got != crc32(image):
            raise TetherError(ERR_FLASH, f"verify: CRC {got:#010x}, expected {crc32(image):#010x}")
        return len(image)

    def boot(self, addr=0):
        self.check(CMD_BOOT, struct.pack("<I", addr), "boot")

    def note(self, text):
        self.check(CMD_NOTE, text.encode()[:31], "note")

    def clear_fault(self):
        self.check(CMD_CLEAR_FAULT, b"", "clear fault")

    def load_blob(self, image):
        """Writes a blob image to its load address and checks it there; returns its header."""
        hdr = parse_blob_header(image)
        self.write(hdr["load_addr"], image)
        got = self.crc32(hdr["load_addr"], len(image))
        if got != crc32(image):
            raise TetherError(ERR_FLASH, f"blob verify: CRC {got:#010x}, expected {crc32(image):#010x}")
        return hdr

    def run_blob(self, image, args=(), timeout_s=10.0):
        hdr = self.load_blob(image)
        status, res, out, fault = self.call(hdr["entry"], args, blob=True, timeout_s=timeout_s)
        return hdr, status, res, out, fault


def parse_blob_header(image):
    if len(image) < BLOB_HEADER_SIZE:
        raise ValueError("too short for a blob")
    magic, version, load, entry, end, bss_end, name = struct.unpack_from(BLOB_HEADER_FMT, image)
    if magic != BLOB_MAGIC:
        raise ValueError("no blob header: link with experiments/blob.ld and blob_crt0.c")
    hdr = dict(version=version, load_addr=load, entry=entry, image_end=end, bss_end=bss_end,
               name=name.split(b"\0")[0].decode(errors="replace"))
    if not (LOAD_BASE <= load and bss_end <= LOAD_END and load + len(image) <= LOAD_END):
        raise ValueError(f"blob at {load:#010x}..{bss_end:#010x} is outside the load region "
                         f"{LOAD_BASE:#010x}..{LOAD_END:#010x}")
    if not (entry & 1) or not load <= (entry & ~1) < load + len(image):
        raise ValueError(f"entry {entry:#010x} is not Thumb code in the image")
    return hdr


# Command line


def num(s):
    return int(s, 0)


PORTS = "ABCDEFGHI"


def parse_pin(s):
    s = s.upper()
    if len(s) < 3 or s[0] != "P" or s[1] not in PORTS:
        raise argparse.ArgumentTypeError(f"{s}: expected a pin such as PA5 or PD15")
    pin = int(s[2:])
    if not 0 <= pin <= 15:
        raise argparse.ArgumentTypeError(f"{s}: pins go from 0 to 15")
    return PORTS.index(s[1]), pin


def hexdump(addr, data, out=sys.stdout):
    for off in range(0, len(data), 16):
        row = data[off : off + 16]
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        out.write(f"{addr + off:08x}  {row.hex(' '):<47}  {text}\n")


def cmd_info(t, args):
    i = t.info()
    flags = [n for b, n in enumerate(INFO_FLAGS) if i["flags"] & (1 << b)]
    resets = [n for b, n in RESET_FLAGS.items() if i["reset_csr"] & (1 << b)]
    speed = {1: "full speed", 2: "high speed"}.get(i["usb_speed"], "?")
    state = USB_STATES[i["usb_state"]] if i["usb_state"] < len(USB_STATES) else i["usb_state"]
    who = "firmware" if is_pebbleos(i) else "stub    "
    print(f"{who}  {i['version']} (protocol {i['proto_version']}), up {i['uptime_ms'] / 1000:.1f} s")
    print(f"chip      IDCODE {i['idcode']:#010x} (dev {i['idcode'] & 0xfff:#05x}, rev {i['idcode'] >> 16:#06x}), "
          f"{i['flash_kb']} KiB flash, OPTR {i['optr']:#010x}")
    print(f"uid       {i['uid0']:08x} {i['uid1']:08x} {i['uid2']:08x}")
    print(f"clocks    SYSCLK {i['sysclk_hz'] / 1e6:g} MHz, HCLK {i['hclk_hz'] / 1e6:g} MHz")
    print(f"reset     RCC_CSR {i['reset_csr']:#010x}: {' '.join(resets) or 'none'}, TAMP BKP3R {i['bkp3r']:#x}")
    print(f"usb       {state}, {speed}" + (f", init failed at step {i['usb_error']}" if i["usb_error"] else ""))
    print(f"flags     {' '.join(flags)}")
    print(f"buttons   {i['buttons']:#x}")
    if is_pebbleos(i):
        print("blobs     not under PebbleOS: `reset` goes back to the stub in a slot build")
        if i["slot_base"]:
            print(f"slot      running from {i['slot_base']:#010x}")
    else:
        print(f"blobs     load {i['load_base']:#010x}..{i['load_end']:#010x}, stack top {i['blob_stack_top']:#010x}, "
              f"output {i['out_buf']:#010x} ({i['out_len']}/{i['out_size']}), api {i['api']:#010x}")
        print(f"slot      {i['slot_base']:#010x}..{i['slot_end']:#010x}")
    print(f"commands  {i['cmd_count']}, {i['err_count']} failed")
    for key, label in (("boot_fault", "boot fault"), ("last_fault", "last fault")):
        if i[key]:
            print(f"{label}: " + format_fault(i[key]))


def cmd_peek(t, args):
    w = args.width
    data = t.read(args.addr, args.count * w, w)
    fmt = {1: "B", 2: "H", 4: "I"}[w]
    for k, (v,) in enumerate(struct.iter_unpack("<" + fmt, data)):
        print(f"{args.addr + k * w:08x}: {v:#0{2 + 2 * w}x}")


def cmd_poke(t, args):
    w = args.width
    fmt = {1: "B", 2: "H", 4: "I"}[w]
    t.write(args.addr, b"".join(struct.pack("<" + fmt, v) for v in args.values), w)


def cmd_dump(t, args):
    data = t.read(args.addr, args.length, args.width)
    if args.output:
        with open(args.output, "wb") as f:
            f.write(data)
        print(f"{args.output}: {len(data)} bytes")
    else:
        hexdump(args.addr, data)


def cmd_load(t, args):
    data = open(args.file, "rb").read()
    t.write(args.addr, data)
    got = t.crc32(args.addr, len(data))
    if got != crc32(data):
        raise TetherError(ERR_FLASH, "verify")
    print(f"{len(data)} bytes at {args.addr:#010x}")


def cmd_fill(t, args):
    t.fill(args.addr, args.value, args.count, args.width)


def cmd_crc(t, args):
    print(f"{t.crc32(args.addr, args.length):#010x}")


def _print_call(status, res, out, fault):
    sys.stdout.write(out.decode(errors="replace"))
    if out and not out.endswith(b"\n"):
        sys.stdout.write("\n")
    print(f"r0 = {res['r0']:#010x} ({res['r0']}), r1 = {res['r1']:#010x}, {res['elapsed_us']} us",
          file=sys.stderr)
    if status != OK:
        print(f"tether: {STATUS_NAMES.get(status, status)}", file=sys.stderr)
        if fault:
            print(format_fault(fault), file=sys.stderr)
        return 1
    return 0


def cmd_run(t, args):
    image = open(args.blob, "rb").read()
    hdr, status, res, out, fault = t.run_blob(image, args.args, args.timeout)
    print(f"{hdr['name']}: {len(image)} bytes at {hdr['load_addr']:#010x}", file=sys.stderr)
    return _print_call(status, res, out, fault)


def cmd_call(t, args):
    status, res, out, fault = t.call(args.addr, args.args, blob=args.blob, anywhere=args.anywhere,
                                     timeout_s=args.timeout)
    return _print_call(status, res, out, fault)


def cmd_i2c(t, args):
    data = t.i2c(args.bus, args.addr, bytes.fromhex(args.write or ""), args.read, args.unsafe, args.fast)
    if data:
        print(data.hex(" "))


def cmd_i2c_scan(t, args):
    found = t.i2c_scan(args.bus)
    print(" ".join(f"{a:#04x}" for a in found) or "nothing")


def cmd_gpio(t, args):
    port, pin = args.pin
    op, arg = GPIO_READ, 0
    action = args.action
    if action in ("high", "low"):
        op, arg = GPIO_OUT, int(action == "high")
    elif action in ("od-high", "od-low"):
        op, arg = GPIO_OUT_OD, int(action == "od-high")
    elif action == "in":
        op, arg = GPIO_IN, {"none": 0, "up": 1, "down": 2}[args.pull]
    elif action == "analog":
        op = GPIO_ANALOG
    regs, level = t.gpio(port, pin, op, arg)
    mode = ["input", "output", "alternate", "analog"][(regs["moder"] >> (2 * pin)) & 3]
    pull = ["none", "up", "down", "?"][(regs["pupdr"] >> (2 * pin)) & 3]
    af = ((regs["afrl"] if pin < 8 else regs["afrh"]) >> (4 * (pin % 8))) & 0xF
    od = "open drain" if regs["otyper"] & (1 << pin) else "push-pull"
    print(f"P{PORTS[port]}{pin} = {level} ({mode}, {od}, pull {pull}, AF{af})")


def cmd_reset(t, args):
    t.reset(RESET_SYSTEM)


def cmd_poweroff(t, args):
    t.reset(RESET_POWER_OFF)


def cmd_flash(t, args):
    image = open(args.image, "rb").read()
    if args.addr < SLOT_BASE:
        raise SystemExit(f"tether: {args.addr:#010x} is below the slot at {SLOT_BASE:#010x}")
    sp, pc = struct.unpack_from("<II", image)
    if not (0x20000000 <= sp <= 0x20270000 and pc & 1 and args.addr <= pc & ~1 < args.addr + len(image)):
        if not args.force:
            raise SystemExit(f"tether: {args.image} is not linked at {args.addr:#010x} "
                             f"(SP {sp:#010x}, reset {pc:#010x}); --force to flash it anyway")

    def progress(what, done, total):
        print(f"\r{what} {done * 100 // total:3d}%", end="", file=sys.stderr, flush=True)

    if is_pebbleos(t.info()):
        print("PebbleOS answers: resetting it to the stub", file=sys.stderr)
        t = to_stub(t, args)
    t0 = time.monotonic()
    n = t.flash_image(image, args.addr, progress)
    print(f"\r{n} bytes at {args.addr:#010x}, verified, {time.monotonic() - t0:.1f} s", file=sys.stderr)
    if args.boot:
        t.boot(args.addr)
        print("booting", file=sys.stderr)


def to_stub(t, args, timeout=30.0):
    """Resets PebbleOS, a slot build, and returns a Tether on the stub once it answers. Over TCP
    the bridge follows the ports, so the connection stays; a serial port is opened again."""
    t.reset(RESET_SYSTEM)
    deadline = time.monotonic() + timeout
    time.sleep(1.0)
    while time.monotonic() < deadline:
        try:
            if not args.tcp and not os.environ.get("TETHER_TCP"):
                t.t.close()
                t = Tether(open_transport(args.port, None), timeout=1.0, verbose=t.verbose)
            info = t.info()
            if not is_pebbleos(info):
                t.timeout = args.timeout
                return t
        except (TimeoutError, ConnectionError, OSError, SystemExit, TetherError):
            pass
        time.sleep(0.5)
    raise TimeoutError("the stub did not answer after the reset: is this a slot build?")


def cmd_boot(t, args):
    t.boot(args.addr)


def cmd_note(t, args):
    t.note(" ".join(args.text))


def cmd_clear_fault(t, args):
    t.clear_fault()


def build_parser():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", help="serial port of the stub or of PebbleOS's tether interface "
                                  "(default: found by their USB IDs, $TETHER_PORT)")
    p.add_argument("--tcp", help="HOST:PORT of a bridge.py or socat serving the port ($TETHER_TCP)")
    p.add_argument("--timeout", type=float, default=3.0, help="seconds to wait for a reply")
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="command", required=True)

    def add(name, func, help):
        s = sub.add_parser(name, help=help, description=help)
        s.set_defaults(func=func)
        return s

    add("info", cmd_info, "stub version, chip, clocks, reset cause, USB, faults")
    s = add("peek", cmd_peek, "read values")
    s.add_argument("addr", type=num)
    s.add_argument("count", type=num, nargs="?", default=1)
    s.add_argument("-w", "--width", type=int, choices=(1, 2, 4), default=4)
    s = add("poke", cmd_poke, "write values (not the flash, the stub's RAM or the USB)")
    s.add_argument("addr", type=num)
    s.add_argument("values", type=num, nargs="+")
    s.add_argument("-w", "--width", type=int, choices=(1, 2, 4), default=4)
    s = add("dump", cmd_dump, "read memory, as a hex dump or to a file")
    s.add_argument("addr", type=num)
    s.add_argument("length", type=num)
    s.add_argument("-o", "--output")
    s.add_argument("-w", "--width", type=int, choices=(0, 1, 2, 4), default=0,
                   help="access width; 0: words where aligned")
    s = add("load", cmd_load, "write a file to RAM")
    s.add_argument("addr", type=num)
    s.add_argument("file")
    s = add("fill", cmd_fill, "fill memory with a value")
    s.add_argument("addr", type=num)
    s.add_argument("value", type=num)
    s.add_argument("count", type=num)
    s.add_argument("-w", "--width", type=int, choices=(1, 2, 4), default=4)
    s = add("crc", cmd_crc, "CRC-32 (zlib's) of a range")
    s.add_argument("addr", type=num)
    s.add_argument("length", type=num)
    s = add("run", cmd_run, "load a blob built with experiments/blob.ld and run it")
    s.add_argument("blob")
    s.add_argument("args", type=num, nargs="*")
    s.add_argument("-t", "--timeout", type=float, default=10.0, help="seconds; 0: none")
    s = add("call", cmd_call, "call Thumb code at an address with r0-r3")
    s.add_argument("addr", type=num)
    s.add_argument("args", type=num, nargs="*")
    s.add_argument("--blob", action="store_true", help="r0 = api, r1 = args, r2 = count")
    s.add_argument("--anywhere", action="store_true", help="allow a target outside the load region")
    s.add_argument("-t", "--timeout", type=float, default=5.0)
    s = add("i2c", cmd_i2c, "an I2C transfer: write, then read after a repeated start")
    s.add_argument("bus", type=int, choices=(1, 2, 3, 4))
    s.add_argument("addr", type=num)
    s.add_argument("-w", "--write", help="hex bytes, e.g. 09 or 1f0a")
    s.add_argument("-r", "--read", type=num, default=0)
    s.add_argument("--unsafe", action="store_true",
                   help="allow writes to the PMIC, the fuel gauge and the USB-PD chip")
    s.add_argument("--fast", action="store_true", help="400 kHz instead of 100 kHz")
    s = add("i2c-scan", cmd_i2c_scan, "probe every address of a bus")
    s.add_argument("bus", type=int, choices=(1, 2, 3, 4))
    s = add("gpio", cmd_gpio, "read or set a pin, e.g. gpio PF3 low")
    s.add_argument("pin", type=parse_pin)
    s.add_argument("action", nargs="?", default="get",
                   choices=("get", "high", "low", "od-high", "od-low", "in", "analog"))
    s.add_argument("pull", nargs="?", default="none", choices=("none", "up", "down"))
    add("reset", cmd_reset, "reset the watch (the bootloader starts the stub again; from PebbleOS, "
                            "only a slot build goes back to it)")
    add("poweroff", cmd_poweroff, "power off through the bootloader")
    s = add("flash", cmd_flash, "program a PebbleOS image linked for the slot")
    s.add_argument("image")
    s.add_argument("--addr", type=num, default=SLOT_BASE)
    s.add_argument("--boot", action="store_true", help="boot it once verified")
    s.add_argument("--force", action="store_true", help="flash an image whose vectors look wrong")
    s = add("boot", cmd_boot, "start the image in the slot (or at an address above it)")
    s.add_argument("addr", type=num, nargs="?", default=0)
    s = add("note", cmd_note, "show a line of text on the watch")
    s.add_argument("text", nargs="+")
    add("clear-fault", cmd_clear_fault, "forget the recorded faults")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    transport = open_transport(args.port, args.tcp)
    try:
        t = Tether(transport, timeout=args.timeout, verbose=args.verbose)
        return args.func(t, args) or 0
    except TetherError as e:
        print(f"tether: {e}", file=sys.stderr)
        return 1
    except (TimeoutError, ConnectionError) as e:
        print(f"tether: {e}", file=sys.stderr)
        return 2
    finally:
        transport.close()


if __name__ == "__main__":
    sys.exit(main())
