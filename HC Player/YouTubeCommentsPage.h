#pragma once

#include "YouTubeCommentsPage.g.h"
#include "PlayerBridge.h"

#include <cstdint>
#include <string>

namespace winrt::HCPlayer::implementation
{
    struct YouTubeCommentsPage : YouTubeCommentsPageT<YouTubeCommentsPage>
    {
        YouTubeCommentsPage();

        void PrepareForOpen();
        void PrepareForClose();
        void BeginOpenAnimation();
        void RequestClose();
        void ScrollBy(int wheelDelta);

        void CommentsLoaded(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void RefreshClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void LoadMoreClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void SortSelectionChanged(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        void CloseClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        winrt::fire_and_forget DetectModeAndLoadAsync();
        winrt::fire_and_forget LoadCommentsAsync(bool append);
        winrt::fire_and_forget PollLiveChatAsync(
            std::uint64_t generation,
            std::wstring liveChatId);
        void SetCommentsModeUi();
        void SetLiveChatModeUi();
        void ShowStatus(std::wstring const& message);
        void ShowLoading(bool loading, bool append);
        Microsoft::UI::Xaml::Controls::Border CreateCommentCard(
            std::wstring const& commentId,
            std::wstring const& author,
            std::wstring const& avatarUrl,
            std::wstring const& text,
            int64_t likeCount,
            int64_t replyCount,
            std::wstring const& publishedAt,
            std::wstring const& updatedAt);
        Microsoft::UI::Xaml::Controls::Border CreateLiveChatCard(
            std::wstring const& author,
            std::wstring const& avatarUrl,
            std::wstring const& text,
            std::wstring const& amount,
            bool isOwner,
            bool isModerator,
            bool isMember);
        Microsoft::UI::Xaml::Controls::Grid CreateReplyRow(
            std::wstring const& author,
            std::wstring const& avatarUrl,
            std::wstring const& text,
            int64_t likeCount,
            std::wstring const& publishedAt,
            std::wstring const& updatedAt);
        winrt::fire_and_forget LoadRepliesAsync(
            std::wstring parentCommentId,
            Microsoft::UI::Xaml::Controls::StackPanel repliesHost,
            Microsoft::UI::Xaml::Controls::Button toggleButton,
            int64_t replyCount);
        winrt::fire_and_forget LoadMoreRepliesAsync(
            std::wstring parentCommentId,
            Microsoft::UI::Xaml::Controls::StackPanel repliesHost,
            Microsoft::UI::Xaml::Controls::Button moreButton);

        bool m_loaded{};
        bool m_loading{};
        bool m_closing{};
        std::uint64_t m_requestGeneration{};
        std::wstring m_videoId;
        std::wstring m_nextPageToken;
        std::wstring m_order{ L"relevance" };
        bool m_liveMode{};
        bool m_reuseNormalState{};
        std::uint32_t m_replyRequestsInFlight{};
        std::wstring m_liveChatId;
        std::wstring m_liveNextPageToken;
    };
}

namespace winrt::HCPlayer::factory_implementation
{
    struct YouTubeCommentsPage :
        YouTubeCommentsPageT<YouTubeCommentsPage, implementation::YouTubeCommentsPage>
    {
    };
}
