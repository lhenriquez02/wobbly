# Wobbly Windows for Hyprland (hyprwobbly port)

KDE/Compiz-style wobbly windows for Hyprland 0.56.2 on Arch Linux.
This file is the authoritative spec + progress record. Read it before touching anything.

## THE TASK (original requirements)

- Compiz/KDE-style wobbly windows: grabbing a floating window deforms it
  elastically; contents lag behind the cursor, overshoot on direction change,
  settle naturally after release.
- **Floating windows only**, and **only while actually moved** (KDE behavior:
  no wobble on open/close/resize/idle).
- Real application pixels must be deformed — a white rectangle is a failed
  renderer, not partial success.
- No synthetic impulses; physics may only be driven by real window motion
  (position delta / user drag). Stationary window MUST converge to
  displacement = 0, velocity = 0.
- Explicit state machine: IDLE (no physics, no damage, no impulses) /
  DRAGGING (physics from real movement, damage while active) / SETTLING
  (converge, then snap to zero and stop redrawing).
- Framerate-independent integration (semi-implicit Euler, substepping, dt clamp).
- Framerate-independent physics — no 60 Hz constants.
- Per-window independent physics state; clean up on window destroy; no
  dangling pointers.
- Popups/subsurfaces: preserving correct rendering is more important than
  deforming them (document limitations in v1).
- Performance: no per-frame allocations, no shader rebuilds, reuse buffers;
  zero work when idle. An idle iGPU burn of ~50% with the
  broken build — that class of bug is a release blocker.
- Development safety: no repeated crashes of the main compositor. (Nested
  Hyprland sessions are NOT possible on this setup — Hyprland has no nested
  backend — so all testing happens on the live session and must be careful.)
- Config keys: `plugin:hyprwobbly:enabled`, `mode` (always/style/floating),
  spring_k, friction, mass, move_factor, resize_factor, max_warp.
- Priority order: correct rendering of the real app > correct lifecycle/motion
  detection > stable physics > correct settling > aesthetics > config polish.
- Mandatory test sequence: identity render (deformation = 0 must look exactly
  like normal Hyprland) BEFORE adding visible deformation. Then slow drag,
  fast drag, release, repeated start/stop (no drift), multi-window
  independence, resize stability, refresh-rate independence.
- Rendering must retain: real texture, transparency, opacity, rounded corners,
  orientation, UVs, clipping, fractional monitor scaling (must work at
  fractional scales, e.g. 1.5), shadows where compatible.

## ENVIRONMENT

- Hyprland 0.56.2 (tag v0.56.2, commit efb50993780079460b0cbed1363e2166a2de1d9f)
  - ABI string: `efb50993780079460b0cbed1363e2166a2de1d9f_aq_0.15_hu_0.14_hg_0.5_hc_0.1_hlg_0.6`
  - Plugins are ABI-checked: `__hyprland_api_get_hash()` must equal the client
    hash or the plugin refuses to load (throws in PLUGIN_INIT).
- **Hyprland 0.56.2 has NO native wobble.** `decoration:wobble` does not exist;
  grep of installed headers and full v0.56.2 source finds nothing. A plugin is
  genuinely required (checked: hyprwm/hyprland-plugins has no wobble either).
- Dev headers come from the Arch `hyprland` package (`pkg-config --cflags
  hyprland` → /usr/include/hyprland). Build: `make all` in src/.
- hyprpm is installed but was not used for the build (sudo/polkit friction);
  manual `hyprctl plugin load <path>` works.
- Upstream base: https://github.com/colonelpanic8/hyprwobbly (master) —
  targets the pre-0.56 API; this project is the 0.56.2 port.
