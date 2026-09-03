#pragma once

#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprutils/math/Box.hpp>
#include <vector>
#include <hyprutils/math/Region.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

class CGlassDecoration;

class CGlassPassElement : public IPassElement {
  public:
    struct SGlassPassData {
        WP<CGlassDecoration> decoration;
        float                alpha = 1.0f;
        bool                 glass  = true;  // draw the glass pane
        bool                 shadow = false; // draw the overlap shadow (see PluginConfig OVERLAP_SHADOW_*)
        std::vector<CBox>    beneath;        // boxes of the windows the shadow may fall on (pixel coords)
    };

    explicit CGlassPassElement(const SGlassPassData& data);
    ~CGlassPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;
    [[nodiscard]] bool                needsLiveBlur() override;
    [[nodiscard]] bool                needsPrecomputeBlur() override;
    [[nodiscard]] std::optional<CBox> boundingBox() override;
    [[nodiscard]] bool                disableSimplification() override;

    [[nodiscard]] const char* passName() override { return "CGlassPassElement"; }
    [[nodiscard]] ePassElementType type() override { return EK_CUSTOM; }

  private:
    SGlassPassData m_data;
};
