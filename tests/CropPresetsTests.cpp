#include "../HC Player/CropPresets.h"

#include <cassert>
#include <limits>
#include <string_view>

int main()
{
    using namespace hc::crop;
    assert(NextPreset(0, false) == 1);
    assert(NextPreset(PresetCount - 1, false) == 0);
    assert(NextPreset(0, true) == PresetCount - 1);
    assert(NextPreset(-1, false) == 0);
    assert(NextPreset(-1, true) == 0);
    for (int index = 0; index < PresetCount; ++index)
        assert(NextPreset(NextPreset(index, false), true) == index);
    for (int index = 1; index < PresetCount; ++index)
        assert(Presets[index].aspect > Presets[index - 1].aspect);

    for (auto [label, expectedHeight] : { std::pair{L"1.8:1", 1067},
                                        std::pair{L"2:1", 960},
                                        std::pair{L"2.2:1", 873} })
    {
        auto preset = std::find_if(std::begin(Presets), std::end(Presets),
            [label](Preset const& value) { return std::wstring_view(value.label) == label; });
        assert(preset != std::end(Presets));
        assert(Dimensions(1920, 1080, 1.0, 0, preset->aspect) ==
               std::pair(1920, expectedHeight));
    }

    auto size = Dimensions(1920, 1080, 1.0, 0, 2.39);
    assert(size.first == 1920 && size.second == 803);
    size = Dimensions(1920, 1080, 1.0, 0, 4.0 / 3.0);
    assert(size.first == 1440 && size.second == 1080);
    size = Dimensions(1920, 1080, 1.0, 0, 16.0 / 9.0);
    assert(size.first == 1920 && size.second == 1080);
    // Anamorphic DVD: the source pixels do not have the displayed aspect ratio.
    size = Dimensions(720, 576, 64.0 / 45.0, 0, 4.0 / 3.0);
    assert(size.first == 540 && size.second == 576);
    // A portrait source rotated into landscape must crop in source coordinates.
    size = Dimensions(1080, 1920, 1.0, 90, 2.39);
    assert(size.first == 803 && size.second == 1920);
    auto rotated = Dimensions(1080, 1920, 1.0, 270, 2.39);
    assert(rotated == size);
    assert(Dimensions(1920, 1080, 1.0, 180, 2.39) ==
           Dimensions(1920, 1080, 1.0, 0, 2.39));
    assert(Dimensions(1920, 1080, 1.0, 45, 2.39) == std::pair(0, 0));

    for (auto [width, height] : { std::pair{3840, 2160}, std::pair{640, 480},
                                std::pair{1080, 1920}, std::pair{1, 1} })
    {
        for (int index = 1; index < PresetCount; ++index)
        {
            auto [croppedWidth, croppedHeight] = Dimensions(
                width, height, 1.0, 0, Presets[index].aspect);
            assert(croppedWidth > 0 && croppedWidth <= width);
            assert(croppedHeight > 0 && croppedHeight <= height);
            assert(croppedWidth == width || croppedHeight == height);
        }
    }
    assert(Dimensions(0, 1080, 1.0, 0, 2.39) == std::pair(0, 0));
    assert(Dimensions(1920, -1, 1.0, 0, 2.39) == std::pair(0, 0));
    assert(Dimensions(1920, 1080, 0.0, 0, 2.39) == std::pair(0, 0));
    assert(Dimensions(1920, 1080, 1.0, 0, 0.0) == std::pair(0, 0));
    assert(Dimensions(1920, 1080, std::numeric_limits<double>::quiet_NaN(),
                      0, 2.39) == std::pair(0, 0));
    assert(Dimensions(1920, 1080, std::numeric_limits<double>::infinity(),
                      0, 2.39) == std::pair(0, 0));
}
