#include "Wobbly.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/memory/Casts.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include "globals.hpp"

#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/layout/target/Target.hpp>

using namespace Hyprutils::Memory;
using Render::GL::g_pHyprOpenGL;
namespace {
    // Is THIS window currently being dragged with the mouse (real compositor
    // drag state, not a heuristic)?
    bool isUserDragging(const PHLWINDOW& w) {
        const auto TARGET = g_layoutManager->dragController()->target();
        return TARGET && TARGET->window() == w;
    }

    int cfgInt(const SP<Config::Values::CIntValue>& value, int fallback) {
        return value ? sc<int>(value->value()) : fallback;
    }

    float cfgFloat(const SP<Config::Values::CFloatValue>& value, float fallback) {
        return value ? sc<float>(value->value()) : fallback;
    }

    std::string cfgString(const SP<Config::Values::CStringValue>& value, const std::string& fallback) {
        return value ? value->value() : fallback;
    }

    float length(const Vector2D& vec) {
        return std::sqrt(vec.x * vec.x + vec.y * vec.y);
    }

    Vector2D lerp(const Vector2D& a, const Vector2D& b, float t) {
        return a + (b - a) * t;
    }
}

CWobblyTransformer::CWobblyTransformer(PHLWINDOW pWindow) : m_window(pWindow) {
    g_transformers.push_back(this);
}

CWobblyTransformer::~CWobblyTransformer() {
    damage();
    std::erase(g_transformers, this);
}

bool CWobblyTransformer::belongsTo(PHLWINDOW pWindow) const {
    return m_window.lock() == pWindow;
}

int CWobblyTransformer::gridWidth() const {
    return std::clamp(cfgInt(g_pGridWidth, 8), 2, 16);
}

int CWobblyTransformer::gridHeight() const {
    return std::clamp(cfgInt(g_pGridHeight, 8), 2, 16);
}

int CWobblyTransformer::tileCountX() const {
    return std::clamp(cfgInt(g_pTilesX, 16), 1, 80);
}

int CWobblyTransformer::tileCountY() const {
    return std::clamp(cfgInt(g_pTilesY, 16), 1, 80);
}

float CWobblyTransformer::springK() const {
    // KWin "stiffness": spring force factor (F = stiffness * spacingError).
    // Softer than KWin's 0.15 -> more lag, slower, more visible wobble.
    return std::clamp(cfgFloat(g_pSpringK, 0.05F), 0.01F, 1.0F);
}

float CWobblyTransformer::friction() const {
    // KWin "drag": velocity RETENTION factor per 10ms step (0.8 = lose 20%)
    return std::clamp(cfgFloat(g_pFriction, 0.85F), 0.05F, 1.0F);
}

float CWobblyTransformer::mass() const {
    return std::clamp(cfgFloat(g_pMass, 1.0F), 0.1F, 10.0F);
}

float CWobblyTransformer::moveFactor() const {
    // KWin "moveFactor": scales position integration (lag amount)
    return std::clamp(cfgFloat(g_pMoveFactor, 0.25F), 0.01F, 2.0F);
}

float CWobblyTransformer::grabFalloff() const {
    // Exponent of the grab-distance falloff w = smootherstep(d/diagonal)^p.
    // Higher -> a wider glued/smooth region around the mouse. Clamped >= 1:
    // sub-1 exponents reintroduce a nonzero slope at the grab (the cusp we
    // are eliminating) since the quintic onset behaves like d^(3p).
    return std::clamp(cfgFloat(g_pGrabFalloff, 1.0F), 1.0F, 4.0F);
}

float CWobblyTransformer::resizeFactor() const {
    return std::clamp(cfgFloat(g_pResizeFactor, 0.45F), 0.F, 4.F);
}

float CWobblyTransformer::maxWarp() const {
    return std::clamp(cfgFloat(g_pMaxWarp, 140.F), 1.F, 600.F);
}

bool CWobblyTransformer::enabled() const {
    return cfgInt(g_pEnabled, 1) != 0;
}

bool CWobblyTransformer::testIdentity() const {
    return cfgInt(g_pTestIdentity, 0) != 0;
}

bool CWobblyTransformer::deformBlurMatte() const {
    // Deforming the blur matte is pure aesthetics (the blur alpha mask
    // follows the wobble) and costs a second full transformed pass per
    // frame. Default OFF — the undeformed matte is exactly what core
    // renders for non-transformed blurred windows.
    return cfgInt(g_pDeformBlurMatte, 0) != 0;
}

std::string CWobblyTransformer::mode() const {
    auto MODE = cfgString(g_pMode, "floating");
    std::ranges::transform(MODE, MODE.begin(), [](unsigned char c) { return std::tolower(c); });
    return MODE;
}

