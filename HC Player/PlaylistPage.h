#pragma once

#include "PlaylistPage.g.h"
#include "PlayerBridge.h"

#include <string>
#include <vector>

namespace winrt::HCPlayer::implementation
{
    struct PlaylistPage : PlaylistPageT<PlaylistPage>
    {
        PlaylistPage();

        void PrepareForOpen();
        void PrepareForClose();
        void BeginOpenAnimation();
        void RequestClose();
        void ScrollBy(int wheelDelta);
        void SetExternalDropActive(bool active);
        void CompleteExternalDrop(bool queueChanged);

        void PlaylistLoaded(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void AddClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void AddFolderClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ClearQueueClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void CloseClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        void RefreshList();
        void CancelReorderDrag();
        int CalculateDropSlot(double pointerY);
        void ShowDropSlot(int slot);
        bool CommitReorderDrag();
        void RefreshTimerTick(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);
        Microsoft::UI::Xaml::Controls::Grid CreateItemButton(
            MediaPlaylistItem const& item,
            int displayIndex,
            bool paused,
            bool eofReached);

        Microsoft::UI::Xaml::DispatcherTimer m_refreshTimer{ nullptr };
        std::vector<Microsoft::UI::Xaml::Controls::Border> m_dropTopIndicators;
        std::vector<Microsoft::UI::Xaml::Controls::Border> m_dropBottomIndicators;
        std::vector<std::wstring> m_dragSnapshotFilenames;
        std::wstring m_dragSourceFilename;
        int64_t m_dragSourceIndex{ -1 };
        int m_dragDropSlot{ -1 };
        bool m_reorderDragging{};
        std::wstring m_lastSignature;
        bool m_hasSnapshot{};
        bool m_closing{};
    };
}

namespace winrt::HCPlayer::factory_implementation
{
    struct PlaylistPage :
        PlaylistPageT<PlaylistPage, implementation::PlaylistPage>
    {
    };
}
