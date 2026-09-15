# hyprwobbly — Wobbly Windows for Hyprland

KDE/Compiz-style wobbly windows for **Hyprland 0.56.2**, implemented as a
C++ plugin. Grabbing a floating window deforms it elastically: contents lag
behind the cursor, overshoot on direction changes, and settle naturally after
release.

Hyprland 0.56.2 has no native wobble effect (`decoration:wobble` does not
exist), and [hyprwm/hyprland-plugins](https://github.com/hyprwm/hyprland-plugins)
has none either — so a plugin is genuinely required. This is a port of
[colonelpanic8/hyprwobbly](https://github.com/colonelpanic8/hyprwobbly)
(targeting the pre-0.56 API) to the current plugin interface.

## Features

- **Floating windows only, while actually moved** — no wobble on open, close,
  resize, or idle (KDE behavior), enforced by an explicit state machine:
  `IDLE → DRAGGING → SETTLING → IDLE`.
- **Real application pixels are deformed** — the window's actual texture is
  drawn through a GPU mesh deformation; transparency, opacity, rounded
  corners, shadows and fractional monitor scaling are preserved.
- **Grab-point aware deformation** — the deformation depends on where inside
  the window you grab it: the area under the cursor follows the mouse almost
  rigidly while distant regions lag, stretch and oscillate (like KDE/Compiz).
- **KWin/Compiz spring-mass-damper physics** — an 8x8 spring mesh integrated
  with semi-implicit Euler at fixed ~10ms substeps (framerate-independent).
  Physics is driven *only* by real window motion: no synthetic impulses, a
  stationary window always converges to zero displacement and zero velocity.
- **Zero idle cost** — no per-frame allocations or shader rebuilds, no
  unconditional damage, and transformers are detached entirely once a window
  settles, so undragged windows pay nothing.

## Requirements

- Arch Linux (or similar) with the `hyprland` package installed — the plugin
  builds against the installed dev headers (`pkg-config --cflags hyprland`).
- Hyprland **0.56.2**. Plugins are ABI-checked; after any Hyprland update,
  rebuild with `build.sh` or the plugin will refuse to load.

## Build & Install

```sh
./build.sh
```

This rebuilds the plugin against the installed Hyprland headers and installs
the resulting `.so` to `~/.config/hypr/plugins/hyprwobbly.so` via `mv` (new
inode) so a rebuild can never corrupt a running session.

## Loading the plugin

```sh
hyprctl plugin load ~/.config/hypr/plugins/hyprwobbly.so
```

Or load it automatically at session start from your Hyprland config, e.g.:

```sh
exec-once = hyprctl plugin load ~/.config/hypr/plugins/hyprwobbly.so
```

> Note: if the currently loaded plugin build predates the hook-removal fix in
> `PLUGIN_EXIT`, unloading it will crash the compositor. In that case, end the
> Hyprland session and let the new build load on login.

## Configuration

All keys live under `plugin:hyprwobbly:` and are settable at runtime:

```sh
hyprctl keyword plugin:hyprwobbly:spring_k 0.05
```

| Key | Default | Description |
|---|---|---|
| `enabled` | `1` | Master switch. |
| `mode` | `floating` | `always` (all windows), `style` (windows with a wobble animation style), or `floating` (floating windows only). |
| `spring_k` | `0.05` | Spring stiffness (KWin `stiffness`). Lower = more lag, slower. |
| `friction` | `0.85` | Velocity **retention** per 10ms step (KWin `drag`). 0.85 = lose 15% per step. |
| `mass` | `1.0` | Point mass in F = m·a. |
| `move_factor` | `0.25` | Scales position integration (the lag amount). |
| `grab_falloff` | `1.0` | Exponent of the impulse falloff cone around the grab point (1–4). Higher = wider rigid region near the cursor. |
| `resize_factor` | `0.0` | Reserved for resize deformation (currently resize silently re-grids, no wobble). |
| `max_warp` | — | Reserved. |
| `deform_blur_matte` | `0` | Deform the blur matte pass of blurred windows (costs an extra full-monitor clear + mesh draw per frame). Default off matches core's non-transformed blur. |
| `test_identity` | `0` | Debug harness: runs the full custom render path with **zero** deformation — output must be pixel-identical to normal Hyprland. |
| `grid_width` / `grid_height` | `8` / `8` | Physics mesh resolution. |
| `tiles_x` / `tiles_y` | `16` / `16` | Render tile resolution. |

## Known limitations

- **Popups / subsurfaces**: correct rendering is prioritized over deforming
  them; they are not deformed in v1.
- **Resize** does not wobble (by design); the physics mesh is silently
  re-gridded to the new size.
- While a window wobbles, the transformed-render path itself (plus the blur
  pipeline for blurred windows) is area-scaled GPU work that belongs to core,
  so very large blurred windows are inherently heavier while dragging.

## Development

- `src/` contains the plugin sources (`main.cpp`, `Wobbly*.cpp/hpp`,
  `shaders.hpp`, `Makefile`).
- Debug logging goes to `hyprctl rollinglog` at `LOG_DEBUG` level, including
  per-window state, impulses, grab points and transform stats.
- `AGENTS.md` is the authoritative spec, architecture description, and
  progress record — read it before touching anything.