bool CWobblyTransformer::hasWobblyAnimationStyle() const {
    const auto PWINDOW = m_window.lock();
    if (!PWINDOW)
        return false;

    auto style = PWINDOW->positionAnimation()->getStyle();
    std::ranges::transform(style, style.begin(), [](unsigned char c) { return std::tolower(c); });
    return style == "wobbly" || style.starts_with("wobbly ");
}

bool CWobblyTransformer::shouldWobble() const {
    if (!enabled())
        return false;

    const auto MODE = mode();
    if (MODE == "style" || MODE == "animation")
        return hasWobblyAnimationStyle();

    if (MODE == "floating") {
        const auto PWINDOW = m_window.lock();
        return PWINDOW && PWINDOW->m_isFloating;
    }

    return true;
}

size_t CWobblyTransformer::index(int x, int y) const {
    return sc<size_t>(y * gridWidth() + x);
}

void CWobblyTransformer::resetModel(const Vector2D& size) {
    m_sizePx = {std::max(1.0, size.x), std::max(1.0, size.y)};
    m_points.clear();
    m_points.resize(sc<size_t>(gridWidth() * gridHeight()));
    m_maxDisp = 0.F;

    for (int y = 0; y < gridHeight(); ++y) {
        for (int x = 0; x < gridWidth(); ++x) {
            const Vector2D rest   = {x * m_sizePx.x / (gridWidth() - 1), y * m_sizePx.y / (gridHeight() - 1)};
            m_points[index(x, y)] = {.pos = rest, .rest = rest, .velocity = {}};
        }
    }
}

Vector2D CWobblyTransformer::currentPointerLocalPx(const CBox& boxLayout) const {
    const auto POINTER = g_pInputManager->getMouseCoordsInternal();
    return (POINTER - boxLayout.pos()) * (m_sourceBoxPx.w / std::max(1.0, boxLayout.w));
}

float CWobblyTransformer::grabWeight(const Vector2D& rest) const {
    // Impulse weight w(d) in [0, 1]: how much of the window's motion each
    // vertex lags behind (shift = -delta * w).
    //
    // Near the cursor w has a C2 (quintic smootherstep) onset — w and its
    // first two derivatives are 0 at the grab, so the displacement field's
    // slope varies smoothly and no kink forms. Beyond the blend radius it
    // is EXACTLY the old t^falloff cone, preserving the far-field wobble
    // (the pure quintic reduced mid-range lag and made the whole wobble
    // weaker).
    const double DIAG = std::max(1.0, std::hypot(m_sizePx.x, m_sizePx.y));
    const double t    = std::clamp(length(rest - m_grabLocal) / DIAG, 0.0, 1.0);
    constexpr double BLEND = 0.4;
    const double u    = std::clamp(t / BLEND, 0.0, 1.0);
    const double s    = u * u * u * (10.0 + u * (-15.0 + 6.0 * u));
    return std::pow(t, grabFalloff()) * s;
}

float CWobblyTransformer::grabConstraintGain(const Vector2D& rest) const {
    // Spread grab constraint (replaces the hard-pinned vertex): a Gaussian
    // radial falloff over a neighborhood of the cursor — vertices within a
    // couple of grid cells are strongly driven toward the window, beyond
    // that the gain is ~0 and the mesh participates FULLY in the spring
    // simulation. (The previous 1 - w gain reached across the whole window
    // and suppressed the spring-carried far-field lag — the wobble away
    // from the grab nearly vanished.)
    const double DIAG = std::max(1.0, std::hypot(m_sizePx.x, m_sizePx.y));
    const double d    = length(rest - m_grabLocal) / DIAG;
    constexpr double SIGMA = 0.22;
    return std::exp(-(d * d) / (SIGMA * SIGMA));
}

void CWobblyTransformer::applyMoveImpulse(const Vector2D& delta) {
    // Window motion reaches the mesh ONLY through real position deltas.
    //
    // Grab-point weighted KWin/Compiz model: the mesh keeps its ABSOLUTE
    // positions while the window moves under it. In window-local mesh
    // coordinates each vertex shifts by -delta scaled by its distance from
    // the grab point: w = 0 at the cursor (the grabbed area is glued to the
    // window) growing to w = 1 at the far side (maximum lag). The smooth
    // spatial falloff means the region next to the mouse follows with a
    // gentle spring-mediated delay instead of jerking with the full lag —
    // a uniform shift plus a pinned vertex concentrates the whole
    // deformation on the vertices right next to the grab and looks angular.
    // The springs then propagate the motion through the sheet (delayed
    // movement, overshoot on reversal). A stationary window has delta == 0
    // and receives zero force regardless of the mouse position.
    if (m_points.empty())
        return;

    const int GW = gridWidth(), GH = gridHeight();

    // Refresh m_maxDisp with the post-impulse displacement: transform() uses
    // it for the sub-pixel pass-through decision, and the value computed by
    // the last physics tick is stale the moment an impulse shifts the mesh
    // between ticks.
    float maxDisp = 0.F;
    for (int y = 0; y < GH; ++y) {
        for (int x = 0; x < GW; ++x) {
            auto& p = m_points[index(x, y)];
            p.pos -= delta * grabWeight(p.rest);
            maxDisp = std::max(maxDisp, sc<float>(length(p.pos - p.rest)));
        }
    }
    m_maxDisp = maxDisp;

    m_active      = true;
    m_lastImpulse = std::chrono::steady_clock::now();
}

