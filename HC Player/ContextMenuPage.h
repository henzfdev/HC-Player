#pragma once

#include "ContextMenuPage.g.h"
#include "PlayerBridge.h"

namespace winrt::HCPlayer::implementation
{
    struct ContextMenuPage : ContextMenuPageT<ContextMenuPage>
    {
        ContextMenuPage();

        void ShowAt(double x, double y);
        void MenuItemClicked(
            Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MenuClosed(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);

    private:
        void RefreshProfilesMenu();
        void RefreshRecentMenu();
        void RefreshTracksMenus();
        void RefreshChaptersMenu();
        void RefreshEditionsMenu();
        void RefreshPlaylistMenu();
        void RefreshSpeedMenu();
        void ResetTopLevelMenuVisibility();
        void ApplyCompactMenuVisibility(bool compact);
        void FillTrackMenu(
            Microsoft::UI::Xaml::Controls::MenuFlyoutSubItem const& menu,
            std::vector<MediaTrackOption> const& tracks,
            std::wstring const& type,
            std::wstring const& property,
            int selectionIndex,
            bool allowDisabled);
        bool m_openSettingsAfterClose{};
        bool m_openMediaInfoAfterClose{};
        std::vector<std::wstring> m_renderedProfiles;
        std::wstring m_renderedActiveProfile;
    };
}

namespace winrt::HCPlayer::factory_implementation
{
    struct ContextMenuPage : ContextMenuPageT<ContextMenuPage, implementation::ContextMenuPage>
    {
    };
}
