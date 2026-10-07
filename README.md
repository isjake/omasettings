# omasettings

Click-through settings for Omarchy, in the same native Qt6 style as omatimer.
Each page writes a plain config file under `~/.config`, so the file stays the
source of truth: change it in the app or by hand, and the other side follows
live.

![omasettings, mouse pointer page](docs/screenshot.png)

## What it is

A native Qt app in the same style as omatimer. Its first page is the mouse
pointer.

**What it does:**
- Each installed pointer style shows as a card with four of its shapes
  (arrow, hand, text, loading), drawn at your chosen size.
- Click a card to switch to it. Hover over a card to try its arrow before you
  pick it.
- The size buttons are 24, 32, 40, 48 and 64.
- Colors follow your Omarchy theme and update when you change themes.
- You can also set it from the terminal:
  `omasettings pointer Bibata-Modern-Ice 40`.
- You can open it from the app launcher like any other app.

**The file is still in charge:** the pointer setting lives in its own file,
`~/.config/hypr/cursor.lua`, which `hyprland.lua` loads. The app saves to that
file, and if you edit the file by hand, the app picks up the change right
away.

**Styles:** a fresh Omarchy has just Adwaita. **Get more styles** downloads
16 more (Bibata, Catppuccin, GoogleDot, macOS and Phinger) into
`~/.local/share/icons`, so no password is needed, and they appear straight
away. See [Installing more pointer styles](#installing-more-pointer-styles).

**Fresh installs:** `cursor.lua` only counts if `hyprland.lua` loads it, and a
stock one doesn't. The first time a pointer is saved, the app adds
`require("hypr.cursor")` to the end of `hyprland.lua` (once), so the choice
survives a restart.

**Other pages:** Windows, Keyboard & touchpad, and Display (see
[Pages](#pages)). Sound would fit next. See [Adding a page](#adding-a-page).

## Install

```sh
git clone https://github.com/isjake/omasettings.git
cd omasettings && ./install.sh
```

It installs any missing build tools (`qt6-base`, `base-devel`; asks for your
password only then), builds the app, and puts it in your app launcher. Nothing
goes outside your home folder. `./install.sh --remove` uninstalls;
`./install.sh --link` links to the build in this folder instead of copying it,
so `make` updates the installed app (handy while working on it).

## Build by hand

```sh
qmake6 omasettings.pro -o Makefile
make -j$(nproc)
./omasettings
```

## Use

```sh
omasettings                          # open the window
omasettings pointer                  # print the current pointer and size
omasettings pointer Yaru 32          # set it without opening the window
omasettings --page windows           # open on a page (any word of its name)
```

Hyprland's rules in `~/.config/hypr/hyprland.lua` open it floating and
centered at 1040×680, since a narrow tile is too tight for its pages.

Esc or Ctrl+Q closes the window.

## Pages

### Mouse pointer

Every installed cursor theme shows as a card with its arrow, hand, text and
wait shapes, drawn at the size you've picked. Click a card to switch; hover
one to try its arrow on first. Sizes are 24 (Omarchy's default) to 64.

- **File:** `~/.config/hypr/cursor.lua`, loaded by `require("hypr.cursor")`
  at the end of `~/.config/hypr/hyprland.lua`. The app rewrites it whole.
- **Live switch:** `hyprctl setcursor` for the pointer on screen, and
  `gsettings` for GTK apps, which read their own setting. Apps that were
  already open may keep the old pointer until they're reopened.
- **Themes:** found in `~/.local/share/icons`, `~/.icons` and
  `/usr/share/icons` (any folder with a `cursors/` inside). Drop a new theme
  in `~/.local/share/icons` and it appears next launch. Shapes a theme leaves
  out are borrowed from its `Inherits=` parent, as libXcursor does.

Cursor files are Xcursor format, which Qt can't read, so a small reader in
`main.cpp` pulls out the first frame at the nearest size.

### Windows, Keyboard & touchpad, Display

Sliders and On/Off switches for the settings in Omarchy's own files. Each change
is saved a beat after you stop dragging, then `hyprctl reload` applies it; if
Hyprland rejects the file, the error shows under the page.

| Page | File | Settings |
|---|---|---|
| Windows | `hypr/looknfeel.lua` | gap between windows, each screen edge, border, corner rounding, dimming, animations |
| Keyboard & touchpad | `hypr/input.lua` | key repeat speed and delay, pointer speed, natural scrolling, scroll speed, ignore while typing, two-finger right-click |
| Display | `hypr/monitors.lua` | screen scale (`omarchy_monitor_scale`; Auto is Omarchy's default) |

**Trying the defaults:** a ↺ shows beside every setting the file changes;
it takes that line out (or, for monitors.lua's scale, puts back the stock
`"auto"`), so Omarchy's default applies. **Try Omarchy defaults** swaps in a
fresh install's copy of the whole file (from `$OMARCHY_PATH/config/hypr/`),
keeping yours in `~/.local/state/omasettings/<file>.mine`; **Undo, back to
mine** puts it back. In the pointer downloads, **Remove** deletes a style's
folder from `~/.local/share/icons`, switching to Adwaita if it was in use.

The app's font is the system monospace font and follows `omarchy font set`
live, the same as omatimer.

Sliders jump to where you click, and scrolling the page over one scrolls the
page instead of changing it.

These files are hand-written with comments, so the app never rewrites them
whole. A small Lua reader in `main.cpp` (`scanLua` / `setLuaValue`) finds
`key = value` inside the nested tables and swaps just that value. A setting
the file doesn't have yet is added to the table it belongs in. Its current
value then comes from `hyprctl getoption`, which is Omarchy's default.

Some settings aren't in those files and are changed directly:

- **A lone window's shape** (Windows): Omarchy's Super+Ctrl+Backspace toggle,
  `~/.local/state/omarchy/toggles/hypr/single-window-aspect-ratio.lua`. Fill
  removes the file; Square, 4:3, 3:2 and 16:9 write it. The shortcut still works.
- **Keyboard light** and **Screen brightness**: `brightnessctl`, the same as
  the `kbhigh`/`kblow` aliases. They follow the brightness keys too.
- **Text size (GTK apps)**: gsettings `text-scaling-factor`.
- **System font**: runs `omarchy font set`.

## Colors

From the current Omarchy theme (`~/.local/state/omarchy/current/theme/colors.toml`),
re-tinted live on theme change, the same as omatimer and omacalc.

## Adding a page

Write a `QWidget` for it and call `addPage("Name", widget)` in the
`Omasettings` constructor. For Hyprland settings, a `HyprPage` with a list of
`HyprField`s (Lua path, kind, range) is all it takes; `addRow` and
`addLiveSlider` add rows for things outside the file.

## Installing more pointer styles

The **Get more styles** button does this for you. It downloads with `curl`,
unpacks with `bsdtar` (both ship with every Arch install, and bsdtar handles
zip and every tar type), and moves any folder with a `cursors/` inside into
`~/.local/share/icons`. The list is `cursorDownloads()` in `main.cpp`, from
these GitHub releases:

| Style | Source |
|---|---|
| Bibata Modern Amber / Classic / Ice | [ful1e5/Bibata_Cursor](https://github.com/ful1e5/Bibata_Cursor/releases) |
| Catppuccin Mocha Dark / Light / Mauve, Latte Light | [catppuccin/cursors](https://github.com/catppuccin/cursors/releases) |
| GoogleDot Black / White / Blue | [ful1e5/Google_Cursor](https://github.com/ful1e5/Google_Cursor/releases) |
| macOS, macOS White | [ful1e5/apple_cursor](https://github.com/ful1e5/apple_cursor/releases) |
| Phinger dark / light (plus left-handed) | [phisch/phinger-cursors](https://github.com/phisch/phinger-cursors/releases) |

```sh
curl -sLO https://github.com/ful1e5/Bibata_Cursor/releases/download/v2.0.7/Bibata-Modern-Ice.tar.xz
tar -xf Bibata-Modern-Ice.tar.xz -C ~/.local/share/icons
```

## License

MIT. See [LICENSE](LICENSE).