void CWobblyTransformer::preWindowRender(CSurfacePassElement::SRenderData* pRenderData) {
    // Called once per rendered frame (during window-pass assembly) for every
    // transformer: reset the transform-call counter so transform() can tell
    // the content pass (1st) from the blur-matte pass (2nd) this frame.
    // MUST happen before any early return.
    m_transformedThisFrame = false;

    const auto PWINDOW = m_window.lock();
    if (!PWINDOW || !pRenderData || !pRenderData->pMonitor)
        return;

    const auto PMONITOR = pRenderData->pMonitor.lock();
    if (!PMONITOR)
        return;

    const CBox     currentClientBox   = {PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
    const CBox     currentFullBox     = PWINDOW->getFullWindowBoundingBox();
    const Vector2D topLeftExtents     = currentClientBox.pos() - currentFullBox.pos();
    const Vector2D bottomRightExtents = currentFullBox.pos() + currentFullBox.size() - currentClientBox.pos() - currentClientBox.size();

    CBox           fullBox = {
        pRenderData->pos.x - topLeftExtents.x,
        pRenderData->pos.y - topLeftExtents.y,
        pRenderData->w + topLeftExtents.x + bottomRightExtents.x,
        pRenderData->h + topLeftExtents.y + bottomRightExtents.y,
    };

    if (fullBox.empty())
        fullBox = {pRenderData->pos.x, pRenderData->pos.y, pRenderData->w, pRenderData->h};

    CBox        fullBoxScaled = fullBox.copy().translate(-PMONITOR->m_position).scale(PMONITOR->m_scale);
    CBox        fullBoxPx     = fullBoxScaled.copy().round();
    // Precise (unrounded) geometry: deltas measured on the ROUNDED box
    // quantize to whole pixels. With fractional monitor scaling (1.5 here) a
    // slow drag alternates 0/1px deltas -> jerky 1px impulse kicks or none at
    // all (stiff, glitchy wobble). Sub-pixel deltas accumulate smoothly.
    const Vector2D precisePos  = fullBoxScaled.pos();
    const Vector2D preciseSize = fullBoxScaled.size();

    if (fullBoxPx.w <= 1 || fullBoxPx.h <= 1)
        return;

    const Vector2D newSize = fullBoxPx.size();

    if (!m_initialized || m_points.size() != sc<size_t>(gridWidth() * gridHeight())) {
        m_sourceBoxLayout    = fullBox;
        m_sourceBoxPx        = fullBoxPx;
        m_sourcePosPrecise   = precisePos;
        m_sourceSizePrecise  = preciseSize;
        resetModel(newSize);
        m_initialized = true;
        return;
    }

    const Vector2D deltaPos  = precisePos - m_sourcePosPrecise;
    const Vector2D deltaSize = preciseSize - m_sourceSizePrecise;

    static std::chrono::steady_clock::time_point LASTLOG = std::chrono::steady_clock::now();
    const auto                                    NOWLOG = std::chrono::steady_clock::now();
    if (std::chrono::duration<float>(NOWLOG - LASTLOG).count() > 1.0F) {
        LASTLOG = NOWLOG;
        Log::logger->log(Hyprutils::CLI::LOG_DEBUG,
            std::format("[hyprwobbly] preWindowRender win={} fullBoxPx=({:.1f},{:.1f},{:.1f},{:.1f}) src=({:.1f},{:.1f},{:.1f},{:.1f}) deltaPos=({:.1f},{:.1f}) deltaSize=({:.1f},{:.1f}) mode={} floating={}",
                (void*)PWINDOW.get(), fullBoxPx.x, fullBoxPx.y, fullBoxPx.w, fullBoxPx.h, m_sourceBoxPx.x, m_sourceBoxPx.y, m_sourceBoxPx.w, m_sourceBoxPx.h, deltaPos.x, deltaPos.y, deltaSize.x, deltaSize.y, mode(), PWINDOW->m_isFloating));
    }

    if (!shouldWobble()) {
        if (m_active) {
            damage();
            resetModel(newSize);
            m_active = false;
        }
        m_sourceBoxLayout   = fullBox;
        m_sourceBoxPx       = fullBoxPx;
        m_sourcePosPrecise  = precisePos;
        m_sourceSizePrecise = preciseSize;
        return;
    }

    const auto  POINTER        = g_pInputManager->getMouseCoordsInternal();
    const bool  POINTER_INSIDE = fullBox.containsPoint(POINTER);
    const bool  MOVING         = std::abs(deltaPos.x) > 0.25 || std::abs(deltaPos.y) > 0.25;

    // The physics model was never re-gridded on size changes before, leaving
    // m_sizePx stale after any resize (deformation + identity mapping broke).
    // Resizes must not wobble, so re-grid silently and keep velocities.
    if (std::abs(deltaSize.x) > 0.5 || std::abs(deltaSize.y) > 0.5) {
        const auto oldPts = m_points;
        resetModel(newSize);
        if (oldPts.size() == m_points.size()) {
            for (size_t i = 0; i < m_points.size(); ++i)
                m_points[i].velocity = oldPts[i].velocity;
        }
    }

    // Test 1 harness (test_identity): never feed impulses so the mesh stays at
    // rest — the custom render path must then reproduce the window EXACTLY.
    if (testIdentity()) {
        m_dragActive        = false;
        m_wasDragging       = POINTER_INSIDE;
        m_sourceBoxLayout   = fullBox;
        m_sourceBoxPx       = fullBoxPx;
        m_sourcePosPrecise  = precisePos;
        m_sourceSizePrecise = preciseSize;
        return;
    }

    // Grab-point wobble: the grab position is recorded when a drag STARTS
    // (real cursor position in mesh space) and stays fixed for the whole
    // drag. Motion impulses are scaled by the vertex distance from the grab
    // point, and the nearest mesh vertex is pinned to the window (zero
    // displacement) while dragging — like holding one point of an elastic
    // sheet. Impulses exist only for real position deltas: a stationary
    // window receives no force regardless of where the mouse is.
    const bool DRAGGING = isUserDragging(PWINDOW) || (POINTER_INSIDE && MOVING);

    if (DRAGGING) {
        if (!m_dragActive) {
            m_grabLocal   = currentPointerLocalPx(fullBox);
            m_grabLocal.x = std::clamp(m_grabLocal.x, 0.0, m_sizePx.x);
            m_grabLocal.y = std::clamp(m_grabLocal.y, 0.0, m_sizePx.y);
            m_dragActive  = true;

            Log::logger->log(Hyprutils::CLI::LOG_DEBUG,
                std::format("[hyprwobbly] GRAB win={} grab=({:.1f},{:.1f}) compositorDrag={}", (void*)PWINDOW.get(), m_grabLocal.x, m_grabLocal.y, isUserDragging(PWINDOW)));
        }

        if (MOVING) {
            applyMoveImpulse(deltaPos);
            // Damage BEFORE the window pass draws this frame: the mesh was
            // just shifted further behind the window, and tick() damage
            // (issued earlier) covers only the pre-impulse bounds. Without
            // this the lag trail behind the window is not in the frame's
            // commit damage and stale (ghost) content stays visible where
            // the window used to be.
            damage();
            Log::logger->log(Hyprutils::CLI::LOG_DEBUG,
                std::format("[hyprwobbly] IMPULSE win={} delta=({:.2f},{:.2f}) grab=({:.1f},{:.1f}) compositorDrag={}", (void*)PWINDOW.get(), deltaPos.x, deltaPos.y, m_grabLocal.x, m_grabLocal.y, isUserDragging(PWINDOW)));
        }
    } else if (m_dragActive) {
        m_dragActive = false; // release: unpin; the grabbed vertex has zero displacement, so no jump
    }

    m_wasDragging        = POINTER_INSIDE;
    m_sourceBoxLayout    = fullBox;
    m_sourceBoxPx        = fullBoxPx;
    m_sourcePosPrecise   = precisePos;
    m_sourceSizePrecise  = preciseSize;
}

void CWobblyTransformer::stepSimulation(float dt) {
    // KWin/Compiz wobbly spring-mass-damper system (per mesh point, mass m):
    //
    //   acceleration = mean(neighbor spring forces),  F = stiffness * spacingError
    //   grabbed vertex = constraint: acceleration = (rest - pos) * stiffness
    //   velocity       = acc * dt + velocity * drag      (drag = retention / 10ms)
    //   position      += velocity * dt * move_factor
    //
    // KWin integrates in fixed 10ms substeps with time in ms and clamps/
    // smooths the acceleration and velocity fields for numerical stability.
    if (m_points.empty())
        return;

    const float STIFFNESS = springK();   // 0.15
    const float DRAG      = friction();  // 0.80 velocity retention per 10ms
    const float MOVEF     = moveFactor(); // 0.10
    const float MASS      = mass();      // 1.0

    constexpr float MAX_ACC = 1000.F; // px/ms^2 (KWin maxAcceleration)
    constexpr float MAX_VEL = 1000.F; // px/ms   (KWin maxVelocity)
    constexpr float STOP_ACC = 0.3125F; // KWin stopAcceleration 5.0, mean over 16 points
    constexpr float STOP_VEL = 0.03125F; // KWin stopVelocity 0.5, mean over 16 points

    const int    SUBSTEPS = std::clamp(sc<int>(std::ceil(dt / 0.010F)), 1, 8);
    const double STEP_MS  = (dt / SUBSTEPS) * 1000.0;
    const double RETAIN   = std::pow(DRAG, STEP_MS / 10.0);

    const int    GW = gridWidth(), GH = gridHeight();
    const size_t N = m_points.size();
    if (m_acc.size() != N) {
        m_acc.resize(N);
        m_velBuf.resize(N);
        m_scratchA.resize(N);
        m_scratchB.resize(N);
    }

    const int DXS[4] = {-1, 1, 0, 0};
    const int DYS[4] = {0, 0, -1, 1};

    // A free spring mesh conserves its mean displacement: spring forces are
    // internal action-reaction pairs, so once released the average lag the
    // window accumulated during the drag can NEVER return to rest on its
    // own — the sim froze visually displaced and then hard-snapped when the
    // settle check fired (the random "jumps to another position" glitch).
    // While dragging the pinned vertex anchors the sheet; after release this
    // weak anchor bleeds the residual offset off smoothly instead.
    const float ANCHOR = m_dragActive ? 0.F : STIFFNESS * 0.04F;

    for (int substep = 0; substep < SUBSTEPS; ++substep) {
        // 1. forces -> accelerations
        for (int y = 0; y < GH; ++y) {
            for (int x = 0; x < GW; ++x) {
                const size_t i = index(x, y);
                const auto&  p = m_points[i];

                Vector2D force{};
                int      count = 0;
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + DXS[k], ny = y + DYS[k];
                    if (nx < 0 || ny < 0 || nx >= GW || ny >= GH)
                        continue;
                    const auto& n = m_points[index(nx, ny)];
                    force += (n.pos - p.pos) - (n.rest - p.rest);
                    ++count;
                }
                Vector2D acc{};
                if (count > 0)
                    acc = force * sc<float>(STIFFNESS / (MASS * count));

                if (m_dragActive) {
                    // Spread grab constraint (replaces the old single pinned
                    // vertex): Gaussian gain over a neighborhood of the
                    // cursor — full (rest - pos) * stiffness gain near the
                    // grab, smoothly ~0 beyond it, so the rest of the mesh
                    // is driven purely by springs. Smooth in space -> no
                    // kink next to the cursor.
                    acc += (p.rest - p.pos) * (STIFFNESS / MASS) * grabConstraintGain(p.rest);
                } else if (ANCHOR > 0.F) {
                    acc += (p.rest - p.pos) * ANCHOR;
                }

                m_acc[i] = acc;
            }
        }

        smoothField(m_acc, m_scratchA);

        // 2. velocity integration (semi-implicit), retention per 10ms step
        for (size_t i = 0; i < N; ++i) {
            auto  acc = m_acc[i];
            const float alen = length(acc);
            if (alen > MAX_ACC)
                acc *= MAX_ACC / alen;

            auto& v    = m_points[i].velocity;
            v          = acc * STEP_MS + v * RETAIN;
            const float vlen = length(v);
            if (vlen > MAX_VEL)
                v *= MAX_VEL / vlen;
        }

        // 3. field smoothing on velocity, then position integration
        for (size_t i = 0; i < N; ++i)
            m_velBuf[i] = m_points[i].velocity;
        smoothField(m_velBuf, m_scratchB);
        for (size_t i = 0; i < N; ++i)
            m_points[i].pos += m_velBuf[i] * (STEP_MS * MOVEF);
    }

    constrainWarp();

    // Settle: KWin stop criteria (normalized by point count), gated on
    // displacement — velocities can be ~0 at a displacement extreme, and
    // snapping a visibly displaced mesh is the glitch we just fixed. Above
    // 1px the anchor keeps relaxing the mesh smoothly instead. While the
    // drag is active the constraint keeps the window in wobbly mode.
    double accSum = 0.0, velSum = 0.0;
    m_maxDisp = 0.F;
    for (size_t i = 0; i < N; ++i) {
        accSum += std::abs(m_acc[i].x) + std::abs(m_acc[i].y);
        velSum += std::abs(m_points[i].velocity.x) + std::abs(m_points[i].velocity.y);
        m_maxDisp = std::max(m_maxDisp, sc<float>(length(m_points[i].pos - m_points[i].rest)));
    }

    if (!m_dragActive && accSum / sc<double>(N) < STOP_ACC && velSum / sc<double>(N) < STOP_VEL && m_maxDisp < 1.0F) {
        resetModel(m_sizePx);
        m_maxDisp = 0.F;
        m_active  = false;
        // Final damage: erase whatever the last deformed frame left on
        // screen (transform() now renders undeformed with m_active false).
        damage();
    }
}

