#pragma once

#include "SettingsPage.g.h"
#include "PlayerBridge.h"
#include <map>

namespace winrt::HCPlayer::implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage>
    {
        SettingsPage();

        void ScrollBy(int wheelDelta);
        void ImportPath(std::wstring const& path);
        void RefreshImportedConfig();
        void PrepareForOpen();
        void BeginOpenAnimation();
        void SyncAlwaysOnTopState(bool enabled);
        void SyncLiveRuntimeStates();

        void SettingsLoaded(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void CloseClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void SaveClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ThemeClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void SettingsTabClicked(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void AlwaysOnTopToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void InterfaceVisibilityToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void TimelineStyleChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void PreciseSeekingToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void KeepOpenToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ChapterHoverCardToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void DebandingToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void AutoSubtitlesToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ImportFontFolderClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ImportMediaBadgeSetClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ResetMediaBadgeSetClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ImportMediaBadgeIndividualClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void RefreshMediaBadgeSetStatus();

        void SubtitleFontSelectionChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void ScalingQualityChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void DownscaleFilterChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void ChromaFilterChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void ToneMappingChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void TargetColorspaceHintChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void DitheringChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void VSyncToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void DisplayResampleToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void InterpolationToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void UpdateTemporalScalerAvailability();

        void Anime4KProfileChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void Anime4KModeChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void Anime4KEnabledToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ImportShaderClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void RemoveAllShadersClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ShaderEnabledToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ShaderMoveUpClicked(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ShaderMoveDownClicked(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ShaderRemoveClicked(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void NativeSubtitleToggleToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void NativeSubtitleChoiceChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void AudioDeviceChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void NativeSubtitleTextLostFocus(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void NativeSubtitleNumericTextChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);

        void TemporalScalerChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void BlendSubtitlesChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void ScreenshotFormatChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void ScreenshotHighBitDepthToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ScreenshotPngCompressionChanged(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void ChooseScreenshotDirectoryClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ScreenshotTemplateLostFocus(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void HardwareDecodingToggled(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void SubtitleSizeChanged(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);

        void ImportConfigClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ResetConfigClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ResetAllSettingsClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        winrt::fire_and_forget ConfirmResetAllSettingsAsync();

        void ImportYtdlpClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ResetYtdlpClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ImportDenoClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ResetDenoClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OpenDefaultAppsClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OpenGitHubClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void CopyAboutInfoClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        bool m_ready{};
        bool m_closing{};
        bool m_nativeAdvancedPopulated{};
        bool m_refreshingShaders{};
        bool m_refreshingAnime4K{};
        bool m_importedConfigRendered{};
        // m_fontNames stores the technical name sent to mpv/libass; the
        // parallel display/alias vectors keep imported faces distinct in UI.
        std::vector<std::wstring> m_fontNames;
        std::vector<std::wstring> m_fontDisplayNames;
        std::vector<std::wstring> m_fontFamilyAliases;
        std::vector<std::wstring> m_systemFontNames;
        std::vector<std::wstring> m_audioDeviceNames;
        std::map<std::wstring, std::wstring> m_pendingOptions;
        std::map<std::wstring, Microsoft::UI::Xaml::Controls::ToggleSwitch>
            m_nativeAdvancedToggles;
        Microsoft::UI::Xaml::Controls::ComboBox m_hardwareDecodingChoice{ nullptr };
        void SelectSettingsTab(std::wstring const& tab);
        void StageOption(std::wstring const& name, std::wstring const& value);
        bool PlayerSetMpvOption(std::wstring const& name, std::wstring const& value);
        void RenderImportedConfig(ImportedMpvConfig const& config);
        void PopulateScalerChoices();
        void PopulateNativeAdvancedOptions();
        void RefreshShaderList();
        void RefreshAnime4KPanel();
        winrt::fire_and_forget ConfirmRemoveAllShadersAsync();
        bool ApplySelectedAnime4KMode();
        void SetShaderStatus(
            bool success,
            std::wstring const& title,
            std::wstring const& message);
        void UpdateYtdlpStatus();
        void RestoreSavedControls(Microsoft::UI::Xaml::DependencyObject const& root);
        void UpdateBuiltInOptionVisibility(ImportedMpvConfig const* config);
        void UpdateThemeButton();
        void RefreshAboutInfo();
        bool UpdateImportedValue(
            std::wstring const& section,
            std::wstring const& name,
            std::wstring const& value,
            bool profile);

    };
}

namespace winrt::HCPlayer::factory_implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage, implementation::SettingsPage>
    {
    };
}
