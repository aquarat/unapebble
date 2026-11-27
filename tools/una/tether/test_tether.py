# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Tests of tether.py and bridge.py against the stub's own protocol code.

The simulator (sim/sim_port.c with stub/proto.c, built by `make sim`) stands
in for the watch; its path comes from $TETHER_SIM. Without it only the tests
that need no device run.

    make -C tools/una/tether test
"""

import os
import pty
import re
import struct
import subprocess
import threading
import time
import unittest
import zlib

import bridge
import console
import tether

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.environ.get("TETHER_SIM", os.path.join(HERE, "build", "tether_sim"))


def header_defines():
    """The #defines and enum values of stub/tether_proto.h."""
    with open(os.path.join(HERE, "stub", "tether_proto.h")) as f:
        text = f.read()
    values = {}
    for name, value in re.findall(r"#define (TETHER_\w+)\s+([^/\n]+)", text):
        values[name] = value.strip()
    for name, value in re.findall(r"\b(TETHER_\w+) = (0x[0-9a-fA-F]+|\d+)", text):
        values[name] = value
    return values


def c_value(values, name):
    expr = values[name]
    expr = re.sub(r"\b(TETHER_\w+)\b", lambda m: str(c_value(values, m.group(1))), expr)
    expr = re.sub(r"(0x[0-9a-fA-F]+|\d+)U\b", r"\1", expr)
    return eval(expr, {})  # constants of our own header only


def blob(name, size=256):
    """A blob image the simulator recognises by its name."""
    entry = tether.LOAD_BASE + tether.BLOB_HEADER_SIZE + 1
    end = tether.LOAD_BASE + size
    hdr = struct.pack(tether.BLOB_HEADER_FMT, tether.BLOB_MAGIC, 1, tether.LOAD_BASE, entry, end, end,
                      name.encode())
    return hdr + bytes((i * 7) & 0xFF for i in range(size - len(hdr)))


def image(size=0x5000, base=tether.SLOT_BASE):
    vec = struct.pack("<II", 0x2025E000, base + 0x201)
    return vec + bytes((i * 13 + 5) & 0xFF for i in range(size - len(vec)))


