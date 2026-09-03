#pragma once

#include "MainPage.g.h"
#include "PlayerBridge.h"
#include "ThumbnailController.h"

#include <memory>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

namespace winrt::HCPlayer::implementation
{
    struct MainPage : MainPageT<MainPage>
    {
        MainPage();

        void MainPageLoaded(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        bool OpenPath(std::wstring const& path);
        void RestorePlayerState(std::wstring const& path);
        void ClearPlayerState();
        void SetSettingsOverlayOpen(bool open);
        void PrepareSilentFullscreenEntry();
        void SetPictureInPictureMode(bool enabled);
        void TransportHostPointerEntered();
        void TransportHostPointerExited();
        void TransportVideoPointerMoved(bool overControls);
        void ShowVolumeFeedback();
        void RefreshThemeVisuals();
        void RefreshInterfacePreferences();
        bool UsesMinimalTransportStyle() const noexcept;
        Windows::Foundation::Rect MinimalTransportRegion();

        void OpenFromDialog();

        void OpenClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OpenFolderClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void PlayClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void SeekBackwardClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SeekForwardClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PreviousChapterClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void NextChapterClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void LoopClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ShuffleClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MediaInfoClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void SpeedFlyoutOpening(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);
        void TracksFlyoutOpening(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);
        void ProfilesFlyoutOpening(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);
        void MinimalMoreFlyoutOpening(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);
        void TransportFlyoutOpened(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);
        void TransportFlyoutClosed(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);

        void PositionChanged(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void MinimalTimelineSizeChanged(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void MinimalTimelinePointerEntered(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void MinimalTimelinePointerPressed(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void MinimalTimelinePointerMoved(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void MinimalTimelinePointerExited(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void TimelinePointerPressed(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void TimelinePointerMoved(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void TimelinePointerReleased(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void TimelinePointerExited(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void TimelinePointerCaptureLost(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void TimeDisplayFlyoutOpening(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);
        void RemainingTimeClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void HighPrecisionTimeClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ShowPercentageClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void VolumeChanged(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void VolumeIconTapped(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);
        void VolumeSliderSizeChanged(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void VolumeSliderPointerEntered(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void VolumeSliderPointerExited(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);

        void FullscreenClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void TimelineSizeChanged(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void PlaybackControlsSizeChanged(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
        void MinimalTransportBarSizeChanged(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void TransportRootSizeChanged(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::SizeChangedEventArgs const&);

        void PipClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void StatsClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void CaptureClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CaptureWithSubtitlesClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CaptureVideoOnlyClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenCaptureFolderClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void SettingsClicked(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SettingsPointerEntered(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void SettingsPointerExited(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);

        void ProgressTimerTick(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);

        void TransportPointerEntered(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void TransportPointerExited(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void TransportHideTimerTick(
            Windows::Foundation::IInspectable const&,
            Windows::Foundation::IInspectable const&);

    private:
        struct ChapterView
        {
            double time{};
            std::wstring title;
        };
        struct ChapterSegmentVisual
        {
            double start{};
            double end{};
            double width{};
            Microsoft::UI::Xaml::Controls::Border root{ nullptr };
            Microsoft::UI::Xaml::Controls::Border cache{ nullptr };
            Microsoft::UI::Xaml::Controls::Border fill{ nullptr };
        };

        bool m_isPlaying{ false };
        bool m_isReplay{ false };
        bool m_loopEnabled{ false };
        bool m_shuffleEnabled{ false };
        bool m_ready{ false };
        bool m_isUpdatingPosition{ false };

        // TimelineInputSurface is the sole mouse authority. While captured,
        // ProgressTimerTick must not overwrite the pointer-owned position.
        bool m_timelineUserInteraction{ false };
        bool m_timelineInteractionHasTarget{ false };
        bool m_timelineInteractionLastSeekWasExact{ false };
        double m_timelineInteractionSeconds{};

        // After a final precise seek, give mpv a short window to publish the
        // new playback-time before the 250-ms progress timer resumes ownership.
        int m_timelineProgressHoldTicks{};
        bool m_isUpdatingVolume{ false };
        bool m_controlsReady{ false };
        bool m_transportVisible{ true };
        // After a fresh media open, transport reveal stays disarmed until the
        // native video surface reports genuine pointer movement. During the
        // short mpv opening/presentation phase, even genuine movement is
        // intentionally ignored so pre-frame input cannot reveal the bar.
        bool m_transportRevealArmed{ true };
        bool m_transportStartupGuard{ false };
        uint32_t m_transportStartupGuardTicks{};
        uint32_t m_transportStartupReadySamples{};
        bool m_consoleOpen{ false };
        std::chrono::steady_clock::time_point m_transportHideNotBefore{};
        bool m_pictureInPicture{ false };
        bool m_settingsOverlayOpen{ false };
        bool m_transportFlyoutOpen{ false };
        bool m_mediaControlsExpanded{ false };
        bool m_minimalTransportStyle{ false };
        // Bar mode defaults to the new two-row layout. The original three-row
        // presentation remains available as the saved "full" preference.
        bool m_compactBarLayout{ true };
        bool m_showSeekButtons{ true };
        bool m_showStatsButton{ true };
        bool m_showCaptureButton{ true };
        bool m_showProfilesButton{ false };
        bool m_hasImportedProfiles{ false };
        // Capture and imported-profile shortcuts are video-only UI actions.
        // This cache is refreshed from read-only media classification.
        bool m_videoOnlyActionsAllowed{ false };
        bool m_showShuffleButton{ true };
        bool m_showMediaInfoButton{ true };

        // Video thumbnails are opt-in. No saved value means OFF, so the
        // auxiliary thumbnail decoder never starts unless the user explicitly
        // enables previews in Settings > Interface.
        bool m_videoThumbnailsEnabled{ false };

        bool m_continuousPlayback{ false };
        bool m_lastEofReached{ false };
        bool m_filledTimelineStyle{ false };
        bool m_filledTimelineHovered{ false };
        bool m_volumeSliderHovered{ false };
        bool m_timelinePointerArmed{ true };
        int32_t m_timelineResumeCursorX{};
        int32_t m_timelineResumeCursorY{};
        bool m_chapterThemeInitialized{ false };
        bool m_chapterThemeWasLight{ false };
        bool m_showRemainingTime{ false };
        bool m_highPrecisionTime{ false };
        bool m_showTimePercentage{ false };
        MediaSourceBadge m_sourceBadge{ MediaSourceBadge::None };
        MediaVideoBadge m_videoBadge{ MediaVideoBadge::None };
        MediaAudioBadge m_audioBadge{ MediaAudioBadge::None };
        int64_t m_badgeVideoTrackId{ -1 };
        int64_t m_badgeAudioTrackId{ -1 };
        bool m_hdr10PlusDetected{ false };
        uint32_t m_hdr10PlusHitCount{};
        uint32_t m_hdr10PlusMissCount{};
        std::uint64_t m_badgeVisualGeneration{};
        uint32_t m_progressTickCount{};
        uint32_t m_chapterRefreshCountdown{};
        double m_chapterMarkerDuration{};
        double m_chapterMarkerWidth{};
        double m_minimalChapterMarkerDuration{};
        double m_minimalChapterMarkerWidth{};
        bool m_minimalChapterMarkerFilledStyle{};
        bool m_minimalChapterThemeWasLight{};
        double m_webCacheEnd{};
        int32_t m_hoveredChapterSegment{ -1 };
        std::vector<ChapterView> m_chapters;
        std::vector<ChapterSegmentVisual> m_chapterSegments;
        std::vector<ChapterSegmentVisual> m_minimalChapterSegments;
        Microsoft::UI::Xaml::DispatcherTimer m_progressTimer{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer m_transportHideTimer{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer m_transportCollapseTimer{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer m_thumbnailMotionTimer{ nullptr };
        std::unique_ptr<ThumbnailController> m_thumbnailController;
        bool m_thumbnailHoverActive{ false };
        double m_thumbnailHoveredTime{};
        double m_thumbnailHoverPointX{};

        // Logical identity of the hover request currently under the pointer.
        // The displayed thumbnail may be used as a click target only when both
        // generations are identical. This avoids comparing floating-point time
        // or X coordinates to decide whether the visible image is current.
        std::uint64_t m_thumbnailHoverGeneration{};
        std::uint64_t m_thumbnailDisplayedGeneration{};

        // UI-only motion state. These values never decide seek ownership;
        // generation remains the sole authority for thumbnail/click sync.
        bool m_thumbnailMotionInitialized{ false };
        double m_thumbnailMotionCurrentX{};
        double m_thumbnailMotionTargetX{};
        double m_thumbnailOpacityCurrent{ 1.0 };
        double m_thumbnailOpacityTarget{ 1.0 };

        // Cached popup geometry. 38P computes expensive Measure/TransformToVisual
        // only when layout/thumbnail dimensions change, not every 16-ms tick.
        bool m_thumbnailGeometryValid{ false };
        double m_thumbnailGeometrySliderOriginX{};
        double m_thumbnailGeometryTop{};
        double m_thumbnailGeometryCardWidth{};
        double m_thumbnailGeometryOverlayWidth{};
        double m_thumbnailGeometrySliderWidth{};

        // Reuse the same 320x180 WriteableBitmap instead of allocating a new
        // XAML image object for every FAST/EXACT frame.
        Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap
            m_thumbnailBitmap{ nullptr };
        int m_thumbnailBitmapWidth{};
        int m_thumbnailBitmapHeight{};

        bool m_thumbnailDisplayedValid{ false };
        double m_thumbnailDisplayedRequestTime{};
        double m_thumbnailDisplayedFrameTime{};
        double m_thumbnailDisplayedPointX{};

        void SetTransportVisible(bool visible, bool animate = true);
        void SetMediaControlsExpanded(bool expanded);
        void UpdateVideoOnlyActionVisibility();
        void ApplyTransportStyleVisuals();
        void RefreshVideoOnlyActionEligibility();
        void UpdateResponsiveControls(double width);
        void RefreshMediaBadges();
        void ClearMediaBadges();
        void ApplyMediaBadgeVisuals();
        winrt::fire_and_forget LoadCustomBadgeImageAsync(
            Microsoft::UI::Xaml::Controls::Image image,
            std::wstring path,
            std::uint64_t generation);
        void ScheduleTransportHide(std::chrono::milliseconds delay);
        void UpdateSpeedLabel(double speed);
        void UpdatePlayButtonState();
        void UpdateLoopButtonState();
        void UpdateShuffleButtonState();
        void RefreshChapterData(double duration, double elapsed);
        void UpdateChapterTitle(double elapsed);
        void RenderChapterMarkers(double duration, double elapsed);
        void RenderMinimalChapterMarkers(double duration, double elapsed);
        void UpdateMinimalTimelineVisual();
        void SetHoveredChapterSegment(int32_t index);
        void SetFilledTimelineHovered(bool hovered);
        void SetVolumeSliderHovered(bool hovered);
        double TimelineTimeFromPointerX(double pointerX, double duration);
        bool ApplyTimelinePointerPosition(double pointerX, bool exact);
        void HideThumbnailPreview();
        void ThumbnailMotionTimerTick(
            Windows::Foundation::IInspectable const& sender,
            Windows::Foundation::IInspectable const& args);
        void RefreshThumbnailPreviewGeometry();
        void PositionThumbnailPreview(double pointerX);
        void ApplyThumbnailResult(
            ThumbnailController::Result&& result,
            std::uint64_t generation,
            double requestPointX);
        void UpdateVolumeMarker();
        void UpdateTimeDisplay(double elapsed, double duration);
        static std::wstring FormatPlaybackTime(
            double seconds, bool highPrecision = false);
        void TransportCollapseTimerTick(
            Windows::Foundation::IInspectable const& sender,
            Windows::Foundation::IInspectable const& args);
    };
}

namespace winrt::HCPlayer::factory_implementation
{
    struct MainPage : MainPageT<MainPage, implementation::MainPage>
    {
    };
}