void CWobblyTransformer::smoothField(std::vector<Vector2D>& data, std::vector<Vector2D>& scratch) {
    // KWin heightRingLinearMean: each field value is replaced by the weighted
    // mean of itself and its (up to 8) ring neighbors, self-weighted by the
    // neighbor count. Stabilizes the explicit integration.
    const int    GW = gridWidth(), GH = gridHeight();
    const size_t N = data.size();
    if (scratch.size() != N)
        scratch.resize(N);

    for (int y = 0; y < GH; ++y) {
        for (int x = 0; x < GW; ++x) {
            const size_t i = index(x, y);
            Vector2D     sum{};
            int          count = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0)
                        continue;
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= GW || ny >= GH)
                        continue;
                    sum += data[index(nx, ny)];
                    ++count;
                }
            }
            if (count > 0)
                scratch[i] = (sum + data[i] * count) / sc<float>(count * 2);
            else
                scratch[i] = data[i];
        }
    }
    data.swap(scratch);
}

void CWobblyTransformer::constrainWarp() {
    const auto LIMIT = maxWarp();
    for (auto& point : m_points) {
        auto diff = point.pos - point.rest;
        auto len  = length(diff);
        if (len > LIMIT)
            point.pos = point.rest + diff * (LIMIT / len);
    }
}