class Constants(unittest.TestCase):
    def test_constants_match_the_header(self):
        v = header_defines()
        pairs = {
            "TETHER_PROTO_VERSION": tether.PROTO_VERSION,
            "TETHER_HDR_LEN": tether.HDR_LEN,
            "TETHER_CRC_LEN": tether.CRC_LEN,
            "TETHER_MAX_DATA": tether.MAX_DATA,
            "TETHER_MAX_PAYLOAD": tether.MAX_PAYLOAD,
            "TETHER_REPLY": tether.REPLY,
            "TETHER_SLOT_BASE": tether.SLOT_BASE,
            "TETHER_LOAD_BASE": tether.LOAD_BASE,
            "TETHER_LOAD_END": tether.LOAD_END,
            "TETHER_BLOB_STACK_TOP": tether.BLOB_STACK_TOP,
            "TETHER_STUB_RAM": tether.STUB_RAM,
            "TETHER_STUB_RAM_SIZE": tether.STUB_RAM_SIZE,
            "TETHER_FLASH_PAGE": tether.FLASH_PAGE,
            "TETHER_INFO_PEBBLEOS": tether.INFO_PEBBLEOS,
            "TETHER_FLASH_PROG_ALIGN": tether.FLASH_PROG_ALIGN,
            "TETHER_BLOB_MAGIC": tether.BLOB_MAGIC,
            "TETHER_FAULT_MAGIC": tether.FAULT_MAGIC,
            "TETHER_FAULT_TIMEOUT": tether.FAULT_TIMEOUT,
            "TETHER_CALL_BLOB": tether.CALL_BLOB,
            "TETHER_CALL_ANYWHERE": tether.CALL_ANYWHERE,
            "TETHER_CALL_KEEP_OUT": tether.CALL_KEEP_OUT,
            "TETHER_CALL_MAX_ARGS": tether.CALL_MAX_ARGS,
            "TETHER_I2C_UNSAFE": tether.I2C_UNSAFE,
            "TETHER_I2C_400K": tether.I2C_400K,
            "TETHER_I2C_MAX": tether.I2C_MAX,
        }
        for name, py in pairs.items():
            self.assertEqual(c_value(v, name), py, name)
        for name in ("INFO", "READ", "WRITE", "FILL", "CRC32", "CALL", "I2C", "GPIO", "RESET",
                     "FLASH_ERASE", "FLASH_WRITE", "BOOT", "NOTE", "CLEAR_FAULT", "BAD_FRAME"):
            self.assertEqual(c_value(v, f"TETHER_CMD_{name}"), getattr(tether, f"CMD_{name}"), name)
        for name in ("CRC", "CMD", "ARG", "FAULT", "TIMEOUT", "PROTECTED", "NACK", "BUS", "FLASH",
                     "LENGTH", "UNSUPPORTED"):
            self.assertEqual(c_value(v, f"TETHER_ERR_{name}"), getattr(tether, f"ERR_{name}"), name)

    def test_fault_layout_matches_the_header(self):
        with open(os.path.join(HERE, "stub", "tether_proto.h")) as f:
            text = f.read()
        body = re.search(r"struct tether_fault \{(.*?)\}", text, re.S).group(1)
        names = re.findall(r"\b(\w+)[,;]", re.sub(r"/\*.*?\*/", "", body, flags=re.S))
        self.assertEqual(names, tether.FAULT_FIELDS)

    def test_frame_encoding(self):
        frame = tether.encode_frame(tether.CMD_READ, 7, b"\x01\x02")
        self.assertEqual(frame[:6], b"\xa5\x5a\x02\x07\x02\x00")
        self.assertEqual(struct.unpack("<I", frame[-4:])[0], zlib.crc32(frame[2:-4]))
        with self.assertRaises(ValueError):
            tether.encode_frame(1, 0, bytes(tether.MAX_PAYLOAD + 1))

    def test_blob_header_checks(self):
        tether.parse_blob_header(blob("x"))
        bad = bytearray(blob("x"))
        bad[0] ^= 1
        with self.assertRaises(ValueError):
            tether.parse_blob_header(bytes(bad))
        outside = struct.pack(tether.BLOB_HEADER_FMT, tether.BLOB_MAGIC, 1, tether.STUB_RAM,
                              tether.STUB_RAM + 0x31, tether.STUB_RAM + 64, tether.STUB_RAM + 64, b"x")
        with self.assertRaises(ValueError):
            tether.parse_blob_header(outside + bytes(32))

    def test_pins(self):
        self.assertEqual(tether.parse_pin("pd15"), (3, 15))
        self.assertEqual(tether.parse_pin("PA5"), (0, 5))

    def test_fault_format(self):
        f = dict.fromkeys(tether.FAULT_FIELDS, 0)
        f.update(magic=tether.FAULT_MAGIC, exc=5, cfsr=0x8200, bfar=0x1234, pc=0x20020040)
        text = tether.format_fault(f)
        self.assertIn("BusFault", text)
        self.assertIn("PRECISERR", text)
        self.assertIn("bfar 0x00001234", text)


