#pragma once

#include "MediaInfoPage.g.h"
#include "PlayerBridge.h"
#include "MediaInfoBridge.h"

namespace winrt::HCPlayer::implementation
{
    struct MediaInfoPage : MediaInfoPageT<MediaInfoPage>
    {
        MediaInfoPage();

        void PrepareForOpen();
        void BeginOpenAnimation();
        void RequestClose();
        void ScrollBy(int wheelDelta);

        void MediaInfoLoaded(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void CopyAllClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void CloseClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        winrt::fire_and_forget RefreshAsync();
        void ShowLoading();
        void ShowAnalysis(MediaInfoBridge::Analysis const& analysis);
        void ShowError(std::wstring const& message);

        std::wstring m_copyText;
        bool m_loaded{};
        bool m_closing{};
        uint64_t m_requestGeneration{};
    };
}

namespace winrt::HCPlayer::factory_implementation
{
    struct MediaInfoPage :
        MediaInfoPageT<MediaInfoPage, implementation::MediaInfoPage>
    {
    };
}
