# gpadmapper-cpp

Remap a gamepad to pointer, scroll and keyboard actions on Linux. The right
stick moves the pointer, the left stick scrolls in all four directions, and
buttons can be mapped to mouse buttons or key combinations. One small C++ daemon
built on evdev + uinput: ~4 MB RSS and 0% CPU when idle.

Developed with an **8BitDo Ultimate 2C** on **Wayland (Wayfire/wlroots)**, but
it works with any gamepad exposed through evdev, including Xbox-style pads and
PlayStation controllers (DualShock 3/4, DualSense). It also runs on X11.

[Русская версия](README.ru.md)

## What makes it different

- **Smooth four-way scrolling.** On wlroots, high-resolution wheel events are
  often quantized into notches by clients. The default touchpad mode produces a
  continuous finger-scroll gesture instead, so scrolling has no "steps".
- **No phantom right click.** Lifting two virtual fingers at once is a
  two-finger tap when `tap_to_click` is enabled (Wayfire's default). The daemon
  lifts the fingers in stages, so scrolling never turns into a right click.
- **No Chromium fling.** Chromium starts kinetic scrolling if `axis_stop`
  arrives less than 200 ms after the last movement. The daemon holds the
  gesture still for 300 ms, so releasing the stick does not make the page
  coast.
- **Pause combo.** Hold both stick clicks for one second; the daemon stops
  emitting input and the gamepad is free for games. Hold again to resume.
- **Quiet when idle.** The 250 Hz timer only runs while a stick is deflected or
  the pause combo is being held; otherwise the process sleeps in `ppoll`.
- **No grab by default.** Games still see the physical gamepad (`grab = false`).

## Requirements

- Linux with evdev and uinput
- `libevdev` development files (`libevdev-dev` on Debian/Ubuntu,
  `libevdev` on Arch)
- A C++20 compiler and `make`
- Read access to `/dev/input/event*` and write access to `/dev/uinput`.
  Normally this means being in the `input` group:

  ```sh
  sudo usermod -aG input "$USER"   # log out and back in afterwards
  ```

  Or add a udev rule if your distribution does not provide one:

  ```
  KERNEL=="uinput", GROUP="input", MODE="0660"
  ```

## Build and install

### One command

Requires a C++20 compiler, `make`, `pkg-config`, and the libevdev headers (see
[Requirements](#requirements)). This downloads the source, builds it, installs
to `~/.local/bin`, and starts the systemd user service:

```sh
curl -fsSL https://raw.githubusercontent.com/Quieet88/gpadmapper-cpp/main/install.sh | sh
```

Set `PREFIX=/usr/local` to install elsewhere. Without systemd the service step
is skipped.

### From source

```sh
make
make install            # installs to ~/.local/bin (PREFIX=... to change)
```

`sudo make install PREFIX=/usr/local` installs to `/usr/local/bin`.

Run it once to create the default config:

```sh
gpadmapper-cpp -v        # prints the detected device and emitted buttons
```

The daemon creates `~/.config/gpadmapper-cpp/config.toml` on first start if the
file does not exist.

### systemd user service

```sh
mkdir -p ~/.config/systemd/user
cp systemd/gpadmapper-cpp.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now gpadmapper-cpp
```

The unit assumes the binary is in `~/.local/bin`. If you installed it elsewhere,
edit `ExecStart=`.

## Default mapping

| Input                      | Action                  |
| -------------------------- | ----------------------- |
| Right stick                | Pointer                 |
| Left stick                 | Scroll (up/down/left/right) |
| RT                         | Left click              |
| RB                         | Right click             |
| MODE (8BitDo home)         | Super                   |
| START                      | Tab                     |
| SELECT                     | Space                   |
| D-pad                      | Arrow keys              |
| A                          | Enter                   |
| B                          | Escape                  |
| X                          | Alt+Tab                 |
| THUMBL + THUMBR (1 s)      | Pause / resume          |

While paused the daemon emits nothing at all: no pointer motion, no scroll, no
keys. The physical gamepad is still available to games if `grab = false`.
Pause state is not persisted across service restarts.

## PlayStation controllers

The mapping is positional, so the names above are shared with Xbox-style pads:

| Config name | Xbox label | PlayStation label |
| --- | --- | --- |
| `BTN_A` | A | Cross |
| `BTN_B` | B | Circle |
| `BTN_X` | X | Square |
| `BTN_Y` | Y | Triangle |
| `LB` / `RB` | LB / RB | L1 / R1 |
| `LT` / `RT` | LT / RT | L2 / R2 |
| `SELECT` / `START` | Back / Start | Create-Share / Options |
| `MODE` | Guide | PS button |
| `THUMBL` / `THUMBR` | L3 / R3 | L3 / R3 |

- **DualShock 4 / DualSense** expose L2/R2 as `ABS_Z`/`ABS_RZ`; they work as
  `LT`/`RT` with no extra setup.
- **DualShock 3** exposes L2/R2 as digital `BTN_TL2`/`BTN_TR2`; those are
  handled as `LT`/`RT` too. If a pad exposes both paths, they are combined, so
  releasing one does not release the other.
- The d-pad can arrive as `ABS_HAT0X/Y` (PS pads usually do) or as
  `BTN_DPAD_*` on some clones; both are supported, with HAT taking precedence.
- If several pads are connected, pin the one you want with
  `[device] name = "DualSense"` (or another substring of its evdev name).
- Steam Input can replace the physical pad with a virtual Xbox controller. If
  buttons behave unexpectedly, disable Steam Input for that controller.
- Worn DS4 sticks may drift; raise `[pointer] deadzone` to `0.10–0.15` if the
  pointer crawls with the stick at rest.

`tests/ps_mock.py` exercises the DS3 and DualSense paths without hardware,
including shared-key ownership and hybrid HAT + `BTN_DPAD_*` d-pads.

DualShock 3 was not tested on physical hardware: only a mock device with digital
triggers and `BTN_DPAD_*` was used.

## Configuration

Edit `~/.config/gpadmapper-cpp/config.toml` and restart the service:

```sh
systemctl --user restart gpadmapper-cpp
```

See [`config.example.toml`](config.example.toml) for a fully commented file.
The parser understands a TOML subset: sections, `key = value`, quoted strings,
numbers, `true`/`false`, and `#` comments. Unknown keys are ignored.

### Button actions

An action is one of:

- a mouse button: `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`, `BTN_SIDE`,
  `BTN_EXTRA`
- any `KEY_*`, for example `KEY_ENTER`
- a key combination joined with `+`, for example `KEY_LEFTALT+KEY_TAB`
- `none` to disable the button

Pad button names: `BTN_A`, `BTN_B`, `BTN_X`, `BTN_Y`, `LB`, `RB`, `LT`, `RT`,
`SELECT`, `START`, `MODE`, `THUMBL`, `THUMBR`, `DPAD_UP`, `DPAD_DOWN`,
`DPAD_LEFT`, `DPAD_RIGHT`.

## Scrolling: `touchpad` vs `wheel`

```toml
[scroll]
mode = "touchpad"   # default: smooth gesture scrolling
# mode = "wheel"    # classic REL_WHEEL / REL_HWHEEL
```

**`touchpad` (default).** The daemon creates a second virtual device,
`gpadmapper-scroll`, with two multitouch slots. Moving the left stick moves two
virtual fingers; libinput turns that into a continuous
`POINTER_SCROLL_FINGER` gesture that every toolkit renders smoothly. The
touchpad speed is calibrated so `max_speed` means the same number of wheel
notches per second as in wheel mode (`TP_UNITS_PER_NOTCH = 115`). The virtual
mouse does not emit wheel events in this mode.

**`wheel`.** No virtual touchpad is created; the mouse emits `REL_WHEEL`,
`REL_HWHEEL`, and their hi-res variants when `hi_res = true`. Use this on X11
or if touchpad emulation does not fit your setup.

### The two hard parts

Both behaviors come from the same gesture tail. After the stick returns to the
center, the daemon does **not** lift the fingers immediately:

1. Both fingers stay still for **300 ms**. Chromium computes fling velocity from
   the time between the last motion and `axis_stop`; if that gap is over 200 ms,
   the velocity is zero and no momentum scrolling starts.
2. The second finger is lifted, and the first one stays for another **250 ms**.
   If both fingers were lifted together, libinput would recognize a two-finger
   tap, which `tap_to_click` turns into a right click.

If the stick leaves the deadzone during this 550 ms tail, the same gesture
continues without a gap. The constants are at the top of `gpadmapper-cpp.cpp`:
`TP_FLING_GUARD`, `TP_TAP_GUARD`, `TP_UNITS_PER_NOTCH`.

## Tuning the sticks

The response is radial:

```
magnitude = hypot(x, y)
strength  = ((magnitude - deadzone) / (1 - deadzone)) ^ curve
```

- `deadzone` — deflection below this is ignored; the rest is rescaled to 0..1.
- `curve` — `1` is linear; values above `1` give finer control near the center.
- `max_speed` — pointer pixels per second, or wheel notches per second for
  scrolling. In touchpad mode the calibration keeps the same physical speed.

The shipped deadzones (`0.06` for the pointer, `0.10` for scroll) were measured
on a hall-effect 8BitDo Ultimate 2C: at rest the axes report exactly `0`/`-1`,
the kernel `flat` value is 128 of 32768 (0.39%), and `fuzz` is 16 (0.05%). That
leaves a comfortable margin without a mushy center. Raise the deadzone if your
sticks drift.

## Testing

```sh
make test              # selftest in both scroll modes
./gpadmapper-cpp --selftest
```

`--selftest` creates the virtual devices, feeds a synthetic stick vector, and
checks that pointer motion or touchpad motion comes out. It does not need a
physical gamepad. The exit code is 0 on success.

Useful flags:

```
gpadmapper-cpp [-v] [--selftest] [--config PATH] [--device PATH]
```

- `-v` — print the detected device and every emitted button.
- `--config PATH` — use another config file.
- `--device PATH` — use a specific `/dev/input/eventN` instead of auto-detection.

## Troubleshooting

- **`permission denied` on `/dev/uinput` or `/dev/input/event*`** — add your
  user to the `input` group (see Requirements) and re-login.
- **Gamepad not found** — run `gpadmapper-cpp -v`; it prints candidate devices.
  Use `--device` to pin one while debugging. Event numbers change after
  reconnect, so the daemon searches by name on every connect.
- **Scrolling is stepped** — you are probably in `wheel` mode or the client
  ignores hi-res wheel events. Switch to `mode = "touchpad"`.
- **False right clicks or momentum scrolling** — make sure you are running the
  current binary; both guards are implemented in the touchpad gesture tail.
- **The gamepad does not work in a game** — `grab = true` hides it from games.
  Keep `grab = false` unless you want exclusive control.

## Performance

Measured on the development machine (8BitDo Ultimate 2C, Wayfire):

- Resident memory: ~4 MB
- CPU when idle: 0.0%
- CPU with both sticks fully deflected: 0.6%
- Binary size: ~150 KB

The gamepad reports at about 1 kHz; the daemon integrates at 250 Hz
(`TICK = 0.004`). Real touchpads report at 100–125 Hz, so the 250 Hz output adds
at most ~4 ms of latency. If you can see stepping in pointer motion, change
`TICK` to `0.002` (500 Hz) and rebuild.

## Repository layout

```
gpadmapper-cpp.cpp        daemon source
install.sh               one-command install script
config.example.toml      commented example config
Makefile                 build / install / test
systemd/                 user service unit
tests/run.sh             selftest in both scroll modes
tests/ps_mock.py         PlayStation mapping test (mock DS3/DualSense)
```

## License

MIT — see [LICENSE](LICENSE).