@unittest.skipUnless(os.access(SIM, os.X_OK), f"no simulator at {SIM}: make sim")
class Simulated(unittest.TestCase):
    def setUp(self):
        self.proc = subprocess.Popen([SIM], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, bufsize=0)
        self.t = tether.Tether(tether.PipeTransport(self.proc.stdout.fileno(), self.proc.stdin.fileno(),
                                                    "sim"))

    def tearDown(self):
        self.proc.stdin.close()
        self.proc.wait(5)
        self.proc.stdout.close()
        self.proc.stderr.close()

    def test_info(self):
        i = self.t.info()
        self.assertEqual(i["magic"], b"UTS1")
        self.assertEqual(i["info_size"], tether.INFO_SIZE)
        self.assertEqual(i["version"], "sim")
        self.assertEqual(i["slot_base"], tether.SLOT_BASE)
        self.assertEqual(i["max_data"], tether.MAX_DATA)
        self.assertIsNone(i["last_fault"])

    def test_memory(self):
        data = bytes(range(256)) * 40  # spans several frames
        self.t.write(0x20100003, data)
        self.assertEqual(self.t.read(0x20100003, len(data)), data)
        self.assertEqual(self.t.crc32(0x20100003, len(data)), zlib.crc32(data))
        self.t.write32(0x20100000, 0xCAFEF00D)
        self.assertEqual(self.t.read32(0x20100000), 0xCAFEF00D)
        self.t.fill(0x20101000, 0x1234, 8, 2)
        self.assertEqual(self.t.read(0x20101000, 16), struct.pack("<H", 0x1234) * 8)

    def test_alignment(self):
        with self.assertRaises(tether.TetherError) as e:
            self.t.read(0x20100001, 4, 4)
        self.assertEqual(e.exception.status, tether.ERR_ARG)

    def test_fault_is_reported(self):
        with self.assertRaises(tether.TetherError) as e:
            self.t.read(0x60000000, 4)
        self.assertEqual(e.exception.status, tether.ERR_FAULT)
        self.assertEqual(e.exception.fault["bfar"], 0x60000000)
        self.assertEqual(self.t.info()["last_fault"]["bfar"], 0x60000000)
        self.t.clear_fault()
        self.assertIsNone(self.t.info()["last_fault"])

    def test_protected_writes(self):
        for addr in (0x08000000, 0x08060000, 0x0C000000, 0x20000000, 0x2001FFFC, 0x40022040, 0x42040800):
            with self.assertRaises(tether.TetherError, msg=hex(addr)) as e:
                self.t.write32(addr, 0)
            self.assertEqual(e.exception.status, tether.ERR_PROTECTED, hex(addr))
        with self.assertRaises(tether.TetherError) as e:
            self.t.fill(0x2001FF00, 0, 0x100, 4)  # runs into the stub's RAM
        self.assertEqual(e.exception.status, tether.ERR_PROTECTED)
        # Reading the stub's RAM is fine, the USB FIFOs are not
        self.t.read(0x20000000, 16)
        with self.assertRaises(tether.TetherError):
            self.t.read(0x42040020, 4, 4)

    def test_resync_and_bad_frames(self):
        self.t.t.write(b"\x00garbage\xa5\xa5\x5a")  # noise, a false start, then a real frame
        self.assertEqual(self.t.info()["magic"], b"UTS1")
        frame = bytearray(tether.encode_frame(tether.CMD_INFO, 9))
        frame[-1] ^= 0xFF
        self.t.t.write(bytes(frame))
        self.t.seq = 8
        with self.assertRaises(tether.TetherError) as e:
            self.t.request(tether.CMD_INFO)  # its reply follows the BAD_FRAME of seq 9
        self.assertEqual(e.exception.status, tether.ERR_CRC)
        self.t.drain()
        self.assertEqual(self.t.request(0x55)[0], tether.ERR_CMD)
        self.assertEqual(self.t.info()["magic"], b"UTS1")

    def test_false_start_with_a_plausible_length(self):
        # A magic in the noise, then a header asking for more than comes: the real frame
        # inside is found once the half frame times out
        self.t.t.write(b"\xa5\x5a\x01\x00\x40\x00")
        self.assertEqual(self.t.info()["magic"], b"UTS1")

    def test_half_frame_is_dropped(self):
        self.t.t.write(tether.encode_frame(tether.CMD_INFO, 1)[:5])
        time.sleep(0.7)
        self.assertEqual(self.t.info()["magic"], b"UTS1")

    def test_run_blob(self):
        hdr, status, res, out, fault = self.t.run_blob(blob("sim_echo"), [1, 2, 0x30])
        self.assertEqual(status, tether.OK)
        self.assertEqual(hdr["name"], "sim_echo")
        self.assertEqual(res["r0"], 0x33)
        self.assertEqual(res["r1"], 1)  # called with the blob convention
        self.assertEqual(out, b"echo 3 args: 1 2 30\n")
        self.assertIsNone(fault)

    def test_long_output_is_fetched(self):
        _, status, res, out, _ = self.t.run_blob(blob("sim_big"))
        self.assertEqual(status, tether.OK)
        self.assertEqual(len(out), 10000)
        self.assertEqual(out[:3], b"abc")
        self.assertEqual(out[-1:], bytes([ord("a") + 9999 % 26]))

    def test_blob_fault_and_timeout(self):
        _, status, res, out, fault = self.t.run_blob(blob("sim_fault"))
        self.assertEqual(status, tether.ERR_FAULT)
        self.assertEqual(out, b"about to fault\n")
        self.assertEqual(fault["exc"], 3)
        self.assertEqual(fault["pc"], tether.LOAD_BASE + tether.BLOB_HEADER_SIZE + 8)
        _, status, _, _, fault = self.t.run_blob(blob("sim_hang"))
        self.assertEqual(status, tether.ERR_TIMEOUT)
        self.assertEqual(fault["exc"], tether.FAULT_TIMEOUT)

    def test_call_outside_load_region(self):
        with self.assertRaises(tether.TetherError) as e:
            self.t.call(0x08000101)
        self.assertEqual(e.exception.status, tether.ERR_PROTECTED)

    def test_i2c(self):
        self.assertEqual(self.t.i2c(1, 0x36, b"\x06", 2), struct.pack("<H", 50 * 256))
        with self.assertRaises(tether.TetherError) as e:
            self.t.i2c(1, 0x36, b"\x06\x00\x00")
        self.assertEqual(e.exception.status, tether.ERR_PROTECTED)
        with self.assertRaises(tether.TetherError) as e:
            self.t.i2c(1, 0x61, b"\x10\x00")
        self.assertEqual(e.exception.status, tether.ERR_PROTECTED)
        self.t.i2c(1, 0x61, b"\x10\x05", unsafe=True)
        self.assertEqual(self.t.i2c(1, 0x61, b"\x10", 1), b"\x05")
        with self.assertRaises(tether.TetherError) as e:
            self.t.i2c(1, 0x5A, b"\x01\x02")  # the haptics are not protected, only absent here
        self.assertEqual(e.exception.status, tether.ERR_NACK)
        self.assertEqual(self.t.i2c_scan(1), [0x36, 0x61])
        with self.assertRaises(tether.TetherError) as e:
            self.t.i2c(2, 0x40, b"", 1)
        self.assertEqual(e.exception.status, tether.ERR_NACK)

    def test_gpio(self):
        regs, level = self.t.gpio(5, 3, tether.GPIO_OUT_OD, 0)
        self.assertEqual(level, 0)
        self.assertEqual((regs["moder"] >> 6) & 3, 1)
        self.assertTrue(regs["otyper"] & (1 << 3))
        _, level = self.t.gpio(3, 4, tether.GPIO_IN, 1)
        self.assertEqual(level, 1)
        for pin in (11, 12, 13, 14):
            with self.assertRaises(tether.TetherError) as e:
                self.t.gpio(0, pin, tether.GPIO_OUT, 1)
            self.assertEqual(e.exception.status, tether.ERR_PROTECTED)
        self.t.gpio(0, 12)  # reading them is fine

    def test_flash_and_boot(self):
        with self.assertRaises(tether.TetherError) as e:
            self.t.boot()
        self.assertEqual(e.exception.status, tether.ERR_ARG)  # empty slot
        img = image()
        self.assertEqual(self.t.flash_image(img), len(img))
        self.assertEqual(self.t.read(tether.SLOT_BASE, len(img)), img)
        self.assertTrue(self.t.info()["flags"] & (1 << tether.INFO_FLAGS.index("slot-valid")))
        self.t.boot()
        # Programming over what is there fails; erasing first is the host's job
        with self.assertRaises(tether.TetherError) as e:
            self.t.flash_write(tether.SLOT_BASE, bytes(16))
        self.assertEqual(e.exception.status, tether.ERR_FLASH)

    def test_flash_below_the_slot_is_refused(self):
        with self.assertRaises(ValueError):
            self.t.flash_image(image(), tether.STUB_FLASH)
        for cmd, payload in (
            (tether.CMD_FLASH_ERASE, struct.pack("<II", tether.STUB_FLASH, tether.FLASH_PAGE)),
            (tether.CMD_FLASH_ERASE, struct.pack("<II", 0x08000000, tether.FLASH_PAGE)),
            (tether.CMD_FLASH_ERASE, struct.pack("<II", tether.SLOT_BASE - tether.FLASH_PAGE,
                                                 2 * tether.FLASH_PAGE)),
            (tether.CMD_FLASH_WRITE, struct.pack("<I", tether.SLOT_BASE - 16) + bytes(32)),
            (tether.CMD_FLASH_ERASE, struct.pack("<II", 0x083FE000, 2 * tether.FLASH_PAGE)),
            (tether.CMD_BOOT, struct.pack("<I", tether.STUB_FLASH)),
        ):
            self.assertEqual(self.t.request(cmd, payload)[0], tether.ERR_PROTECTED)
        self.assertEqual(self.t.read(0x08000000, 14), b"UNA bootloader")

    def test_note_and_reset(self):
        self.t.note("hello from the test")
        self.t.reset()
        self.t.reset(tether.RESET_POWER_OFF)


