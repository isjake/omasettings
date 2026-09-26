# omasettings

Click-through settings for Omarchy, in the same native Qt6 style as omatimer.
Each page writes a plain config file under `~/.config`, so the file stays the
source of truth: change it in the app or by hand, and the other side follows
live.

## Build

Needs `qt6-base`.

```sh
qmake6 omasettings.pro -o Makefile
make -j$(nproc)
./omasettings
```

`~/.local/bin/omasettings` links to the built binary, and
`~/.local/share/applications/omasettings.desktop` puts it in the launcher.

## Use

```sh
omasettings                          # open the window
omasettings pointer                  # print the current pointer and size
omasettings pointer Yaru 32          # set it without opening the window
```

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

## Colors

From the current Omarchy theme (`~/.local/state/omarchy/current/theme/colors.toml`),
re-tinted live on theme change, the same as omatimer and omacalc.

## Adding a page

Write a `QWidget` for it and call `addPage("Name", widget)` in the
`Omasettings` constructor. Keep its settings in their own file under
`~/.config` so hand edits and the app never fight over a shared file.
