#!/usr/bin/env python3
"""End-to-end check of PlayStation controller mapping.

Creates a mock DS3/DS4-like evdev device, starts gpadmapper-cpp on it, grabs the
daemon's virtual keyboard and verifies that buttons produce the configured keys.
No physical controller is needed. Requires python-evdev and /dev/uinput access.

The test config uses grab = true so that the daemon owns the mock pad and a
running gpadmapper-cpp service cannot also react to the injected events.
"""

from __future__ import annotations

import os
import select
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import evdev
from evdev import AbsInfo, InputDevice, UInput, ecodes

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "gpadmapper-cpp"

CONFIG = """\
[device]
grab = true

[pointer]
deadzone = 0.06
curve = 2.2
max_speed = 0

[scroll]
deadzone = 0.10
curve = 2.0
max_speed = 0
invert_y = true
hi_res = false
mode = "wheel"

[triggers]
threshold = 0.25

[toggle]
enabled = false

[buttons]
BTN_A = "KEY_F12"
BTN_B = "KEY_F11"
BTN_X = "KEY_F10"
BTN_Y = "KEY_F9"
LT = "KEY_F8"
RT = "KEY_F7"
DPAD_UP = "KEY_F6"
DPAD_DOWN = "KEY_F5"
DPAD_LEFT = "KEY_F4"
DPAD_RIGHT = "KEY_F3"
LB = "KEY_F2"
RB = "KEY_F2"
"""


def abs_info(lo: int, hi: int) -> AbsInfo:
    return AbsInfo(0, lo, hi, 0, 0, 0)


def make_mock(name: str, *, analog_triggers: bool, hat_x: bool, hat_y: bool,
              dpad_buttons: list[int]) -> UInput:
    keys = [
        ecodes.BTN_SOUTH, ecodes.BTN_EAST, ecodes.BTN_NORTH, ecodes.BTN_WEST,
        ecodes.BTN_TL, ecodes.BTN_TR, ecodes.BTN_SELECT, ecodes.BTN_START,
        ecodes.BTN_MODE, ecodes.BTN_THUMBL, ecodes.BTN_THUMBR,
    ]
    axes = [
        (ecodes.ABS_X, abs_info(-32768, 32767)),
        (ecodes.ABS_Y, abs_info(-32768, 32767)),
        (ecodes.ABS_RX, abs_info(-32768, 32767)),
        (ecodes.ABS_RY, abs_info(-32768, 32767)),
    ]
    if analog_triggers:
        axes += [
            (ecodes.ABS_Z, abs_info(0, 255)),
            (ecodes.ABS_RZ, abs_info(0, 255)),
        ]
    else:
        keys += [ecodes.BTN_TL2, ecodes.BTN_TR2]
    if hat_x:
        axes.append((ecodes.ABS_HAT0X, abs_info(-1, 1)))
    if hat_y:
        axes.append((ecodes.ABS_HAT0Y, abs_info(-1, 1)))
    keys += dpad_buttons

    dev = UInput(
        {ecodes.EV_KEY: keys, ecodes.EV_ABS: axes},
        name=name,
        vendor=0x054C,
        product=0x0268,
        bustype=ecodes.BUS_USB,
    )
    for _ in range(50):
        if dev.device.path and os.path.exists(dev.device.path):
            return dev
        time.sleep(0.02)
    raise RuntimeError("mock input device did not appear")


def find_new_virtual_mouse(before: set[str]) -> InputDevice:
    for _ in range(100):
        for path in set(evdev.list_devices()) - before:
            try:
                dev = InputDevice(path)
            except OSError:
                continue
            if dev.name == "gpadmapper-cpp":
                return dev
            dev.close()
        time.sleep(0.02)
    raise RuntimeError("gpadmapper-cpp virtual device did not appear")