class Ports(unittest.TestCase):
    """Telling the stub's port from PebbleOS's two."""

    def test_classify(self):
        self.assertEqual(tether.classify(0x1209, 0x0001, 0), "stub")
        self.assertEqual(tether.classify(0x1209, 0x0002, 0), "console")
        self.assertEqual(tether.classify(0x1209, 0x0002, 2), "tether")
        self.assertIsNone(tether.classify(0x1209, 0x0002, 1))
        self.assertIsNone(tether.classify(0x0483, 0x5740, 0))

    def test_macos_names_without_ioreg(self):
        real = tether.glob.glob
        try:
            tether.glob.glob = lambda pat: ["/dev/cu.usbmodem1234"]
            self.assertEqual(tether._guess_macos_names("tether"), ["/dev/cu.usbmodem1234"])
            self.assertEqual(tether._guess_macos_names("console"), [])
            tether.glob.glob = lambda pat: ["/dev/cu.usbmodemAB121", "/dev/cu.usbmodemAB123"]
            self.assertEqual(tether._guess_macos_names("tether"), ["/dev/cu.usbmodemAB123"])
            self.assertEqual(tether._guess_macos_names("console"), ["/dev/cu.usbmodemAB121"])
        finally:
            tether.glob.glob = real


class FakeShell(threading.Thread):
    """PebbleOS's shell as the console port shows it: echo, CR LF, a prompt, logs between."""

    def __init__(self, rfd, wfd):
        super().__init__(daemon=True)
        self.rfd, self.wfd = rfd, wfd
        self.active = False
        self.line = b""
        self.stop = False

    def out(self, data):
        os.write(self.wfd, data)

    def run(self):
        while not self.stop:
            try:
                data = os.read(self.rfd, 1)
            except OSError:
                return
            if not data:
                return
            if not self.active:
                self.active = True
                self.out(b"\r\npebble> ")
            c = data
            if c == b"\x03":
                self.line = b""
                self.out(b"^C\r\npebble> ")
            elif c == b"\r":
                self.out(b"\r\n")
                cmd, self.line = self.line.decode(), b""
                if cmd == "battery status":
                    self.out(b"\r\x1b[K[INFO] a log line\r\n")
                    self.out(b"Charge: 50%\r\nVoltage: 4000 mV\r\n")
                    self.out(b"pebble> ")
                elif cmd == "reboot":
                    self.out(b"Restarting\r\n")
                else:
                    self.out(f"error: unknown command '{cmd}'\r\npebble> ".encode())
            else:
                self.line += c
                self.out(c)


