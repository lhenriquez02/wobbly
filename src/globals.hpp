#pragma once

#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Shader.hpp>

#include <vector>

class CWobblyTransformer;

inline HANDLE                           PHANDLE = nullptr;
inline CHyprSignalListener              g_openWindowListener;
inline CHyprSignalListener              g_floatingListener;
inline CFunctionHook*                   g_pStyleValidHook = nullptr;
inline wl_event_source*                 g_pTick           = nullptr;
inline SP<CShader>                      g_pWobblyShader;
inline bool                             g_shaderReady = false;
inline std::vector<CWobblyTransformer*> g_transformers;

inline SP<Config::Values::CIntValue>    g_pEnabled;
inline SP<Config::Values::CIntValue>    g_pTestIdentity;
inline SP<Config::Values::CIntValue>    g_pDeformBlurMatte;
inline SP<Config::Values::CStringValue> g_pMode;
inline SP<Config::Values::CIntValue>    g_pGridWidth;
inline SP<Config::Values::CIntValue>    g_pGridHeight;
inline SP<Config::Values::CIntValue>    g_pTilesX;
inline SP<Config::Values::CIntValue>    g_pTilesY;
inline SP<Config::Values::CFloatValue>  g_pSpringK;
inline SP<Config::Values::CFloatValue>  g_pFriction;
inline SP<Config::Values::CFloatValue>  g_pMass;
inline SP<Config::Values::CFloatValue>  g_pMoveFactor;
inline SP<Config::Values::CFloatValue>  g_pGrabFalloff;
inline SP<Config::Values::CFloatValue>  g_pResizeFactor;
inline SP<Config::Values::CFloatValue>  g_pMaxWarp;
