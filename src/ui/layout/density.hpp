#pragma once
// Density tiers — borrowed from sbtui/gtui DESIGN discipline.
// Cortex-local; inkcell stays dumb about layout policy.

#include <algorithm>

#include "inkcell/surface.hpp"

namespace cortex::mk3::ui::layout {

enum class DensityTier { Narrow, Standard, Wide };

inline DensityTier densityOf(int width) {
    if (width >= 160) return DensityTier::Wide;
    if (width >= 100) return DensityTier::Standard;
    return DensityTier::Narrow;
}

inline DensityTier densityOf(const inkcell::Surface& s) {
    return densityOf(s.bounds().w);
}

inline const char* densityName(DensityTier t) {
    switch (t) {
        case DensityTier::Wide: return "wide";
        case DensityTier::Standard: return "standard";
        case DensityTier::Narrow: return "narrow";
    }
    return "standard";
}

// Content columns for manifests hub.
struct ManifestLayout {
    int listW = 0;
    int detailW = 0;
    int detailX = 0;
    bool showDetail = false;
    bool showTagColumn = false;
    bool showCategoryStrip = true;
    int tagColMax = 0;
};

inline ManifestLayout manifestLayoutFor(int frameW) {
    ManifestLayout L;
    auto tier = densityOf(frameW);
    if (tier == DensityTier::Wide) {
        L.showDetail = true;
        L.showTagColumn = true;
        L.listW = (frameW * 55) / 100;
        L.detailW = frameW - L.listW - 2;
        L.detailX = L.listW + 2;
        L.tagColMax = std::min(36, L.listW / 3);
    } else if (tier == DensityTier::Standard) {
        L.showDetail = true;
        L.showTagColumn = true;
        L.listW = (frameW * 52) / 100;
        L.detailW = frameW - L.listW - 2;
        L.detailX = L.listW + 2;
        L.tagColMax = std::min(28, L.listW / 3);
    } else {
        L.showDetail = false;
        L.showTagColumn = frameW >= 72;
        L.listW = frameW;
        L.tagColMax = frameW >= 72 ? 18 : 0;
    }
    return L;
}

}  // namespace cortex::mk3::ui::layout