class Console(unittest.TestCase):
    def setUp(self):
        to_shell_r, to_shell_w = os.pipe()
        from_shell_r, from_shell_w = os.pipe()
        self.shell = FakeShell(to_shell_r, from_shell_w)
        self.shell.start()
        self.fds = (to_shell_r, to_shell_w, from_shell_r, from_shell_w)
        self.con = console.Console(tether.PipeTransport(from_shell_r, to_shell_w, "shell"))

    def tearDown(self):
        self.shell.stop = True
        for fd in self.fds:
            os.close(fd)

    def test_command(self):
        self.assertTrue(self.con.prompt())
        out, ok = self.con.run("battery status", 5.0)
        self.assertTrue(ok)
        self.assertEqual(out, "[INFO] a log line\nCharge: 50%\nVoltage: 4000 mV\n")

    def test_unknown_and_reboot(self):
        self.assertTrue(self.con.prompt())
        out, ok = self.con.run("nope", 5.0)
        self.assertTrue(ok)
        self.assertIn("unknown command", out)
        out, ok = self.con.run("reboot", 5.0)
        self.assertTrue(ok)
        self.assertIn("Restarting", out)

    def test_clean(self):
        self.assertEqual(console.clean("pebble> \r\x1b[Klog\r\npebble> "), "log\npebble> ")


def have_pty():
    try:
        m, s = pty.openpty()
    except OSError:
        return False
    os.close(m)
    os.close(s)
    return True


