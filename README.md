# window-shadow

A lightweight X11 daemon that renders drop shadows behind Electron and other
client-side-decorated (CSD) windows that don't provide their own.

## The Problem

On Linux/X11, many Electron apps (and other apps using `frame: false` or custom
titlebars) use client-side decorations but don't render window shadows. This
makes them look flat and disconnected from the rest of the desktop.

## How It Works

`window-shadow` monitors all toplevel windows and detects those using CSD by
checking `_MOTIF_WM_HINTS` (decorations == 0). For each such window, it creates
a transparent ARGB shadow window positioned just behind it. The shadow is
rendered using a 3-pass box blur approximation of a gaussian blur.

Key features:
- **Non-invasive**: Does not modify target windows or force server-side decorations
- **Click-through**: Shadow windows have an empty XFixes input shape
- **Stacking-aware**: Shadows stay just below their target window
- **Reparenting WM support**: Works with both reparenting (Openbox, XFWM4) and
  non-reparenting (i3, dwm, bspwm) window managers
- **GTK CSD filtering**: Skips GTK apps that provide their own shadows via
  `_GTK_FRAME_EXTENTS`
- **Dynamic**: Responds to window creation, destruction, move, resize, and
  decoration property changes in real time

## Requirements

- A running **X11 compositor** (picom, compton, xcompmgr, or built-in like
  mutter/kwin) for ARGB transparency
- Build dependencies:
  - `libx11-dev`
  - `libxext-dev`
  - `libxfixes-dev`
  - `libxrender-dev`
  - `libcairo2-dev`
  - `pkg-config`

On Arch: `sudo pacman -S libx11 libxext libxfixes libxrender cairo`
On Ubuntu/Debian: `sudo apt install libx11-dev libxext-dev libxfixes-dev libxrender-dev libcairo2-dev pkg-config`
On Fedora: `sudo dnf install libX11-devel libXext-devel libXfixes-devel libXrender-devel cairo-devel`

## Building

```bash
make
```

## Usage

```bash
# Run with defaults
./window-shadow

# Custom shadow appearance
./window-shadow --radius 50 --opacity 0.6 --offset-y 12

# Subtle shadow
./window-shadow --radius 20 --opacity 0.3 --offset-y 4

# Colored shadow
./window-shadow --color 1a1a2e --opacity 0.4
```

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--radius N` | `40` | Shadow blur radius in pixels |
| `--opacity F` | `0.5` | Shadow opacity (0.0–1.0) |
| `--offset-x N` | `0` | Horizontal shadow offset |
| `--offset-y N` | `8` | Vertical shadow offset |
| `--color RRGGBB` | `000000` | Shadow color in hex |

### Autostart

Add to your `.xinitrc`, window manager config, or XDG autostart:

```bash
window-shadow &
```

## How CSD Detection Works

A window is considered CSD (needing a shadow) if:

1. It has `_MOTIF_WM_HINTS` with decorations set to 0 (this is what Electron
   sets when using `frame: false` or custom titlebars)
2. It is a `_NET_WM_WINDOW_TYPE_NORMAL` or `_DIALOG` (or has no type set)
3. It does **not** have `_GTK_FRAME_EXTENTS` with large values (≥10px), which
   would indicate the app renders its own shadow (e.g., GNOME apps)
4. It is not an override-redirect window (popups, tooltips, menus)

## License

Public domain / unlicense. Do whatever you want with it.