- 0.56 API notes discovered during the port (see src/ for usage):
  - `IWindowTransformer` is now `Render::IWindowTransformer`
    (src/render/transformer/Transformer.hpp); hooks: `transform(SP<IFramebuffer>)`,
    `preWindowRender(CSurfacePassElement::SRenderData*)`.
  - Window position/size vars (`m_realPosition`/`m_realSize`) are protected;
    use `position(Desktop::View::IGeometric::GEOMETRIC_CURRENT)` /
    `size(...)` and `positionAnimation()` (for animation style).
  - Window list: `Desktop::windowState()->windows()` (not `g_pCompositor->m_windows`).
  - Animation manager: `Animation::CHyprAnimationManager`.
  - Transformers pipeline (src/render/Renderer.cpp `renderWindow` →
    `CTransformedWindowPassElement` → ElementRenderer `drawTransformedWindow`):
    the window pass is rendered into a pooled work buffer, then each
    transformer's `transform(fb)` is chained, and the final fb is composited.
    `preWindowRender` is called per rendered frame for every window that has
    transformers attached.
  - Config values registered via `HyprlandAPI::addConfigValueV2` (e.g.
    `plugin:hyprwobbly:mode`); settable from the session config via
    `hl.config({ plugin = { hyprwobbly = {...} } })` (plain `hyprctl keyword`
    does NOT work for plugin values).
  - Drag state (real, not heuristic):
    `g_layoutManager->dragController()->target()` → `ITarget::window()`
    (src/layout/supplementary/DragController.hpp).
  - Logging: `Log::logger->log(Hyprutils::CLI::LOG_DEBUG, ...)`; visible via
    `hyprctl rollinglog` (debug level already enabled on this system).
    NOTE: `LOG_INF` no longer exists in Hyprutils 0.14 (LOG_TRACE, LOG_DEBUG,
    LOG_WARN, LOG_ERR, LOG_CRIT).

## ARCHITECTURE (current implementation)

```
window.open / window.floating events -> attach CWobblyTransformer (floating only)
Hyprland renders window -> pooled work buffer (normal rendering, real pixels)
  -> preWindowRender(&renderData)  [geometry observation + impulses]
  -> transform(fb)                 [mesh-deformed GPU draw of the real texture]
  -> normal compositor output
tick timer (wl_event_loop): physics substeps + damage only while active
```

- Per-window physics: 4x4 spring-mass grid, semi-implicit Euler, substepped.
- Impulses: only from real position delta (`fullBoxPx.pos() -
  m_sourceBoxPx.pos()`), only while the pointer is inside the window box.
- State lives per-window in CWobblyTransformer; transformers erased on
  destroy/unload/tile.

## CRASH HISTORY & SAFETY RULES

- **Crash root cause (proven):** `PLUGIN_INIT` installs a trampoline hook on
  `CHyprAnimationManager::styleValidInConfigVar` but the original
  `PLUGIN_EXIT` never called `HyprlandAPI::removeFunctionHook`. After unloading
  the plugin, the next config-style validation jumped into freed plugin memory
   -> SIGSEGV. **This is now fixed in
  the current source** (removeFunctionHook + delete in PLUGIN_EXIT).
- **RULE: never `hyprctl plugin unload` an instance of the OLD build.** The
  build currently loaded in the running session (handle varies) predates the
  fix and will segfault the compositor on unload. Swapping the plugin requires
  ending the Hyprland session and letting the new build load on login.
- Opt-out switch: `touch ~/.config/hypr/plugins/disable-wobbly` (or any
  agreed sentinel file) prevents the session config from loading the plugin
  at startup (no code edit needed).
- The .so embeds a version-mismatch check; after any Hyprland update, rebuild:
  `~/Projects/wobbly/build.sh` (builds against the installed headers via
  `pkg-config --cflags hyprland`).

## CURRENT STATUS

Plugin is LIVE-VERIFIED WORKING (current session):
no crashes, floating windows wobble and settle. The abrupt "snap to static"
was the 1s force-settle failsafe (hard resetModel) firing because the old
spring constants converged far too slowly — fixed with a smooth exponential
decay failsafe (99.99%/s, framerate-independent) and retuned defaults
(springK 18→90, friction 8→12, mass 12→4, moveFactor 0.65→0.9) so natural
settle happens first. NOT yet verified: the full test plan + idle GPU.

Progress so far:

1. Ported colonelpanic8/hyprwobbly to the 0.56.2 API (compiles clean against
   installed headers; ABI hash check passes).
2. Added a `floating` mode (upstream only had `always`/`style`): only floating
   windows attach transformers; a `window.floating` event listener
   attaches/detaches on toggle so tiled windows never pay the transformed-
   render path cost.
3. KDE-style motion gating: resize impulses removed; move impulses only when
   the pointer is inside the window box (drag) — plus real drag state available
   via `g_layoutManager->dragController()` (instrumented, used by logs).
