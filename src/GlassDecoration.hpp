#pragma once

#include <array>
#include <vector>

#include "GlassRenderer.hpp"
#include "PluginConfig.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/Framebuffer.hpp>

class CGlassDecoration : public IHyprWindowDecoration {
  public:
    explicit CGlassDecoration(PHLWINDOW window);
    ~CGlassDecoration() override;

    [[nodiscard]] SDecorationPositioningInfo getPositioningInfo() override;
    void                                     onPositioningReply(const SDecorationPositioningReply& reply) override;
    void                                     draw(PHLMONITOR monitor, float const& alpha) override;
    [[nodiscard]] eDecorationType            getDecorationType() override;
    void                                     updateWindow(PHLWINDOW window) override;
    void                                     damageEntire() override;
    [[nodiscard]] eDecorationLayer           getDecorationLayer() override;
    [[nodiscard]] uint64_t                   getDecorationFlags() override;
    [[nodiscard]] std::string                getDisplayName() override;

    [[nodiscard]] PHLWINDOW getOwner();
    void                    renderPass(PHLMONITOR monitor, const float& alpha, bool glass, bool shadow, const std::vector<CBox>& beneath);
    void                    onFullscreenStateChanged();

    // Configured overlap shadow range in logical pixels (0 = off).
    [[nodiscard]] static int overlapShadowRange();

    WP<CGlassDecoration> m_self;

  private:
    PHLWINDOWREF m_window;
    SP<Render::IFramebuffer> m_sampleFramebuffer;
    Vector2D     m_samplePaddingRatio;

    // Track last rendered position/size to detect actual changes and seed damage
    Vector2D m_lastPosition;
    Vector2D m_lastSize;

    // Whether we currently hold the noblur window property on our window.
    // Glass replaces Hyprland's blur, and Hyprland's cached-blur optimization
    // (blur:new_optimizations) composites translucent windows against a
    // snapshot taken before plugin decorations render — without noblur the
    // glass is invisible on static windows (#46).
    bool m_noBlurApplied = false;

    void updateNoBlurProp(bool glassEnabled);
    void withdrawNoBlur();

    [[nodiscard]] bool        resolveEnabled() const;
    [[nodiscard]] bool        resolveXray() const;
    // Whether this window is unfocused, not fullscreen, and has at least one
    // window drawn beneath it that its shadow box touches; fills `beneath`
    // (monitor-local pixel coords) with those windows' boxes.
    [[nodiscard]] bool        resolveOverlapShadow(PHLMONITOR monitor, std::vector<CBox>* beneath) const;
    void                      drawOverlapShadow(PHLMONITOR monitor, const CBox& windowBox, const std::vector<CBox>& beneath, float alpha);
    // Last frame's overlap-shadow state, to damage the whole ring when it
    // changes (focus gained/lost, a window beneath moved): the frame damage
    // that triggers such a change (the pane's box, its border) does not
    // cover the ring, so without this only the damaged slice of the ring is
    // ever painted and hard-edged partial shadows are left behind.
    bool                        m_lastShadow = false;
    std::vector<std::array<int, 4>> m_lastShadowClip;
    [[nodiscard]] bool        resolveThemeIsDark() const;
    [[nodiscard]] std::string resolvePresetName() const;

    friend class CGlassPassElement;
};
