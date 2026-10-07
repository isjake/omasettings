#!/bin/sh
# Builds the app and installs it for the current user (no password needed
# except to install missing build tools).
#   ./install.sh            build and install
#   ./install.sh --link     link to the build here instead of copying (for development)
#   ./install.sh --remove   uninstall
set -e

app=omasettings
cd "$(dirname "$0")"

bin="$HOME/.local/bin/$app"
desktop="$HOME/.local/share/applications/$app.desktop"
icon="$HOME/.local/share/icons/hicolor/scalable/apps/$app.svg"

if [ "$1" = "--remove" ]; then
    rm -f "$bin" "$desktop" "$icon"
    echo "Removed $app."
    exit 0
fi

# Build tools: compiler, make, Qt 6, fontconfig. Checked by what the build
# uses rather than by package name, so a compiler installed without the
# base-devel group still counts.
missing=""
{ command -v g++ && command -v make; } >/dev/null 2>&1 || missing="$missing base-devel"
command -v qmake6 >/dev/null 2>&1 || missing="$missing qt6-base"
[ -f /usr/include/fontconfig/fontconfig.h ] || missing="$missing fontconfig"
if [ -n "$missing" ]; then
    echo "Installing build tools:$missing"
    sudo pacman -S --needed --noconfirm $missing
fi

qmake6 "$app.pro" -o Makefile
make -j"$(nproc)"

mkdir -p "$(dirname "$bin")"
rm -f "$bin"
if [ "$1" = "--link" ]; then
    ln -s "$PWD/$app" "$bin"
else
    install -m 755 "$app" "$bin"
fi
install -Dm 644 "data/$app.desktop" "$desktop"
install -Dm 644 "data/$app.svg" "$icon"
update-desktop-database -q "$(dirname "$desktop")" 2>/dev/null || true

echo "Installed $app. Find it in the app launcher, or run: $app"
case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) echo "Note: add ~/.local/bin to your PATH to run it from a terminal." ;; esac