4. Idle-wobble mitigation: no unconditional `damage()` per frame; 1s
   force-settle timeout; tick timer idles at 200 ms when nothing is active.
5. White-rectangle fix attempt #1 (REPLACED by #8): output framebuffer changed
   from pooled `createFB("hyprwobbly")` to a dedicated `CGLFramebuffer`.
6. Crash fix: `HyprlandAPI::removeFunctionHook` in PLUGIN_EXIT (see above).
7. Instrumentation (LOG_DEBUG, visible via `hyprctl rollinglog`):
   - `[hyprwobbly] STATE win=... t= x=(..) prev=(...) drag= maxDisp= maxVel= active=`
   - `[hyprwobbly] IMPULSE win=... delta=(dx,dy) compositorDrag=`
   - `[hyprwobbly] preWindowRender win=... fullBoxPx=... src=... deltaPos=... deltaSize=...`
   - `[hyprwobbly] transform in=(WxH tx=) mon=(WxH) srcBoxPx=... verts=N`
8. **White-rectangle ROOT CAUSE found by reading the 0.56.2 core sources**
   (ElementRenderer.cpp `drawTransformedWindow`, verified against v0.56.2 tag):
   - When a window has blur, core runs the transformer chain a SECOND time on
     a matte fb (black bg + white rounded rect, `transformWindowFB(matteFB)`).
   - The plugin's single persistent output fb was clobbered by that matte
     pass; core then composited the deformed matte (white rect on black) as
     the window -> exactly the "white rectangle while moving", only while
     `m_active`.
   - FIX (in current source): `transform()` allocates its output from
     `pMonitor->resources()->getUnusedWorkBuffer()` every call. Core cannot
     hand that buffer out again while it holds our returned ref
     (`strongRef < 2` check in getUnusedWorkBuffer), so it survives until the
     composite is done. Also fixes color management (work buffers carry the
     monitor's image description; the old private CGLFramebuffer did not)
     and avoids a monitor-sized fb per window.
   - Also fixed: the physics model was never re-gridded on resize (m_sizePx
     went stale after any resize); resizes now silently re-grid (no wobble,
     per spec).
   - Verified-by-reading (no live session available): UV orientation for
     NORMAL monitor transform is `u=x/W, v=y/H` with no flip (hyprutils
     `outputProjection` does NOT flip y; `fullVerts` in OpenGL.hpp confirms
     fb textures are consumed with v=0 at logical top). Projection/blend/VAO
     usage in transform() all match core's renderTextureInternal.
9. Test 1 harness: config key `plugin:hyprwobbly:test_identity` (int, default
   0). When 1: impulses are suppressed, the mesh stays at rest, m_active is
   forced every tick and damage continues — the full custom render path runs
   with ZERO deformation. Output must be pixel-identical to normal Hyprland.
   This is the mandatory identity test from the spec (Test 1), runnable via
   `hl.config({ plugin = { hyprwobbly = { test_identity = 1 } } })`.
10. Grab-point deformation implemented (see the GRAB-POINT SPEC section):
    grabLocal recorded at drag start, constraint-point drive + spring
    propagation (KWin model, no velocity kicks). GRAB/IMPULSE/STATE logs
    include the grab point. Live verify: grab top-left / center / right edge
    and confirm the deformation pattern changes with the grab position.
11. Physics replaced wholesale with the KWin/Compiz spring-mass-damper
    (stiffness 0.15, drag 0.80 retention, moveFactor 0.10, mass 1.0) after
    the user found the hand-tuned spring mesh too stiff; see PHYSICS MODEL.

## MOUSE-POSITION-DEPENDENT DEFORMATION (GRAB-POINT SPEC — ESSENTIAL)

The wobble must depend on the mouse grab position within the window, similar
to KDE/Compiz wobbly windows. The point of the window closest to where the
mouse is grabbing behaves as the most strongly constrained/anchored part of
the mesh; points farther away have progressively more freedom to lag, bend,
and oscillate. Do NOT apply the same displacement or force uniformly to every
mesh vertex.

With grab = (gx, gy) (cursor position in window-local/mesh coordinates) and
vertex = (vx, vy):

    d      = length(vertex - grab)
    d_norm = clamp(d / window_diagonal, 0, 1)
    w      = pow(d_norm, exponent)        # w = 0 near mouse, w -> 1 far away

Movement-induced forces scale by w: external_force(vertex) =
movement_force * distance_weight(vertex, grab). Do not interpret too
literally if a more physically consistent spring formulation is better; the
behavioral requirement is: deformation increases with distance from the
cursor/grab point.

### DESIRED BEHAVIOR

- Grab top-left + move right fast: top-left follows the mouse almost
  immediately; center trails slightly; bottom-right trails the most; far edge
  stretches backward; on reversal the far edge swings past rest; after
  release the deformation propagates through the mesh and settles.
- Grab center: center stays relatively stable; all four outer regions lag
  more; opposite edges wobble somewhat symmetrically.
- Grab right edge: right edge tracks the mouse; left side deforms most.

### GRAB POINT

Record the ACTUAL cursor position relative to the window at the moment the
drag starts: grabLocal = cursorGlobal - windowPosition, converted correctly
into the mesh coordinate system. Do not assume grab = window center. Keep the
same local grab point throughout the drag unless Hyprland's move semantics
require updating it.

### IMPORTANT — MOUSE POSITION IS NOT AN IMPULSE

The mouse position must influence only the SPATIAL DISTRIBUTION of
deformation and must NEVER cause stationary wobbling. force =
distanceFromMouse * constant is WRONG. The force must originate from real
window motion only:

    force(vertex) = motionImpulse * spatialWeight(distance(vertex, grabPoint))

motionImpulse == 0  =>  external_force == 0 regardless of mouse position.

### PHYSICS INTERPRETATION

The cursor grab point approximates pinning one area of an elastic sheet:
grab/nearby vertices are driven strongly by window motion; neighbors are
influenced through spring connections; far vertices receive motion indirectly
through the mesh (delayed movement, larger oscillation). Prefer this locally
driven elastic mesh.

### STATUS (implementation)

- Implemented: grabLocal recorded at drag START (real cursor position,
  mesh-space), kept fixed for the whole drag. Impulses are GRAB-WEIGHTED:
  each vertex shifts by -delta * w(distance(vertex, grab)) where
  w = t^grab_falloff * smootherstep(t/0.4) with t = d/diagonal — C2 onset
  at the grab (rounded, no kink), EXACTLY the old cone beyond t=0.4 (far
  wobble preserved). The grab constraint is SPREAD, not a pinned vertex:
  while dragging every vertex gets
  acc += (rest - pos) * stiffness * G, G = exp(-(d/diag / 0.22)^2) — a
  Gaussian over a neighborhood of the cursor (strong for a couple of grid
  cells, ~0 beyond), so the far mesh participates FULLY in the spring sim.
  History: uniform shift + pinned vertex, then a linear cone, then a
  smoothstep+rigid glued core (kink relocated to the core edge — user:
  "made everything worse"), then quintic w + spread gain (1-w) — the (1-w)
  constraint reached across the whole window and suppressed the
  spring-carried far lag (user: "only wobbles in y, far field dead").
  Lesson: impulse weight and constraint gain must be DECOUPLED — the
  constraint must stay LOCAL (Gaussian) while the impulse weight keeps the
  far-field cone. Stationary windows receive zero force.
- `grab_falloff` config key (float, default 1.0, clamp 1..4): exponent of
  the impulse falloff cone. Higher = wider glued/smooth region around the
  mouse. Clamped >= 1 because sub-1 exponents reintroduce a nonzero slope
  (cusp) at the grab.
- Window motion reaches the mesh ONLY through the constraint point — no
  per-vertex velocity kicks.

## PHYSICS MODEL (KWin/Compiz SPRING-MASS-DAMPER PORT)

The mesh is a genuine spring-mass-damper system, ported from KWin's
wobblywindows effect (which uses EXACTLY the user's factors: stiffness 0.15,
drag 0.80, moveFactor 0.10 — kcfg defaults 15/80/10 divided by 100):

    acceleration = mean(neighbor spring forces), F = stiffness * spacingError
    constraint (grabbed) vertex: acceleration = (rest - pos) * stiffness
    velocity     = acc * dt + velocity * drag     # drag = RETENTION per 10ms
    position    += velocity * dt * move_factor

- Integration: fixed ~10ms substeps (dt in ms), semi-implicit (velocity
  before position), framerate-independent via retention^ (dt/10ms).
- drag is a velocity RETENTION factor (0.8 = lose 20% per step), NOT a
  damping coefficient — do not confuse with the old friction semantics.
- moveFactor scales the position integration (the lag amount).
- mass is the M in F = m·a (default 1 = KWin).
- RETUNED from KWin defaults for feel (user: "too stiff/fast"): spring_k
  0.15→0.05 (more lag, slower), friction 0.80→0.85, move_factor 0.10→0.25.
- Impulses use PRECISE (unrounded) window deltas, not the rounded fullBoxPx:
  with fractional scale 1.5, rounded deltas quantize slow drags to 0/1px
  steps -> jerky 1px kicks or none (stiff, glitchy wobble that varied per
  window/drag speed). Sub-pixel deltas now accumulate smoothly.
- Release anchor: a free spring mesh conserves its mean displacement (spring
  forces are internal pairs), so the lag accumulated during a drag could
  never return to rest — the mesh froze visually displaced and then
  hard-snapped when the settle check fired (random "jumps to another
  position" glitch after release). While NOT dragging, a weak anchor
  (acc += (rest - pos) * stiffness * 0.04) bleeds the residual offset off
  smoothly. Zero while dragging (would kill the lag).
- Settle is displacement-gated: snap to rest only when maxDisp < 1px in
  addition to the KWin acc/vel criteria (velocity ~0 at displacement
  extremes previously allowed snapping a visibly displaced mesh).
- KWin stability ported verbatim: acceleration/velocity clamped (1000/1000),
  acceleration AND velocity fields smoothed with KWin's
  heightRingLinearMean (self-weighted ring mean), stop criteria
  acc_sum < 5.0 && vel_sum < 0.5 (used as per-point means 0.3125/0.03125).
- On settle: mesh snaps to rest (imperceptible at those magnitudes) and the
  transformer goes passive. The 1s no-impulse failsafe now decays smoothly
  (99.99%/s exponential, framerate-independent) instead of snapping.
- Config keys (semantics changed to KWin): `spring_k` = stiffness (0.15),
  `friction` = drag retention (0.80), `mass` = point mass (1.0),
  `move_factor` = 0.10. Grid/tiles/max_warp/test_identity unchanged.

## LARGE-WINDOW PERFORMANCE (diagnosed + optimized, needs live verify)

User report: "wobbly gets laggy and slower when the window is larger."

DIAGNOSIS (verified against v0.56.2 core source, ElementRenderer.cpp
drawTransformedWindow + Renderer.cpp renderWindow):
- Plugin CPU physics is O(1) in window size (fixed 8x8 grid, fixed 16x16
  render tiles) — NOT the cause.
- All size-dependence is GPU fill-rate in the transformed-render pipeline.
  Per frame per ACTIVE window core does: full-monitor-fb clear + window-area
  render into a pooled work buffer + composite over the damage region; for
  BLURRED windows it runs the whole chain a SECOND time on the blur matte
  fb (matte = clear + white rounded rect). The plugin's transform() added,
  PER CALL: one full-monitor clear (~67MB fill at 5184x3240) + a full
  window-area mesh draw. For a blurred window that was 2x (clear + draw) of
  pure plugin overhead per frame — with the area-scaled core passes this
  blows the iGPU frame budget as window area grows.
- Core's composite reads the FULL monitor texture scissored to frame damage
  (outputBox = full monitor), so the work buffer must be fully cleared — a
  scissored clear of ours would leak garbage from the buffer's previous user
  (matte = opaque black!) wherever unrelated frame damage exists (cursor,
  other windows). Do NOT "optimize" the clear to the window box.

FIXES (in source, needs live verify):
1. **Blur-matte deformation skipped by default** — new config
   `plugin:hyprwobbly:deform_blur_matte` (int, default 0). Core calls
   transform() twice per frame for blurred windows; the second call (matte)
   now returns `in` unchanged — EXACTLY core's non-transformed blur
   behavior, so it cannot regress the white-rect bug class — saving one
   full-monitor clear + one window-area mesh draw + one pooled monitor-size
   buffer per frame. Detection: `m_transformedThisFrame` reset in
   preWindowRender (which core calls at pass-assembly time, once per
   rendered frame, before any transform), set at transform() entry; the
   second call in the same frame is the matte. Opt back in (old visuals)
   with `hyprctl keyword plugin:hyprwobbly:deform_blur_matte 1`.
2. **Sub-pixel pass-through**: transform() returns `in` when
   `m_maxDisp < 0.5px` (pixel-identical to the deformed draw at that
   magnitude) — skips our clear+draw during mid-drag pauses, drag starts,
   and failsafe decay. test_identity is EXEMPT so the harness still
   exercises the custom draw path. `m_maxDisp` is now recomputed inside
   applyMoveImpulse (it was only updated by physics ticks, so it was stale
   for one frame after every impulse).
3. **No per-frame allocations in transform()**: the vertex vector is now a
   member (`m_vertices`, reserved per call, reused); previously a fresh
   ~37KB heap alloc per call (x2 for blurred windows).

Residual (inherent to core, not fixable from a plugin): while a window
wobbles, the transformed path itself (core's window-pass clear + render +
composite, plus multi-pass blur for blurred windows) is area-scaled and
cannot be avoided — large blurred windows dragged under wobbly will still
be heavier than small ones. If that remains too heavy, the only lever is
core's blur (xray/disable blur on those windows), not the plugin.

## OPEN BUGS (Phase 1 diagnosis incomplete)

1. **White rectangle while moving** — ROOT CAUSE FOUND (see Progress #8):
   blur-matte pass clobbered the persistent output fb. Fixed in source by
   using pooled work buffers for output; **NOT yet verified on a live
   session**. If the white rect somehow persists after re-enabling, fall back
   to the Test 1 harness (test_identity=1) and diff identity output.
2. **CRASH on floating window (FIXED in source, needs live verify)** —
   SIGABRT, root cause proven from
   the core dump: `CWobblyTransformer::transform()` called
   `PMONITOR->resources().lock()`, but `CMonitor::m_resources` is a
   `UP<>` (Monitor.hpp:409) and `resources()` returns a WP over that
   UNIQUE pointer. Hyprutils `CWeakPointer::lock()` asserts
   "tried to lock a CWeakPointer over a CUniquePointer"
   (/usr/include/hyprutils/memory/WeakPtr.hpp) -> std::terminate -> abort.
   Crash triggered whenever transform()'s full body ran (m_active = any real
   wobble; floating windows opened under the cursor trigger an impulse via
   the open animation). The identity test never crashed only because no
   floating window was active at that moment. FIX: use the raw observing
   `PMONITOR->resources()->getUnusedWorkBuffer()` (WP::operator-> never
   asserts; core itself uses resources()-> the same way, Monitor.cpp:2522).
   LESSON: audit every WP `.lock()` — only lock WPs over SP-owned objects.
2. **Idle wobble + ~50% iGPU** — believed to be continuous damage from the
   render loop (preWindowRender used to damage() unconditionally; fixed in
   current source but unverified on the live session). The 1s force-settle
   guarantees convergence now.
3. Physics currently steps in tick() but impulses were also possible from
   non-drag geometry changes; must be proven clean with the STATE/IMPULSE logs.

## FILE MAP

- `~/Projects/wobbly/` — THIS project (canonical source).
  - `src/` — plugin sources (main.cpp, Wobble*.cpp/hpp, shaders.hpp, Makefile).
  - `build.sh` — rebuild against installed Hyprland headers, installs .so to
    `~/.config/hypr/plugins/hyprwobbly.so`.
  - `AGENTS.md` — this file.
- `~/.config/hypr/plugins/hyprwobbly.so` — installed plugin binary.
- `~/.config/hypr/custom.lua` — session integration: config values
  (`plugin:hyprwobbly:enabled/mode`) + plugin load; skipped when a
  disable sentinel file exists (see the opt-out switch above).
- Upstream reference (pre-0.56 API): github.com/colonelpanic8/hyprwobbly.

## CURRENT STATE

- **Plugin LIVE and working**: floating windows wobble and settle smoothly,
  no crashes on open/move (post-26914 build). test_identity harness remains
  available for regression checks.
- Glitch/feel round (needs live verify): sub-pixel impulse deltas (fixes
  stiff/jerky slow drags + per-window variance at scale 1.5), release anchor
  + displacement-gated settle (fixes random post-release jump glitches),
  defaults retuned softer/slower (spring_k 0.05, friction 0.85,
  move_factor 0.25).
- Smoothness + ghost round (needs live verify): grid 4x4→8x8 and render
  tiles 12→16 (user: deformation looked "sharp and angular"); ghost
  remnants behind a moved window fixed by damaging in preWindowRender right
  after the impulse (tick damage lags one frame behind the mesh shift) and
  damaging on the final settle resets; damage padding 8→12px.
- Toasts removed (user: no plugin popups at login): all
  HyprlandAPI::addNotification calls in PLUGIN_INIT replaced with
  Log::logger->log; failures log LOG_ERR and still throw. custom.lua now
  loads the plugin BEFORE setting plugin:* config values (values set before
  registration surface config errors at login) and only zeroes `enabled`
  when the plugin is actually loaded (guarded via `hyprctl plugin list`).
- LOGIN CONFIG ERRORS (user: "config errors about undefined plugins" at
  session start): ROOT CAUSE — hl.dsp.exec_cmd is a DEFERRED dispatcher, so
  custom.lua's hl.config({ plugin = ... }) ran BEFORE the deferred
  `hyprctl plugin load` registered the values -> unknown-option errors that
  cleared once the load landed. FIX: custom.lua no longer sets ANY
  plugin:hyprwobbly values; all desired defaults live in the plugin binary
  (mode default changed to "floating", enabled 1). Runtime tuning after
  load: `hyprctl keyword plugin:hyprwobbly:<key> <value>`. The disable-wobbly
  branch only zeroes `enabled` if `hyprctl plugin list` shows the plugin
  actually loaded (avoids the same error class).
- IDLE OVERHEAD FIX (user: "the longer a window is floating, the laggier the
  desktop"): core renders EVERY window with an attached transformer through
  the transformed path per frame (work-buffer render + composite, + matte
  pass when blurred) even when the transformer is passive — floating
  windows paid that forever, cost stacking per floating window. Transformers
  are now DETACHED on the tick whenever they are inactive (settled) and
  RE-ATTACHED the moment a drag starts: onTick polls
  g_layoutManager->dragController()->target() (set in dragBegin) every 25ms
  while idle, so a grabbed window has its transformer back within its first
  frames (up to ~30-50ms of rigid movement at drag start is the tradeoff).
  No window.move event exists in the 0.56 event bus (checked) — polling the
  drag controller is the only reliable trigger. Consequences: windows never
  dragged carry zero cost; open-animation wobble no longer fires (window is
  detached before the animation moves it — desired per spec: no wobble on
  open); after a >1s mid-drag pause the mesh may re-init (settled anyway).
- build.sh now installs via `mv` (new inode) — rebuilding can no longer crash
  a running session; `cp` over the loaded .so did (in-place inode write).
- The installed .so at `~/.config/hypr/plugins/hyprwobbly.so` is the latest
  build (work-buffer output fix, WP-over-UP crash fix, resize re-grid,
  test_identity harness, floating gating, crash-safe exit, graceful
  force-settle, retuned spring defaults). Compiles clean against the
  installed 0.56.2 headers.

## NEXT STEPS (in order)

1. Watch for the white rect specifically on BLURRED floating windows while
   wobbling (the matte-clobber fix is live but was only spot-checked).
2. Run the full test plan (stationary convergence, slow drag, fast drag,
   release settle, repeated start/stop with no drift, multi-window
   independence, resize stability, refresh-rate independence) — collect
   STATE/IMPULSE logs via `hyprctl rollinglog` to prove physics is driven
   only by real drags.
3. Measure idle GPU: must return to baseline (~0%) when no window is
   wobbling (release blocker — the old build burned ~50% iGPU at idle).
4. Restructure sources per the spec (WobbleManager/WobbleState/WobblePhysics/
   WobbleRenderer split) once testing is green.
5. Config polish: all keys registered (enabled, mode, grid_width/height,
   tiles_x/y, spring_k, friction, mass, move_factor, grab_falloff,
   resize_factor, max_warp, test_identity, deform_blur_matte) — document
   them for the user.