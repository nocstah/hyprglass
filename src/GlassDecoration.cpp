#include "GlassDecoration.hpp"
#include "BuiltInPresets.hpp"
#include "GlassPassElement.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "WindowGeometry.hpp"

#include <algorithm>
#include <GLES3/gl32.h>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Misc.hpp>

CGlassDecoration::CGlassDecoration(PHLWINDOW window)
    : IHyprWindowDecoration(window), m_window(window) {
}

CGlassDecoration::~CGlassDecoration() {
    withdrawNoBlur();
}

// Glass replaces Hyprland's blur for this window. Mark glassed windows with
// the noblur property so Hyprland composites their translucency against the
// live framebuffer (which contains the glass) instead of its pre-frame cached
// blur snapshot, which is captured before plugin decorations render (#46).
void CGlassDecoration::updateNoBlurProp(bool glassEnabled) {
    const auto& config = g_pGlobalState->config;
    const bool manage = config.manageWindowBlur && **config.manageWindowBlur;

    if (!manage || !glassEnabled) {
        withdrawNoBlur();
        return;
    }

    if (m_noBlurApplied)
        return;

    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            window->m_ruleApplicator->noBlur().set(true, Desktop::Types::PRIORITY_SET_PROP);
            m_noBlurApplied = true;
            damageEntire();
        }
    } catch (...) {}
}

void CGlassDecoration::withdrawNoBlur() {
    if (!m_noBlurApplied)
        return;
    m_noBlurApplied = false;

    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            window->m_ruleApplicator->noBlur().unset(Desktop::Types::PRIORITY_SET_PROP);
            damageEntire();
        }
    } catch (...) {}
}

// Fullscreen toggles re-apply window rules, which can drop the noblur prop
// while m_noBlurApplied still claims it's held.
void CGlassDecoration::onFullscreenStateChanged() {
    m_noBlurApplied = false;
    damageEntire();
}

bool CGlassDecoration::resolveEnabled() const {
    const auto& config = g_pGlobalState->config;
    const bool globalEnabled = config.enabled && **config.enabled;

    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            const auto& tags = window->m_ruleApplicator->m_tagKeeper;
            // isTagged() already matches dynamic tags ("tag*") — no stripping needed here.
            // Disabled tag wins over enabled tag if both are present.
            if (tags.isTagged(std::string(TAG_DISABLED)))
                return false;
            if (tags.isTagged(std::string(TAG_ENABLED)))
                return true;
        }
    } catch (...) {}

    return globalEnabled;
}

bool CGlassDecoration::resolveThemeIsDark() const {
    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            const std::string lightTag = std::string(TAG_THEME_PREFIX) + "light";
            const std::string darkTag  = std::string(TAG_THEME_PREFIX) + "dark";
            if (window->m_ruleApplicator->m_tagKeeper.isTagged(lightTag))
                return false;
            if (window->m_ruleApplicator->m_tagKeeper.isTagged(darkTag))
                return true;
        }

        const auto& config = g_pGlobalState->config;
        const auto theme = readStringConfig(config.defaultTheme);
        if (!theme.empty())
            return theme != "light";
    } catch (...) {}

    return true;
}

bool CGlassDecoration::resolveXray() const {
    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            const auto& tags = window->m_ruleApplicator->m_tagKeeper;
            // As with Hyprland's `xray off` window rule, off wins over the global.
            if (tags.isTagged(std::string(TAG_NOXRAY)))
                return false;
            if (tags.isTagged(std::string(TAG_XRAY)))
                return true;
        }
    } catch (...) {}

    const auto& config = g_pGlobalState->config;
    return config.xray && **config.xray;
}

std::string CGlassDecoration::resolvePresetName() const {
    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            for (const auto& tag : window->m_ruleApplicator->m_tagKeeper.getTags()) {
                if (tag.starts_with(TAG_PRESET_PREFIX))
                    return stripDynamicTagMarker(tag.substr(TAG_PRESET_PREFIX.size()));
            }
        }

        const auto& config = g_pGlobalState->config;
        const auto preset = readStringConfig(config.defaultPreset);
        if (!preset.empty())
            return std::string(preset);
    } catch (...) {}

    return "default";
}

SDecorationPositioningInfo CGlassDecoration::getPositioningInfo() {
    SDecorationPositioningInfo info;
    info.priority       = 10000;
    info.policy         = DECORATION_POSITION_ABSOLUTE;
    // The overlap shadow reaches this far outside the window; reporting it
    // makes Hyprland's own window damage (focus changes, moves) include it.
    const double r      = overlapShadowRange();
    info.desiredExtents = {{r, r}, {r, r}};
    return info;
}

void CGlassDecoration::onPositioningReply(const SDecorationPositioningReply& reply) {}

