#include "pch.h"
#include "LocalizationManager.h"

#include <winrt/Microsoft.Windows.Globalization.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Windows.System.UserProfile.h>

#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <cwchar>

namespace
{
    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return value;
    }

    uint32_t Fnv1aUtf16(std::wstring_view text)
    {
        uint32_t hash = 2166136261u;
        for (wchar_t ch : text)
        {
            uint16_t code = static_cast<uint16_t>(ch);
            hash ^= static_cast<uint8_t>(code & 0xffu);
            hash *= 16777619u;
            hash ^= static_cast<uint8_t>((code >> 8) & 0xffu);
            hash *= 16777619u;
        }
        return hash;
    }

    bool IsLanguage(std::wstring const& tag, std::wstring const& base)
    {
        return tag == base ||
            (tag.size() > base.size() &&
             tag.starts_with(base) &&
             tag[base.size()] == L'-');
    }
}

namespace hc::localization
{
    std::wstring NormalizePreference(std::wstring value)
    {
        auto lower = Lower(value);
        if (lower == L"pt-br") return L"pt-BR";
        if (lower == L"en-us") return L"en-US";
        return L"system";
    }

    std::wstring ResolveEffectiveLanguage(std::wstring const& preference)
    {
        auto normalized = NormalizePreference(preference);
        if (normalized == L"pt-BR" || normalized == L"en-US")
            return normalized;

        try
        {
            auto languages =
                winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages();

            for (auto const& language : languages)
            {
                auto tag = Lower(std::wstring{ language.c_str() });
                if (IsLanguage(tag, L"pt")) return L"pt-BR";
                if (IsLanguage(tag, L"en")) return L"en-US";
            }
        }
        catch (...)
        {
            // Language detection is presentation-only. Never make app startup
            // depend on a globalization API being available.
        }

        return L"en-US";
    }

    void ApplyPrimaryLanguageOverride(std::wstring const& preference) noexcept
    {
        try
        {
            auto effective = ResolveEffectiveLanguage(preference);
            winrt::Microsoft::Windows::Globalization::ApplicationLanguages::
                PrimaryLanguageOverride(winrt::hstring{ effective });
        }
        catch (...)
        {
            // Resource localization is optional presentation state. If the
            // override cannot be applied, keep the app fully usable with the
            // literal Portuguese fallback strings already present in XAML.
        }
    }

    std::wstring GetString(
        std::wstring_view resourceId,
        std::wstring_view fallback) noexcept
    {
        try
        {
            winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader loader;
            auto value = loader.GetString(winrt::hstring{ resourceId });
            if (!value.empty())
            {
                return value.c_str();
            }
        }
        catch (...)
        {
            // C++-created presentation strings use the same non-fatal policy
            // as the XAML resource layer.
        }
        return std::wstring{ fallback };
    }

    std::wstring GetStringForFallback(
        std::wstring_view scope,
        std::wstring_view fallback) noexcept
    {
        wchar_t suffix[9]{};
        swprintf_s(suffix, L"%08X", Fnv1aUtf16(fallback));
        std::wstring key{ scope };
        key += L"_";
        key += suffix;
        return GetString(key, fallback);
    }

}