Vector2D CWobblyTransformer::sample(float u, float v) const {
    if (m_points.empty())
        return {u * m_sizePx.x, v * m_sizePx.y};

    const float gx = std::clamp(u, 0.F, 1.F) * (gridWidth() - 1);
    const float gy = std::clamp(v, 0.F, 1.F) * (gridHeight() - 1);
    const int   x0 = std::clamp(sc<int>(std::floor(gx)), 0, gridWidth() - 1);
    const int   y0 = std::clamp(sc<int>(std::floor(gy)), 0, gridHeight() - 1);
    const int   x1 = std::min(x0 + 1, gridWidth() - 1);
    const int   y1 = std::min(y0 + 1, gridHeight() - 1);
    const float tx = gx - x0;
    const float ty = gy - y0;

    const auto  top    = lerp(m_points[index(x0, y0)].pos, m_points[index(x1, y0)].pos, tx);
    const auto  bottom = lerp(m_points[index(x0, y1)].pos, m_points[index(x1, y1)].pos, tx);
    return lerp(top, bottom, ty);
}

CBox CWobblyTransformer::deformedBoundsLayout() const {
    if (m_points.empty())
        return m_sourceBoxLayout;

    Vector2D min = m_points.front().pos;
    Vector2D max = m_points.front().pos;

    for (const auto& point : m_points) {
        min.x = std::min(min.x, point.pos.x);
        min.y = std::min(min.y, point.pos.y);
        max.x = std::max(max.x, point.pos.x);
        max.y = std::max(max.y, point.pos.y);
    }

    const double SCALE = std::max(0.01, m_sourceBoxPx.w / std::max(1.0, m_sourceBoxLayout.w));
    return {m_sourceBoxLayout.x + min.x / SCALE, m_sourceBoxLayout.y + min.y / SCALE, (max.x - min.x) / SCALE, (max.y - min.y) / SCALE};
}