def wait_for_daemon(log_path: str, proc: subprocess.Popen) -> None:
    for _ in range(100):
        if proc.poll() is not None:
            raise RuntimeError(f"daemon exited early:\n{read_log(log_path)}")
        if "слушаю" in read_log(log_path):
            time.sleep(0.1)  # "слушаю" is printed just before grab()
            text = read_log(log_path)
            if "не смог захватить" in text:
                raise RuntimeError(f"daemon could not grab the mock pad:\n{text}")
            return
        time.sleep(0.02)
    raise RuntimeError(f"daemon did not open the mock pad:\n{read_log(log_path)}")


def read_log(path: str) -> str:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def expect_key(dev: InputDevice, code: int, value: int, timeout: float = 1.0) -> bool:
    deadline = time.monotonic() + timeout
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            return False
        ready, _, _ = select.select([dev.fd], [], [], left)
        if not ready:
            return False
        for ev in dev.read():
            if ev.type == ecodes.EV_KEY and ev.code == code and ev.value == value:
                return True


def expect_no_key(dev: InputDevice, code: int, timeout: float = 0.2) -> bool:
    deadline = time.monotonic() + timeout
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            return True
        ready, _, _ = select.select([dev.fd], [], [], left)
        if not ready:
            return True
        for ev in dev.read():
            if ev.type == ecodes.EV_KEY and ev.code == code:
                return False


def check(cond: bool, message: str) -> None:
    if not cond:
        raise AssertionError(message)
    print(f"  OK: {message}")


def tap(ui: UInput, dev: InputDevice, in_type: int, in_code: int,
        out_code: int, message: str, press_value: int = 1) -> None:
    ui.write(in_type, in_code, press_value)
    ui.syn()
    check(expect_key(dev, out_code, 1), f"{message}: press")
    ui.write(in_type, in_code, 0)
    ui.syn()
    check(expect_key(dev, out_code, 0), f"{message}: release")


