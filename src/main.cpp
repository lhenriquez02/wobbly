#define WLR_USE_UNSTABLE

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <any>
#include <chrono>
#include <ranges>
#include <unistd.h>
#include <hyprutils/memory/Casts.hpp>

#include "Wobbly.hpp"
#include "globals.hpp"
#include "shaders.hpp"

#include <hyprland/src/debug/log/Logger.hpp>

using Render::GL::g_pHyprOpenGL;

typedef std::string (*origStyleValid)(Animation::CHyprAnimationManager*, const std::string&, const std::string&);

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static bool isWobblyStyle(const std::string& config, std::string style) {
    std::ranges::transform(style, style.begin(), [](unsigned char c) { return std::tolower(c); });
    return config == "windowsMove" && (style == "wobbly" || style.starts_with("wobbly "));
}

template <typename T>
static void addConfigValue(SP<T>& storage, SP<T> value) {
    storage = std::move(value);
    HyprlandAPI::addConfigValueV2(PHANDLE, storage);
}

static void registerConfigValues() {
    addConfigValue(g_pEnabled, makeShared<Config::Values::CIntValue>("plugin:hyprwobbly:enabled", "enabled", 1));
    addConfigValue(g_pTestIdentity, makeShared<Config::Values::CIntValue>("plugin:hyprwobbly:test_identity", "identity render test harness (Test 1)", 0));
    addConfigValue(g_pDeformBlurMatte, makeShared<Config::Values::CIntValue>("plugin:hyprwobbly:deform_blur_matte", "deform the blur matte (aesthetics; costs a second full-screen pass per frame)", 0));
    addConfigValue(g_pMode, makeShared<Config::Values::CStringValue>("plugin:hyprwobbly:mode", "mode", "floating"));
    addConfigValue(g_pGridWidth, makeShared<Config::Values::CIntValue>("plugin:hyprwobbly:grid_width", "grid width", 8));
    addConfigValue(g_pGridHeight, makeShared<Config::Values::CIntValue>("plugin:hyprwobbly:grid_height", "grid height", 8));
    addConfigValue(g_pTilesX, makeShared<Config::Values::CIntValue>("plugin:hyprwobbly:tiles_x", "horizontal mesh tiles", 16));
    addConfigValue(g_pTilesY, makeShared<Config::Values::CIntValue>("plugin:hyprwobbly:tiles_y", "vertical mesh tiles", 16));
    addConfigValue(g_pSpringK, makeShared<Config::Values::CFloatValue>("plugin:hyprwobbly:spring_k", "spring stiffness (KWin wobbly)", 0.05F));
    addConfigValue(g_pFriction, makeShared<Config::Values::CFloatValue>("plugin:hyprwobbly:friction", "drag: velocity retention per 10ms (KWin wobbly)", 0.85F));
    addConfigValue(g_pMass, makeShared<Config::Values::CFloatValue>("plugin:hyprwobbly:mass", "point mass", 1.0F));
    addConfigValue(g_pMoveFactor, makeShared<Config::Values::CFloatValue>("plugin:hyprwobbly:move_factor", "move factor (KWin wobbly)", 0.25F));
    addConfigValue(g_pGrabFalloff, makeShared<Config::Values::CFloatValue>("plugin:hyprwobbly:grab_falloff", "grab-point lag falloff exponent (lower = lag closer to the cursor)", 1.0F));
    addConfigValue(g_pResizeFactor, makeShared<Config::Values::CFloatValue>("plugin:hyprwobbly:resize_factor", "resize factor", 0.45F));
    addConfigValue(g_pMaxWarp, makeShared<Config::Values::CFloatValue>("plugin:hyprwobbly:max_warp", "maximum warp", 140.F));
}

static std::string hkStyleValidInConfigVar(Animation::CHyprAnimationManager* thisptr, const std::string& config, const std::string& style) {
    if (isWobblyStyle(config, style))
        return "";

    return (*(origStyleValid)g_pStyleValidHook->m_original)(thisptr, config, style);
}

static bool hasWobbly(PHLWINDOW pWindow) {
    return std::ranges::any_of(g_transformers, [pWindow](const auto* wobbly) { return wobbly && wobbly->belongsTo(pWindow); });
}

static bool isWobblyTransformer(const UP<Render::IWindowTransformer>& transformer) {
    return std::ranges::any_of(g_transformers, [&](const auto* wobbly) { return static_cast<const void*>(wobbly) == static_cast<const void*>(transformer.get()); });
}

static void addWobbly(PHLWINDOW pWindow) {
    if (!pWindow || hasWobbly(pWindow))
        return;

    // Perf: in floating mode only attach to floating windows — tiled windows
    // then skip the transformed-render path (no extra framebuffer per frame)
    if (g_pMode && g_pMode->value() == "floating" && !pWindow->m_isFloating)
        return;

    pWindow->m_transformers.push_back(makeUnique<CWobblyTransformer>(pWindow));
}

