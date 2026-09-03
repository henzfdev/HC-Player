#include "pch.h"
#include "SettingsPage.h"
#include "PlayerBridge.h"
#include "LocalizationManager.h"
#include "StoragePaths.h"

#include <shobjidl.h>
#include <dwrite_3.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <functional>
#include <set>
#include <sstream>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.System.h>

#pragma comment(lib, "dwrite.lib")

namespace
{
    winrt::fire_and_forget OpenHCPlayerDefaultAppsSettingsAsync()
    {
        try
        {
            using namespace winrt::Windows::Foundation;
            using namespace winrt::Windows::System;

            bool const launched = co_await Launcher::LaunchUriAsync(
                Uri(L"ms-settings:defaultapps?registeredAppUser=HC%20Player"));

            if (!launched)
            {
                co_await Launcher::LaunchUriAsync(
                    Uri(L"ms-settings:defaultapps"));
            }
        }
        catch (...)
        {
            // Windows integration is optional and must never affect settings
            // or player operation if the Settings URI cannot be launched.
        }
    }

    winrt::fire_and_forget OpenHCPlayerRepositoryAsync()
    {
        try
        {
            using namespace winrt::Windows::Foundation;
            using namespace winrt::Windows::System;

            co_await Launcher::LaunchUriAsync(
                Uri(L"https://github.com/henzfdev/HC-Player"));
        }
        catch (...)
        {
            // Opening an external project page is optional and must never
            // affect the settings page or player operation.
        }
    }

    std::filesystem::path ImportedFontsDirectory()
    {
        return hc::storage::UserDataRoot() / L"fonts";
    }

    bool IsFontFile(std::filesystem::path const& path)
    {
        std::wstring extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        return extension == L".ttf" || extension == L".otf" || extension == L".ttc";
    }

    std::wstring PreferredLocalizedFontName(IDWriteLocalizedStrings* strings)
    {
        if (!strings || strings->GetCount() == 0) return {};

        UINT32 index{};
        BOOL found{};
        wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
        if (GetUserDefaultLocaleName(localeName, ARRAYSIZE(localeName)) > 0)
        {
            strings->FindLocaleName(localeName, &index, &found);
        }
        if (!found)
        {
            strings->FindLocaleName(L"en-us", &index, &found);
        }
        if (!found) index = 0;

        UINT32 length{};
        if (FAILED(strings->GetStringLength(index, &length))) return {};
        std::wstring value(length + 1, L'\0');
        if (FAILED(strings->GetString(index, value.data(), length + 1))) return {};
        value.resize(length);
        return value;
    }

    struct ImportedFontChoice
    {
        std::wstring displayName;
        std::wstring mpvName;
        std::wstring familyName;
    };

    std::wstring FontInformationalName(
        IDWriteFontFace3* face, DWRITE_INFORMATIONAL_STRING_ID id)
    {
        if (!face) return {};

        BOOL exists{};
        winrt::com_ptr<IDWriteLocalizedStrings> strings;
        if (FAILED(face->GetInformationalStrings(
            id, strings.put(), &exists)) || !exists || !strings.get())
        {
            return {};
        }
        return PreferredLocalizedFontName(strings.get());
    }

    std::vector<ImportedFontChoice> FontChoicesFromFile(
        std::filesystem::path const& path)
    {
        std::vector<ImportedFontChoice> choices;
        std::set<std::wstring> technicalNames;

        winrt::com_ptr<IDWriteFactory> factory;
        if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.put()))))
        {
            return {};
        }

        winrt::com_ptr<IDWriteFontFile> fontFile;
        if (FAILED(factory->CreateFontFileReference(
            path.c_str(), nullptr, fontFile.put())))
        {
            return {};
        }

        BOOL supported{};
        DWRITE_FONT_FILE_TYPE fileType{};
        DWRITE_FONT_FACE_TYPE faceType{};
        UINT32 faceCount{};
        if (FAILED(fontFile->Analyze(
            &supported, &fileType, &faceType, &faceCount)) ||
            !supported || faceCount == 0)
        {
            return {};
        }

        IDWriteFontFile* files[]{ fontFile.get() };
        for (UINT32 faceIndex = 0; faceIndex < faceCount; ++faceIndex)
        {
            winrt::com_ptr<IDWriteFontFace> face;
            if (FAILED(factory->CreateFontFace(
                faceType,
                1,
                files,
                faceIndex,
                DWRITE_FONT_SIMULATIONS_NONE,
                face.put())))
            {
                continue;
            }

            winrt::com_ptr<IDWriteFontFace3> face3;
            if (FAILED(face->QueryInterface(IID_PPV_ARGS(face3.put()))))
            {
                continue;
            }

            std::wstring familyName;
            winrt::com_ptr<IDWriteLocalizedStrings> familyNames;
            if (SUCCEEDED(face3->GetFamilyNames(familyNames.put())))
            {
                familyName = PreferredLocalizedFontName(familyNames.get());
            }

            std::wstring faceName;
            winrt::com_ptr<IDWriteLocalizedStrings> faceNames;
            if (SUCCEEDED(face3->GetFaceNames(faceNames.put())))
            {
                faceName = PreferredLocalizedFontName(faceNames.get());
            }

            auto fullName = FontInformationalName(
                face3.get(), DWRITE_INFORMATIONAL_STRING_FULL_NAME);
            auto postScriptName = FontInformationalName(
                face3.get(), DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME);

            // libass distinguishes full-name matching from PostScript-name
            // matching according to the font's outline type. Keep the UI name
            // human-readable, but send the identifier that uniquely selects
            // the physical face whenever DirectWrite exposes one.
            auto actualFaceType = face->GetType();
            bool const postScriptOutline =
                actualFaceType == DWRITE_FONT_FACE_TYPE_CFF ||
                actualFaceType == DWRITE_FONT_FACE_TYPE_RAW_CFF ||
                actualFaceType == DWRITE_FONT_FACE_TYPE_TYPE1 ||
                fileType == DWRITE_FONT_FILE_TYPE_CFF ||
                fileType == DWRITE_FONT_FILE_TYPE_TYPE1_PFM ||
                fileType == DWRITE_FONT_FILE_TYPE_TYPE1_PFB;

            std::wstring mpvName;
            if (postScriptOutline && !postScriptName.empty())
            {
                mpvName = postScriptName;
            }
            else if (!fullName.empty())
            {
                mpvName = fullName;
            }
            else if (!familyName.empty())
            {
                mpvName = familyName;
            }
            else if (!postScriptName.empty())
            {
                mpvName = postScriptName;
            }

            std::wstring displayName = fullName;
            if (displayName.empty())
            {
                displayName = familyName;
                if (!displayName.empty() && !faceName.empty() &&
                    faceName != L"Regular")
                {
                    displayName += L" — ";
                    displayName += faceName;
                }
            }
            if (displayName.empty()) displayName = postScriptName;

            if (mpvName.empty()) mpvName = displayName;
            if (displayName.empty()) displayName = mpvName;
            if (mpvName.empty() || displayName.empty()) continue;

            if (technicalNames.insert(mpvName).second)
            {
                choices.push_back({
                    std::move(displayName),
                    std::move(mpvName),
                    std::move(familyName) });
            }
        }

        return choices;
    }

    std::vector<ImportedFontChoice> ImportedFontChoices(
        std::filesystem::path const& path)
    {
        auto choices = FontChoicesFromFile(path);
        if (choices.empty())
        {
            // Preserve the old filename behavior only as a last-resort
            // compatibility fallback for malformed or unsupported font files.
            auto fallback = path.stem().wstring();
            if (!fallback.empty())
            {
                choices.push_back({ fallback, fallback, fallback });
            }
        }
        return choices;
    }

    int CALLBACK CollectFontFamily(
        LOGFONTW const* font, TEXTMETRICW const*, DWORD, LPARAM parameter)
    {
        auto& names = *reinterpret_cast<std::set<std::wstring>*>(parameter);
        std::wstring name = font->lfFaceName;
        if (!name.empty() && name.front() != L'@') names.insert(std::move(name));
        return 1;
    }

    std::vector<std::wstring> InstalledFontFamilies()
    {
        std::set<std::wstring> names;
        HDC dc = GetDC(nullptr);
        if (dc)
        {
            LOGFONTW query{};
            query.lfCharSet = DEFAULT_CHARSET;
            EnumFontFamiliesExW(dc, &query,
                reinterpret_cast<FONTENUMPROCW>(CollectFontFamily),
                reinterpret_cast<LPARAM>(&names), 0);
            ReleaseDC(nullptr, dc);
        }
        return { names.begin(), names.end() };
    }

    bool BrowserExecutableRegistered(wchar_t const* executable)
    {
        std::wstring subkey =
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";
        subkey += executable;
        wchar_t path[32768]{};
        DWORD bytes = sizeof(path);
        for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
        {
            bytes = sizeof(path);
            if (RegGetValueW(root, subkey.c_str(), nullptr, RRF_RT_REG_SZ,
                nullptr, path, &bytes) == ERROR_SUCCESS)
            {
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error)) return true;
            }
        }
        std::vector<wchar_t> resolved(32768);
        DWORD length = SearchPathW(nullptr, executable, nullptr,
            static_cast<DWORD>(resolved.size()), resolved.data(), nullptr);
        return length > 0 && length < resolved.size();
    }

    std::vector<std::wstring> DetectedCookieBrowsers()
    {
        struct Browser { wchar_t const* value; wchar_t const* executable; };
        static const Browser candidates[] = {
            // Firefox first: yt-dlp's own FAQ reports the browser-cookie flow
            // tends to work best with Firefox, especially on Windows.
            { L"firefox", L"firefox.exe" },
            { L"edge", L"msedge.exe" },
            { L"chrome", L"chrome.exe" },
            { L"brave", L"brave.exe" },
            { L"vivaldi", L"vivaldi.exe" },
            { L"opera", L"opera.exe" },
            { L"chromium", L"chromium.exe" }
        };
        std::vector<std::wstring> choices{ L"no" };
        for (auto const& browser : candidates)
        {
            if (BrowserExecutableRegistered(browser.executable))
                choices.emplace_back(browser.value);
        }
        // Portable browsers may not register App Paths. Keep the complete
        // supported set available after the automatically detected entries.
        for (auto const& browser : candidates)
        {
            if (std::find(choices.begin(), choices.end(), browser.value) == choices.end())
                choices.emplace_back(browser.value);
        }
        return choices;
    }


    std::wstring T(std::wstring_view fallback)
    {
        return hc::localization::GetStringForFallback(L"SettingsDyn", fallback);
    }


    std::wstring SettingsResource(
        std::wstring_view id,
        std::wstring_view fallback)
    {
        return hc::localization::GetString(id, fallback);
    }


    std::wstring LocalizeSettingsMessage(std::wstring const& message)
    {
        if (message.empty()) return {};

        auto startsWith = [&message](std::wstring_view prefix)
        {
            return message.size() >= prefix.size() &&
                message.compare(0, prefix.size(), prefix) == 0;
        };

        for (auto prefix : {
            std::wstring_view{ L"O arquivo do shader não foi encontrado: " },
            std::wstring_view{ L"O mecanismo de reprodução rejeitou o shader: " },
            std::wstring_view{ L"O mecanismo de reprodução rejeitou " },
            std::wstring_view{ L"O yt-dlp atual não reconhece a opção: " } })
        {
            if (startsWith(prefix))
                return T(prefix) + message.substr(prefix.size());
        }

        constexpr std::wstring_view missingPrefix =
            L"Este modo não pode ser ativado porque faltam ";
        constexpr std::wstring_view missingMiddle =
            L" shader(s). Primeiro arquivo ausente: ";
        if (startsWith(missingPrefix))
        {
            auto middle = message.find(missingMiddle, missingPrefix.size());
            if (middle != std::wstring::npos)
            {
                return T(missingPrefix) +
                    message.substr(missingPrefix.size(), middle - missingPrefix.size()) +
                    T(missingMiddle) +
                    message.substr(middle + missingMiddle.size());
            }
        }

        constexpr std::wstring_view cleanupPrefix =
            L"A lista e o pipeline foram limpos, mas ";
        constexpr std::wstring_view cleanupSuffix =
            L" arquivo(s) privado(s) não puderam ser apagados do disco.";
        if (startsWith(cleanupPrefix) && message.ends_with(cleanupSuffix))
        {
            auto countLength = message.size() - cleanupPrefix.size() - cleanupSuffix.size();
            return T(cleanupPrefix) +
                message.substr(cleanupPrefix.size(), countLength) +
                T(cleanupSuffix);
        }

        for (auto suffix : {
            std::wstring_view{ L". Aplicada preservando a reprodução atual." },
            std::wstring_view{ L". Salva; será aplicada na próxima inicialização do motor." },
            std::wstring_view{ L". Será aplicada ao iniciar o player." } })
        {
            if (message.ends_with(suffix))
            {
                return LocalizeSettingsMessage(
                    message.substr(0, message.size() - suffix.size())) +
                    T(suffix);
            }
        }

        constexpr std::wstring_view incorporated =
            L" opções incorporadas ao painel; ";
        constexpr std::wstring_view additional = L" opções adicionais";
        constexpr std::wstring_view ignoredPrefix = L"; ";
        constexpr std::wstring_view ignoredSuffix = L" ignoradas";
        auto incorporatedAt = message.find(incorporated);
        if (incorporatedAt != std::wstring::npos)
        {
            auto additionalAt = message.find(
                additional, incorporatedAt + incorporated.size());
            if (additionalAt != std::wstring::npos)
            {
                std::wstring localized =
                    message.substr(0, incorporatedAt) + T(incorporated);
                auto additionalCountStart = incorporatedAt + incorporated.size();
                localized += message.substr(
                    additionalCountStart, additionalAt - additionalCountStart);
                localized += T(additional);

                auto tailStart = additionalAt + additional.size();
                if (tailStart < message.size() &&
                    message.compare(tailStart, ignoredPrefix.size(), ignoredPrefix) == 0 &&
                    message.ends_with(ignoredSuffix))
                {
                    auto countStart = tailStart + ignoredPrefix.size();
                    auto countLength = message.size() - countStart - ignoredSuffix.size();
                    localized += ignoredPrefix;
                    localized += message.substr(countStart, countLength);
                    localized += T(ignoredSuffix);
                }
                return localized;
            }
        }

        return T(message);
    }


    bool IsWindows11CardStyle(std::wstring const& value)
    {
        return _wcsicmp(value.c_str(), L"windows11") == 0;
    }

    std::wstring SelectedCardStyle(
        winrt::HCPlayer::implementation::SettingsPage& page)
    {
        auto combo = page.CardStyleCombo();
        if (auto item = combo.SelectedItem().try_as<
            winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem>())
        {
            auto value = winrt::unbox_value_or<winrt::hstring>(
                item.Tag(), L"hcplayer");
            if (!value.empty())
            {
                return value.c_str();
            }
        }

        std::wstring saved = L"hcplayer";
        PlayerTryGetSavedMpvOption(L"ui-card-style", saved);
        return saved;
    }

    void ApplySettingsCardStyle(
        winrt::HCPlayer::implementation::SettingsPage& page,
        std::wstring const& value)
    {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        using namespace winrt::Microsoft::UI::Xaml::Media;

        bool const windows11 = IsWindows11CardStyle(value);

        auto theme = page.Resources().ThemeDictionaries().Lookup(
            winrt::box_value(PlayerIsLightTheme() ? L"Light" : L"Dark"))
            .as<ResourceDictionary>();

        auto surface = theme.Lookup(winrt::box_value(
            windows11
                ? L"SettingsSurfaceWindows11"
                : L"SettingsSurface"))
            .as<Brush>();

        page.SettingsSurfaceHost().Background(surface);
        page.SettingsRoot().Background(surface);

        auto hcStyle = page.Resources().Lookup(
            winrt::box_value(L"SettingsCard")).as<Style>();
        auto windowsStyle = page.Resources().Lookup(
            winrt::box_value(L"SettingsCardWindows11")).as<Style>();
        auto targetStyle = windows11 ? windowsStyle : hcStyle;

        std::function<void(DependencyObject const&)> apply;
        apply = [&](DependencyObject const& root)
        {
            if (auto border = root.try_as<Border>())
            {
                auto current = border.Style();
                if (current == hcStyle || current == windowsStyle)
                {
                    border.Style(targetStyle);
                }
            }

            int const count = VisualTreeHelper::GetChildrenCount(root);
            for (int index = 0; index < count; ++index)
            {
                apply(VisualTreeHelper::GetChild(root, index));
            }
        };

        apply(page.SettingsRoot());
    }


    std::wstring AboutArchitecture()
    {
#if defined(_M_X64)
        return L"x64";
#elif defined(_M_ARM64)
        return L"ARM64";
#elif defined(_M_IX86)
        return L"x86";
#else
        return T(L"Desconhecida");
#endif
    }

    std::wstring AboutBuildType()
    {
#ifdef _DEBUG
        return L"Debug";
#else
        return SettingsResource(
            L"SettingsAboutStableBuild",
            L"Estável • Agosto de 2026");
#endif
    }

    std::wstring AboutVersionValue(std::wstring value, std::wstring const& prefix)
    {
        if (value.empty()) return T(L"Indisponível");
        if (value.starts_with(prefix))
        {
            value.erase(0, prefix.size());
        }
        return value;
    }

    std::vector<std::wstring> ChoicesForOption(std::wstring const& name)
    {
        if (name == L"tone-mapping")
            return { L"auto", L"clip", L"mobius", L"reinhard", L"hable", L"gamma", L"linear", L"bt.2390", L"bt.2446a", L"spline", L"st2094-10", L"st2094-40" };
        if (name == L"gamut-mapping-mode")
            return { L"auto", L"clip", L"perceptual", L"relative", L"saturation", L"absolute", L"desaturate", L"darken", L"warn", L"linear" };
        if (name == L"target-trc")
            return { L"auto", L"bt.1886", L"srgb", L"linear", L"gamma1.8", L"gamma2.0", L"gamma2.2", L"gamma2.4", L"pq", L"hlg" };
        if (name == L"target-prim")
            return { L"auto", L"bt.709", L"bt.2020", L"display-p3", L"dci-p3", L"adobe" };
        if (name == L"hwdec")
            return { L"no", L"auto", L"auto-safe", L"d3d11va", L"d3d11va-copy", L"dxva2-copy", L"nvdec", L"nvdec-copy" };
        if (name == L"gpu-api")
            return { L"auto", L"d3d11", L"vulkan", L"opengl" };
        if (name == L"gpu-context")
            return { L"auto", L"d3d11", L"winvk", L"win", L"angle" };
        if (name == L"scale" || name == L"dscale" || name == L"cscale")
            return { L"bilinear", L"bicubic_fast", L"spline16", L"spline36", L"spline64", L"lanczos", L"ewa_lanczos", L"hermite", L"mitchell", L"catmull_rom" };
        if (name == L"dither")
            return { L"no", L"ordered", L"fruit", L"error-diffusion" };
        if (name == L"video-sync")
            return { L"audio", L"display-resample", L"display-resample-vdrop", L"display-resample-desync", L"display-tempo", L"display-vdrop", L"display-adrop", L"display-desync", L"desync" };
        if (name == L"tscale")
            return { L"oversample", L"linear", L"box", L"triangle", L"sphinx", L"mitchell", L"catmull_rom" };
        if (name == L"tscale-window")
            return { L"box", L"triangle", L"hann", L"hamming", L"quadric", L"sphinx", L"kaiser", L"blackman" };
        if (name == L"screenshot-format")
            return { L"png", L"jpg", L"jpeg", L"webp", L"jxl", L"avif" };
        if (name == L"sub-auto" || name == L"audio-file-auto")
            return { L"no", L"exact", L"fuzzy", L"all" };
        if (name == L"profile-restore")
            return { L"default", L"copy", L"copy-equal" };
        return {};
    }
}

