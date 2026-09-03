#pragma once

#include <string>
#include <vector>

namespace MediaInfoBridge
{
    enum class StreamKind : size_t
    {
        General = 0,
        Video = 1,
        Audio = 2,
        Text = 3,
        Other = 4,
        Image = 5,
        Menu = 6,
    };

    struct Field
    {
        std::wstring name;
        std::wstring value;
    };

    struct Section
    {
        StreamKind kind{ StreamKind::General };
        size_t streamIndex{};
        std::wstring title;
        std::vector<Field> fields;
    };

    struct Analysis
    {
        std::vector<Section> sections;

        // Kept only for "Copy all". The visible HC Player UI should use
        // the structured sections above instead of parsing this raw text.
        std::wstring rawReport;
    };

    // Returns true only when MediaInfo.dll can be loaded from the same
    // directory as the HC Player executable and all required exports exist.
    bool IsAvailable();

    // New structured API. It asks MediaInfo only for the fields HC Player
    // actually wants to display, avoiding the repeated aliases found in
    // Complete=1 / Inform().
    bool AnalyzeFileStructured(
        std::wstring const& filePath,
        Analysis& analysis,
        std::wstring& error);

    // Backward-compatible raw report API. Keep this while the current
    // MediaInfoPage is being migrated to the structured layout.
    bool AnalyzeFile(
        std::wstring const& filePath,
        std::wstring& report,
        std::wstring& error);
}