@unittest.skipUnless(os.access(SIM, os.X_OK), f"no simulator at {SIM}: make sim")
@unittest.skipUnless(have_pty(), "no pseudo-terminals here")
class OverSerialAndTcp(unittest.TestCase):
    """The simulator behind a pty, as a CDC-ACM port; tether.py through bridge.py."""

    def setUp(self):
        # The slave stays open: a pty master with no slave open reads EIO, which ends the sim
        self.master, self.slave = pty.openpty()
        self.slave_path = os.ttyname(self.slave)
        tether._raw_tty(self.slave)
        self.proc = subprocess.Popen([SIM], stdin=self.master, stdout=self.master,
                                     stderr=subprocess.DEVNULL)
        self.stop = threading.Event()

    def tearDown(self):
        self.stop.set()
        self.proc.kill()
        self.proc.wait(5)
        os.close(self.slave)
        os.close(self.master)

    def test_serial(self):
        t = tether.Tether(tether.SerialTransport(self.slave_path))
        data = os.urandom(9000)
        t.write(0x20200000, data)
        self.assertEqual(t.read(0x20200000, len(data)), data)
        t.t.close()

    def test_bridge(self):
        ready = threading.Event()
        stop = self.stop
        where = {}

        def on_ready(addr):
            where["addr"] = addr
            ready.set()

        th = threading.Thread(target=bridge.serve, args=("127.0.0.1:0", self.slave_path),
                              kwargs=dict(once=True, ready=on_ready, stop=stop), daemon=True)
        th.start()
        self.assertTrue(ready.wait(5))
        host, port = where["addr"]
        t = tether.Tether(tether.TcpTransport(f"{host}:{port}"))
        self.assertEqual(t.info()["version"], "sim")
        _, status, res, out, _ = t.run_blob(blob("sim_big"))
        self.assertEqual(len(out), 10000)
        t.t.close()
        stop.set()
        th.join(5)

    def test_cli(self):
        env = dict(os.environ, TETHER_PORT=self.slave_path)
        run = lambda *a: subprocess.run(  # noqa: E731
            ["python3", os.path.join(HERE, "tether.py"), *a], env=env, capture_output=True, text=True,
            timeout=30)
        r = run("info")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("stub      sim", r.stdout)
        r = run("poke", "0x20200000", "0x11223344")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run("peek", "0x20200000")
        self.assertIn("0x11223344", r.stdout)
        r = run("poke", "0x08060000", "0")
        self.assertEqual(r.returncode, 1)
        self.assertIn("protected", r.stderr)
        r = run("i2c", "1", "0x36", "-w", "09", "-r", "2")
        self.assertEqual(r.stdout.strip(), "00 c8")
        r = run("gpio", "PF3", "od-low")
        self.assertIn("PF3 = 0", r.stdout)


@unittest.skipUnless(os.access(SIM, os.X_OK), f"no simulator at {SIM}: make sim")
@unittest.skipUnless(have_pty(), "no pseudo-terminals here")
class PebbleOS(unittest.TestCase):
    """The simulator answering as PebbleOS does, a slot build that resets into the stub."""

    def setUp(self):
        self.master, self.slave = pty.openpty()
        self.slave_path = os.ttyname(self.slave)
        tether._raw_tty(self.slave)
        env = {**os.environ, "TETHER_SIM_PEBBLEOS": "1"}
        self.proc = subprocess.Popen([SIM], stdin=self.master, stdout=self.master,
                                     stderr=subprocess.DEVNULL, env=env)

    def tearDown(self):
        self.proc.kill()
        self.proc.wait(5)
        os.close(self.slave)
        os.close(self.master)

    def test_info_and_refusals(self):
        t = tether.Tether(tether.SerialTransport(self.slave_path))
        i = t.info()
        self.assertTrue(tether.is_pebbleos(i))
        self.assertEqual(i["version"], "PebbleOS sim")
        t.write32(0x20200000, 0x55AA55AA)
        self.assertEqual(t.read32(0x20200000), 0x55AA55AA)
        with self.assertRaises(tether.TetherError) as e:
            t.flash_erase(tether.SLOT_BASE, tether.FLASH_PAGE)
        self.assertEqual(e.exception.status, tether.ERR_UNSUPPORTED)
        self.assertEqual(t.call(tether.LOAD_BASE + 0x31)[0], tether.ERR_UNSUPPORTED)
        t.t.close()

    def test_flash_goes_through_the_stub(self):
        path = os.path.join(HERE, "build", "test_slot.bin")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        img = image(0x3000)
        with open(path, "wb") as f:
            f.write(img)
        r = subprocess.run(["python3", os.path.join(HERE, "tether.py"), "--port", self.slave_path,
                            "flash", path], capture_output=True, text=True, timeout=60, check=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("resetting it to the stub", r.stderr)
        t = tether.Tether(tether.SerialTransport(self.slave_path))
        self.assertFalse(tether.is_pebbleos(t.info()))
        self.assertEqual(t.crc32(tether.SLOT_BASE, len(img)), zlib.crc32(img))
        t.t.close()


if __name__ == "__main__":
    unittest.main()
