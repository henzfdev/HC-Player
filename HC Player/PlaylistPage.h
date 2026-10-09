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

        winrt::fire_and_forget AddUrlClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void AddFolderClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void ClearQueueClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void RemoveSelectedClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void PreviousWindowClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void NextWindowClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void PlaylistKeyDown(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);

        void CloseClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        void RefreshList();
        void CancelReorderDrag();
        int CalculateDropSlot(double pointerY);
        void ShowDropSlot(int slot);
        bool CommitReorderDrag();
        bool IsItemSelected(int64_t index, std::wstring const& filename) const;
        void ToggleItemSelection(int64_t index, std::wstring const& filename);
        void ClearItemSelection();
        void PruneItemSelection(std::vector<MediaPlaylistItem> const& playlist);
        bool RemoveSelectedItems();
        void RefreshTimerTick(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);
        Microsoft::UI::Xaml::Controls::Grid CreateItemButton(
            MediaPlaylistItem const& item,
            int displayIndex,
            bool paused,
            bool eofReached);

        struct SelectedItem
        {
            int64_t index{ -1 };
            std::wstring filename;
        };

        Microsoft::UI::Xaml::DispatcherTimer m_refreshTimer{ nullptr };
        Microsoft::UI::Xaml::Controls::MenuFlyout m_selectionContextMenu{ nullptr };
        Microsoft::UI::Xaml::Controls::MenuFlyoutItem m_selectionContextRemoveItem{ nullptr };
        std::vector<SelectedItem> m_selectedItems;
        std::vector<Microsoft::UI::Xaml::Controls::Border> m_dropTopIndicators;
        std::vector<Microsoft::UI::Xaml::Controls::Border> m_dropBottomIndicators;
        std::vector<std::wstring> m_dragSnapshotFilenames;
        std::wstring m_dragSourceFilename;
        int64_t m_dragSourceIndex{ -1 };
        int m_dragDropSlot{ -1 };
        bool m_reorderDragging{};
        size_t m_visualWindowStart{};
        int64_t m_lastCurrentIndex{ -1 };
        bool m_showLastWindowOnNextRefresh{};
        std::wstring m_lastSignature;
        bool m_hasSnapshot{};
        int m_webLimitNoticeTicksRemaining{};
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
