# Omasettings

Click-through settings app for my Omarchy desktop. Native Qt6, same style as Omatimer. Every page saves to a plain config file, so the app and hand edits stay in sync.

![Omasettings, mouse pointer page](screenshot.png)

**Code:** `~/Work/omasettings` · [github.com/isjake/omasettings](https://github.com/isjake/omasettings) (private)
**Started:** 2026-09-26

## Mouse pointer page

- Each installed pointer style shows as a card with four shapes: arrow, hand, text, loading.
- Click a card to switch. Hover over one to try it on first.
- Sizes: 24, 32, 40, 48, 64. Currently **Yaru at 32**.
- Colors follow the Omarchy theme, live.
- Terminal: `omasettings pointer Bibata-Modern-Ice 40`, or `omasettings pointer` to see the current one.
- In the app launcher as **Omasettings**.

## Where things live

| What | Where |
|---|---|
| Pointer setting | `~/.config/hypr/cursor.lua` (loaded from `hyprland.lua`) |
| Pointer styles | `~/.local/share/icons` (16 added: Bibata, Catppuccin, GoogleDot, macOS, Phinger) |
| App binary | `~/.local/bin/omasettings` → `~/Work/omasettings/omasettings` |
| Launcher entry | `~/.local/share/applications/omasettings.desktop` |

## Ideas for next pages

- [ ] Keyboard
- [ ] Display
- [ ] Sound
- [ ] Fonts