namespace winrt::HCPlayer::implementation
{
    SettingsPage::SettingsPage()
    {
        RequestedTheme(PlayerIsLightTheme()
            ? Microsoft::UI::Xaml::ElementTheme::Light
            : Microsoft::UI::Xaml::ElementTheme::Dark);
    }

    void SettingsPage::UpdateThemeButton()
    {
        bool light = PlayerIsLightTheme();
        ThemeIcon().Glyph(light ? L"\uE708" : L"\uE706");
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            ThemeButton(), winrt::box_value(
                light ? T(L"Usar modo escuro") : T(L"Usar modo claro")));
    }

    void SettingsPage::ThemeClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        PlayerSetLightTheme(!PlayerIsLightTheme());
        UpdateThemeButton();
        ApplySettingsCardStyle(*this, SelectedCardStyle(*this));
    }

    void SettingsPage::SettingsTabClicked(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto button = sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
        if (!button || !button.Tag()) return;

        auto tag = winrt::unbox_value_or<winrt::hstring>(
            button.Tag(), winrt::hstring{});
        if (tag.empty()) return;

        SelectSettingsTab(tag.c_str());
    }

    void SettingsPage::SelectSettingsTab(std::wstring const& tab)
    {
        using Microsoft::UI::Xaml::Visibility;

        bool const general = tab == L"general";
        bool const playback = tab == L"playback";
        bool const media = tab == L"media";
        bool const interfacePage = tab == L"interface";
        bool const advanced = tab == L"advanced";
        bool const about = tab == L"about";

        // Imported profiles can contain dozens of options. Building one XAML
        // editor for every option is intentionally deferred until the Advanced
        // tab is actually requested, and is performed only once per imported
        // configuration. Doing this while opening the Settings overlay stalls
        // video presentation, especially with a busy 4K HDR renderer.
        if (advanced)
            RefreshImportedConfig();

        GeneralSettingsPanel().Visibility(general ? Visibility::Visible : Visibility::Collapsed);
        PlaybackSettingsPanel().Visibility(playback ? Visibility::Visible : Visibility::Collapsed);
        MediaSettingsPanel().Visibility(media ? Visibility::Visible : Visibility::Collapsed);
        InterfaceSettingsPanel().Visibility(interfacePage ? Visibility::Visible : Visibility::Collapsed);
        AdvancedSettingsPanel().Visibility(advanced ? Visibility::Visible : Visibility::Collapsed);
        AboutSettingsPanel().Visibility(about ? Visibility::Visible : Visibility::Collapsed);

        auto setTabVisual = [](Microsoft::UI::Xaml::Controls::TextBlock const& text,
                               Microsoft::UI::Xaml::Controls::Border const& indicator,
                               bool selected)
        {
            text.Opacity(selected ? 1.0 : 0.68);
            indicator.Visibility(selected ? Visibility::Visible : Visibility::Collapsed);
        };

        setTabVisual(GeneralTabText(), GeneralTabIndicator(), general);
        setTabVisual(PlaybackTabText(), PlaybackTabIndicator(), playback);
        setTabVisual(MediaTabText(), MediaTabIndicator(), media);
        setTabVisual(InterfaceTabText(), InterfaceTabIndicator(), interfacePage);
        setTabVisual(AdvancedTabText(), AdvancedTabIndicator(), advanced);
        setTabVisual(AboutTabText(), AboutTabIndicator(), about);

        SettingsScrollViewer().ChangeView(nullptr, 0.0, nullptr, false);
    }

    void SettingsPage::ScrollBy(int wheelDelta)
    {
        double target = SettingsScrollViewer().VerticalOffset() - wheelDelta;
        SettingsScrollViewer().ChangeView(nullptr, target, nullptr, false);
    }

    void SettingsPage::ImportPath(std::wstring const& path)
    {
        auto config = PlayerImportMpvConfig(path);
        if (config.success)
        {
            // Importing promotes matching mpv options into the existing
            // controls. Refresh them without staging artificial UI changes.
            bool wasReady = m_ready;
            m_ready = false;
            RestoreSavedControls(SettingsRoot());
            UpdateTemporalScalerAvailability();

            std::wstring audioDevice;
            if (PlayerTryGetSavedMpvOption(L"audio-device", audioDevice))
            {
                auto found = std::find(
                    m_audioDeviceNames.begin(), m_audioDeviceNames.end(), audioDevice);
                if (found != m_audioDeviceNames.end())
                {
                    AudioDeviceCombo().SelectedIndex(
                        static_cast<int32_t>(found - m_audioDeviceNames.begin()));
                }
            }
            std::wstring screenshotDirectory;
            if (PlayerTryGetSavedMpvOption(
                L"screenshot-directory", screenshotDirectory))
            {
                ScreenshotDirectoryText().Text(screenshotDirectory);
            }
            m_ready = wasReady;
            m_pendingOptions.clear();
            SaveButton().IsEnabled(false);
        }
        RenderImportedConfig(config);
    }

    void SettingsPage::RefreshImportedConfig()
    {
        if (m_importedConfigRendered) return;

        auto config = PlayerGetImportedConfig();
        if (!config.success)
        {
            m_importedConfigRendered = true;
            return;
        }

        RenderImportedConfig(config);
    }

    void SettingsPage::PrepareForOpen()
    {
        m_closing = false;
        UpdateYtdlpStatus();
        RefreshAboutInfo();
        UpdateTemporalScalerAvailability();
        SyncLiveRuntimeStates();

        // Always-on-top is a live window state shared with Ctrl+T and the
        // context menu. Re-sync the Toggle every time Settings opens so this
        // persistent page can never display a stale value.
        std::wstring alwaysOnTopValue;
        bool const alwaysOnTopEnabled =
            PlayerTryGetSavedMpvOption(L"ui-ontop", alwaysOnTopValue) &&
            alwaysOnTopValue == L"yes";
        SyncAlwaysOnTopState(alwaysOnTopEnabled);

        SettingsRoot().IsHitTestVisible(true);
        SettingsTranslate().X(0.0);
        SettingsRoot().Opacity(1.0);

        // Keep the panel surface fully opaque. Only its compositor visuals
        // move, so the theme-colored host remains visible underneath instead
        // of exposing a black frame.
        auto visual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(SettingsRoot());
        visual.StopAnimation(L"Offset");
        visual.StopAnimation(L"Opacity");
        visual.Offset({ 112.0f, 0.0f, 0.0f });
        visual.Opacity(1.0f);

        auto headerVisual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(SettingsHeader());
        headerVisual.StopAnimation(L"Opacity");
        headerVisual.Opacity(0.76f);

        auto tabsVisual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(SettingsTabs());
        tabsVisual.StopAnimation(L"Opacity");
        tabsVisual.Opacity(0.84f);

        auto contentVisual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(SettingsScrollViewer());
        contentVisual.StopAnimation(L"Opacity");
        contentVisual.Opacity(0.88f);
    }

    void SettingsPage::SyncAlwaysOnTopState(bool enabled)
    {
        // Updating IsOn programmatically fires Toggled. Temporarily suppress
        // event staging so a runtime Ctrl+T refresh cannot feed back into the
        // setting handler or spuriously enable the Save button.
        bool const wasReady = m_ready;
        m_ready = false;
        AlwaysOnTopToggle().IsOn(enabled);
        m_ready = wasReady;
    }

    void SettingsPage::SyncLiveRuntimeStates()
    {
        // These properties can be changed directly by mpv shortcuts while the
        // Settings page remains alive. Read them back from the running engine
        // so the UI reflects reality without staging or saving any change.
        // Keep direct references to the three editors: walking the full visual
        // tree here made every panel open pay for all six Settings tabs.
        std::wstring deinterlace;
        std::wstring deband;
        std::wstring hwdec;
        bool const hasDeinterlace =
            PlayerTryGetMpvRuntimeOption(L"deinterlace", deinterlace);
        bool const hasDeband =
            PlayerTryGetMpvRuntimeOption(L"deband", deband);
        bool const hasHwdec =
            PlayerTryGetMpvRuntimeOption(L"hwdec", hwdec);
        if (!hasDeinterlace && !hasDeband && !hasHwdec) return;

        using namespace Microsoft::UI::Xaml::Controls;

        auto selectValue = [](ComboBox const& combo, std::wstring const& value)
        {
            if (!combo) return;
            for (uint32_t index = 0; index < combo.Items().Size(); ++index)
            {
                auto candidate = combo.Items().GetAt(index);
                winrt::hstring candidateValue;
                if (auto item = candidate.try_as<ComboBoxItem>())
                {
                    auto stored = item.Tag() ? item.Tag() : item.Content();
                    candidateValue = winrt::unbox_value_or<winrt::hstring>(
                        stored, winrt::hstring{});
                }
                else
                {
                    candidateValue = winrt::unbox_value_or<winrt::hstring>(
                        candidate, winrt::hstring{});
                }

                if (candidateValue == value)
                {
                    combo.SelectedIndex(static_cast<int32_t>(index));
                    return;
                }
            }
        };

        auto enabled = [](std::wstring const& value)
        {
            return value != L"no" && value != L"0" && value != L"false";
        };

        bool const wasReady = m_ready;
        m_ready = false;
        try
        {
            if (hasDeinterlace)
            {
                selectValue(DeinterlaceCombo(), deinterlace);
            }
            if (hasDeband)
            {
                DebandingToggle().IsOn(enabled(deband));
            }
            if (hasHwdec)
            {
                // Keep the legacy hidden toggle coherent as well as the
                // first-class advanced choice currently presented to users.
                HardwareDecodingToggle().IsOn(enabled(hwdec));
                selectValue(m_hardwareDecodingChoice, hwdec);
            }
        }
        catch (...)
        {
            // Runtime reflection is best-effort. A missing/unloaded editor must
            // never prevent Settings from opening or leave handlers disabled.
        }
        m_ready = wasReady;
    }

    void SettingsPage::BeginOpenAnimation()
    {
        auto root = SettingsRoot();
        auto header = SettingsHeader();
        auto tabs = SettingsTabs();
        auto content = SettingsScrollViewer();
        root.DispatcherQueue().TryEnqueue([root, header, tabs, content]()
            {
                auto visual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                    GetElementVisual(root);
                auto compositor = visual.Compositor();
                auto arrival = compositor.CreateCubicBezierEasingFunction(
                    { 0.16f, 1.0f }, { 0.30f, 1.0f });
                auto settle = compositor.CreateCubicBezierEasingFunction(
                    { 0.20f, 0.0f }, { 0.20f, 1.0f });

                // The shell arrives quickly, crosses its resting point by only
                // three pixels, then settles. This reads as physical movement
                // without the rubber-band feel of a large spring.
                auto slide = compositor.CreateVector3KeyFrameAnimation();
                slide.InsertKeyFrame(0.82f, { -3.0f, 0.0f, 0.0f }, arrival);
                slide.InsertKeyFrame(1.0f, { 0.0f, 0.0f, 0.0f }, settle);
                slide.Duration(std::chrono::milliseconds(360));
                slide.StopBehavior(
                    Microsoft::UI::Composition::AnimationStopBehavior::SetToFinalValue);
                visual.StartAnimation(L"Offset", slide);

                auto animateContent = [compositor, arrival](auto const& target,
                    std::chrono::milliseconds delay,
                    std::chrono::milliseconds duration)
                    {
                        auto targetVisual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                            GetElementVisual(target);
                        auto opacity = compositor.CreateScalarKeyFrameAnimation();
                        opacity.InsertKeyFrame(1.0f, 1.0f, arrival);
                        opacity.DelayTime(delay);
                        opacity.Duration(duration);
                        opacity.StopBehavior(
                            Microsoft::UI::Composition::AnimationStopBehavior::SetToFinalValue);
                        targetVisual.StartAnimation(L"Opacity", opacity);
                    };
                animateContent(header, std::chrono::milliseconds(45),
                    std::chrono::milliseconds(235));
                animateContent(tabs, std::chrono::milliseconds(62),
                    std::chrono::milliseconds(255));
                animateContent(content, std::chrono::milliseconds(75),
                    std::chrono::milliseconds(285));
            });
    }

    void SettingsPage::SettingsLoaded(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        UpdateThemeButton();
        m_systemFontNames = InstalledFontFamilies();
        PopulateScalerChoices();
        PopulateNativeAdvancedOptions();
        UpdateYtdlpStatus();
        RefreshShaderList();
        RefreshAboutInfo();

        auto audioDevices = PlayerGetAudioDevices();
        int32_t selectedAudioDevice = 0;
        std::wstring savedAudioDevice;
        PlayerTryGetSavedMpvOption(L"audio-device", savedAudioDevice);
        for (auto const& device : audioDevices)
        {
            m_audioDeviceNames.push_back(device.name);
            AudioDeviceCombo().Items().Append(winrt::box_value(
                device.name == L"auto"
                    ? T(L"Padrão do sistema")
                    : device.description));
            if (device.name == savedAudioDevice)
            {
                selectedAudioDevice = static_cast<int32_t>(AudioDeviceCombo().Items().Size()) - 1;
            }
        }
        AudioDeviceCombo().SelectedIndex(selectedAudioDevice);

        auto fontsDirectory = ImportedFontsDirectory();
        std::error_code error;
        std::filesystem::create_directories(fontsDirectory, error);
        m_fontNames = m_systemFontNames;
        m_fontDisplayNames = m_systemFontNames;
        m_fontFamilyAliases = m_systemFontNames;
        SubtitleFontCombo().Items().Clear();
        for (auto const& name : m_fontDisplayNames)
            SubtitleFontCombo().Items().Append(winrt::box_value(name));
        for (auto const& entry : std::filesystem::directory_iterator(
            fontsDirectory, std::filesystem::directory_options::skip_permission_denied, error))
        {
            if (entry.is_regular_file() && IsFontFile(entry.path()))
            {
                for (auto const& choice : ImportedFontChoices(entry.path()))
                {
                    if (std::find(m_fontNames.begin(), m_fontNames.end(), choice.mpvName) == m_fontNames.end())
                    {
                        m_fontNames.push_back(choice.mpvName);
                        m_fontDisplayNames.push_back(choice.displayName);
                        m_fontFamilyAliases.push_back(
                            choice.familyName.empty() ? choice.mpvName : choice.familyName);
                        SubtitleFontCombo().Items().Append(
                            winrt::box_value(choice.displayName));
                    }
                }
            }
        }
        std::wstring selectedSubtitleFont = L"Segoe UI";
        PlayerTryGetSavedMpvOption(L"sub-font", selectedSubtitleFont);
        int32_t selectedFontIndex = -1;
        if (auto selected = std::find(
            m_fontNames.begin(), m_fontNames.end(), selectedSubtitleFont);
            selected != m_fontNames.end())
        {
            selectedFontIndex = static_cast<int32_t>(selected - m_fontNames.begin());
        }
        else
        {
            // 34.7 stored only the imported family name. Preserve that exact
            // legacy value instead of pretending that one particular face was
            // selected. As soon as the user picks a face, its unique technical
            // name is saved and this compatibility entry is no longer needed.
            auto legacy = std::find(
                m_fontFamilyAliases.begin(), m_fontFamilyAliases.end(), selectedSubtitleFont);
            if (legacy != m_fontFamilyAliases.end())
            {
                m_fontNames.push_back(selectedSubtitleFont);
                m_fontDisplayNames.push_back(selectedSubtitleFont);
                m_fontFamilyAliases.push_back(selectedSubtitleFont);
                SubtitleFontCombo().Items().Append(
                    winrt::box_value(selectedSubtitleFont));
                selectedFontIndex = static_cast<int32_t>(m_fontNames.size()) - 1;
            }
        }
        if (selectedFontIndex >= 0)
        {
            SubtitleFontCombo().SelectedIndex(selectedFontIndex);
        }
        SubtitleFontStatus().Text(std::to_wstring(m_systemFontNames.size()) +
            T(L" fontes instaladas no Windows; também é possível importar uma pasta"));
        RestoreSavedControls(SettingsRoot());
        SyncLiveRuntimeStates();
        UpdateTemporalScalerAvailability();

        std::wstring savedCardStyle = L"hcplayer";
        PlayerTryGetSavedMpvOption(L"ui-card-style", savedCardStyle);
        ApplySettingsCardStyle(*this, savedCardStyle);

        std::wstring screenshotDirectory;
        if (PlayerTryGetSavedMpvOption(L"screenshot-directory", screenshotDirectory))
        {
            ScreenshotDirectoryText().Text(screenshotDirectory);
        }

        // The Settings island is created while still hidden during startup.
        // Prebuild imported editors here, before libmpv begins presenting, so
        // the first Advanced-tab visit is also free of playback-time XAML work.
        RefreshImportedConfig();

        m_ready = true;
        RefreshMediaBadgeSetStatus();
    }

    void SettingsPage::RefreshAboutInfo()
    {
        constexpr wchar_t AppVersion[] = L"1.0";

        PlayerEngineVersionInfo const engine = PlayerGetEngineVersionInfo();
        std::wstring const architecture = AboutArchitecture();
        std::wstring const buildType = AboutBuildType();
        std::wstring const mpvVersion = AboutVersionValue(engine.mpv, L"mpv ");
        std::wstring const ffmpegVersion = AboutVersionValue(engine.ffmpeg, L"FFmpeg ");
        std::wstring const libplaceboVersion = AboutVersionValue(engine.libplacebo, L"libplacebo ");

        AboutVersionBadgeText().Text(T(L"Versão ") + AppVersion);
        AboutArchitectureBadgeText().Text(architecture);
        AboutMpvVersionText().Text(mpvVersion);
        AboutFfmpegVersionText().Text(ffmpegVersion);
        AboutLibplaceboVersionText().Text(libplaceboVersion);
        AboutArchitectureText().Text(architecture);
        AboutBuildText().Text(buildType);

        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            AboutMpvVersionText(), winrt::box_value(mpvVersion));
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            AboutFfmpegVersionText(), winrt::box_value(ffmpegVersion));
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            AboutLibplaceboVersionText(), winrt::box_value(libplaceboVersion));
    }

    void SettingsPage::OpenDefaultAppsClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        OpenHCPlayerDefaultAppsSettingsAsync();
    }

    void SettingsPage::OpenGitHubClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        OpenHCPlayerRepositoryAsync();
    }

    void SettingsPage::CopyAboutInfoClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        RefreshAboutInfo();

        std::wostringstream text;
        text << L"HC Player 1.0\r\n"
            << T(L"Arquitetura: ") << AboutArchitectureText().Text().c_str() << L"\r\n"
            << T(L"Compilação: ") << AboutBuildText().Text().c_str() << L"\r\n"
            << T(L"Interface: WinUI 3 / Windows App SDK 2.4.0") << L"\r\n"
            << T(L"Renderização: gpu-next / D3D11") << L"\r\n"
            << T(L"Saída de áudio: WASAPI") << L"\r\n"

            << L"mpv: " << AboutMpvVersionText().Text().c_str() << L"\r\n"
            << L"FFmpeg: " << AboutFfmpegVersionText().Text().c_str() << L"\r\n"
            << L"libplacebo: " << AboutLibplaceboVersionText().Text().c_str();

        Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(winrt::hstring{ text.str() });
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
        Windows::ApplicationModel::DataTransfer::Clipboard::Flush();

        AboutCopyStatus().Title(T(L"Informações copiadas"));
        AboutCopyStatus().Message(
            T(L"Os dados de versão e do motor foram copiados para a área de transferência."));
        AboutCopyStatus().Severity(
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success);
        AboutCopyStatus().IsOpen(true);
    }

    void SettingsPage::StageOption(
        std::wstring const& name, std::wstring const& value)
    {
        if (!m_ready) return;
        m_pendingOptions[name] = value;
        SaveButton().IsEnabled(true);
        SaveStatus().IsOpen(false);
    }

    bool SettingsPage::PlayerSetMpvOption(
        std::wstring const& name, std::wstring const& value)
    {
        StageOption(name, value);
        return true;
    }

    void SettingsPage::SaveClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        std::vector<std::pair<std::wstring, std::wstring>> options{
            m_pendingOptions.begin(), m_pendingOptions.end() };
        std::wstring error;
        bool saved = PlayerApplyMpvOptions(options, error);
        SaveStatus().IsOpen(true);
        SaveStatus().Title(saved ? T(L"Configurações salvas") : T(L"Não foi possível salvar"));
        SaveStatus().Message(saved
            ? T(L"As opções foram validadas e aplicadas ao player.")
            : LocalizeSettingsMessage(error));
        SaveStatus().Severity(saved
            ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success
            : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        if (saved)
        {
            m_pendingOptions.clear();
            SaveButton().IsEnabled(false);
        }
    }

    void SettingsPage::RestoreSavedControls(
        Microsoft::UI::Xaml::DependencyObject const& root)
    {
        using namespace Microsoft::UI::Xaml::Controls;
        using Microsoft::UI::Xaml::Media::VisualTreeHelper;

        if (auto element = root.try_as<Microsoft::UI::Xaml::FrameworkElement>())
        {
            if (auto boxedTag = element.Tag())
            {
                auto tag = winrt::unbox_value_or<winrt::hstring>(boxedTag, winrt::hstring{});
                std::wstring value;
                if (!tag.empty() && PlayerTryGetSavedMpvOption(tag.c_str(), value))
                {
                    if (auto toggle = element.try_as<ToggleSwitch>())
                    {
                        bool enabled = value != L"no" && value != L"0" && value != L"false";
                        if (tag == L"video-sync") enabled = value.starts_with(L"display");
                        toggle.IsOn(enabled);
                    }
                    else if (auto text = element.try_as<TextBox>())
                    {
                        text.Text(value);
                    }
                    else if (auto slider = element.try_as<Slider>())
                    {
                        try { slider.Value(std::stod(value)); }
                        catch (...) {}
                    }
                    else if (auto combo = element.try_as<ComboBox>())
                    {
                        for (uint32_t index = 0; index < combo.Items().Size(); ++index)
                        {
                            auto candidate = combo.Items().GetAt(index);
                            winrt::hstring content;
                            if (auto item = candidate.try_as<ComboBoxItem>())
                            {
                                content = item.Tag()
                                    ? winrt::unbox_value_or<winrt::hstring>(
                                        item.Tag(), winrt::hstring{})
                                    : winrt::unbox_value_or<winrt::hstring>(
                                        item.Content(), winrt::hstring{});
                            }
                            else
                                content = winrt::unbox_value_or<winrt::hstring>(candidate, winrt::hstring{});
                            if (content == value)
                            {
                                combo.SelectedIndex(static_cast<int32_t>(index));
                                break;
                            }
                        }
                    }
                }
            }
        }

        int count = VisualTreeHelper::GetChildrenCount(root);
        for (int index = 0; index < count; ++index)
        {
            RestoreSavedControls(VisualTreeHelper::GetChild(root, index));
        }
    }

    void SettingsPage::PopulateScalerChoices()
    {
        auto choices = PlayerGetMpvOptionChoices(L"dscale");
        if (choices.empty())
        {
            // Used only with older libmpv builds that do not expose
            // option-info choices. Current builds provide the exact list.
            choices = {
                L"bilinear", L"bicubic_fast", L"oversample", L"linear",
                L"spline16", L"spline36", L"spline64", L"sinc", L"lanczos",
                L"ginseng", L"jinc", L"ewa_lanczos", L"ewa_hanning",
                L"ewa_ginseng", L"ewa_lanczossharp", L"ewa_lanczos4sharpest",
                L"ewa_lanczossoft", L"haasnsoft", L"bicubic", L"bcspline",
                L"catmull_rom", L"mitchell", L"robidoux", L"robidouxsharp",
                L"ewa_robidoux", L"ewa_robidouxsharp", L"box", L"nearest",
                L"triangle", L"gaussian", L"bartlett", L"cosine", L"hanning",
                L"hamming", L"quadric", L"welch", L"kaiser", L"blackman",
                L"sphinx", L"hermite"
            };
        }

        auto fill = [&choices](Microsoft::UI::Xaml::Controls::ComboBox const& combo,
            std::wstring const& selectedValue)
            {
                combo.Items().Clear();
                int32_t selectedIndex = -1;
                for (auto const& choice : choices)
                {
                    combo.Items().Append(winrt::box_value(choice));
                    if (choice == selectedValue)
                    {
                        selectedIndex = static_cast<int32_t>(combo.Items().Size()) - 1;
                    }
                }
                combo.SelectedIndex(selectedIndex);
            };

        fill(DownscaleCombo(), L"hermite");
        fill(ChromaCombo(), L"bicubic_fast");
    }

    void SettingsPage::PopulateNativeAdvancedOptions()
    {
        if (m_nativeAdvancedPopulated) return;
        m_nativeAdvancedPopulated = true;

        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;

        enum class EditorKind { Toggle, Choice, Text, SystemFont };
        struct OptionSpec
        {
            wchar_t const* title;
            wchar_t const* name;
            wchar_t const* description;
            wchar_t const* defaultValue;
            EditorKind editor;
            std::vector<std::wstring> choices{};
        };

        // Options which were formerly displayed as raw imported rows. Values
        // are intentionally kept in mpv's original spelling inside the badge.
        const std::vector<OptionSpec> general = {
            { L"Permitir busca sempre", L"force-seekable",
              L"Tenta avançar e retroceder mesmo quando a mídia não anuncia suporte", L"yes", EditorKind::Toggle }
        };

        const std::vector<OptionSpec> window = {
            { L"Progresso na barra de tarefas", L"taskbar-progress",
              L"Mostra o andamento da mídia no ícone da barra de tarefas", L"yes", EditorKind::Toggle },
            { L"Abrir mídia em tela cheia", L"fullscreen",
              L"Entra automaticamente em tela cheia ao iniciar uma nova mídia", L"no", EditorKind::Toggle },
            { L"Tamanho inicial da janela", L"autofit",
              L"Ajusta a janela ao abrir uma mídia sem ultrapassar este tamanho", L"1216x714", EditorKind::Text },
            { L"Limite relativo da janela", L"autofit-larger",
              L"Limita o ajuste automático à área útil da tela", L"81%x81%", EditorKind::Text }
        };

        const std::vector<OptionSpec> video = {
            { L"Decodificação por hardware", L"hwdec",
              L"Método usado pela GPU para decodificar o vídeo", L"d3d11va", EditorKind::Choice,
              { L"no", L"auto", L"auto-safe",
                L"d3d11va", L"d3d11va-copy",
                L"dxva2-copy",
                L"nvdec", L"nvdec-copy" } },
            { L"Saída de vídeo", L"vo",
              L"Motor gráfico usado para apresentar os quadros", L"gpu-next", EditorKind::Choice,
              { L"gpu-next", L"gpu" } },
            { L"API gráfica", L"gpu-api",
              L"API gráfica usada no Windows", L"d3d11", EditorKind::Choice,
              { L"d3d11", L"vulkan", L"opengl" } },
            { L"Contexto gráfico", L"gpu-context",
              L"Contexto de apresentação usado pela API gráfica", L"d3d11", EditorKind::Choice,
              { L"d3d11", L"winvk", L"win", L"angle" } },
            { L"Perfil ICC do Windows", L"icc-profile-auto",
              L"Usa automaticamente o perfil de cores associado à tela para o gerenciamento de cor durante a reprodução", L"no", EditorKind::Toggle },
            { L"Profundidade do dithering", L"dither-depth",
              L"Profundidade alvo usada pelo processo de dithering", L"auto", EditorKind::Choice,
              { L"auto", L"8", L"10", L"12", L"no" } },
            { L"Upscaling em luz linear", L"linear-upscaling",
              L"Processa a ampliação em luz linear; opção avançada", L"no", EditorKind::Toggle },
            { L"Upscaling sigmoidal", L"sigmoid-upscaling",
              L"Reduz halos de filtros de ampliação; substitui o modo linear", L"yes", EditorKind::Toggle },
            { L"Correção ao reduzir", L"correct-downscaling",
              L"Ajusta filtros convolucionais para preservar detalhes na redução", L"yes", EditorKind::Toggle },
            { L"Níveis de saída do vídeo", L"video-output-levels",
              L"Seleciona automaticamente ou força faixa limitada/completa", L"auto", EditorKind::Choice,
              { L"auto", L"limited", L"full" } }
        };

        const std::vector<OptionSpec> subtitles = {
            { L"Escalar legenda com a janela", L"sub-scale-by-window",
              L"Acompanha o tamanho da janela ao dimensionar as legendas", L"yes", EditorKind::Toggle },
            { L"Usar margens para legendas", L"sub-use-margins",
              L"Permite posicionar legendas também nas margens do vídeo", L"yes", EditorKind::Toggle },
            { L"Escala adicional da legenda", L"sub-scale",
              L"Multiplicador aplicado ao tamanho final das legendas", L"1.0", EditorKind::Text }
        };

        const std::vector<OptionSpec> streaming = {
            { L"Formato de vídeos online", L"ytdl-format",
              L"Deixe vazio para seleção automática do yt-dlp; preencha apenas para usar um formato personalizado", L"", EditorKind::Text },
            { L"Opções do yt-dlp", L"ytdl-raw-options",
              L"Use a sintaxe do mpv: chave=valor,flag= (sem --; separe por vírgulas)", L"", EditorKind::Text },
            { L"Cookies do navegador", L"ui-ytdl-cookie-browser",
              L"Lê a sessão só ao resolver links. Firefox é recomendado no Windows", L"no", EditorKind::Choice,
              DetectedCookieBrowsers() }
        };

        const std::vector<OptionSpec> osd = {
            { L"Ocultar cursor só em tela cheia", L"cursor-autohide-fs-only",
              L"Mantém o cursor visível enquanto o player está em modo janela", L"no", EditorKind::Toggle },
            { L"Tempo para ocultar o cursor", L"cursor-autohide",
              L"Milissegundos de inatividade; também aceita no ou always", L"780", EditorKind::Text },
            { L"Nível das informações na tela", L"osd-level",
              L"0 desativa; 1 mostra interações; 2 inclui tempo; 3 inclui status", L"1", EditorKind::Choice,
              { L"0", L"1", L"2", L"3" } },
            { L"Duração das mensagens", L"osd-duration",
              L"Tempo em milissegundos que as mensagens permanecem visíveis", L"1000", EditorKind::Text },
            { L"Fonte das informações", L"osd-font",
              L"Fonte instalada no Windows usada nas mensagens de reprodução", L"Verdana", EditorKind::SystemFont },
            { L"Tamanho da fonte", L"osd-font-size",
              L"Tamanho base do texto mostrado sobre o vídeo", L"30", EditorKind::Text },
            { L"Cor do texto", L"osd-color",
              L"Cor RGB do texto das informações na tela", L"#FFFFFF", EditorKind::Text },
            { L"Cor do contorno", L"osd-border-color",
              L"Cor RGB usada no contorno do texto", L"#000000", EditorKind::Text },
            { L"Tamanho do contorno", L"osd-border-size",
              L"Espessura do contorno; zero o desativa", L"0.6", EditorKind::Text },
            { L"Desfoque do contorno", L"osd-blur",
              L"Desfoque gaussiano aplicado ao contorno do texto", L"0.2", EditorKind::Text }
        };

        const std::vector<OptionSpec> audio = {};

        const std::vector<OptionSpec> debanding = {
            { L"Passagens do filtro", L"deband-iterations",
              L"Número de passagens do filtro; valores maiores custam mais GPU", L"2", EditorKind::Text },
            { L"Limiar de detecção", L"deband-threshold",
              L"Sensibilidade usada para identificar faixas de cor", L"56", EditorKind::Text },
            { L"Alcance da análise", L"deband-range",
              L"Distância máxima considerada ao suavizar gradientes", L"17", EditorKind::Text },
            { L"Grão adicionado", L"deband-grain",
              L"Ruído fino que ajuda a disfarçar faixas residuais", L"12", EditorKind::Text }
        };

        auto addHeader = [](StackPanel const& panel, wchar_t const* text)
            {
                auto header = TextBlock{};
                header.Text(T(text));
                header.Margin({ 4, 10, 0, 0 });
                header.FontSize(13);
                header.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                header.Opacity(0.65);
                panel.Children().Append(header);
            };

        auto addOption = [this](StackPanel const& panel, OptionSpec const& spec)
            {
                auto titleLine = StackPanel{};
                titleLine.Orientation(Orientation::Horizontal);
                titleLine.Spacing(6);

                auto title = TextBlock{};
                title.Text(T(spec.title));
                title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                title.VerticalAlignment(VerticalAlignment::Center);
                titleLine.Children().Append(title);

                auto technicalText = TextBlock{};
                std::wstring compactName = spec.name;
                if (spec.name == L"sub-scale-by-window")
                    compactName = L"sub-scale-by-win...";
                else if (spec.name == L"cursor-autohide-fs-only")
                    compactName = L"cursor-autohi...";
                constexpr size_t maxTechnicalName = 18;
                if (compactName.size() > maxTechnicalName &&
                    spec.name != L"sub-scale-by-window")
                {
                    compactName.resize(maxTechnicalName - 1);
                    compactName += L"\u2026";
                }
                technicalText.Text(compactName);
                technicalText.FontSize(10);
                technicalText.Opacity(0.82);
                technicalText.TextTrimming(TextTrimming::CharacterEllipsis);
                technicalText.MaxWidth(105);

                bool const hideTechnicalBadge =
                    spec.name == L"taskbar-progress" ||
                    spec.name == L"fullscreen" ||
                    spec.name == L"cursor-autohide-fs-only";

                auto badge = Border{};
                if (!hideTechnicalBadge)
                {
                    badge.Style(Resources().Lookup(winrt::box_value(L"TechnicalBadge"))
                        .as<Microsoft::UI::Xaml::Style>());
                    badge.Padding({ 4, 1, 4, 1 });
                    badge.HorizontalAlignment(HorizontalAlignment::Left);
                    badge.Child(technicalText);
                    ToolTipService::SetToolTip(badge, winrt::box_value(spec.name));
                }

                auto description = TextBlock{};
                description.Text(T(spec.description));
                description.TextWrapping(TextWrapping::Wrap);
                description.FontSize(12);
                description.Opacity(0.62);

                auto details = StackPanel{};
                if (!hideTechnicalBadge)
                    titleLine.Children().Append(badge);
                details.Children().Append(titleLine);
                details.Children().Append(description);

                std::wstring value = spec.name == L"osd-msg3"
                    ? T(L"${playback-time:--:--}${?duration: / ${duration}}${!duration: / duração desconhecida}")
                    : spec.defaultValue;
                PlayerTryGetSavedMpvOption(spec.name, value);

                auto card = Border{};
                card.Style(Resources().Lookup(winrt::box_value(L"SettingsCard"))
                    .as<Microsoft::UI::Xaml::Style>());

                if (spec.editor == EditorKind::Text)
                {
                    auto editor = TextBox{};
                    editor.Tag(winrt::box_value(spec.name));
                    editor.Text(value);
                    if (spec.name == L"ytdl-format")
                    {
                        editor.PlaceholderText(T(L"Automático (recomendado)"));
                    }
                    editor.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Consolas" });

                    // Short scalar/color values use a two-column card: descriptive
                    // content stays on the left and the compact editor sits on the right.
                    // Long text options keep the original stacked, full-width layout.
                    std::wstring const editorName = spec.name;
                    bool const compactNumericEditor =
                        editorName == L"sub-scale" ||
                        editorName == L"osd-font-size" ||
                        editorName == L"osd-border-size" ||
                        editorName == L"osd-blur" ||
                        editorName == L"deband-iterations" ||
                        editorName == L"deband-threshold" ||
                        editorName == L"deband-range" ||
                        editorName == L"deband-grain";
                    bool const compactTimingEditor =
                        editorName == L"cursor-autohide" ||
                        editorName == L"osd-duration";
                    bool const compactColorEditor =
                        editorName == L"osd-color" ||
                        editorName == L"osd-border-color";
                    bool const compactEditor =
                        compactNumericEditor || compactTimingEditor || compactColorEditor;

                    bool const compactDebandEditor =
                        editorName == L"deband-iterations" ||
                        editorName == L"deband-threshold" ||
                        editorName == L"deband-range" ||
                        editorName == L"deband-grain";

                    if (compactEditor)
                    {
                        bool const compactOsdEditor =
                            editorName == L"osd-font-size" ||
                            editorName == L"osd-color" ||
                            editorName == L"osd-border-color" ||
                            editorName == L"osd-border-size" ||
                            editorName == L"osd-blur";
                        editor.Width(
                            editorName == L"sub-scale" || compactTimingEditor
                                ? 96.0
                                : (compactDebandEditor
                                    ? 88.0
                                    : (compactOsdEditor ? 96.0 : 88.0)));
                    }

                    editor.LostFocus([this, name = std::wstring(spec.name)](
                        Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&)
                        {
                            if (!m_ready) return;
                            std::wstring value = sender.as<TextBox>().Text().c_str();
                            if (name == L"sub-scale" || name == L"osd-font-size" ||
                                name == L"osd-border-size")
                            {
                                std::replace(value.begin(), value.end(), L',', L'.');
                            }
                            StageOption(name, value);
                        });

                    if (compactEditor)
                    {
                        auto layout = Grid{};
                        layout.ColumnSpacing(18);

                        auto detailsColumn = ColumnDefinition{};
                        detailsColumn.Width(
                            GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                        layout.ColumnDefinitions().Append(detailsColumn);

                        auto editorColumn = ColumnDefinition{};
                        editorColumn.Width(GridLengthHelper::Auto());
                        layout.ColumnDefinitions().Append(editorColumn);

                        details.VerticalAlignment(VerticalAlignment::Center);
                        Grid::SetColumn(details, 0);
                        layout.Children().Append(details);

                        editor.HorizontalAlignment(HorizontalAlignment::Right);
                        editor.VerticalAlignment(VerticalAlignment::Center);
                        Grid::SetColumn(editor, 1);
                        layout.Children().Append(editor);

                        card.Child(layout);
                    }
                    else
                    {
                        auto content = StackPanel{};
                        content.Spacing(9);
                        content.Children().Append(details);
                        editor.HorizontalAlignment(HorizontalAlignment::Stretch);
                        content.Children().Append(editor);
                        card.Child(content);
                    }
                }
                else
                {
                    auto layout = Grid{};
                    layout.ColumnSpacing(12);
                    auto flexible = ColumnDefinition{};
                    flexible.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                    layout.ColumnDefinitions().Append(flexible);
                    auto editorColumn = ColumnDefinition{};
                    editorColumn.Width(GridLengthHelper::Auto());
                    layout.ColumnDefinitions().Append(editorColumn);
                    layout.Children().Append(details);

                    if (spec.editor == EditorKind::Toggle)
                    {
                        auto editor = ToggleSwitch{};
                        editor.Tag(winrt::box_value(spec.name));
                        editor.IsOn(value != L"no" && value != L"false" && value != L"0");
                        editor.VerticalAlignment(VerticalAlignment::Center);
                        editor.HorizontalAlignment(HorizontalAlignment::Right);
                        editor.Toggled([this, name = std::wstring(spec.name)](
                            Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&)
                            {
                                if (!m_ready) return;
                                auto toggle = sender.as<ToggleSwitch>();
                                StageOption(name, toggle.IsOn() ? L"yes" : L"no");
                                if (toggle.IsOn() &&
                                    (name == L"linear-upscaling" || name == L"sigmoid-upscaling"))
                                {
                                    std::wstring other = name == L"linear-upscaling"
                                        ? L"sigmoid-upscaling" : L"linear-upscaling";
                                    if (auto found = m_nativeAdvancedToggles.find(other);
                                        found != m_nativeAdvancedToggles.end() && found->second.IsOn())
                                    {
                                        found->second.IsOn(false);
                                    }
                                }
                            });
                        m_nativeAdvancedToggles[spec.name] = editor;
                        Grid::SetColumn(editor, 1);
                        layout.Children().Append(editor);
                    }
                    else
                    {
                        auto editor = ComboBox{};
                        editor.Tag(winrt::box_value(spec.name));
                        bool const coreVideoBackendChoice =
                            spec.name == L"hwdec" ||
                            spec.name == L"vo" ||
                            spec.name == L"gpu-api" ||
                            spec.name == L"gpu-context";
                        editor.Width(spec.editor == EditorKind::SystemFont
                            ? 170
                            : (spec.name == L"dither-depth" ? 160
                                : (spec.name == L"osd-level" ? 88
                                    : (spec.name == L"video-output-levels" ? 108
                                        : (coreVideoBackendChoice ? 144 : 128)))));
                        int32_t selectedIndex{};
                        auto choices = spec.editor == EditorKind::SystemFont
                            ? m_systemFontNames : spec.choices;
                        for (auto const& choice : choices)
                        {
                            if (spec.name == L"dither-depth")
                            {
                                auto item = ComboBoxItem{};
                                if (choice == L"auto")
                                {
                                    item.Content(winrt::box_value(SettingsResource(
                                        L"SettingsDeinterlaceAuto.Content", L"Automático")));
                                }
                                else if (choice == L"no")
                                {
                                    item.Content(winrt::box_value(T(L"Desativado")));
                                }
                                else
                                {
                                    item.Content(winrt::box_value(choice + L"-bit"));
                                }
                                // Keep friendly UI labels separate from the exact
                                // token persisted and sent to mpv.
                                item.Tag(winrt::box_value(choice));
                                editor.Items().Append(item);
                            }
                            else if (spec.name == L"ui-ytdl-cookie-browser" && choice == L"no")
                            {
                                auto item = ComboBoxItem{};
                                item.Content(winrt::box_value(T(L"Desativado")));
                                // The UI is localized, while the persisted value
                                // remains the exact token expected by yt-dlp.
                                item.Tag(winrt::box_value(L"no"));
                                editor.Items().Append(item);
                            }
                            else if (spec.name == L"ui-ytdl-cookie-browser" && choice == L"firefox")
                            {
                                auto item = ComboBoxItem{};
                                item.Content(winrt::box_value(L"Firefox"));
                                // Keep the real yt-dlp token separate from the
                                // friendly label shown in the settings panel.
                                item.Tag(winrt::box_value(L"firefox"));
                                editor.Items().Append(item);
                            }
                            else
                            {
                                editor.Items().Append(winrt::box_value(choice));
                            }
                            if (choice == value)
                                selectedIndex = static_cast<int32_t>(editor.Items().Size()) - 1;
                        }
                        editor.SelectedIndex(selectedIndex);
                        editor.SelectionChanged([this, name = std::wstring(spec.name)](
                            Windows::Foundation::IInspectable const& sender, SelectionChangedEventArgs const&)
                            {
                                if (!m_ready) return;
                                auto combo = sender.as<ComboBox>();
                                if (auto selected = combo.SelectedItem())
                                {
                                    if (auto item = selected.try_as<ComboBoxItem>())
                                    {
                                        auto stored = item.Tag() ? item.Tag() : item.Content();
                                        StageOption(name,
                                            winrt::unbox_value<winrt::hstring>(stored).c_str());
                                    }
                                    else
                                    {
                                        StageOption(name,
                                            winrt::unbox_value<winrt::hstring>(selected).c_str());
                                    }
                                }
                            });
                        if (spec.name == L"hwdec")
                        {
                            m_hardwareDecodingChoice = editor;
                        }
                        Grid::SetColumn(editor, 1);
                        layout.Children().Append(editor);
                    }
                    card.Child(layout);
                }
                panel.Children().Append(card);
            };

        addHeader(GeneralNativePanel(), L"COMPORTAMENTO");
        for (auto const& option : general) addOption(GeneralNativePanel(), option);

        for (auto const& option : window) addOption(WindowNativePanel(), option);

        addHeader(StreamingNativePanel(), L"STREAMING DA WEB");
        for (auto const& option : streaming) addOption(StreamingNativePanel(), option);

        addHeader(OsdNativePanel(), L"INFORMAÇÕES NA TELA");
        for (auto const& option : osd) addOption(OsdNativePanel(), option);

        for (auto const& option : audio) addOption(AudioNativePanel(), option);

        for (auto const& option : video) addOption(VideoNativePanel(), option);

        addHeader(DebandingNativePanel(), L"AJUSTES DE DEBANDING");
        for (auto const& option : debanding) addOption(DebandingNativePanel(), option);

        for (auto const& option : subtitles) addOption(SubtitleNativePanel(), option);
    }

    void SettingsPage::UpdateBuiltInOptionVisibility(ImportedMpvConfig const*)
    {
        using Microsoft::UI::Xaml::Visibility;
        // Imported values now populate the first-class cards instead of
        // replacing them with raw duplicate rows.
        AlwaysOnTopCard().Visibility(Visibility::Visible);
        HardwareDecodingCard().Visibility(Visibility::Collapsed);
        PreciseSeekingCard().Visibility(Visibility::Visible);
        KeepOpenCard().Visibility(Visibility::Visible);
        AudioLanguageCard().Visibility(Visibility::Visible);
        SubtitleLanguageCard().Visibility(Visibility::Visible);
        DebandingCard().Visibility(Visibility::Visible);
        ScaleCard().Visibility(Visibility::Visible);
        DownscaleCard().Visibility(Visibility::Visible);
        ChromaCard().Visibility(Visibility::Visible);
        ToneMappingCard().Visibility(Visibility::Visible);
        TargetColorspaceHintCard().Visibility(Visibility::Visible);
        DitheringCard().Visibility(Visibility::Visible);
        VSyncCard().Visibility(Visibility::Visible);
        DisplayResampleCard().Visibility(Visibility::Visible);
        InterpolationCard().Visibility(Visibility::Visible);
        TemporalScalerCard().Visibility(Visibility::Visible);
        AutoSubtitlesCard().Visibility(Visibility::Visible);
        SubtitleFontCard().Visibility(Visibility::Visible);
        SubtitleSizeCard().Visibility(Visibility::Visible);
        ScreenshotFormatCard().Visibility(Visibility::Visible);
    }

    void SettingsPage::CloseClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_closing) return;
        m_closing = true;

        auto root = SettingsRoot();
        root.IsHitTestVisible(false);

        auto visual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(root);
        auto compositor = visual.Compositor();
        auto ease = compositor.CreateCubicBezierEasingFunction(
            { 0.55f, 0.055f }, { 0.675f, 0.19f });

        auto batch = compositor.CreateScopedBatch(
            Microsoft::UI::Composition::CompositionBatchTypes::Animation);

        // Preserve the original 520-DIP slide and fade, but animate compositor
        // properties instead of TranslateTransform.X. This keeps every frame
        // off the XAML layout thread and avoids EnableDependentAnimation.
        auto slide = compositor.CreateVector3KeyFrameAnimation();
        slide.InsertKeyFrame(1.0f, { 520.0f, 0.0f, 0.0f }, ease);
        slide.Duration(std::chrono::milliseconds(190));
        slide.StopBehavior(
            Microsoft::UI::Composition::AnimationStopBehavior::SetToFinalValue);
        visual.StartAnimation(L"Offset", slide);

        auto fade = compositor.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1.0f, 0.0f, ease);
        fade.Duration(std::chrono::milliseconds(145));
        fade.StopBehavior(
            Microsoft::UI::Composition::AnimationStopBehavior::SetToFinalValue);
        visual.StartAnimation(L"Opacity", fade);

        batch.Completed([root](auto&&, auto&&)
            {
                PlayerCloseSettings();
            });
        batch.End();
    }

    void SettingsPage::AlwaysOnTopToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;

        // Use the exact same runtime path as Ctrl+T/context-menu. This makes
        // the Toggle and shortcut two views of one shared state, and the
        // native setter already persists ui-ontop.
        PlayerSetAlwaysOnTop(
            sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn());
    }

    void SettingsPage::InterfaceVisibilityToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        auto toggle = sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>();
        auto option = winrt::unbox_value<winrt::hstring>(toggle.Tag());
        StageOption(option.c_str(), toggle.IsOn() ? L"yes" : L"no");
    }

    void SettingsPage::TimelineStyleChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;

        auto combo = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>();
        auto optionName = combo.Tag()
            ? winrt::unbox_value_or<winrt::hstring>(
                combo.Tag(), L"ui-timeline-style")
            : winrt::hstring{ L"ui-timeline-style" };

        if (auto item = combo.SelectedItem().try_as<
            Microsoft::UI::Xaml::Controls::ComboBoxItem>())
        {
            auto value = winrt::unbox_value_or<winrt::hstring>(
                item.Tag(),
                optionName == L"ui-card-style"
                    ? L"hcplayer"
                    : L"default");

            StageOption(optionName.c_str(), value.c_str());

            if (optionName == L"ui-card-style")
            {
                ApplySettingsCardStyle(*this, value.c_str());
            }
        }
    }

    void SettingsPage::PreciseSeekingToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        PlayerSetMpvOption(L"hr-seek",
            sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn() ? L"yes" : L"no");
    }

    void SettingsPage::KeepOpenToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        PlayerSetMpvOption(L"keep-open",
            sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn() ? L"always" : L"no");
    }

    void SettingsPage::ChapterHoverCardToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        StageOption(L"ui-chapter-tooltip",
            sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn()
            ? L"yes" : L"no");
    }

    void SettingsPage::DebandingToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        PlayerSetMpvOption(L"deband",
            sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn() ? L"yes" : L"no");
    }

    void SettingsPage::AutoSubtitlesToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        PlayerSetMpvOption(L"sub-auto",
            sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn() ? L"fuzzy" : L"no");
    }

    void SettingsPage::ImportFontFolderClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        winrt::check_hresult(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.put())));

        dialog->SetTitle(T(L"Selecionar pasta de fontes").c_str());

        FILEOPENDIALOGOPTIONS options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS |
            FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);

        if (FAILED(dialog->Show(nullptr)))
        {
            return;
        }

        winrt::com_ptr<IShellItem> item;
        winrt::check_hresult(dialog->GetResult(item.put()));
        PWSTR rawPath{};
        winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
        std::filesystem::path selectedFolder{ rawPath };
        CoTaskMemFree(rawPath);

        auto destination = ImportedFontsDirectory();
        std::error_code error;
        std::filesystem::create_directories(destination, error);
        size_t imported{};
        int32_t lastIndex = -1;
        for (auto const& entry : std::filesystem::recursive_directory_iterator(
            selectedFolder, std::filesystem::directory_options::skip_permission_denied, error))
        {
            if (!entry.is_regular_file() || !IsFontFile(entry.path())) continue;
            auto target = destination / entry.path().filename();
            std::filesystem::copy_file(entry.path(), target,
                std::filesystem::copy_options::overwrite_existing, error);
            if (error)
            {
                error.clear();
                continue;
            }
            ++imported;
            for (auto const& choice : ImportedFontChoices(target))
            {
                auto existing = std::find(
                    m_fontNames.begin(), m_fontNames.end(), choice.mpvName);
                if (existing == m_fontNames.end())
                {
                    m_fontNames.push_back(choice.mpvName);
                    m_fontDisplayNames.push_back(choice.displayName);
                    m_fontFamilyAliases.push_back(
                        choice.familyName.empty() ? choice.mpvName : choice.familyName);
                    SubtitleFontCombo().Items().Append(
                        winrt::box_value(choice.displayName));
                    lastIndex = static_cast<int32_t>(m_fontNames.size()) - 1;
                }
            }
        }
        PlayerSetMpvOption(L"sub-fonts-dir", destination.wstring());
        SubtitleFontStatus().Text(imported
            ? std::to_wstring(imported) + T(L" arquivos de fonte importados")
            : T(L"Nenhuma fonte .ttf, .otf ou .ttc encontrada"));
        if (lastIndex >= 0)
        {
            SubtitleFontCombo().SelectedIndex(lastIndex);
        }
    }

    void SettingsPage::ImportMediaBadgeSetClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        winrt::check_hresult(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.put())));

        dialog->SetTitle(
            SettingsResource(
                L"MediaBadgeFolderPickerTitle",
                L"Selecionar pasta com badges personalizadas").c_str());

        FILEOPENDIALOGOPTIONS options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS |
            FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);

        if (FAILED(dialog->Show(
            reinterpret_cast<HWND>(PlayerGetMainWindowHandle()))))
        {
            return;
        }

        winrt::com_ptr<IShellItem> item;
        winrt::check_hresult(dialog->GetResult(item.put()));

        PWSTR rawPath{};
        winrt::check_hresult(
            item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
        std::wstring selectedFolder{ rawPath };
        CoTaskMemFree(rawPath);

        int imported{};
        std::wstring error;
        if (!PlayerImportCustomBadgeSet(selectedFolder, imported, error))
        {
            MediaBadgeSetStatus().Text(
                SettingsResource(
                    L"MediaBadgeImportFailed",
                    L"Nenhuma badge compatível foi importada"));
            return;
        }

        bool const wasReady = m_ready;
        m_ready = false;
        MediaBadgeStyleCombo().SelectedIndex(1);
        m_ready = wasReady;

        StageOption(L"ui-media-badge-style", L"custom");

        RefreshMediaBadgeSetStatus();
    }

    void SettingsPage::ResetMediaBadgeSetClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        std::wstring error;
        if (!PlayerResetCustomBadgeSet(error))
        {
            MediaBadgeSetStatus().Text(
                SettingsResource(
                    L"MediaBadgeResetFailed",
                    L"Não foi possível restaurar as badges padrão"));
            return;
        }

        bool const wasReady = m_ready;
        m_ready = false;
        MediaBadgeStyleCombo().SelectedIndex(0);
        m_ready = wasReady;

        StageOption(L"ui-media-badge-style", L"default");
        RefreshMediaBadgeSetStatus();
    }

    void SettingsPage::RefreshMediaBadgeSetStatus()
    {
        MediaBadgeSetStatus().Text(
            PlayerGetCustomBadgeFileCount() > 0
                ? SettingsResource(
                    L"MediaBadgeCustomSetActive",
                    L"Conjunto personalizado ativo")
                : SettingsResource(
                    L"MediaBadgeNoCustomSet",
                    L"Nenhum conjunto personalizado importado"));
    }

    void SettingsPage::ImportMediaBadgeIndividualClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        winrt::check_hresult(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.put())));

        dialog->SetTitle(
            SettingsResource(
                L"MediaBadgeFilePickerTitle",
                L"Selecionar imagem da badge").c_str());

        std::wstring const imageFilter =
            SettingsResource(
                L"MediaBadgeImageFilter",
                L"Imagens de badge (*.png;*.svg)");

        COMDLG_FILTERSPEC filters[] = {
            { imageFilter.c_str(), L"*.png;*.svg" },
            { L"PNG (*.png)", L"*.png" },
            { L"SVG (*.svg)", L"*.svg" },
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetFileTypeIndex(1);

        FILEOPENDIALOGOPTIONS options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(
            options |
            FOS_FILEMUSTEXIST |
            FOS_PATHMUSTEXIST |
            FOS_FORCEFILESYSTEM);

        if (FAILED(dialog->Show(
            reinterpret_cast<HWND>(PlayerGetMainWindowHandle()))))
        {
            return;
        }

        winrt::com_ptr<IShellItem> item;
        winrt::check_hresult(dialog->GetResult(item.put()));

        PWSTR rawPath{};
        winrt::check_hresult(
            item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
        std::wstring selectedFile{ rawPath };
        CoTaskMemFree(rawPath);

        std::wstring badgeName;
        std::wstring variant;
        std::wstring error;

        if (!PlayerImportCustomBadgeFile(
            selectedFile,
            badgeName,
            variant,
            error))
        {
            MediaBadgeIndividualStatus().Text(
                SettingsResource(
                    L"MediaBadgeFilenameInvalid",
                    L"Nome incompatível. Use, por exemplo, DolbyVision.Dark.png"));
            return;
        }

        auto badgeDisplayName =
            [](std::wstring const& name) -> std::wstring
            {
                if (name == L"DolbyAudio") return L"Dolby Audio";
                if (name == L"DolbyVision") return L"Dolby Vision";
                if (name == L"DolbyAtmos") return L"Dolby Atmos";
                if (name == L"DTS") return L"DTS";
                if (name == L"DTSX") return L"DTS:X";
                if (name == L"YouTube") return L"YouTube";
                if (name == L"HDR10Plus") return L"HDR10+";
                if (name == L"HDR") return L"HDR";
                return name;
            };

        std::wstring const themeName =
            variant == L"Both"
                ? SettingsResource(
                    L"SettingsMediaBadgeVariantBoth.Content",
                    L"Ambos os temas")
                : (variant == L"Light"
                    ? SettingsResource(
                        L"MediaBadgeFilenameThemeLight",
                        L"Tema claro")
                    : SettingsResource(
                        L"MediaBadgeFilenameThemeDark",
                        L"Tema escuro"));

        MediaBadgeIndividualStatus().Text(
            badgeDisplayName(badgeName) +
            L" — " +
            themeName +
            L": " +
            SettingsResource(
                L"MediaBadgeFilenameImported",
                L"importada"));

        bool const wasReady = m_ready;
        m_ready = false;
        MediaBadgeStyleCombo().SelectedIndex(1);
        m_ready = wasReady;

        StageOption(L"ui-media-badge-style", L"custom");
        RefreshMediaBadgeSetStatus();
    }

    void SettingsPage::SubtitleFontSelectionChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        int32_t index = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>().SelectedIndex();
        if (index >= 0 && index < static_cast<int32_t>(m_fontNames.size()))
        {
            PlayerSetMpvOption(L"sub-fonts-dir", ImportedFontsDirectory().wstring());
            PlayerSetMpvOption(L"sub-font", m_fontNames[index]);
            auto const& displayName =
                index < static_cast<int32_t>(m_fontDisplayNames.size())
                ? m_fontDisplayNames[index]
                : m_fontNames[index];
            SubtitleFontStatus().Text(T(L"Fonte selecionada: ") + displayName);
        }
    }

    void SettingsPage::ScalingQualityChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        static constexpr wchar_t const* values[] = {
            L"bilinear", L"bicubic_fast", L"spline36", L"ewa_lanczos" };
        int32_t index = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>().SelectedIndex();
        if (index >= 0 && index < ARRAYSIZE(values)) PlayerSetMpvOption(L"scale", values[index]);
    }

    void SettingsPage::DownscaleFilterChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        auto combo = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>();
        if (auto selected = combo.SelectedItem())
        {
            PlayerSetMpvOption(L"dscale", winrt::unbox_value<winrt::hstring>(selected).c_str());
        }
    }

    void SettingsPage::ChromaFilterChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        auto combo = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>();
        if (auto selected = combo.SelectedItem())
        {
            PlayerSetMpvOption(L"cscale", winrt::unbox_value<winrt::hstring>(selected).c_str());
        }
    }

    void SettingsPage::ToneMappingChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        static constexpr wchar_t const* values[] = {
            L"auto", L"bt.2390", L"bt.2446a", L"spline", L"mobius", L"reinhard",
            L"hable", L"clip", L"gamma", L"linear", L"st2094-40", L"st2094-10" };
        int32_t index = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>().SelectedIndex();
        if (index >= 0 && index < ARRAYSIZE(values)) PlayerSetMpvOption(L"tone-mapping", values[index]);
    }

    void SettingsPage::TargetColorspaceHintChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        static constexpr wchar_t const* values[] = { L"auto", L"yes", L"no" };
        int32_t index = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>().SelectedIndex();
        if (index >= 0 && index < ARRAYSIZE(values))
        {
            PlayerSetMpvOption(L"target-colorspace-hint", values[index]);
        }
    }

    void SettingsPage::DitheringChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        static constexpr wchar_t const* values[] = {
            L"no", L"ordered", L"fruit", L"error-diffusion" };
        int32_t index = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>().SelectedIndex();
        if (index >= 0 && index < ARRAYSIZE(values)) PlayerSetMpvOption(L"dither", values[index]);
    }

    void SettingsPage::VSyncToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        PlayerSetMpvOption(L"d3d11-sync-interval",
            sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn() ? L"1" : L"0");
    }

    void SettingsPage::UpdateTemporalScalerAvailability()
    {
        using Microsoft::UI::Xaml::Visibility;

        bool const enabled = InterpolationToggle().IsOn();

        // tscale is a dependent setting: it has meaning only while mpv
        // interpolation is enabled. Keep the selected filter visible so the
        // user's preference is never lost, but make the inactive state
        // unambiguous in the UI.
        TemporalScalerCombo().IsEnabled(enabled);
        TemporalScalerDisabledHint().Visibility(
            enabled ? Visibility::Collapsed : Visibility::Visible);
    }

    void SettingsPage::DisplayResampleToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        bool enabled = sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn();
        PlayerSetMpvOption(L"video-sync", enabled ? L"display-resample" : L"audio");
        if (!enabled && InterpolationToggle().IsOn())
        {
            InterpolationToggle().IsOn(false);
        }
        UpdateTemporalScalerAvailability();
    }

    void SettingsPage::InterpolationToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        bool enabled = sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn();
        PlayerSetMpvOption(L"interpolation", enabled ? L"yes" : L"no");
        if (enabled && !DisplayResampleToggle().IsOn())
        {
            DisplayResampleToggle().IsOn(true);
        }
        if (enabled && TemporalScalerCombo().SelectedIndex() < 0)
        {
            TemporalScalerCombo().SelectedIndex(0);
        }
        UpdateTemporalScalerAvailability();
    }

    void SettingsPage::NativeSubtitleToggleToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        auto toggle = sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>();
        auto option = winrt::unbox_value<winrt::hstring>(toggle.Tag());
        PlayerSetMpvOption(option.c_str(), toggle.IsOn() ? L"yes" : L"no");
    }

    void SettingsPage::NativeSubtitleChoiceChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        auto combo = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>();
        if (auto selected = combo.SelectedItem())
        {
            auto option = winrt::unbox_value<winrt::hstring>(combo.Tag());
            winrt::hstring value;
            if (auto item = selected.try_as<Microsoft::UI::Xaml::Controls::ComboBoxItem>())
            {
                // Localized choices may show a friendly label while Tag keeps
                // the exact mpv value (for example deinterlace: Ativado -> yes).
                value = item.Tag()
                    ? winrt::unbox_value_or<winrt::hstring>(
                        item.Tag(), winrt::hstring{})
                    : winrt::unbox_value_or<winrt::hstring>(
                        item.Content(), winrt::hstring{});
            }
            else
            {
                value = winrt::unbox_value<winrt::hstring>(selected);
            }
            PlayerSetMpvOption(option.c_str(), value.c_str());
        }
    }

    void SettingsPage::AudioDeviceChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        int32_t index = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>().SelectedIndex();
        if (index >= 0 && index < static_cast<int32_t>(m_audioDeviceNames.size()))
        {
            PlayerSetMpvOption(L"audio-device", m_audioDeviceNames[index]);
        }
    }

    void SettingsPage::NativeSubtitleTextLostFocus(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        auto text = sender.as<Microsoft::UI::Xaml::Controls::TextBox>();
        auto option = winrt::unbox_value<winrt::hstring>(text.Tag());
        PlayerSetMpvOption(option.c_str(), text.Text().c_str());
    }

    void SettingsPage::NativeSubtitleNumericTextChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
    {
        if (!m_ready) return;
        auto textBox = sender.as<Microsoft::UI::Xaml::Controls::TextBox>();
        auto option = winrt::unbox_value<winrt::hstring>(textBox.Tag());
        std::wstring value = textBox.Text().c_str();
        std::replace(value.begin(), value.end(), L',', L'.');
        if (!value.empty() && value != L"-" && value != L"." && value != L"-.")
        {
            PlayerSetMpvOption(option.c_str(), value);
        }
    }

    void SettingsPage::TemporalScalerChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        static constexpr wchar_t const* values[] = {
            L"oversample", L"linear", L"box", L"triangle", L"sphinx", L"mitchell", L"catmull_rom" };
        int32_t index = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>().SelectedIndex();
        if (index >= 0 && index < ARRAYSIZE(values)) PlayerSetMpvOption(L"tscale", values[index]);
    }

    void SettingsPage::BlendSubtitlesChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        static constexpr wchar_t const* values[] = { L"no", L"yes", L"video" };
        int32_t index = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>().SelectedIndex();
        if (index >= 0 && index < ARRAYSIZE(values))
        {
            PlayerSetMpvOption(L"blend-subtitles", values[index]);
        }
    }

    void SettingsPage::ScreenshotFormatChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        static constexpr wchar_t const* values[] = { L"png", L"jpg", L"webp", L"jxl", L"avif" };
        int32_t index = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>().SelectedIndex();
        if (index >= 0 && index < ARRAYSIZE(values)) PlayerSetMpvOption(L"screenshot-format", values[index]);
    }

    void SettingsPage::ScreenshotHighBitDepthToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        PlayerSetMpvOption(L"screenshot-high-bit-depth",
            sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn() ? L"yes" : L"no");
    }

    void SettingsPage::ScreenshotPngCompressionChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!m_ready) return;
        int32_t index = sender.as<Microsoft::UI::Xaml::Controls::ComboBox>().SelectedIndex();
        if (index >= 0 && index <= 9)
        {
            PlayerSetMpvOption(L"screenshot-png-compression", std::to_wstring(index));
        }
    }

    void SettingsPage::ChooseScreenshotDirectoryClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        winrt::check_hresult(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.put())));
        dialog->SetTitle(T(L"Selecionar pasta para capturas").c_str());

        FILEOPENDIALOGOPTIONS options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS |
            FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);

        if (FAILED(dialog->Show(nullptr))) return;

        winrt::com_ptr<IShellItem> item;
        winrt::check_hresult(dialog->GetResult(item.put()));
        PWSTR rawPath{};
        winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
        std::wstring path{ rawPath };
        CoTaskMemFree(rawPath);

        if (PlayerSetMpvOption(L"screenshot-directory", path))
        {
            ScreenshotDirectoryText().Text(path);
        }
    }

    void SettingsPage::ScreenshotTemplateLostFocus(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        auto textBox = sender.as<Microsoft::UI::Xaml::Controls::TextBox>();
        if (!PlayerSetMpvOption(L"screenshot-template", textBox.Text().c_str()))
        {
            textBox.Text(L"%f-%wH.%wM.%wS.%wT-#%#00n");
        }
    }

    void SettingsPage::HardwareDecodingToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_ready) return;
        StageOption(L"hwdec",
            sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn() ? L"d3d11va" : L"no");
    }

    void SettingsPage::SubtitleSizeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        SubtitleSizeText().Text(std::to_wstring(static_cast<int>(args.NewValue())));
        if (!m_ready) return;
        StageOption(L"sub-font-size",
            std::to_wstring(static_cast<int>(args.NewValue())));
    }

    void SettingsPage::ImportConfigClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        winrt::check_hresult(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.put())));

        std::wstring configFilterLabel = T(L"Configuração do player");
        std::wstring allFilesFilterLabel = T(L"Todos os arquivos");
        COMDLG_FILTERSPEC filters[] = {
            { configFilterLabel.c_str(), L"*.conf" },
            { allFilesFilterLabel.c_str(), L"*.*" }
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetTitle(T(L"Importar configuração").c_str());
        dialog->SetFileName(L"config.conf");

        if (SUCCEEDED(dialog->Show(nullptr)))
        {
            winrt::com_ptr<IShellItem> item;
            winrt::check_hresult(dialog->GetResult(item.put()));

            PWSTR rawPath{};
            winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
            std::wstring path{ rawPath };
            CoTaskMemFree(rawPath);

            ImportPath(path);
        }
    }

    void SettingsPage::ResetConfigClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        bool reset = PlayerResetImportedConfig();
        if (reset)
        {
            UpdateBuiltInOptionVisibility(nullptr);
            bool wasReady = m_ready;
            m_ready = false;
            RestoreSavedControls(SettingsRoot());
            std::wstring audioDevice;
            if (PlayerTryGetSavedMpvOption(L"audio-device", audioDevice))
            {
                auto found = std::find(
                    m_audioDeviceNames.begin(), m_audioDeviceNames.end(), audioDevice);
                if (found != m_audioDeviceNames.end())
                    AudioDeviceCombo().SelectedIndex(
                        static_cast<int32_t>(found - m_audioDeviceNames.begin()));
            }
            std::wstring screenshotDirectory;
            if (PlayerTryGetSavedMpvOption(
                L"screenshot-directory", screenshotDirectory))
            {
                ScreenshotDirectoryText().Text(screenshotDirectory);
            }
            m_ready = wasReady;
            m_pendingOptions.clear();
            SaveButton().IsEnabled(false);
        }
        ImportedConfigPanel().Children().Clear();
        ImportedConfigPanel().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        m_importedConfigRendered = true;
        ConfigImportStatus().IsOpen(true);
        ConfigImportStatus().Title(reset ? T(L"Configuração restaurada") : T(L"Falha ao restaurar"));
        ConfigImportStatus().Message(reset
            ? T(L"A configuração importada foi removida e os padrões do HC Player foram restaurados.")
            : T(L"Não foi possível remover o arquivo de configuração."));
        ConfigImportStatus().Severity(reset
            ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success
            : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
    }

    void SettingsPage::RefreshAnime4KPanel()
    {
        using namespace Microsoft::UI::Xaml::Controls;

        auto status = PlayerGetAnime4KStatus();
        m_refreshingAnime4K = true;

        auto modeAvailable = [](std::vector<PlayerAnime4KModeInfo> const& modes)
        {
            return std::any_of(
                modes.begin(), modes.end(),
                [](PlayerAnime4KModeInfo const& mode)
                {
                    return mode.available;
                });
        };

        bool const fastAvailable = modeAvailable(status.fastModes);
        bool const hqAvailable = modeAvailable(status.hqModes);

        auto fastItem = Anime4KProfileCombo().Items().GetAt(0)
            .as<ComboBoxItem>();
        auto hqItem = Anime4KProfileCombo().Items().GetAt(1)
            .as<ComboBoxItem>();
        fastItem.IsEnabled(fastAvailable);
        hqItem.IsEnabled(hqAvailable);

        std::wstring currentProfile;
        if (auto current = Anime4KProfileCombo().SelectedItem()
            .try_as<ComboBoxItem>())
        {
            currentProfile = winrt::unbox_value_or<winrt::hstring>(
                current.Tag(), winrt::hstring{}).c_str();
        }

        auto profileIsAvailable = [&](std::wstring const& profile)
        {
            return (_wcsicmp(profile.c_str(), L"fast") == 0 && fastAvailable) ||
                (_wcsicmp(profile.c_str(), L"hq") == 0 && hqAvailable);
        };

        std::wstring selectedProfile;
        if (profileIsAvailable(currentProfile))
        {
            selectedProfile = currentProfile;
        }
        else if (profileIsAvailable(status.activeProfile))
        {
            selectedProfile = status.activeProfile;
        }
        else if (fastAvailable)
        {
            selectedProfile = L"fast";
        }
        else if (hqAvailable)
        {
            selectedProfile = L"hq";
        }

        if (_wcsicmp(selectedProfile.c_str(), L"fast") == 0)
        {
            Anime4KProfileCombo().SelectedIndex(0);
        }
        else if (_wcsicmp(selectedProfile.c_str(), L"hq") == 0)
        {
            Anime4KProfileCombo().SelectedIndex(1);
        }
        else
        {
            Anime4KProfileCombo().SelectedIndex(-1);
        }

        auto const* selectedModes =
            _wcsicmp(selectedProfile.c_str(), L"hq") == 0
            ? &status.hqModes
            : &status.fastModes;

        std::wstring currentMode;
        if (auto current = Anime4KModeCombo().SelectedItem()
            .try_as<ComboBoxItem>())
        {
            currentMode = winrt::unbox_value_or<winrt::hstring>(
                current.Tag(), winrt::hstring{}).c_str();
        }

        auto lookupMode = [&](std::wstring const& mode)
            -> PlayerAnime4KModeInfo const*
        {
            if (selectedProfile.empty())
            {
                return nullptr;
            }

            auto found = std::find_if(
                selectedModes->begin(), selectedModes->end(),
                [&mode](PlayerAnime4KModeInfo const& info)
                {
                    return _wcsicmp(
                        info.mode.c_str(), mode.c_str()) == 0;
                });
            return found == selectedModes->end()
                ? nullptr
                : &*found;
        };

        for (uint32_t index = 0;
            index < Anime4KModeCombo().Items().Size(); ++index)
        {
            auto item = Anime4KModeCombo().Items().GetAt(index)
                .as<ComboBoxItem>();
            std::wstring mode = winrt::unbox_value_or<winrt::hstring>(
                item.Tag(), winrt::hstring{}).c_str();
            auto info = lookupMode(mode);
            bool available = info && info->available;
            item.IsEnabled(available);

            if (info && !info->available && !info->missingFiles.empty())
            {
                std::wostringstream hint;
                hint << T(L"Faltam ") << info->missingFiles.size()
                    << T(L" shader(s). Ex.: ")
                    << info->missingFiles.front();
                ToolTipService::SetToolTip(
                    item, winrt::box_value(hint.str()));
            }
            else
            {
                ToolTipService::SetToolTip(item, winrt::box_value(winrt::hstring{}));
            }
        }

        std::wstring selectedMode;
        if (auto info = lookupMode(currentMode);
            info && info->available)
        {
            selectedMode = currentMode;
        }
        else if (_wcsicmp(
            selectedProfile.c_str(),
            status.activeProfile.c_str()) == 0)
        {
            if (auto info = lookupMode(status.activeMode);
                info && info->available)
            {
                selectedMode = status.activeMode;
            }
        }

        if (selectedMode.empty())
        {
            for (auto const& info : *selectedModes)
            {
                if (info.available)
                {
                    selectedMode = info.mode;
                    break;
                }
            }
        }

        int32_t selectedModeIndex = -1;
        for (uint32_t index = 0;
            index < Anime4KModeCombo().Items().Size(); ++index)
        {
            auto item = Anime4KModeCombo().Items().GetAt(index)
                .as<ComboBoxItem>();
            std::wstring mode = winrt::unbox_value_or<winrt::hstring>(
                item.Tag(), winrt::hstring{}).c_str();
            if (!selectedMode.empty() &&
                _wcsicmp(mode.c_str(), selectedMode.c_str()) == 0)
            {
                selectedModeIndex = static_cast<int32_t>(index);
                break;
            }
        }
        Anime4KModeCombo().SelectedIndex(selectedModeIndex);

        bool const hasSelectablePreset =
            !selectedProfile.empty() && selectedModeIndex >= 0;
        Anime4KProfileCombo().IsEnabled(fastAvailable || hqAvailable);
        Anime4KModeCombo().IsEnabled(hasSelectablePreset);
        Anime4KEnabledToggle().IsEnabled(
            hasSelectablePreset || status.anyActive);
        Anime4KEnabledToggle().IsOn(status.anyActive);

        std::wostringstream summary;
        if (status.detectedShaderCount == 0)
        {
            auto noAnime4KShaders = T(L"Nenhum shader Anime4K detectado.");
            if (!noAnime4KShaders.empty() && noAnime4KShaders.back() == L'.')
                noAnime4KShaders.pop_back();
            summary << noAnime4KShaders;
        }
        else
        {
            summary << status.detectedShaderCount
                << T(L" shader(s) Anime4K detectado(s) • Fast: ")
                << (fastAvailable ? T(L"disponível") : T(L"incompleto"))
                << T(L" • HQ: ")
                << (hqAvailable ? T(L"disponível") : T(L"incompleto"));

            if (!status.activeProfile.empty())
            {
                summary << T(L" • Ativo: ")
                    << (_wcsicmp(
                        status.activeProfile.c_str(), L"hq") == 0
                        ? L"HQ"
                        : L"Fast")
                    << L" / " << status.activeMode;
            }
            else if (status.customActive)
            {
                summary << T(L" • Ativo: configuração personalizada");
            }
            else
            {
                summary << T(L" • Desativado");
            }
        }
        Anime4KDetectedText().Text(summary.str());

        m_refreshingAnime4K = false;
    }

    bool SettingsPage::ApplySelectedAnime4KMode()
    {
        auto profileItem = Anime4KProfileCombo().SelectedItem()
            .try_as<Microsoft::UI::Xaml::Controls::ComboBoxItem>();
        auto modeItem = Anime4KModeCombo().SelectedItem()
            .try_as<Microsoft::UI::Xaml::Controls::ComboBoxItem>();
        if (!profileItem || !modeItem)
        {
            SetShaderStatus(
                false,
                T(L"Anime4K indisponível"),
                T(L"Importe todos os shaders necessários para pelo menos um modo."));
            return false;
        }

        std::wstring profile =
            winrt::unbox_value_or<winrt::hstring>(
                profileItem.Tag(), winrt::hstring{}).c_str();
        std::wstring mode =
            winrt::unbox_value_or<winrt::hstring>(
                modeItem.Tag(), winrt::hstring{}).c_str();

        std::wstring error;
        if (!PlayerSetAnime4KMode(profile, mode, error))
        {
            SetShaderStatus(
                false,
                T(L"Não foi possível ativar o Anime4K"),
                error);
            RefreshShaderList();
            return false;
        }

        SetShaderStatus(
            true,
            T(L"Anime4K ativado"),
            T(L"Perfil ") +
            (_wcsicmp(profile.c_str(), L"hq") == 0
                ? L"HQ"
                : L"Fast") +
            T(L", modo ") + mode +
            T(L", aplicado na ordem oficial."));
        RefreshShaderList();
        return true;
    }

    void SettingsPage::Anime4KProfileChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (m_refreshingAnime4K)
        {
            return;
        }

        RefreshAnime4KPanel();
        if (Anime4KEnabledToggle().IsOn())
        {
            ApplySelectedAnime4KMode();
        }
    }

    void SettingsPage::Anime4KModeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (m_refreshingAnime4K)
        {
            return;
        }

        if (Anime4KEnabledToggle().IsOn())
        {
            ApplySelectedAnime4KMode();
        }
    }

    void SettingsPage::Anime4KEnabledToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_refreshingAnime4K)
        {
            return;
        }

        bool enabled = sender.as<
            Microsoft::UI::Xaml::Controls::ToggleSwitch>().IsOn();
        if (enabled)
        {
            ApplySelectedAnime4KMode();
            return;
        }

        std::wstring error;
        if (PlayerDisableAnime4K(error))
        {
            SetShaderStatus(
                true,
                T(L"Anime4K desativado"),
                T(L"Os shaders Anime4K foram removidos do pipeline. Outros shaders personalizados permanecem como estavam."));
            RefreshShaderList();
        }
        else
        {
            SetShaderStatus(
                false,
                T(L"Não foi possível desativar o Anime4K"),
                error);
            RefreshShaderList();
        }
    }

    void SettingsPage::SetShaderStatus(
        bool success,
        std::wstring const& title,
        std::wstring const& message)
    {
        ShaderStatus().IsOpen(true);
        ShaderStatus().Title(title);
        ShaderStatus().Message(LocalizeSettingsMessage(message));
        ShaderStatus().Severity(success
            ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success
            : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
    }

    void SettingsPage::RefreshShaderList()
    {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;

        m_refreshingShaders = true;
        ShaderListPanel().Children().Clear();

        auto shaders = PlayerGetShaders();
        ShaderMoreButton().IsEnabled(!shaders.empty());
        auto anime4KStatus = PlayerGetAnime4KStatus();
        bool const anime4KPresetActive =
            !anime4KStatus.activeProfile.empty() &&
            !anime4KStatus.activeMode.empty();
        std::wstring anime4KPresetLabel;
        if (anime4KPresetActive)
        {
            anime4KPresetLabel =
                (_wcsicmp(anime4KStatus.activeProfile.c_str(), L"hq") == 0
                    ? L"HQ "
                    : L"Fast ") +
                anime4KStatus.activeMode;
        }

        ShaderEmptyText().Visibility(shaders.empty()
            ? Visibility::Visible
            : Visibility::Collapsed);

        auto cardStyle = Resources().Lookup(
            winrt::box_value(L"SettingsCard"))
            .as<Microsoft::UI::Xaml::Style>();
        auto technicalBadgeStyle = Resources().Lookup(
            winrt::box_value(L"TechnicalBadge"))
            .as<Microsoft::UI::Xaml::Style>();

        for (size_t index = 0; index < shaders.size(); ++index)
        {
            auto const& shader = shaders[index];

            auto card = Border{};
            card.Style(cardStyle);

            auto layout = Grid{};
            layout.ColumnSpacing(6);

            auto nameColumn = ColumnDefinition{};
            nameColumn.Width(GridLengthHelper::FromValueAndType(
                1, GridUnitType::Star));
            layout.ColumnDefinitions().Append(nameColumn);

            for (int column = 0; column < 4; ++column)
            {
                auto definition = ColumnDefinition{};
                definition.Width(GridLengthHelper::Auto());
                layout.ColumnDefinitions().Append(definition);
            }

            auto nameHost = StackPanel{};
            nameHost.VerticalAlignment(VerticalAlignment::Center);

            auto name = TextBlock{};
            name.Text(shader.name);
            name.FontWeight(
                Windows::UI::Text::FontWeights::SemiBold());
            name.TextTrimming(TextTrimming::CharacterEllipsis);
            name.MaxLines(1);
            // The visible shader name can be ellipsized in narrow layouts.
            // Hovering reveals the complete display name without changing the card.
            ToolTipService::SetToolTip(
                name, winrt::box_value(shader.name));
            nameHost.Children().Append(name);

            auto detailRow = Grid{};
            detailRow.ColumnSpacing(6);

            // Keep the preset pill intact when the shader name/details compete
            // for horizontal space. The descriptive text yields first; the badge
            // keeps its natural width instead of being clipped by the name column.
            auto detailTextColumn = ColumnDefinition{};
            detailTextColumn.Width(GridLengthHelper::FromValueAndType(
                1, GridUnitType::Star));
            detailRow.ColumnDefinitions().Append(detailTextColumn);

            auto detailBadgeColumn = ColumnDefinition{};
            detailBadgeColumn.Width(GridLengthHelper::Auto());
            detailRow.ColumnDefinitions().Append(detailBadgeColumn);

            auto detail = TextBlock{};
            bool const anime4K =
                shader.name.size() >= 8 &&
                _wcsnicmp(shader.name.c_str(), L"Anime4K_", 8) == 0;
            bool const managedByAnime4KPreset =
                anime4K && anime4KPresetActive;
            bool const usedByAnime4KPreset =
                managedByAnime4KPreset && shader.enabled;

            if (usedByAnime4KPreset)
            {
                detail.Text(T(L"Anime4K • usado pelo preset"));
                detail.Opacity(0.72);
            }
            else if (managedByAnime4KPreset)
            {
                detail.Text(T(L"Anime4K • não usado neste preset"));
                detail.Opacity(0.46);
            }
            else
            {
                detail.Text(anime4K
                    ? T(L"Anime4K • GLSL importado")
                    : T(L"GLSL personalizado"));
                detail.Opacity(0.52);
            }
            detail.FontSize(10);
            detail.TextTrimming(TextTrimming::CharacterEllipsis);
            detail.MaxLines(1);
            detail.VerticalAlignment(VerticalAlignment::Center);
            // The description deliberately yields space to the preset badge.
            // Keep the full text discoverable when CharacterEllipsis is visible.
            ToolTipService::SetToolTip(
                detail, winrt::box_value(detail.Text()));
            Grid::SetColumn(detail, 0);
            detailRow.Children().Append(detail);

            if (usedByAnime4KPreset)
            {
                auto presetBadge = Border{};
                presetBadge.Style(technicalBadgeStyle);
                // Keep the approved pill geometry. The top padding is already at zero,
                // so create one extra pixel of clearance from descenders (for example,
                // the final "g" in shader names) by nudging the pill down visually.
                // The negative bottom margin keeps the row's reserved height unchanged.
                presetBadge.Padding({ 7, 0, 7, 1 });
                presetBadge.Margin({ 0, 1, 0, -1 });
                presetBadge.VerticalAlignment(VerticalAlignment::Center);

                auto presetText = TextBlock{};
                presetText.Text(anime4KPresetLabel);
                presetText.FontSize(9);
                presetText.Opacity(0.82);
                presetBadge.Child(presetText);
                Grid::SetColumn(presetBadge, 1);
                detailRow.Children().Append(presetBadge);
            }

            nameHost.Children().Append(detailRow);

            layout.Children().Append(nameHost);

            auto enabled = ToggleSwitch{};
            enabled.Tag(winrt::box_value(shader.path));
            enabled.IsOn(shader.enabled);
            enabled.IsEnabled(!managedByAnime4KPreset);
            enabled.VerticalAlignment(VerticalAlignment::Center);
            if (managedByAnime4KPreset)
            {
                ToolTipService::SetToolTip(
                    enabled,
                    winrt::box_value(
                        T(L"Controlado pelo preset Anime4K ativo. Desative o preset para editar manualmente.")));
            }
            enabled.Toggled(
                { this, &SettingsPage::ShaderEnabledToggled });
            Grid::SetColumn(enabled, 1);
            layout.Children().Append(enabled);

            auto makeMoveButton = [&](wchar_t const* glyph,
                bool isEnabled,
                int column,
                auto handler,
                wchar_t const* tooltip)
                {
                    auto button = Button{};
                    button.Tag(winrt::box_value(shader.path));
                    button.Width(30);
                    button.Height(30);
                    button.Padding({ 0, 0, 0, 0 });
                    button.CornerRadius({ 7, 7, 7, 7 });
                    button.IsEnabled(
                        isEnabled && !managedByAnime4KPreset);
                    if (column == 2)
                    {
                        // Give the state label a small visual breath before
                        // the first ordering button without enlarging the card.
                        button.Margin({ 4, 0, 0, 0 });
                    }

                    auto symbol = TextBlock{};
                    symbol.Text(glyph);
                    symbol.FontSize(15);
                    symbol.HorizontalAlignment(
                        HorizontalAlignment::Center);
                    symbol.VerticalAlignment(
                        VerticalAlignment::Center);
                    button.Content(symbol);
                    ToolTipService::SetToolTip(
                        button, winrt::box_value(T(tooltip)));
                    button.Click(handler);
                    Grid::SetColumn(button, column);
                    layout.Children().Append(button);
                };

            makeMoveButton(
                L"\u2191",
                index > 0,
                2,
                RoutedEventHandler{
                    this, &SettingsPage::ShaderMoveUpClicked },
                L"Mover para cima");

            makeMoveButton(
                L"\u2193",
                index + 1 < shaders.size(),
                3,
                RoutedEventHandler{
                    this, &SettingsPage::ShaderMoveDownClicked },
                L"Mover para baixo");

            auto remove = Button{};
            remove.Tag(winrt::box_value(shader.path));
            remove.Width(30);
            remove.Height(30);
            remove.Padding({ 0, 0, 0, 0 });
            remove.CornerRadius({ 7, 7, 7, 7 });
            remove.IsEnabled(!managedByAnime4KPreset);
            if (managedByAnime4KPreset)
            {
                ToolTipService::SetToolTip(
                    remove,
                    winrt::box_value(
                        T(L"Desative o preset Anime4K para remover este shader.")));
            }
            auto deleteIcon = FontIcon{};
            deleteIcon.Glyph(L"\uE74D");
            deleteIcon.FontSize(13);
            remove.Content(deleteIcon);
            if (!managedByAnime4KPreset)
            {
                ToolTipService::SetToolTip(
                    remove, winrt::box_value(T(L"Remover shader")));
            }
            remove.Click(
                { this, &SettingsPage::ShaderRemoveClicked });
            Grid::SetColumn(remove, 4);
            layout.Children().Append(remove);

            card.Child(layout);
            ShaderListPanel().Children().Append(card);
        }

        m_refreshingShaders = false;
        RefreshAnime4KPanel();
        ApplySettingsCardStyle(*this, SelectedCardStyle(*this));
    }

    void SettingsPage::ImportShaderClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        winrt::check_hresult(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.put())));

        std::wstring shaderFilterLabel = T(L"Shader GLSL");
        std::wstring allFilesFilterLabel = T(L"Todos os arquivos");
        COMDLG_FILTERSPEC filters[] = {
            { shaderFilterLabel.c_str(), L"*.glsl" },
            { allFilesFilterLabel.c_str(), L"*.*" }
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetDefaultExtension(L"glsl");
        dialog->SetTitle(T(L"Importar shaders GLSL").c_str());

        FILEOPENDIALOGOPTIONS options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(options |
            FOS_ALLOWMULTISELECT |
            FOS_FILEMUSTEXIST |
            FOS_FORCEFILESYSTEM);

        HWND owner = reinterpret_cast<HWND>(
            PlayerGetMainWindowHandle());
        if (FAILED(dialog->Show(owner)))
        {
            return;
        }

        winrt::com_ptr<IShellItemArray> items;
        winrt::check_hresult(dialog->GetResults(items.put()));

        DWORD count{};
        winrt::check_hresult(items->GetCount(&count));

        uint32_t importedCount{};
        uint32_t failedCount{};
        std::wstring firstError;

        for (DWORD index = 0; index < count; ++index)
        {
            winrt::com_ptr<IShellItem> item;
            if (FAILED(items->GetItemAt(index, item.put())))
            {
                ++failedCount;
                if (firstError.empty())
                {
                    firstError = T(L"Não foi possível ler um dos arquivos selecionados.");
                }
                continue;
            }

            PWSTR rawPath{};
            if (FAILED(item->GetDisplayName(
                SIGDN_FILESYSPATH, &rawPath)))
            {
                ++failedCount;
                if (firstError.empty())
                {
                    firstError = T(L"Não foi possível obter o caminho de um dos shaders.");
                }
                continue;
            }

            std::wstring path{ rawPath };
            CoTaskMemFree(rawPath);

            std::wstring error;
            if (PlayerImportShader(path, error))
            {
                ++importedCount;
            }
            else
            {
                ++failedCount;
                if (firstError.empty())
                {
                    firstError = error;
                }
            }
        }

        if (importedCount > 0)
        {
            RefreshShaderList();
        }

        if (failedCount == 0 && importedCount > 0)
        {
            std::wostringstream message;
            message << importedCount
                << T(L" shader(s) copiado(s) para o HC Player. Todos permanecem desativados até você ativá-los.");
            SetShaderStatus(
                true,
                importedCount == 1
                    ? T(L"Shader importado")
                    : T(L"Shaders importados"),
                message.str());
        }
        else if (importedCount > 0)
        {
            std::wostringstream message;
            message << importedCount << T(L" importado(s), ")
                << failedCount << T(L" não importado(s).");
            if (!firstError.empty())
            {
                message << L" " << firstError;
            }

            SetShaderStatus(
                false,
                T(L"Importação parcial"),
                message.str());
            ShaderStatus().Severity(
                Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
        }
        else if (failedCount > 0)
        {
            SetShaderStatus(
                false,
                T(L"Não foi possível importar os shaders"),
                firstError.empty()
                    ? T(L"Nenhum arquivo selecionado pôde ser importado.")
                    : firstError);
        }
    }

    void SettingsPage::RemoveAllShadersClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (PlayerGetShaders().empty())
        {
            return;
        }
        ConfirmRemoveAllShadersAsync();
    }

    winrt::fire_and_forget SettingsPage::ConfirmRemoveAllShadersAsync()
    {
        auto lifetime = get_strong();
        auto shaders = PlayerGetShaders();
        if (shaders.empty())
        {
            co_return;
        }

        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;

        ContentDialog dialog{};
        dialog.XamlRoot(SettingsRoot().XamlRoot());
        dialog.Title(winrt::box_value(T(L"Remover todos os shaders?")));

        auto message = TextBlock{};
        std::wostringstream text;
        text << T(L"Os ") << shaders.size()
            << T(L" shader(s) importado(s) serão removidos do HC Player. ")
            << T(L"Shaders configurados externamente no mpv.conf não serão alterados.");
        message.Text(text.str());
        message.TextWrapping(TextWrapping::Wrap);
        dialog.Content(message);

        dialog.PrimaryButtonText(winrt::hstring{ T(L"Remover todos") });
        dialog.CloseButtonText(winrt::hstring{ T(L"Cancelar") });
        dialog.DefaultButton(ContentDialogButton::Close);

        ContentDialogResult const result = co_await dialog.ShowAsync();
        if (result != ContentDialogResult::Primary)
        {
            co_return;
        }

        std::wstring error;
        if (PlayerRemoveAllShaders(error))
        {
            RefreshShaderList();
            SetShaderStatus(
                true,
                T(L"Shaders removidos"),
                error.empty()
                    ? T(L"Todos os shaders gerenciados pelo HC Player foram removidos da lista, do pipeline e da pasta privada.")
                    : error);
        }
        else
        {
            RefreshShaderList();
            SetShaderStatus(
                false,
                T(L"Não foi possível remover todos os shaders"),
                error);
        }
    }

    void SettingsPage::ShaderEnabledToggled(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_refreshingShaders)
        {
            return;
        }

        auto toggle =
            sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>();
        std::wstring path = winrt::unbox_value_or<winrt::hstring>(
            toggle.Tag(), winrt::hstring{}).c_str();
        if (path.empty())
        {
            return;
        }

        std::wstring error;
        bool enabled = toggle.IsOn();
        if (PlayerSetShaderEnabled(path, enabled, error))
        {
            SetShaderStatus(
                true,
                enabled ? T(L"Shader ativado") : T(L"Shader desativado"),
                enabled
                    ? T(L"O shader foi adicionado ao pipeline de vídeo.")
                    : T(L"O shader foi removido do pipeline de vídeo."));
            RefreshAnime4KPanel();
        }
        else
        {
            SetShaderStatus(
                false,
                T(L"Não foi possível alterar o shader"),
                error);
            RefreshShaderList();
        }
    }

    void SettingsPage::ShaderMoveUpClicked(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto element =
            sender.as<Microsoft::UI::Xaml::FrameworkElement>();
        std::wstring path = winrt::unbox_value_or<winrt::hstring>(
            element.Tag(), winrt::hstring{}).c_str();

        std::wstring error;
        if (PlayerMoveShader(path, -1, error))
        {
            RefreshShaderList();
            ShaderStatus().IsOpen(false);
        }
        else
        {
            SetShaderStatus(
                false,
                T(L"Não foi possível reordenar os shaders"),
                error);
        }
    }

    void SettingsPage::ShaderMoveDownClicked(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto element =
            sender.as<Microsoft::UI::Xaml::FrameworkElement>();
        std::wstring path = winrt::unbox_value_or<winrt::hstring>(
            element.Tag(), winrt::hstring{}).c_str();

        std::wstring error;
        if (PlayerMoveShader(path, 1, error))
        {
            RefreshShaderList();
            ShaderStatus().IsOpen(false);
        }
        else
        {
            SetShaderStatus(
                false,
                T(L"Não foi possível reordenar os shaders"),
                error);
        }
    }

    void SettingsPage::ShaderRemoveClicked(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto element =
            sender.as<Microsoft::UI::Xaml::FrameworkElement>();
        std::wstring path = winrt::unbox_value_or<winrt::hstring>(
            element.Tag(), winrt::hstring{}).c_str();

        std::wstring error;
        if (PlayerRemoveShader(path, error))
        {
            RefreshShaderList();
            SetShaderStatus(
                true,
                T(L"Shader removido"),
                error.empty()
                    ? T(L"O shader foi removido da lista e da pasta privada do HC Player.")
                    : error);
        }
        else
        {
            SetShaderStatus(
                false,
                T(L"Não foi possível remover o shader"),
                error);
        }
    }

    void SettingsPage::ResetAllSettingsClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ConfirmResetAllSettingsAsync();
    }

    winrt::fire_and_forget SettingsPage::ConfirmResetAllSettingsAsync()
    {
        auto lifetime = get_strong();
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;

        ContentDialog dialog{};
        dialog.XamlRoot(SettingsRoot().XamlRoot());
        dialog.Title(winrt::box_value(SettingsResource(
            L"SettingsResetDialogTitle",
            L"Redefinir configurações?")));

        auto message = TextBlock{};
        message.Text(SettingsResource(
            L"SettingsResetDialogMessage",
            L"As configurações de reprodução, vídeo, áudio, legendas, interface, streaming e captura voltarão aos valores padrão. O tema atual, fontes, shaders, badges, yt-dlp/Deno, capturas e arquivos de mídia serão preservados. Shaders importados permanecerão instalados, mas serão desativados. Alterações ainda não salvas serão descartadas."));
        message.TextWrapping(TextWrapping::Wrap);
        dialog.Content(message);

        dialog.PrimaryButtonText(winrt::hstring{ SettingsResource(
            L"SettingsResetDialogPrimary",
            L"Redefinir") });
        dialog.CloseButtonText(winrt::hstring{ SettingsResource(
            L"SettingsResetDialogCancel",
            L"Cancelar") });
        dialog.DefaultButton(ContentDialogButton::Close);

        ContentDialogResult const result = co_await dialog.ShowAsync();
        if (result != ContentDialogResult::Primary)
        {
            co_return;
        }

        std::wstring error;
        bool const reset = PlayerResetAllSettingsToDefaults(error);
        ResetAllSettingsStatus().IsOpen(true);
        ResetAllSettingsStatus().Severity(reset
            ? InfoBarSeverity::Success
            : InfoBarSeverity::Error);
        ResetAllSettingsStatus().Title(reset
            ? SettingsResource(
                L"SettingsResetSuccessTitle",
                L"Configurações redefinidas")
            : SettingsResource(
                L"SettingsResetFailedTitle",
                L"Não foi possível redefinir"));
        ResetAllSettingsStatus().Message(reset
            ? SettingsResource(
                L"SettingsResetSuccessMessage",
                L"Os valores padrão serão usados na próxima inicialização do HC Player. Reinicie o aplicativo para concluir.")
            : SettingsResource(
                L"SettingsResetFailedMessage",
                L"Não foi possível concluir a redefinição das configurações."));

        if (reset)
        {
            // A pending edit must never be able to repopulate the just-cleared
            // settings file if the user presses Save before restarting.
            m_pendingOptions.clear();
            SaveButton().IsEnabled(false);

            ImportedConfigPanel().Children().Clear();
            ImportedConfigPanel().Visibility(Visibility::Collapsed);
            RefreshShaderList();
        }
    }

    void SettingsPage::UpdateYtdlpStatus()
    {
        auto status = PlayerGetYtdlpStatus();
        if (status.available)
        {
            YtdlpStatusTitle().Text(status.imported
                ? T(L"yt-dlp importado e pronto")
                : T(L"yt-dlp detectado no PATH do Windows"));
            YtdlpStatusPath().Text(status.path);
            YtdlpStatusIcon().Glyph(L"\uE73E");
        }
        else
        {
            YtdlpStatusTitle().Text(T(L"yt-dlp não encontrado"));
            YtdlpStatusPath().Text(
                T(L"Selecione o yt-dlp.exe ou adicione-o ao PATH do Windows"));
            YtdlpStatusIcon().Glyph(L"\uE783");
        }
        YtdlpResetButton().Visibility(status.imported
            ? Microsoft::UI::Xaml::Visibility::Visible
            : Microsoft::UI::Xaml::Visibility::Collapsed);

        if (status.jsRuntimeAvailable)
        {
            DenoStatusTitle().Text(status.jsRuntimeImported
                ? T(L"Deno importado e pronto")
                : T(L"Deno detectado no PATH do Windows"));
            DenoStatusPath().Text(status.jsRuntimePath);
            DenoStatusIcon().Glyph(L"\uE73E");
        }
        else if (status.jsRuntimeInvalid)
        {
            DenoStatusTitle().Text(T(L"Deno inválido ou corrompido"));
            DenoStatusPath().Text(status.jsRuntimePath +
                T(L" — selecione o deno.exe oficial; 'deno --version' precisa funcionar"));
            DenoStatusIcon().Glyph(L"\uE783");
        }
        else
        {
            DenoStatusTitle().Text(T(L"Deno recomendado para o YouTube"));
            DenoStatusPath().Text(
                T(L"Opcional: selecione deno.exe 2.3 ou mais recente para suporte completo"));
            DenoStatusIcon().Glyph(L"\uE783");
        }
        DenoResetButton().Visibility(status.jsRuntimeImported
            ? Microsoft::UI::Xaml::Visibility::Visible
            : Microsoft::UI::Xaml::Visibility::Collapsed);
    }

    void SettingsPage::ImportYtdlpClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        winrt::check_hresult(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.put())));

        std::wstring executableFilterLabel = T(L"Executável do yt-dlp");
        std::wstring allFilesFilterLabel = T(L"Todos os arquivos");
        COMDLG_FILTERSPEC filters[] = {
            { executableFilterLabel.c_str(), L"*.exe" },
            { allFilesFilterLabel.c_str(), L"*.*" }
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetTitle(T(L"Selecionar o executável do yt-dlp").c_str());
        dialog->SetFileName(L"yt-dlp.exe");

        if (SUCCEEDED(dialog->Show(nullptr)))
        {
            winrt::com_ptr<IShellItem> item;
            winrt::check_hresult(dialog->GetResult(item.put()));
            PWSTR rawPath{};
            winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
            std::wstring path{ rawPath };
            CoTaskMemFree(rawPath);

            std::wstring error;
            bool imported = PlayerImportYtdlpBinary(path, error);
            UpdateYtdlpStatus();
            SaveStatus().IsOpen(true);
            SaveStatus().Title(imported
                ? T(L"yt-dlp pronto para uso")
                : T(L"Não foi possível importar o yt-dlp"));
            SaveStatus().Message(imported
                ? T(L"O executável foi armazenado e o mecanismo de reprodução foi atualizado.")
                : LocalizeSettingsMessage(error));
            SaveStatus().Severity(imported
                ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success
                : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        }
    }

    void SettingsPage::ResetYtdlpClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        std::wstring error;
        bool removed = PlayerResetImportedYtdlp(error);
        UpdateYtdlpStatus();
        SaveStatus().IsOpen(true);
        SaveStatus().Title(removed
            ? T(L"Executável importado removido")
            : T(L"Não foi possível remover o yt-dlp"));
        SaveStatus().Message(removed
            ? T(L"O player voltará a usar automaticamente o yt-dlp disponível no PATH, se houver.")
            : LocalizeSettingsMessage(error));
        SaveStatus().Severity(removed
            ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success
            : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
    }

    void SettingsPage::ImportDenoClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        winrt::check_hresult(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.put())));

        std::wstring executableFilterLabel = T(L"Executável do Deno");
        std::wstring allFilesFilterLabel = T(L"Todos os arquivos");
        COMDLG_FILTERSPEC filters[] = {
            { executableFilterLabel.c_str(), L"*.exe" },
            { allFilesFilterLabel.c_str(), L"*.*" }
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetTitle(T(L"Selecionar o runtime JavaScript Deno").c_str());
        dialog->SetFileName(L"deno.exe");

        if (SUCCEEDED(dialog->Show(nullptr)))
        {
            winrt::com_ptr<IShellItem> item;
            winrt::check_hresult(dialog->GetResult(item.put()));
            PWSTR rawPath{};
            winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
            std::wstring path{ rawPath };
            CoTaskMemFree(rawPath);

            std::wstring error;
            bool imported = PlayerImportDenoBinary(path, error);
            UpdateYtdlpStatus();
            SaveStatus().IsOpen(true);
            SaveStatus().Title(imported
                ? T(L"Runtime JavaScript pronto")
                : T(L"Não foi possível importar o Deno"));
            SaveStatus().Message(imported
                ? T(L"O Deno foi armazenado pelo player; links do YouTube agora podem ser resolvidos pelo yt-dlp.")
                : LocalizeSettingsMessage(error));
            SaveStatus().Severity(imported
                ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success
                : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        }
    }

    void SettingsPage::ResetDenoClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        std::wstring error;
        bool removed = PlayerResetImportedDeno(error);
        UpdateYtdlpStatus();
        SaveStatus().IsOpen(true);
        SaveStatus().Title(removed
            ? T(L"Runtime JavaScript importado removido")
            : T(L"Não foi possível remover o Deno"));
        SaveStatus().Message(removed
            ? T(L"O player voltará a usar automaticamente o Deno disponível no PATH, se houver.")
            : LocalizeSettingsMessage(error));
        SaveStatus().Severity(removed
            ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success
            : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
    }

    bool SettingsPage::UpdateImportedValue(
        std::wstring const& section,
        std::wstring const& name,
        std::wstring const& value,
        bool profile)
    {
        bool updated = PlayerUpdateImportedOption(section, name, value, profile);
        ConfigImportStatus().IsOpen(true);
        ConfigImportStatus().Title(updated ? T(L"Opção atualizada") : T(L"Valor inválido"));
        ConfigImportStatus().Message(updated
            ? name + L" = " + value + T(L" foi validado e salvo.")
            : T(L"O mecanismo de reprodução rejeitou o valor informado para ") + name + L".");
        ConfigImportStatus().Severity(updated
            ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success
            : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return updated;
    }

    void SettingsPage::RenderImportedConfig(ImportedMpvConfig const& config)
    {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;

        m_importedConfigRendered = true;

        ImportedConfigPanel().Children().Clear();
        ConfigImportStatus().IsOpen(true);
        ConfigImportStatus().Title(config.success
            ? T(L"Configuração importada")
            : T(L"Não foi possível importar"));
        ConfigImportStatus().Message(LocalizeSettingsMessage(config.message));
        ConfigImportStatus().Severity(config.success
            ? (config.invalidCount == 0 ? InfoBarSeverity::Success : InfoBarSeverity::Warning)
            : InfoBarSeverity::Error);

        UpdateBuiltInOptionVisibility(config.success ? &config : nullptr);

        std::vector<ImportedMpvOption> visibleOptions;
        for (auto const& option : config.options)
        {
            if (option.builtIn) continue;

            // Keep the ORIGINAL section stored by the importer.
            // "OUTRAS OPÇÕES" is only a presentation label. Replacing
            // option.section with that label loses the key that
            // MpvSettingsManager::UpdateImportedOption() needs in order to
            // find and save a global imported option.
            visibleOptions.push_back(option);
        }
        std::stable_sort(visibleOptions.begin(), visibleOptions.end(),
            [](ImportedMpvOption const& left, ImportedMpvOption const& right)
            {
                if (left.profile != right.profile) return !left.profile;

                // All non-profile options are displayed together as
                // "OUTRAS OPÇÕES"; stable_sort preserves their import order.
                if (!left.profile) return false;

                if (left.section != right.section) return left.section < right.section;
                return false;
            });

        if (!config.success || visibleOptions.empty())
        {
            ImportedConfigPanel().Visibility(Visibility::Collapsed);
            return;
        }

        ImportedConfigPanel().Visibility(Visibility::Visible);

        size_t index{};
        while (index < visibleOptions.size())
        {
            bool profile = visibleOptions[index].profile;
            std::wstring sourceSection = visibleOptions[index].section;
            std::wstring displaySection =
                profile ? sourceSection : T(L"OUTRAS OPÇÕES");

            size_t end = index;
            while (end < visibleOptions.size())
            {
                if (visibleOptions[end].profile != profile) break;
                if (profile && visibleOptions[end].section != sourceSection) break;
                ++end;
            }

            auto header = StackPanel{};
            header.Orientation(Orientation::Horizontal);
            header.Spacing(8);

            auto sectionName = TextBlock{};
            sectionName.Text(displaySection + (profile ? T(L"  • perfil") : L""));
            sectionName.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            sectionName.FontSize(13);
            sectionName.VerticalAlignment(VerticalAlignment::Center);
            header.Children().Append(sectionName);

            auto count = TextBlock{};
            count.Text(std::to_wstring(end - index) + T(L" opções"));
            count.FontSize(11);
            count.Opacity(0.68);
            count.VerticalAlignment(VerticalAlignment::Center);

            auto countBadge = Border{};
            countBadge.Style(Resources().Lookup(winrt::box_value(L"ImportedCountBadge"))
                .as<Microsoft::UI::Xaml::Style>());
            countBadge.Child(count);
            header.Children().Append(countBadge);

            auto optionList = StackPanel{};
            optionList.Spacing(4);

            for (size_t optionIndex = index; optionIndex < end; ++optionIndex)
            {
                auto const& option = visibleOptions[optionIndex];
                std::wstring const optionSourceSection = option.section;
                auto row = Grid{};
                row.Padding({ 10, 7, 10, 7 });
                row.ColumnSpacing(10);

                auto columns = row.ColumnDefinitions();
                auto nameColumn = ColumnDefinition{};
                nameColumn.Width(GridLengthHelper::FromPixels(145));
                columns.Append(nameColumn);
                auto valueColumn = ColumnDefinition{};
                valueColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                columns.Append(valueColumn);

                auto name = TextBlock{};
                name.Text(option.name);
                name.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Consolas" });
                name.FontSize(12);
                name.TextWrapping(TextWrapping::Wrap);
                row.Children().Append(name);

                auto choices = ChoicesForOption(option.name);
                if (!choices.empty())
                {
                    auto editor = ComboBox{};
                    editor.HorizontalAlignment(HorizontalAlignment::Stretch);
                    for (auto const& choice : choices)
                    {
                        editor.Items().Append(winrt::box_value(choice));
                    }
                    auto selected = std::find(choices.begin(), choices.end(), option.value);
                    if (selected != choices.end())
                    {
                        editor.SelectedIndex(static_cast<int32_t>(selected - choices.begin()));
                    }
                    editor.SelectionChanged([this, optionSourceSection, name = option.name, profile, choices](
                        Windows::Foundation::IInspectable const& sender,
                        SelectionChangedEventArgs const&)
                        {
                            auto combo = sender.as<ComboBox>();
                            int32_t selectedIndex = combo.SelectedIndex();
                            if (selectedIndex >= 0 && selectedIndex < static_cast<int32_t>(choices.size()))
                            {
                                UpdateImportedValue(optionSourceSection, name, choices[selectedIndex], profile);
                            }
                        });
                    Grid::SetColumn(editor, 1);
                    row.Children().Append(editor);
                }
                else if (option.value == L"yes" || option.value == L"no")
                {
                    auto editor = ToggleSwitch{};
                    editor.IsOn(option.value == L"yes");
                    editor.OnContent(winrt::box_value(L"yes"));
                    editor.OffContent(winrt::box_value(L"no"));
                    editor.Toggled([this, optionSourceSection, name = option.name, profile](
                        Windows::Foundation::IInspectable const& sender,
                        RoutedEventArgs const&)
                        {
                            auto toggle = sender.as<ToggleSwitch>();
                            UpdateImportedValue(optionSourceSection, name, toggle.IsOn() ? L"yes" : L"no", profile);
                        });
                    Grid::SetColumn(editor, 1);
                    row.Children().Append(editor);
                }
                else
                {
                    auto editor = TextBox{};
                    editor.Text(option.value);
                    editor.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Consolas" });
                    editor.FontSize(12);
                    editor.HorizontalAlignment(HorizontalAlignment::Stretch);

                    auto committedValue =
                        std::make_shared<std::wstring>(option.value);

                    editor.LostFocus([this, optionSourceSection,
                        name = option.name, profile, committedValue](
                        Windows::Foundation::IInspectable const& sender,
                        RoutedEventArgs const&)
                        {
                            auto text = sender.as<TextBox>();
                            std::wstring currentValue = text.Text().c_str();

                            // A focus change is not an edit. Do not validate,
                            // save or show a status message when nothing changed.
                            if (currentValue == *committedValue) return;

                            if (UpdateImportedValue(
                                optionSourceSection,
                                name,
                                currentValue,
                                profile))
                            {
                                *committedValue = std::move(currentValue);
                            }
                        });
                    Grid::SetColumn(editor, 1);
                    row.Children().Append(editor);
                }

                optionList.Children().Append(row);
            }

            auto expander = Expander{};
            expander.Header(header);
            expander.Content(optionList);
            expander.HorizontalAlignment(HorizontalAlignment::Stretch);
            expander.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            ImportedConfigPanel().Children().Append(expander);

            index = end;
        }
    }

}