void CGlassDecoration::draw(PHLMONITOR monitor, float const& alpha) {
    if (!g_pGlobalState)
        return;

    const bool enabled = resolveEnabled();
    updateNoBlurProp(enabled);

    // X-ray: ask for next frame's pre-window snapshot while we still sample it.
    if (enabled && monitor && resolveXray()) {
        auto& snapshot          = g_pGlobalState->backgroundSnapshots[monitor->m_id];
        snapshot.requestedFrame = snapshot.frame;
    }

    // Overlap shadow is independent of the glass: an unfocused window that
    // covers another one gets it whether or not it is glassed.
    std::vector<CBox> beneath;
    const bool        shadow = overlapShadowRange() > 0 && resolveOverlapShadow(monitor, &beneath);
    {
        std::vector<std::array<int, 4>> rects;
        for (const auto& b : beneath)
            rects.push_back({static_cast<int>(b.x), static_cast<int>(b.y), static_cast<int>(b.w), static_cast<int>(b.h)});
        if (shadow != m_lastShadow || rects != m_lastShadowClip) {
            m_lastShadow     = shadow;
            m_lastShadowClip = std::move(rects);
            damageEntire();
        }
    }
    if (!enabled && !shadow)
        return;

    CGlassPassElement::SGlassPassData data{m_self, alpha, enabled, shadow, shadow ? beneath : std::vector<CBox>{}};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassPassElement>(data));

    const auto window = m_window.lock();
    if (window) {
        const auto workspace = window->m_workspace;

        const bool wsAnimating = workspace && !window->m_pinned && workspace->m_renderOffset->isBeingAnimated();
        if (wsAnimating)
            damageEntire();

        const auto currentPosition = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto currentSize = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const bool moved = currentPosition != m_lastPosition || currentSize != m_lastSize;
        if (moved) {
            damageEntire();
            m_lastPosition = currentPosition;
            m_lastSize = currentSize;
        }

        // Bump layer cache only for actual scene changes (window moved/animating),
        // NOT from damageEntire() which fires in the damage system feedback path.
        if (moved || wsAnimating) {
            if (auto mon = window->m_monitor.lock())
                g_pGlobalState->bumpSceneGeneration(mon);
        }
    }
}

PHLWINDOW CGlassDecoration::getOwner() {
    return m_window.lock();
}

int CGlassDecoration::overlapShadowRange() {
    if (!g_pGlobalState)
        return 0;
    const auto& config = g_pGlobalState->config;
    return config.overlapShadowRange ? static_cast<int>(std::max<Hyprlang::INT>(0, **config.overlapShadowRange)) : 0;
}

bool CGlassDecoration::resolveOverlapShadow(PHLMONITOR monitor, std::vector<CBox>* beneath) const {
    const auto window = m_window.lock();
    if (!window || !monitor || !window->m_workspace)
        return false;

    try {
        // The focused window keeps Hyprland's own shadow; a fullscreen one
        // covers everything and casts nothing.
        if (Desktop::focusState()->window() == window || Fullscreen::controller()->isFullscreen(window))
            return false;

        const auto optBox = WindowGeometry::computeWindowBox(window, monitor);
        if (!optBox)
            return false;
        CBox shadowBox = *optBox;
        shadowBox.expand(overlapShadowRange() * monitor->m_scale);

        // Hyprland's render order: the workspace's tiled windows, then its
        // floating ones, then the special workspace (tiled, floating), then
        // pinned windows; within a class, the window list order.
        auto rank = [](const PHLWINDOW& w) {
            if (w->m_pinned)
                return 4;
            const bool special = w->m_workspace && w->m_workspace->m_isSpecialWorkspace;
            return (special ? 2 : 0) + (w->m_isFloating ? 1 : 0);
        };
        const int   selfRank   = rank(window);
        const auto& windows    = Desktop::windowState()->windows();
        bool        found      = false;
        bool        passedSelf = false;

        for (const auto& other : windows) {
            if (other == window) {
                passedSelf = true;
                continue;
            }
            if (!other || !other->m_isMapped || other->isHidden() || !other->m_workspace)
                continue;
            const auto ws = other->m_workspace;
            const bool visibleHere = other->m_monitor == monitor && (ws == monitor->m_activeWorkspace || ws == monitor->m_activeSpecialWorkspace);
            if (!visibleHere)
                continue;
            const int otherRank = rank(other);
            bool      isBeneath;
            if (otherRank != selfRank)
                isBeneath = otherRank < selfRank;
            else if (!window->m_isFloating && !other->m_isFloating)
                isBeneath = false; // two tiled windows never overlap
            else
                isBeneath = !passedSelf; // same class: list order
            if (!isBeneath)
                continue;

            const auto otherBox = WindowGeometry::computeWindowBox(other, monitor);
            if (!otherBox)
                continue;
            CBox hit = *otherBox;
            if (hit.intersection(shadowBox).empty())
                continue;
            found = true;
            if (!beneath)
                return true;
            beneath->push_back(hit);
        }
        return found;
    } catch (...) {}
    return false;
}

