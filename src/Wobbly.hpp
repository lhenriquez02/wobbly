#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/transformer/Transformer.hpp>

#include <chrono>
#include <vector>

class CWobblyTransformer : public Render::IWindowTransformer {
  public:
    explicit CWobblyTransformer(PHLWINDOW pWindow);
    ~CWobblyTransformer() override;

    void                     preWindowRender(CSurfacePassElement::SRenderData* pRenderData) override;
    SP<Render::IFramebuffer> transform(SP<Render::IFramebuffer> in) override;

    void                     tick(float dt);
    void                     damage();
    bool                     belongsTo(PHLWINDOW pWindow) const;
    PHLWINDOW                window() const {
        return m_window.lock();
    }
    bool                     isActive() const {
        return m_active;
    }

  private:
    struct SPoint {
        Vector2D pos;
        Vector2D rest;
        Vector2D velocity;
    };

    WP<Desktop::View::CWindow> m_window;

    std::vector<SPoint>   m_points;
    std::vector<Vector2D> m_acc;       // accelerations, px/ms^2
    std::vector<Vector2D> m_velBuf;    // velocity smoothing scratch
    std::vector<Vector2D> m_scratchA;  // field smoothing scratch
    std::vector<Vector2D> m_scratchB;
    float                 m_maxDisp = 0.F;

    Vector2D                   m_sizePx;
    Vector2D                   m_grabLocal; // grab point in mesh px coords, recorded at drag start
    bool                       m_dragActive = false;  // true while a drag is in progress (grab constraint active)
    CBox                       m_sourceBoxLayout;
    CBox                       m_sourceBoxPx;
    Vector2D                   m_sourcePosPrecise;  // unrounded, for sub-pixel delta tracking
    Vector2D                   m_sourceSizePrecise;
    CBox                       m_lastDamage;
    bool                       m_initialized = false;
    bool                       m_active      = false;
    bool                       m_wasDragging = false;
    // Set when transform() has already deformed once since the last
    // preWindowRender: core calls the chain a SECOND time per frame on the
    // blur matte fb (ElementRenderer.cpp drawTransformedWindow); this marks
    // that call so it can be skipped (see transform()).
    bool                       m_transformedThisFrame = false;
    std::chrono::steady_clock::time_point m_lastImpulse;

    // Reused per-frame draw buffer (spec: no per-frame allocations).
    std::vector<Render::GL::SVertex> m_vertices;

    int                        gridWidth() const;
    int                        gridHeight() const;
    int                        tileCountX() const;
    int                        tileCountY() const;
    float                      springK() const;
    float                      friction() const;
    float                      mass() const;
    float                      moveFactor() const;
    float                      grabFalloff() const;
    float                      resizeFactor() const;
    float                      maxWarp() const;
    bool                       enabled() const;
    bool                       testIdentity() const;
    bool                       deformBlurMatte() const;
    std::string                mode() const;
    bool                       hasWobblyAnimationStyle() const;
    bool                       shouldWobble() const;

    size_t                     index(int x, int y) const;
    float                      grabWeight(const Vector2D& rest) const;
    float                      grabConstraintGain(const Vector2D& rest) const;
    void                       resetModel(const Vector2D& size);
    void                       applyMoveImpulse(const Vector2D& delta);
    void                       stepSimulation(float dt);
    void                       smoothField(std::vector<Vector2D>& data, std::vector<Vector2D>& scratch);
    void                       constrainWarp();
    Vector2D                   sample(float u, float v) const;
    CBox                       deformedBoundsLayout() const;
    Vector2D                   currentPointerLocalPx(const CBox& boxLayout) const;
};
