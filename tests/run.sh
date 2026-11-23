#!/bin/sh
# Run the built-in selftest in both scroll modes.
#
# The selftest injects real input events into the session, so the speeds are
# reduced here: the pointer moves a couple of dozen pixels and the wheel turns
# two notches instead of flying across the screen.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

[ -x ./gpadmapper-cpp ] || make

tmp=$(mktemp -d)
service_was_active=0
cleanup() {
    if [ "$service_was_active" = 1 ]; then
        systemctl --user start gpadmapper-cpp || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT

status=0
for mode in touchpad wheel; do
    sed -E \
        -e "s/^mode[[:space:]]*=.*/mode = \"$mode\"/" \
        -e "/^\[pointer\]/,/^\[/ s/^max_speed[[:space:]]*=.*/max_speed = 60/" \
        -e "/^\[scroll\]/,/^\[/ s/^max_speed[[:space:]]*=.*/max_speed = 4/" \
        config.example.toml > "$tmp/config.toml"
    printf '%s\n' "--- scroll mode: $mode"
    ./gpadmapper-cpp --config "$tmp/config.toml" --selftest || status=$?
done

# The config the daemon writes on first run must match the documented defaults.
printf '%s\n' "--- built-in default config"
mkdir -p "$tmp/home"
HOME="$tmp/home" timeout 2 ./gpadmapper-cpp --device /dev/null >/dev/null 2>&1 || true
default_cfg="$tmp/home/.config/gpadmapper-cpp/config.toml"
if [ ! -f "$default_cfg" ]; then
    printf '%s\n' "default config was not created"
    status=1
else
    grep -q '^mode[[:space:]]*=[[:space:]]*"touchpad"' "$default_cfg" || {
        printf '%s\n' "default config: touchpad mode missing"
        status=1
    }
    grep -q '^deadzone[[:space:]]*=[[:space:]]*0\.06' "$default_cfg" || {
        printf '%s\n' "default config: pointer deadzone is not 0.06"
        status=1
    }
    grep -q '^deadzone[[:space:]]*=[[:space:]]*0\.10' "$default_cfg" || {
        printf '%s\n' "default config: scroll deadzone is not 0.10"
        status=1
    }
fi

# PlayStation mapping: mock DS3 (digital triggers, HAT) and DualSense
# (analog triggers, BTN_DPAD_*), no physical controller needed.
printf '%s\n' "--- PlayStation mapping"
if python3 -c 'import evdev' 2>/dev/null; then
    # The mock pad looks like a real controller. A running service could grab it
    # first and send the synthetic keys to the desktop, so pause it.
    if command -v systemctl >/dev/null 2>&1 &&
       systemctl --user is-active --quiet gpadmapper-cpp; then
        service_was_active=1
        systemctl --user stop gpadmapper-cpp || true
    fi
    python3 tests/ps_mock.py || status=$?
else
    printf '%s\n' "skipped: python-evdev is not installed"
fi

exit "$status"
