#pragma once

#include <string>
#include <string_view>

namespace hc::localization
{
    // Stored UI preference. Supported values: system, pt-BR, en-US.
    std::wstring NormalizePreference(std::wstring value);

    // Resolves system -> one of the languages currently shipped by HC Player.
    // Portuguese Windows uses pt-BR; English Windows uses en-US;
    // every unsupported language falls back to en-US.
    std::wstring ResolveEffectiveLanguage(std::wstring const& preference);

    // Must run before the WinUI App object is created / XAML resources load.
    // It is deliberately non-throwing so localization can never block startup.
    void ApplyPrimaryLanguageOverride(std::wstring const& preference) noexcept;

    // Retrieves a string from the app PRI. Falls back to the supplied literal
    // so localization can never make a settings surface unusable.
    std::wstring GetString(
        std::wstring_view resourceId,
        std::wstring_view fallback) noexcept;

    // Deterministic helper for strings created from C++ instead of XAML.
    // The resource identifier is derived from the UTF-16 fallback text.
    std::wstring GetStringForFallback(
        std::wstring_view scope,
        std::wstring_view fallback) noexcept;
}