void CWobblyTransformer::damage() {
    const auto PWINDOW = m_window.lock();
    if (!PWINDOW || !PWINDOW->m_isMapped || PWINDOW->isHidden())
        return;

    const auto NOW = deformedBoundsLayout().expand(12);
    if (!NOW.empty())
        g_pHyprRenderer->damageBox(NOW);

    if (!m_lastDamage.empty())
        g_pHyprRenderer->damageBox(m_lastDamage);

    m_lastDamage = NOW;
}

void CWobblyTransformer::tick(float dt) {
    if (!shouldWobble() || !m_initialized)
        return;

    // Test 1 harness (test_identity): keep the full transformer render path
    // alive with the mesh at rest. Output must be pixel-identical to normal
    // Hyprland. This intentionally keeps damaging every frame — test only.
    if (testIdentity()) {
        m_active = true;
        damage();
        return;
    }

    if (!m_active)
        return;

    // Diagnostic state dump (throttled)
    static std::chrono::steady_clock::time_point LASTLOG = std::chrono::steady_clock::now();
    const auto                                    NOWLOG = std::chrono::steady_clock::now();
    if (std::chrono::duration<float>(NOWLOG - LASTLOG).count() > 0.2F) {
        LASTLOG        = NOWLOG;
        const auto PWINDOW = m_window.lock();
        float       maxDisp = 0.F, maxVel = 0.F;
        for (const auto& point : m_points) {
            maxDisp = std::max(maxDisp, sc<float>(length(point.pos - point.rest)));
            maxVel  = std::max(maxVel, sc<float>(length(point.velocity)));
        }
        const auto POS = PWINDOW ? PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) : Vector2D{};
        Log::logger->log(Hyprutils::CLI::LOG_DEBUG,
            std::format("[hyprwobbly] STATE win={} t={:.3f} x=({:.1f},{:.1f}) prev=({:.1f},{:.1f}) drag={} grab=({:.1f},{:.1f}) maxDisp={:.2f} maxVel={:.2f} active={}",
                (void*)PWINDOW.get(), NOWLOG.time_since_epoch().count() / 1e9, POS.x, POS.y, m_sourceBoxLayout.x, m_sourceBoxLayout.y, isUserDragging(PWINDOW), m_grabLocal.x, m_grabLocal.y, maxDisp, maxVel, m_active));
    }

    // Safety: if no impulse for 1s, converge smoothly instead of snapping
    // (the old hard reset was visible as an abrupt return to static). The
    // decay is framerate-independent: 99.99% per second (~0.4s to vanish).
    const auto  NOW           = std::chrono::steady_clock::now();
    const float SINCE_IMPULSE = std::chrono::duration<float>(NOW - m_lastImpulse).count();

    if (SINCE_IMPULSE > 1.0F) {
        const float DECAY = std::pow(1e-4F, dt);
        float       maxDisp = 0.F;
        for (auto& point : m_points) {
            point.pos       = point.rest + (point.pos - point.rest) * DECAY;
            point.velocity *= DECAY;
            maxDisp         = std::max(maxDisp, sc<float>(length(point.pos - point.rest)));
        }
        if (maxDisp < 0.25) {
            resetModel(m_sizePx);
            m_maxDisp = 0.F;
            m_active  = false;
            damage();
            Log::logger->log(Hyprutils::CLI::LOG_DEBUG, "[hyprwobbly] force-settle done (no impulse for 1s)");
        } else {
            m_maxDisp = maxDisp;
        }
        if (m_maxDisp > 0.25)
            damage();
        return;
    }

    stepSimulation(dt);
    if (m_active && m_maxDisp > 0.25)
        damage();
}