void CGlassDecoration::drawOverlapShadow(PHLMONITOR monitor, const CBox& windowBox, const std::vector<CBox>& beneath, float alpha) {
    const auto window = m_window.lock();
    if (!window || !monitor)
        return;
    const auto& config = g_pGlobalState->config;
    const int   range  = overlapShadowRange();
    if (range <= 0)
        return;

    const float scale = monitor->m_scale;
    CBox        fullBox = windowBox;
    fullBox.expand(range * scale);

    // The pane's own box is taken out as a CROSS — the box inset by the
    // corner radius on each axis — so the four corner squares stay in: there
    // the shader cuts out the pane's real rounded shape (it knows the window
    // through m_renderData.currentWindow). Taking the whole rectangle out
    // left the corner patches between arc and box without any shadow, a
    // light square poking out of every rounded corner.
    const float rounding      = window->rounding() * scale;
    const float roundingPower = window->roundingPower(); // the cutout and the falloff share it; must match the pane
    const float r             = std::min({rounding, static_cast<float>(windowBox.w / 2), static_cast<float>(windowBox.h / 2)});
    CRegion     cross;
    cross.add(CBox{windowBox.x + r, windowBox.y, windowBox.w - 2 * r, windowBox.h});
    cross.add(CBox{windowBox.x, windowBox.y + r, windowBox.w, windowBox.h - 2 * r});

    const uint32_t   rgba  = config.overlapShadowColor ? static_cast<uint32_t>(**config.overlapShadowColor) : 0x00000040u;
    const CHyprColor color{((rgba >> 24) & 0xff) / 255.f, ((rgba >> 16) & 0xff) / 255.f, ((rgba >> 8) & 0xff) / 255.f, (rgba & 0xff) / 255.f};
    const Config::CGradientValueData grad{color};

    // renderRoundedShadow ignores any scissor set by the caller: it derives
    // its own draw region from the frame damage and scissors per damage rect
    // itself. So clip by handing it the damage already intersected with the
    // wanted region, draw, and put the damage back. It also needs to know
    // which window it is shading so the shader cuts the pane's own (rounded)
    // box out instead of painting the interior; that is
    // m_renderData.currentWindow, which a plugin pass element does not get.
    auto&         renderData  = g_pHyprRenderer->m_renderData;
    const CRegion savedDamage = renderData.damage;
    const auto    savedWindow = renderData.currentWindow;
    auto          drawIn      = [&](CRegion region, float a) {
        region.intersect(fullBox);
        region.subtract(cross);
        region.intersect(savedDamage);
        if (region.empty())
            return;
        renderData.damage        = region;
        renderData.currentWindow = window;
        g_pHyprRenderer->drawShadow(fullBox, static_cast<int>(rounding), roundingPower, static_cast<int>(range * scale), grad, a);
    };

    const bool clipToBeneath = config.overlapShadowClip && **config.overlapShadowClip;
    if (!clipToBeneath) {
        // The full ring around the pane.
        drawIn(CRegion{fullBox}, alpha);
    } else {
        // Contact shadow: full strength wherever the pane lies over a window
        // beneath, cut at that window's outline; the part of the pane resting
        // on the wallpaper casts nothing.
        CRegion region;
        for (const auto& box : beneath)
            region.add(box);
        drawIn(region, alpha);
    }
    renderData.currentWindow = savedWindow;
    renderData.damage        = savedDamage;
}