static int onTick(void*) {
    if (!g_pTick)
        return 0;

    static auto LAST = std::chrono::steady_clock::now();
    const auto  NOW  = std::chrono::steady_clock::now();
    const float DT   = std::clamp(std::chrono::duration<float>(NOW - LAST).count(), 0.001F, 0.05F);
    LAST             = NOW;

    const auto TRANSFORMERS = g_transformers; // copy: ticks may destroy transformers
    const bool ANY_ACTIVE   = std::ranges::any_of(TRANSFORMERS, [](const auto* t) { return t && t->isActive(); });
    if (ANY_ACTIVE) {
        for (auto* const transformer : TRANSFORMERS) {
            if (transformer)
                transformer->tick(DT);
        }
    }

    // PERF (release blocker): core renders EVERY window with an attached
    // transformer through the transformed path each frame (pooled work
    // buffer + composite, plus a matte pass when blurred) even when the
    // transformer is passive — floating windows paid that forever, and the
    // cost stacked per floating window ("the longer a window is in floating
    // mode, the laggier the desktop gets"). Detach settled transformers so
    // idle windows cost NOTHING, and re-attach the moment a drag starts:
    // dragController's target is set in dragBegin, so polling it here (idle
    // timer at 25ms) catches a drag within its first frames.
    std::vector<PHLWINDOW> toDetach;
    for (auto* const transformer : TRANSFORMERS) {
        if (transformer && !transformer->isActive())
            if (auto W = transformer->window())
                toDetach.push_back(W);
    }
    for (auto& W : toDetach)
        std::erase_if(W->m_transformers, isWobblyTransformer);

    if (const auto TARGET = g_layoutManager->dragController()->target(); TARGET && TARGET->window())
        addWobbly(TARGET->window());

    const int FRAME_MS = g_pHyprRenderer->m_mostHzMonitor ? sc<int>(1000.0 / g_pHyprRenderer->m_mostHzMonitor->m_refreshRate) : 16;
    const int TIMEOUT  = std::clamp(ANY_ACTIVE ? FRAME_MS : 25, 1, 200);
    wl_event_source_timer_update(g_pTick, TIMEOUT);
    return 0;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        // NO addNotification: toasts pop up on every session start and the
        // user does not want plugin messages on screen. Log only.
        Log::logger->log(Hyprutils::CLI::LOG_ERR, "[hyprwobbly] init failed: version mismatch (headers vs running Hyprland)");
        throw std::runtime_error("[hyprwobbly] Version mismatch");
    }

    registerConfigValues();

    auto FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "styleValidInConfigVar");
    for (auto& fn : FNS) {
        if (!fn.demangled.contains("CHyprAnimationManager"))
            continue;

        g_pStyleValidHook = HyprlandAPI::createFunctionHook(PHANDLE, fn.address, (void*)::hkStyleValidInConfigVar);
        break;
    }

    if (!g_pStyleValidHook || !g_pStyleValidHook->hook()) {
        Log::logger->log(Hyprutils::CLI::LOG_ERR, "[hyprwobbly] init failed: could not hook animation style validator");
        throw std::runtime_error("[hyprwobbly] failed to hook animation style validator");
    }

    g_pHyprOpenGL->makeEGLCurrent();
    g_pWobblyShader = makeShared<CShader>();
    g_shaderReady   = g_pWobblyShader->createProgram(WOBBLY_VERTEX_SHADER, WOBBLY_FRAGMENT_SHADER);
    if (!g_shaderReady) {
        Log::logger->log(Hyprutils::CLI::LOG_ERR, "[hyprwobbly] init failed: shader compilation failed");
        throw std::runtime_error("[hyprwobbly] shader compilation failed");
    }

    g_openWindowListener = Event::bus()->m_events.window.open.listen([](PHLWINDOW pWindow) { addWobbly(pWindow); });
    g_floatingListener   = Event::bus()->m_events.window.floating.listen([](PHLWINDOW pWindow) {
        if (!pWindow)
            return;

        if (pWindow->m_isFloating)
            addWobbly(pWindow);
        else
            std::erase_if(pWindow->m_transformers, isWobblyTransformer);
    });

    g_pTick = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, &onTick, nullptr);
    wl_event_source_timer_update(g_pTick, 1);

    for (auto& window : Desktop::windowState()->windows()) {
        if (window->isHidden() || !window->m_isMapped)
            continue;

        addWobbly(window);
    }

    HyprlandAPI::reloadConfig();
    Log::logger->log(Hyprutils::CLI::LOG_DEBUG, "[hyprwobbly] initialized");

    return {"hyprwobbly", "Compiz-style wobbly windows for Hyprland", "Ivan Malison", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_openWindowListener.reset();
    g_floatingListener.reset();

    // CRITICAL: unhook the styleValidInConfigVar trampoline. Without this,
    // the trampoline points into freed plugin memory after unload -> SEGV.
    if (g_pStyleValidHook) {
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pStyleValidHook);
        delete g_pStyleValidHook;
        g_pStyleValidHook = nullptr;
    }

    if (g_pTick) {
        auto* tick = g_pTick;
        g_pTick    = nullptr; // onTick() checks this before re-arming
        wl_event_source_remove(tick);
    }

    for (auto& window : Desktop::windowState()->windows()) {
        std::erase_if(window->m_transformers, isWobblyTransformer);
    }

    g_transformers.clear();
    if (g_pWobblyShader) {
        g_pWobblyShader->destroy();
        g_pWobblyShader.reset();
    }
    g_shaderReady = false;
}
