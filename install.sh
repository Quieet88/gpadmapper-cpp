#!/bin/sh
# Download, build and install gpadmapper-cpp.
#
#   curl -fsSL https://raw.githubusercontent.com/Quieet88/gpadmapper-cpp/main/install.sh | sh
#
# Installs the binary to ~/.local/bin (override with PREFIX=...), optionally
# installs and starts the systemd user service. No root required.

set -eu

REPO="Quieet88/gpadmapper-cpp"
REF="${REF:-main}"
PREFIX="${PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"

say() { printf '%s\n' "$*"; }
die() { printf 'ОШИБКА: %s\n' "$*" >&2; exit 1; }

for cmd in curl tar make; do
    command -v "$cmd" >/dev/null 2>&1 || die "нужна команда '$cmd'"
done
command -v pkg-config >/dev/null 2>&1 || die "нужен pkg-config"
pkg-config --exists libevdev 2>/dev/null ||
    die "нужен libevdev (Debian/Ubuntu: sudo apt install libevdev-dev, Arch: libevdev)"
if ! command -v c++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1; then
    die "нужен компилятор C++20 (g++)"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

say "Скачиваю $REPO ($REF)..."
curl -fsSL -o "$tmp/src.tar.gz" \
    "https://github.com/$REPO/archive/refs/heads/$REF.tar.gz" ||
    die "не удалось скачать архив"
tar -xzf "$tmp/src.tar.gz" -C "$tmp" || die "не удалось распаковать архив"

src=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | head -n 1)
[ -n "$src" ] && [ -f "$src/Makefile" ] || die "в архиве нет исходников"

say "Собираю..."
make -C "$src" >/dev/null || die "сборка не удалась"

say "Устанавливаю в $BINDIR..."
make -C "$src" install PREFIX="$PREFIX" >/dev/null || die "установка не удалась"

if command -v systemctl >/dev/null 2>&1 && [ -n "${XDG_RUNTIME_DIR:-}" ]; then
    unit_dir="$HOME/.config/systemd/user"
    mkdir -p "$unit_dir"
    sed "s|^ExecStart=.*|ExecStart=$BINDIR/gpadmapper-cpp|" \
        "$src/systemd/gpadmapper-cpp.service" > "$unit_dir/gpadmapper-cpp.service"
    systemctl --user daemon-reload
    systemctl --user enable gpadmapper-cpp >/dev/null 2>&1 || true
    if systemctl --user restart gpadmapper-cpp; then
        say "Сервис gpadmapper-cpp запущен."
    else
        say "Не удалось запустить сервис; запусти вручную: $BINDIR/gpadmapper-cpp"
    fi
else
    say "systemd недоступен. Запуск вручную: $BINDIR/gpadmapper-cpp"
fi

say "Готово: $BINDIR/gpadmapper-cpp"