void CGlassDecoration::renderPass(PHLMONITOR monitor, const float& alpha, bool glass, bool shadow, const std::vector<CBox>& beneath) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    shaderManager.initializeIfNeeded();

    if (!shaderManager.isInitialized())
        return;

    const auto window = m_window.lock();
    if (!window)
        return;

    const auto source = g_pHyprRenderer->m_renderData.currentFB;
    if (!source)
        return;

    // The overlap shadow is drawn AFTER the pane (see the end of this
    // function): the pane paints its padded margin, which would otherwise
    // erase the ring.
    auto drawShadowIfWanted = [&]() {
        if (!shadow || beneath.empty())
            return;
        if (const auto box = WindowGeometry::computeWindowBox(window, monitor); box) {
            float a = window->alphaTotalWithout(Desktop::View::WINDOW_ALPHA_ACTIVE);
            if (const auto workspace = window->m_workspace; workspace && !window->m_pinned)
                a *= workspace->m_alpha->value();
            drawOverlapShadow(monitor, *box, beneath, a);
        }
    };
    if (!glass) {
        drawShadowIfWanted();
        return;
    }

    // What the glass is made from: the live frame (wallpaper plus whatever
    // windows were already drawn beneath this one) or, with x-ray, the
    // pre-window snapshot — wallpaper only. Until the snapshot is complete
    // (the first frames after it was asked for, or after a mode change) fall
    // back to the frame.
    auto sampleSource = source;
    if (monitor && resolveXray()) {
        const auto it = g_pGlobalState->backgroundSnapshots.find(monitor->m_id);
        if (it != g_pGlobalState->backgroundSnapshots.end() && it->second.complete && it->second.fb && it->second.fb->m_size == source->m_size)
            sampleSource = it->second.fb;
    }

    auto optBox = WindowGeometry::computeWindowBox(window, monitor);
    if (!optBox)
        return;

    CBox windowBox    = *optBox;
    CBox transformBox = windowBox;

    const auto transform = Math::wlTransformToHyprutils(
        Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform));
    transformBox.transform(transform,
        g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.x,
        g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.y);

    const bool isDark          = resolveThemeIsDark();
    const std::string preset   = resolvePresetName();
    const SResolveContext ctx  = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

    float blurStrength   = resolvePresetFloat(ctx, &SPresetValues::blurStrength, &SOverridableConfig::blurStrength);
    int downscale        = blurStrength >= GlassRenderer::BLUR_DOWNSCALE_THRESHOLD ? GlassRenderer::BLUR_DOWNSCALE_MAX : 1;

    GlassRenderer::sampleBackground(m_sampleFramebuffer, sampleSource, transformBox, m_samplePaddingRatio, downscale);

    float blurRadius     = blurStrength * 12.0f / downscale;
    int blurIterations   = std::clamp(static_cast<int>(resolvePresetInt(ctx, &SPresetValues::blurIterations, &SOverridableConfig::blurIterations)), 1, 5);
    GlassRenderer::blurBackground(m_sampleFramebuffer, blurRadius, blurIterations, source);

    float monitorScale  = monitor->m_scale;

    // Hyprland renders internal-fullscreen windows unrounded (dontRound), we need to
    // match, or the glass would show rounded gaps at the screen corners
    const bool fsUnrounded = Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN;
    float cornerRadius  = fsUnrounded ? 0.0f : window->rounding() * monitorScale;
    float roundingPower = window->roundingPower();

    // The render alpha Hyprland hands decorations is activeInactive * fade.
    // Glass must follow fades (open/close, fullscreen, workspace moves) but
    // not the active/inactive dimming or opacity rules: those make the surface
    // more translucent — revealing more glass — and shouldn't wash out the
    // glass pane itself. Rebuild the fade-only alpha from its components.
    float glassAlpha = window->alphaTotalWithout(Desktop::View::WINDOW_ALPHA_ACTIVE);
    if (const auto workspace = window->m_workspace; workspace && !window->m_pinned)
        glassAlpha *= workspace->m_alpha->value();

    GlassRenderer::applyGlassEffect(m_sampleFramebuffer, source,
                                     windowBox, transformBox, glassAlpha,
                                     cornerRadius, roundingPower, m_samplePaddingRatio, ctx);
    drawShadowIfWanted();
}

eDecorationType CGlassDecoration::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CGlassDecoration::updateWindow(PHLWINDOW window) {
    damageEntire();
}

void CGlassDecoration::damageEntire() {
    const auto window = m_window.lock();
    if (!window)
        return;

    const auto workspace = window->m_workspace;
    auto surfaceBox = window->getWindowMainSurfaceBox();

    if (workspace && workspace->m_renderOffset->isBeingAnimated() && !window->m_pinned)
        surfaceBox.translate(workspace->m_renderOffset->value());
    surfaceBox.translate(window->m_floatingOffset);

    // Expand damage by our sampling padding so the render pass re-renders
    // background content (wallpaper, other windows) in the padded margin.
    // Without this, the scissored render pass leaves stale previous-frame
    // content in the padding area, causing noise artifacts.
    // surfaceBox is in logical coords; convert pixel padding to logical.
    const auto monitor = window->m_monitor.lock();
    const float scale = monitor ? monitor->m_scale : 1.0f;
    surfaceBox.expand(std::max(GlassRenderer::SAMPLE_PADDING_PX / scale, static_cast<float>(overlapShadowRange())));

    g_pHyprRenderer->damageBox(surfaceBox);
}

eDecorationLayer CGlassDecoration::getDecorationLayer() {
    return DECORATION_LAYER_BOTTOM;
}

uint64_t CGlassDecoration::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

std::string CGlassDecoration::getDisplayName() {
    return "HyprGlass";
}