def run_case(name: str, config_path: str, *, analog_triggers: bool, hat_x: bool,
             hat_y: bool, dpad_buttons: list[int]) -> None:
    print(f"--- {name}")
    ui = make_mock(name, analog_triggers=analog_triggers, hat_x=hat_x,
                   hat_y=hat_y, dpad_buttons=dpad_buttons)
    log = tempfile.NamedTemporaryFile(prefix="gm-ps-", suffix=".log", delete=False)
    log_path = log.name
    log.close()
    proc = None
    out = None
    try:
        before = set(evdev.list_devices())
        with open(log_path, "w") as log_fh:
            proc = subprocess.Popen(
                [str(BIN), "-v", "--config", config_path, "--device", ui.device.path],
                stdout=log_fh, stderr=subprocess.STDOUT,
            )
        out = find_new_virtual_mouse(before)
        out.grab()  # keep synthetic F-keys out of the desktop session
        wait_for_daemon(log_path, proc)

        for in_code, out_code, label in [
            (ecodes.BTN_SOUTH, ecodes.KEY_F12, "Cross -> BTN_A"),
            (ecodes.BTN_EAST, ecodes.KEY_F11, "Circle -> BTN_B"),
            (ecodes.BTN_NORTH, ecodes.KEY_F10, "Square -> BTN_X"),
            (ecodes.BTN_WEST, ecodes.KEY_F9, "Triangle -> BTN_Y"),
        ]:
            tap(ui, out, ecodes.EV_KEY, in_code, out_code, label)

        if analog_triggers:
            tap(ui, out, ecodes.EV_ABS, ecodes.ABS_Z, ecodes.KEY_F8,
                "L2 (ABS_Z)", press_value=255)
            tap(ui, out, ecodes.EV_ABS, ecodes.ABS_RZ, ecodes.KEY_F7,
                "R2 (ABS_RZ)", press_value=255)
        else:
            tap(ui, out, ecodes.EV_KEY, ecodes.BTN_TL2, ecodes.KEY_F8,
                "L2 (BTN_TL2)")
            tap(ui, out, ecodes.EV_KEY, ecodes.BTN_TR2, ecodes.KEY_F7,
                "R2 (BTN_TR2)")

        if hat_y:
            tap(ui, out, ecodes.EV_ABS, ecodes.ABS_HAT0Y, ecodes.KEY_F6,
                "d-pad up (HAT)", press_value=-1)
            tap(ui, out, ecodes.EV_ABS, ecodes.ABS_HAT0Y, ecodes.KEY_F5,
                "d-pad down (HAT)", press_value=1)
        if hat_x:
            tap(ui, out, ecodes.EV_ABS, ecodes.ABS_HAT0X, ecodes.KEY_F4,
                "d-pad left (HAT)", press_value=-1)
            tap(ui, out, ecodes.EV_ABS, ecodes.ABS_HAT0X, ecodes.KEY_F3,
                "d-pad right (HAT)", press_value=1)
        if ecodes.BTN_DPAD_UP in dpad_buttons:
            tap(ui, out, ecodes.EV_KEY, ecodes.BTN_DPAD_UP, ecodes.KEY_F6,
                "d-pad up (BTN_DPAD)")
            tap(ui, out, ecodes.EV_KEY, ecodes.BTN_DPAD_DOWN, ecodes.KEY_F5,
                "d-pad down (BTN_DPAD)")
        if ecodes.BTN_DPAD_LEFT in dpad_buttons:
            tap(ui, out, ecodes.EV_KEY, ecodes.BTN_DPAD_LEFT, ecodes.KEY_F4,
                "d-pad left (BTN_DPAD)")
            tap(ui, out, ecodes.EV_KEY, ecodes.BTN_DPAD_RIGHT, ecodes.KEY_F3,
                "d-pad right (BTN_DPAD)")

        # LB and RB both map to KEY_F2: releasing one must not release the key.
        ui.write(ecodes.EV_KEY, ecodes.BTN_TL, 1)
        ui.syn()
        check(expect_key(out, ecodes.KEY_F2, 1), "LB press -> KEY_F2")
        ui.write(ecodes.EV_KEY, ecodes.BTN_TR, 1)
        ui.syn()
        check(expect_no_key(out, ecodes.KEY_F2), "RB press does not resend KEY_F2")
        ui.write(ecodes.EV_KEY, ecodes.BTN_TL, 0)
        ui.syn()
        check(expect_no_key(out, ecodes.KEY_F2), "LB release keeps KEY_F2 held")
        ui.write(ecodes.EV_KEY, ecodes.BTN_TR, 0)
        ui.syn()
        check(expect_key(out, ecodes.KEY_F2, 0), "RB release sends KEY_F2 up")
    finally:
        if out is not None:
            try:
                out.ungrab()
            except OSError:
                pass
            out.close()
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        ui.close()
        try:
            os.unlink(log_path)
        except OSError:
            pass


def main() -> int:
    if not BIN.exists():
        print(f"build first: {BIN} not found", file=sys.stderr)
        return 1

    with tempfile.NamedTemporaryFile("w", prefix="gm-ps-", suffix=".toml",
                                     delete=False) as f:
        f.write(CONFIG)
        config_path = f.name
    try:
        run_case("Mock DualShock 3 Controller", config_path,
                 analog_triggers=False, hat_x=True, hat_y=True, dpad_buttons=[])
        run_case("Mock DualSense Controller", config_path,
                 analog_triggers=True, hat_x=False, hat_y=False,
                 dpad_buttons=[ecodes.BTN_DPAD_UP, ecodes.BTN_DPAD_DOWN,
                               ecodes.BTN_DPAD_LEFT, ecodes.BTN_DPAD_RIGHT])
        run_case("Mock hybrid d-pad (HAT X + digital up/down)", config_path,
                 analog_triggers=True, hat_x=True, hat_y=False,
                 dpad_buttons=[ecodes.BTN_DPAD_UP, ecodes.BTN_DPAD_DOWN])
        run_case("Mock hybrid d-pad (HAT Y + digital left/right)", config_path,
                 analog_triggers=True, hat_x=False, hat_y=True,
                 dpad_buttons=[ecodes.BTN_DPAD_LEFT, ecodes.BTN_DPAD_RIGHT])
    finally:
        os.unlink(config_path)
    print("PlayStation mapping: OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