SP<Render::IFramebuffer> CWobblyTransformer::transform(SP<Render::IFramebuffer> in) {
    // PERF (large windows): core's drawTransformedWindow calls the
    // transformer chain TWICE per frame for blurred windows — once for the
    // window pass and once for the blur matte fb (black bg + white rounded
    // rect used as the blur alpha mask). Deforming the matte is pure
    // aesthetics; each extra call costs a full monitor-size clear (~67MB of
    // fill at 5184x3240) plus a full window-area mesh draw. Detection:
    // preWindowRender reset the flag at pass-assembly time; the second call
    // within the same frame is the matte. Returning `in` unchanged for the
    // matte is EXACTLY core's non-transformed blur behavior, so this cannot
    // regress the old white-rectangle bug class.
    const bool MATTE_CALL    = m_transformedThisFrame;
    m_transformedThisFrame = true;

    if (MATTE_CALL && !deformBlurMatte())
        return in;

    if (!shouldWobble() || !m_initialized || !m_active || !in || !in->getTexture() || !g_shaderReady || !g_pWobblyShader)
        return in;

    // PERF: below 0.5px of deformation the mesh draw is pixel-identical to
    // passing the buffer through — skip our clear + draw entirely (mid-drag
    // pauses, drag starts, settle tails). test_identity must NOT take this
    // branch: its whole purpose is to exercise the custom draw path with
    // zero deformation.
    if (!testIdentity() && m_maxDisp < 0.5F)
        return in;

    const auto PMONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!PMONITOR)
        return in;

    // CAUTION: resources() returns a WP over a UNIQUELY-OWNED object
    // (CMonitor::m_resources is UP<>). NEVER .lock() it — CWeakPointer::lock()
    // asserts "tried to lock a CWeakPointer over a CUniquePointer" and
    // terminates the compositor (observed as SIGABRT).
    // Use the raw observing operator-> instead; the object lives as long as
    // the monitor, which we hold via PMONITOR.
    const auto& RESOURCES = PMONITOR->resources();
    if (!RESOURCES)
        return in;

    // Output MUST come from the monitor's work-buffer pool, never a private fb.
    // Core (IElementRenderer::drawTransformedWindow) runs the transformer chain
    // once for the window pass and, when the window has blur, a SECOND time on
    // the blur matte fb. A single persistent output fb gets clobbered by the
    // matte pass (black bg + white rounded rect), and core then composites that
    // as the window -> the "white rectangle while moving" bug. Pool guarantees:
    // monitor-sized, correct work-buffer image description (CM), and a buffer
    // we return stays ours while core holds the ref (strongRef >= 2 keeps it
    // out of getUnusedWorkBuffer until the composite is done).
    auto out = RESOURCES->getUnusedWorkBuffer();
    if (!out || out == in)
        return in;

    static std::chrono::steady_clock::time_point LASTLOG = std::chrono::steady_clock::now();
    const auto                                    NOWLOG = std::chrono::steady_clock::now();
    const bool                                    DOLOG  = std::chrono::duration<float>(NOWLOG - LASTLOG).count() > 1.0F;
    if (DOLOG)
        LASTLOG = NOWLOG;

    const Vector2D MONSIZE = PMONITOR->m_transformedSize;

    g_pHyprRenderer->bindFB(out);
    g_pHyprOpenGL->scissor(nullptr);
    g_pHyprOpenGL->blend(true); // premultiplied alpha, matches core's tex pass
    glClearColor(0.F, 0.F, 0.F, 0.F);
    glClear(GL_COLOR_BUFFER_BIT);

    // Reused across frames (spec: no per-frame allocations); the matte pass
    // (deform_blur_matte=1) simply refills it.
    m_vertices.clear();
    m_vertices.reserve(sc<size_t>(tileCountX() * tileCountY() * 6));

    const auto pushVertex = [&](float u, float v) {
        const auto dst = m_sourceBoxPx.pos() + sample(u, v);
        const auto src = m_sourceBoxPx.pos() + Vector2D{u * m_sourceBoxPx.w, v * m_sourceBoxPx.h};
        m_vertices.push_back({
            .x = sc<float>(dst.x / MONSIZE.x),
            .y = sc<float>(dst.y / MONSIZE.y),
            .u = sc<float>(src.x / MONSIZE.x),
            .v = sc<float>(src.y / MONSIZE.y),
        });
    };

    for (int y = 0; y < tileCountY(); ++y) {
        const float v0 = sc<float>(y) / tileCountY();
        const float v1 = sc<float>(y + 1) / tileCountY();
        for (int x = 0; x < tileCountX(); ++x) {
            const float u0 = sc<float>(x) / tileCountX();
            const float u1 = sc<float>(x + 1) / tileCountX();

            pushVertex(u0, v0);
            pushVertex(u0, v1);
            pushVertex(u1, v0);

            pushVertex(u1, v0);
            pushVertex(u0, v1);
            pushVertex(u1, v1);
        }
    }

    glActiveTexture(GL_TEXTURE0);
    in->getTexture()->bind();
    in->getTexture()->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    in->getTexture()->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    auto shader = g_pHyprOpenGL->useShader(g_pWobblyShader);
    shader->setUniformInt(SHADER_TEX, 0);

    CBox monbox   = {0, 0, MONSIZE.x, MONSIZE.y};
    auto glMatrix = g_pHyprRenderer->projectBoxToTarget(monbox);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, glMatrix.getMatrix());

    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    glBindBuffer(GL_ARRAY_BUFFER, shader->getUniformLocation(SHADER_SHADER_VBO));
    glBufferData(GL_ARRAY_BUFFER, m_vertices.size() * sizeof(Render::GL::SVertex), m_vertices.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, m_vertices.size());
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    in->getTexture()->unbind();

    if (DOLOG)
        Log::logger->log(Hyprutils::CLI::LOG_DEBUG,
            std::format("[hyprwobbly] transform in=({:.0f}x{:.0f} tx={}) mon=({:.0f}x{:.0f}) srcBoxPx=({:.1f},{:.1f},{:.1f},{:.1f}) verts={} matte={}",
                in->m_size.x, in->m_size.y, (int)in->getTexture()->m_transform, MONSIZE.x, MONSIZE.y, m_sourceBoxPx.x, m_sourceBoxPx.y, m_sourceBoxPx.w, m_sourceBoxPx.h, m_vertices.size(), MATTE_CALL));

    return out;
}
