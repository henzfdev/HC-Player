#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

namespace hc::crop
{
    struct Preset
    {
        wchar_t const* label;
        double aspect;
    };

    inline constexpr Preset Presets[] = {
        { L"Original", 0.0 },
        { L"16:9", 16.0 / 9.0 },
        { L"4:3", 4.0 / 3.0 },
        { L"2.35:1", 2.35 },
        { L"2.39:1", 2.39 },
    };
    inline constexpr int PresetCount = 5;

    inline constexpr int NextPreset(int current, bool backwards)
    {
        if (current < 0 || current >= PresetCount) return 0;
        return (current + (backwards ? PresetCount - 1 : 1)) % PresetCount;
    }

    // Crop in source pixels, accounting for anamorphic pixels and display rotation.
    inline std::pair<int, int> Dimensions(
        int width, int height, double pixelAspect, int rotation, double aspect)
    {
        if (width <= 0 || height <= 0 || !std::isfinite(pixelAspect) ||
            pixelAspect <= 0.0 || !std::isfinite(aspect) || aspect <= 0.0)
            return {};
        // ponytail: preset ratios support quarter turns; arbitrary angles need rotated bounds.
        if (rotation % 90 != 0) return {};
        if (rotation % 180 != 0) aspect = 1.0 / aspect;
        double const sourceAspect = static_cast<double>(width) * pixelAspect / height;
        if (!std::isfinite(sourceAspect) || sourceAspect <= 0.0) return {};
        if (sourceAspect > aspect)
            width = (std::max)(1, static_cast<int>(std::round(width * aspect / sourceAspect)));
        else
            height = (std::max)(1, static_cast<int>(std::round(height * sourceAspect / aspect)));
        return { width, height };
    }
}
