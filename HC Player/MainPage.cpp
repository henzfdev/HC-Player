#include "pch.h"
#include "MainPage.h"
#include "PlayerBridge.h"
#include "LocalizationManager.h"
#include "SystemMediaControlsManager.h"

#include <shobjidl.h>
#include <filesystem>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>

// WriteableBitmap::PixelBuffer implements this COM ABI contract.
// Local declaration avoids SDK/header namespace differences.
struct __declspec(uuid("905A0FEF-BC53-11DF-8C49-001E4FC686DA"))
    __declspec(novtable) HcBufferByteAccess : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Buffer(uint8_t** value) = 0;
};

namespace
{
    std::wstring MainPageString(
        std::wstring_view resourceId,
        std::wstring_view fallback)
    {
        return hc::localization::GetString(resourceId, fallback);
    }

    winrt::Windows::Foundation::IInspectable MainPageBoxString(
        std::wstring_view resourceId,
        std::wstring_view fallback)
    {
        return winrt::box_value(winrt::hstring{
            MainPageString(resourceId, fallback) });
    }


}


namespace winrt::HCPlayer::implementation
{
    MainPage::MainPage()
    {
        RequestedTheme(PlayerIsLightTheme()
            ? Microsoft::UI::Xaml::ElementTheme::Light
            : Microsoft::UI::Xaml::ElementTheme::Dark);
        m_progressTimer = Microsoft::UI::Xaml::DispatcherTimer{};
        m_progressTimer.Interval(std::chrono::milliseconds(250));
        m_progressTimer.Tick({ this, &MainPage::ProgressTimerTick });
        m_progressTimer.Start();

        m_transportHideTimer = Microsoft::UI::Xaml::DispatcherTimer{};
        m_transportHideTimer.Tick({ this, &MainPage::TransportHideTimerTick });

        m_transportCollapseTimer = Microsoft::UI::Xaml::DispatcherTimer{};
        m_transportCollapseTimer.Interval(std::chrono::milliseconds(230));
        m_transportCollapseTimer.Tick({ this, &MainPage::TransportCollapseTimerTick });

        // The thumbnail's visual shell moves independently from decode delivery.
        // A light ~50 Hz UI-only easing pass makes the preview follow the pointer
        // smoothly without changing which decoded frame owns click sync.
        m_thumbnailMotionTimer = Microsoft::UI::Xaml::DispatcherTimer{};
        m_thumbnailMotionTimer.Interval(std::chrono::milliseconds(20));
        m_thumbnailMotionTimer.Tick({ this, &MainPage::ThumbnailMotionTimerTick });

        // Lazy internally: this object does not create a thread or second mpv
        // instance until the first stable timeline-hover request arrives.
        m_thumbnailController = std::make_unique<ThumbnailController>();
    }

    void MainPage::SetSettingsOverlayOpen(bool open)
    {
        m_settingsOverlayOpen = open;
        if (open)
        {
            m_transportHideTimer.Stop();
            SetTransportVisible(false, false);
        }
        else
        {
            SetTransportVisible(true, false);
            // Fullscreen enter/exit reuses this reveal path. Keep the bar
            // available briefly, but make the transition feel more immediate.
            ScheduleTransportHide(std::chrono::milliseconds(1000));
        }
    }

    void MainPage::PrepareSilentFullscreenEntry()
    {
        // Enter/double-click fullscreen is intentionally immersive: do not
        // reveal the transport merely because the top-level HWND changed size.
        // Stop any pending reveal/hide work and collapse the XAML island
        // immediately. The normal bottom hot-zone can bring it back on genuine
        // pointer activity.
        if (!m_ready)
        {
            return;
        }

        m_transportHideTimer.Stop();
        m_transportCollapseTimer.Stop();
        m_transportHideNotBefore = {};
        SetTransportVisible(false, false);
    }

    void MainPage::SetPictureInPictureMode(bool enabled)
    {
        m_pictureInPicture = enabled;
        if (enabled && m_thumbnailController)
        {
            m_thumbnailController->Cancel();
            HideThumbnailPreview();
        }

        POINT cursor{};
        if (GetCursorPos(&cursor))
        {
            m_timelineResumeCursorX = cursor.x;
            m_timelineResumeCursorY = cursor.y;
        }
        // A XAML island can emit PointerMoved when the restored layout moves
        // underneath a stationary cursor. Require a real cursor displacement
        // before the timeline tooltip is allowed to return.
        m_timelinePointerArmed = false;
        auto visibility = enabled
            ? Microsoft::UI::Xaml::Visibility::Collapsed
            : Microsoft::UI::Xaml::Visibility::Visible;

        // PiP de áudio usa navegação por faixa em vez dos botões de seek ±10.
        // Vídeo preserva exatamente o conjunto já aprovado: Play, Loop e +10.
        bool const pipAudio = enabled && PlayerIsCurrentMediaAudio();

        InformationRow().Visibility(visibility);
        PreviousChapterButton().Visibility(
            pipAudio ? Microsoft::UI::Xaml::Visibility::Visible : visibility);
        SeekBackwardButton().Visibility(enabled || !m_showSeekButtons
            ? Microsoft::UI::Xaml::Visibility::Collapsed
            : Microsoft::UI::Xaml::Visibility::Visible);
        SeekForwardButton().Visibility(enabled
            ? (pipAudio
                ? Microsoft::UI::Xaml::Visibility::Collapsed
                : Microsoft::UI::Xaml::Visibility::Visible)
            : (m_showSeekButtons
                ? Microsoft::UI::Xaml::Visibility::Visible
                : Microsoft::UI::Xaml::Visibility::Collapsed));
        NextChapterButton().Visibility(
            pipAudio ? Microsoft::UI::Xaml::Visibility::Visible : visibility);
        ShuffleButton().Visibility(
            enabled || !m_showShuffleButton
                ? Microsoft::UI::Xaml::Visibility::Collapsed
                : Microsoft::UI::Xaml::Visibility::Visible);
        MediaInfoButton().Visibility(
            enabled || !m_showMediaInfoButton
                ? Microsoft::UI::Xaml::Visibility::Collapsed
                : Microsoft::UI::Xaml::Visibility::Visible);
        LoopButton().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
        SpeedButton().Visibility(visibility);
        ProfilesButton().Visibility(
            enabled || !m_videoOnlyActionsAllowed ||
                !m_showProfilesButton || !m_hasImportedProfiles
                ? Microsoft::UI::Xaml::Visibility::Collapsed
                : Microsoft::UI::Xaml::Visibility::Visible);
        TracksButton().Visibility(visibility);
        StatsButton().Visibility(enabled || !m_showStatsButton
            ? Microsoft::UI::Xaml::Visibility::Collapsed
            : Microsoft::UI::Xaml::Visibility::Visible);
        VolumeIcon().Visibility(visibility);
        VolumeSliderHost().Visibility(visibility);
        if (enabled)
            TimeDisplayHost().Visibility(
                Microsoft::UI::Xaml::Visibility::Visible);

        // Pointer interaction in a DesktopWindowXamlSource can leave the
        // island itself focused. Its host focus outline is composited outside
        // TransportRoot, so it remained visible for one frame of the fade.
        // PiP controls stay clickable but no longer take keyboard focus.
        bool allowFocus = !enabled;
        AllowFocusOnInteraction(allowFocus);
        IsTabStop(allowFocus);
        UseSystemFocusVisuals(allowFocus);
        TransportHitArea().AllowFocusOnInteraction(allowFocus);
        TransportHitArea().UseSystemFocusVisuals(allowFocus);
        TransportRoot().AllowFocusOnInteraction(allowFocus);
        TransportRoot().UseSystemFocusVisuals(allowFocus);
        PositionSlider().AllowFocusOnInteraction(allowFocus);
        PositionSlider().IsTabStop(allowFocus);
        PositionSlider().UseSystemFocusVisuals(allowFocus);
        PreviousChapterButton().AllowFocusOnInteraction(allowFocus);
        PreviousChapterButton().IsTabStop(allowFocus);
        PreviousChapterButton().UseSystemFocusVisuals(allowFocus);
        PlayButton().AllowFocusOnInteraction(allowFocus);
        PlayButton().IsTabStop(allowFocus);
        PlayButton().UseSystemFocusVisuals(allowFocus);
        NextChapterButton().AllowFocusOnInteraction(allowFocus);
        NextChapterButton().IsTabStop(allowFocus);
        NextChapterButton().UseSystemFocusVisuals(allowFocus);
        LoopButton().AllowFocusOnInteraction(allowFocus);
        LoopButton().IsTabStop(allowFocus);
        LoopButton().UseSystemFocusVisuals(allowFocus);
        SeekForwardButton().AllowFocusOnInteraction(allowFocus);
        SeekForwardButton().IsTabStop(allowFocus);
        SeekForwardButton().UseSystemFocusVisuals(allowFocus);
        PipButton().AllowFocusOnInteraction(allowFocus);
        PipButton().IsTabStop(allowFocus);
        PipButton().UseSystemFocusVisuals(allowFocus);

        // O StackPanel é compartilhado pelos modos normal e PiP.
        // Vídeo mantém Play/Pause, Loop, +10. Áudio usa
        // Anterior, Play/Pause, Próxima, Loop — sem +10.
        // Reposicionamos apenas Loop; os demais controles mantêm a ordem
        // original e são filtrados por Visibility.
        auto buttons = PlaybackButtons().Children();
        uint32_t loopIndex{};
        if (buttons.IndexOf(LoopButton(), loopIndex))
        {
            buttons.RemoveAt(loopIndex);

            if (enabled && !pipAudio)
            {
                uint32_t forwardIndex{};
                if (buttons.IndexOf(SeekForwardButton(), forwardIndex))
                    buttons.InsertAt(forwardIndex, LoopButton());
                else
                    buttons.Append(LoopButton());
            }
            else
            {
                // Normal e PiP de áudio compartilham a ordem natural:
                // ... Próxima, [ações secundárias], Loop. Como as ações
                // secundárias ficam ocultas no PiP, o resultado visível é
                // Anterior | Play/Pause | Próxima | Loop.
                buttons.Append(LoopButton());
            }
        }

        // In PiP the time readout is informational only. This suppresses both
        // its tooltip and its context/tap flyout without affecting playback.
        TimeDisplayHost().IsHitTestVisible(!enabled);
        if (enabled)
        {
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                TimeDisplayHost(), nullptr);
        }
        else
        {
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                TimeDisplayHost(), MainPageBoxString(
                    L"MainPageDynTimeDisplayOptions",
                    L"Opções de exibição do tempo"));
        }
        ChapterHoverCard().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        ChapterHoverPopup().IsOpen(false);
        SetHoveredChapterSegment(-1);
        SetFilledTimelineHovered(false);
        SetVolumeSliderHovered(false);

        PipIcon().Glyph(enabled ? L"\uE944" : L"\uE8A7");
        TransportRoot().Padding(enabled
            ? Microsoft::UI::Xaml::Thickness{ 16.0, 2.0, 16.0, 3.0 }
            : (m_mediaControlsExpanded
                ? Microsoft::UI::Xaml::Thickness{ 10.0, 5.0, 10.0, 2.0 }
        : Microsoft::UI::Xaml::Thickness{ 10.0, 8.0, 10.0, 8.0 }));
        TimelineRow().Margin(enabled
            ? Microsoft::UI::Xaml::Thickness{ 0.0, 1.0, 0.0, 0.0 }
        : Microsoft::UI::Xaml::Thickness{ 0.0, 4.0, 0.0, 0.0 });
        // Move only the PiP timeline visually, without changing the measured
        // row height or pulling the playback buttons along with it.
        TimelineRow().Translation(Windows::Foundation::Numerics::float3{
            0.0f, enabled ? -3.0f : 0.0f, 0.0f });
        PlaybackControlsRow().Margin(enabled
            ? Microsoft::UI::Xaml::Thickness{ 0.0, 0.0, 0.0, 5.0 }
        : Microsoft::UI::Xaml::Thickness{ 0.0, 8.0, 0.0, 0.0 });

        PlaybackControlsRow().Translation(
            Windows::Foundation::Numerics::float3{
                0.0f,
                // PiP keeps its established 1 DIP compensation. Returning to
                // the normal player restores the image-only +6 DIP placement.
                enabled
                    ? 1.0f
                    : ((m_mediaControlsExpanded &&
                        PlayerIsCurrentMediaImage()) ? 6.0f : 0.0f),
                0.0f });

        TimeDisplayHost().MinWidth(
            enabled ? 0.0 : 102.0);

        ElapsedTimeText().FontSize(
            enabled ? 13.0 : 14.0);

        TimeDisplayHost().Margin(enabled
            ? Microsoft::UI::Xaml::Thickness{ 0.0, 0.0, 0.0, 0.0 }
        : Microsoft::UI::Xaml::Thickness{ 84.0, 0.0, 0.0, 0.0 });

        // Keep the existing PiP geometry untouched (1.5 DIP). In the normal
        // player, move the readout slightly lower so its visual bottom spacing
        // matches the left transport controls more closely.
        TimeDisplayHost().Translation(
            Windows::Foundation::Numerics::float3{
                0.0f,
                1.5f,
                0.0f });

        TimeDisplayHost().HorizontalAlignment(enabled
            ? Microsoft::UI::Xaml::HorizontalAlignment::Right
            : Microsoft::UI::Xaml::HorizontalAlignment::Left);

        if (enabled)
        {
            UpdateResponsiveControls(PlaybackControlsRow().ActualWidth());
        }

        if (!enabled)
        {
            CaptureButton().Visibility(
                m_videoOnlyActionsAllowed &&
                m_showCaptureButton && m_mediaControlsExpanded
                    ? Microsoft::UI::Xaml::Visibility::Visible
                    : Microsoft::UI::Xaml::Visibility::Collapsed);
            UpdateResponsiveControls(PlaybackControlsRow().ActualWidth());
        }
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            PipButton(), enabled
                ? MainPageBoxString(L"MainPageDynBackToWindow", L"Voltar para a janela")
                : MainPageBoxString(L"MainPageDynPictureInPicture", L"Picture-in-Picture"));
        ApplyTransportStyleVisuals();
        if (enabled) PlayerReleaseTransportFocus();
        SetTransportVisible(true, false);
        ScheduleTransportHide(std::chrono::milliseconds(1800));
    }

    void MainPage::SetMediaControlsExpanded(bool expanded)
    {
        m_mediaControlsExpanded = expanded;
        auto visibility = expanded
            ? Microsoft::UI::Xaml::Visibility::Visible
            : Microsoft::UI::Xaml::Visibility::Collapsed;

        // Still images have no meaningful seek position. Remove the entire
        // timeline row in BOTH timeline styles, so there is no dead seek area
        // and the transport can physically shrink by exactly that row's height.
        bool const imageWithoutTimeline =
            expanded && PlayerIsCurrentMediaImage();

        TimelineRow().Visibility(
            imageWithoutTimeline
                ? Microsoft::UI::Xaml::Visibility::Collapsed
                : visibility);

        PlaybackControlsRow().Visibility(visibility);

        // Normal host = 132 DIP. TimelineRow = 24 DIP + 4 DIP top margin.
        // Images therefore use 104 DIP, preserving every other spacing value.
        PlayerSetTransportImageMode(imageWithoutTimeline);
        if (!m_pictureInPicture)
        {
            // With the 24+4 DIP timeline row removed, the 104-DIP image bar
            // has extra visual air below the second row. Move only that row
            // closer to the bottom edge; the host height remains unchanged.
            PlaybackControlsRow().Translation(
                Windows::Foundation::Numerics::float3{
                    0.0f,
                    imageWithoutTimeline ? 6.0f : 0.0f,
                    0.0f });

            CaptureButton().Visibility(
                expanded && m_videoOnlyActionsAllowed && m_showCaptureButton
                    ? Microsoft::UI::Xaml::Visibility::Visible
                    : Microsoft::UI::Xaml::Visibility::Collapsed);
            TransportRoot().Padding(expanded
                ? Microsoft::UI::Xaml::Thickness{ 10.0, 5.0, 10.0, 2.0 }
            : Microsoft::UI::Xaml::Thickness{ 10.0, 8.0, 10.0, 8.0 });
        }
        PlayerSetTransportCompact(!expanded);
        UpdateVideoOnlyActionVisibility();
        ApplyTransportStyleVisuals();
    }

    void MainPage::ApplyTransportStyleVisuals()
    {
        using Microsoft::UI::Xaml::Visibility;
        using Microsoft::UI::Xaml::Media::Brush;

        bool const minimalActive =
            m_minimalTransportStyle &&
            m_mediaControlsExpanded &&
            !m_pictureInPicture;

        // Bar Compact is a layout-only mode. Reuse the one approved badge host
        // and the one approved Capture button by moving them between dedicated
        // XAML panels; no playback/capture/badge logic is cloned.
        bool const compactBarActive =
            m_compactBarLayout &&
            m_mediaControlsExpanded &&
            !minimalActive &&
            !m_pictureInPicture;

        // These two controls have exactly two legal XAML owners each. During a
        // hidden shell cold-start, OpenPath() can expand the transport before the
        // page has entered the realized visual tree. VisualTreeHelper::GetParent
        // may then report no visual parent even though the element is still in its
        // XAML Panel.Children collection; appending it to the Compact panel would
        // throw "Element is already the child of another element." Detach by
        // querying the known logical owner collections instead, which works both
        // before and after realization and leaves normal runtime reparenting intact.
        auto moveBetweenPanels = [](Microsoft::UI::Xaml::UIElement const& element,
            Microsoft::UI::Xaml::Controls::Panel const& firstOwner,
            Microsoft::UI::Xaml::Controls::Panel const& secondOwner,
            Microsoft::UI::Xaml::Controls::Panel const& target)
            {
                if (!element || !target) return;

                auto targetChildren = target.Children();
                uint32_t targetIndex{};
                if (targetChildren.IndexOf(element, targetIndex)) return;

                auto detachFrom = [&](Microsoft::UI::Xaml::Controls::Panel const& owner)
                    {
                        if (!owner || owner == target) return;
                        auto children = owner.Children();
                        uint32_t index{};
                        if (children.IndexOf(element, index))
                        {
                            children.RemoveAt(index);
                        }
                    };

                detachFrom(firstOwner);
                detachFrom(secondOwner);
                target.Children().Append(element);
            };

        if (compactBarActive)
        {
            moveBetweenPanels(MediaBadgesHost(),
                InformationCenterGrid(), CompactBadgeSlot(), CompactBadgeSlot());
            moveBetweenPanels(CaptureButton(),
                InformationActions(), RightControls(), RightControls());

            // Match the Compact row's approved 40x38 bottom-control geometry.
            // The Capture button keeps its original style/logic and is restored
            // to the frozen 36x34 Full-bar geometry below.
            CaptureButton().Width(40.0);
            CaptureButton().Height(38.0);

            // Keep Capture exactly between Tracks (subtitles/audio) and Stats
            // in Bar Compact. Reparenting the approved button preserves the
            // existing flyout, feedback pulse and capture implementation.
            auto rightControls = RightControls().Children();
            uint32_t captureIndex{};
            if (rightControls.IndexOf(CaptureButton(), captureIndex))
            {
                rightControls.RemoveAt(captureIndex);
            }

            uint32_t statsIndex{};
            if (rightControls.IndexOf(StatsButton(), statsIndex))
            {
                rightControls.InsertAt(statsIndex, CaptureButton());
            }
            else
            {
                rightControls.Append(CaptureButton());
            }

            MediaBadgesHost().Margin(
                Microsoft::UI::Xaml::Thickness{ 0.0, 0.0, 2.0, 0.0 });
            CompactBarExtras().Visibility(Visibility::Visible);
        }
        else
        {
            moveBetweenPanels(MediaBadgesHost(),
                InformationCenterGrid(), CompactBadgeSlot(), InformationCenterGrid());
            moveBetweenPanels(CaptureButton(),
                InformationActions(), RightControls(), InformationActions());

            // Restore the exact frozen Full-bar Capture size after leaving
            // Compact so the original three-row layout remains unchanged.
            CaptureButton().Width(36.0);
            CaptureButton().Height(34.0);

            // Capture preceded Settings in the frozen three-row bar. Reinsert
            // it at index zero so Full layout is visually identical after any
            // Compact -> Full round-trip.
            auto actions = InformationActions().Children();
            uint32_t captureIndex{};
            if (actions.IndexOf(CaptureButton(), captureIndex) && captureIndex != 0)
            {
                actions.RemoveAt(captureIndex);
                actions.InsertAt(0, CaptureButton());
            }

            MediaBadgesHost().Margin(
                Microsoft::UI::Xaml::Thickness{ 0.0, 0.0, 18.0, 0.0 });
            CompactBarExtras().Visibility(Visibility::Collapsed);
        }

        if (!minimalActive || PlayerIsCurrentMediaImage())
        {
            MinimalTimelineHoverPopup().IsOpen(false);
        }

        MinimalTransportBar().Visibility(
            minimalActive ? Visibility::Visible : Visibility::Collapsed);

        // PiP intentionally retains the established compact bar. If we entered
        // it from Minimal mode, restore the normal transport surface first.
        auto restoreSurface = [this]()
            {
                TransportRoot().Background(
                    Resources().Lookup(winrt::box_value(L"TransportMicaToneBrush"))
                        .as<Brush>());
                TransportRoot().BorderBrush(
                    Resources().Lookup(winrt::box_value(L"TransportTopEdgeBrush"))
                        .as<Brush>());
                TransportRoot().BorderThickness(
                    Microsoft::UI::Xaml::Thickness{ 0.0, 0.5, 0.0, 0.0 });
            };

        if (m_pictureInPicture)
        {
            // Minimal mode collapses the normal playback row. PiP must restore
            // the established compact transport explicitly when entered from
            // that state. Individual PiP button visibility/order remains owned
            // by SetPictureInPictureMode().
            InformationRow().Visibility(Visibility::Collapsed);
            bool const pipImageWithoutTimeline =
                m_mediaControlsExpanded && PlayerIsCurrentMediaImage();
            TimelineRow().Visibility(
                pipImageWithoutTimeline ? Visibility::Collapsed : Visibility::Visible);
            PlaybackControlsRow().Visibility(Visibility::Visible);
            restoreSurface();
            PlayerSetTransportMinimal(false);
            return;
        }

        bool const imageWithoutTimeline =
            m_mediaControlsExpanded && PlayerIsCurrentMediaImage();

        if (minimalActive)
        {
            InformationRow().Visibility(Visibility::Collapsed);
            TimelineRow().Visibility(Visibility::Collapsed);
            PlaybackControlsRow().Visibility(Visibility::Collapsed);
            MinimalTimelineHost().Visibility(
                imageWithoutTimeline ? Visibility::Collapsed : Visibility::Visible);
            MinimalCurrentTimeText().Visibility(
                imageWithoutTimeline ? Visibility::Collapsed : Visibility::Visible);
            MinimalDurationTimeText().Visibility(
                imageWithoutTimeline ? Visibility::Collapsed : Visibility::Visible);
            UpdateMinimalTimelineVisual();
            MinimalSeekBackwardButton().Visibility(
                m_showSeekButtons ? Visibility::Visible : Visibility::Collapsed);
            MinimalSeekForwardButton().Visibility(
                m_showSeekButtons ? Visibility::Visible : Visibility::Collapsed);

            // Only the pill paints pixels; the surrounding island stays
            // transparent over the video. Playback itself is untouched.
            TransportRoot().Background(nullptr);
            TransportRoot().BorderBrush(nullptr);
            TransportRoot().BorderThickness(
                Microsoft::UI::Xaml::Thickness{ 0.0, 0.0, 0.0, 0.0 });
            TransportRoot().Padding(
                Microsoft::UI::Xaml::Thickness{ 8.0, 7.0, 8.0, 7.0 });

            PlayerSetTransportMinimal(true);

            auto queue = DispatcherQueue();
            if (queue)
            {
                queue.TryEnqueue([]() { PlayerRefreshTransportLayout(); });
            }
            return;
        }

        PlayerSetTransportMinimal(false);
        PlayerSetTransportBarCompactLayout(m_compactBarLayout);
        restoreSurface();
        InformationRow().Visibility(
            compactBarActive ? Visibility::Collapsed : Visibility::Visible);
        TimelineRow().Visibility(
            m_mediaControlsExpanded && !imageWithoutTimeline
                ? Visibility::Visible : Visibility::Collapsed);
        PlaybackControlsRow().Visibility(
            m_mediaControlsExpanded ? Visibility::Visible : Visibility::Collapsed);
        TransportRoot().Padding(m_mediaControlsExpanded
            ? Microsoft::UI::Xaml::Thickness{ 10.0, 5.0, 10.0, 2.0 }
            : Microsoft::UI::Xaml::Thickness{ 10.0, 8.0, 10.0, 8.0 });
        PlayerSetTransportImageMode(imageWithoutTimeline);
        PlayerSetTransportCompact(!m_mediaControlsExpanded);
        UpdateResponsiveControls(PlaybackControlsRow().ActualWidth());
    }

    void MainPage::UpdateVideoOnlyActionVisibility()
    {
        using Microsoft::UI::Xaml::Visibility;

        bool const normalVideoActions =
            !m_pictureInPicture &&
            m_mediaControlsExpanded &&
            m_videoOnlyActionsAllowed;

        CaptureButton().Visibility(
            normalVideoActions && m_showCaptureButton
                ? Visibility::Visible
                : Visibility::Collapsed);

        // Profiles are already a secondary responsive action. Preserve the
        // approved 780-DIP threshold, but never expose them for audio/images.
        constexpr double ProfilesButtonThreshold = 780.0;
        double const width = PlaybackControlsRow().ActualWidth();
        ProfilesButton().Visibility(
            normalVideoActions &&
                m_showProfilesButton &&
                m_hasImportedProfiles &&
                width >= ProfilesButtonThreshold
                ? Visibility::Visible
                : Visibility::Collapsed);
    }

    void MainPage::RefreshVideoOnlyActionEligibility()
    {
        // Classification is read-only. PresentationReady keeps stale state from
        // the outgoing item from exposing video-only actions during a replace.
        bool const allowed =
            m_ready &&
            PlayerIsMediaPresentationReady() &&
            !PlayerIsCurrentMediaAudio() &&
            !PlayerIsCurrentMediaImage();

        if (m_videoOnlyActionsAllowed == allowed)
        {
            return;
        }

        m_videoOnlyActionsAllowed = allowed;
        UpdateVideoOnlyActionVisibility();
    }

    bool MainPage::UsesMinimalTransportStyle() const noexcept
    {
        return m_minimalTransportStyle &&
            m_mediaControlsExpanded &&
            !m_pictureInPicture;
    }

    Windows::Foundation::Rect MainPage::MinimalTransportRegion()
    {
        if (!UsesMinimalTransportStyle())
        {
            return {};
        }

        try
        {
            auto bar = MinimalTransportBar();
            if (!bar ||
                bar.Visibility() != Microsoft::UI::Xaml::Visibility::Visible ||
                bar.ActualWidth() <= 1.0 ||
                bar.ActualHeight() <= 1.0)
            {
                return {};
            }

            auto transform = bar.TransformToVisual(TransportRoot());
            auto origin = transform.TransformPoint({ 0.0f, 0.0f });
            return Windows::Foundation::Rect{
                origin.X, origin.Y,
                static_cast<float>(bar.ActualWidth()),
                static_cast<float>(bar.ActualHeight()) };
        }
        catch (winrt::hresult_error const&)
        {
            return {};
        }
    }

    void MainPage::RefreshInterfacePreferences()
    {
        auto enabled = [](wchar_t const* name)
            {
                std::wstring value;
                return !PlayerTryGetSavedMpvOption(name, value) || value != L"no";
            };
        m_showSeekButtons = enabled(L"ui-show-seek-buttons");
        m_showStatsButton = enabled(L"ui-show-stats-button");
        m_showCaptureButton = enabled(L"ui-show-capture-button");
        m_showShuffleButton = enabled(L"ui-show-shuffle-button");
        m_showMediaInfoButton = enabled(L"ui-show-mediainfo-button");

        std::wstring transportStyle;
        m_minimalTransportStyle =
            PlayerTryGetSavedMpvOption(
                L"ui-transport-style", transportStyle) &&
            transportStyle == L"minimal";

        std::wstring barLayout;
        bool const hasBarLayout = PlayerTryGetSavedMpvOption(
            L"ui-bar-layout", barLayout);
        // Compact is the product default. Existing users without this new key
        // automatically receive the two-row Bar; choosing "full" restores the
        // original three-row layout byte-for-byte at the control level.
        m_compactBarLayout = !hasBarLayout || barLayout != L"full";

        // Quick profile access is deliberately opt-in. A missing saved value
        // means OFF, and the button also stays hidden when no imported profile
        // exists.
        std::wstring profilesButton;
        m_showProfilesButton =
            PlayerTryGetSavedMpvOption(
                L"ui-show-profiles-button", profilesButton) &&
            profilesButton == L"yes";
        m_hasImportedProfiles =
            !PlayerGetImportedProfileNames().empty();

        // 38T: video thumbnail previews are explicitly opt-in. Unlike the
        // normal interface visibility toggles above, a missing saved value must
        // mean OFF. This keeps the private thumbnail worker completely idle for
        // users who never enable the feature.
        std::wstring videoThumbnails;
        m_videoThumbnailsEnabled =
            PlayerTryGetSavedMpvOption(
                L"ui-video-thumbnails", videoThumbnails) &&
            videoThumbnails == L"yes";

        if (!m_videoThumbnailsEnabled)
        {
            if (m_thumbnailController)
            {
                m_thumbnailController->Cancel();
            }

            HideThumbnailPreview();
        }

        // Opt-in behavior: if no value has ever been saved, stay disabled.
        std::wstring continuousPlayback;
        m_continuousPlayback =
            PlayerTryGetSavedMpvOption(
                L"ui-continuous-playback", continuousPlayback) &&
            continuousPlayback == L"yes";

        std::wstring timelineStyle;
        bool hasTimelineStyle = PlayerTryGetSavedMpvOption(
            L"ui-timeline-style", timelineStyle);
        bool const filledTimeline =
            !hasTimelineStyle || timelineStyle == L"filled";
        bool const timelineStyleChanged =
            m_filledTimelineStyle != filledTimeline;

        if (timelineStyleChanged)
        {
            SetFilledTimelineHovered(false);
        }

        m_filledTimelineStyle = filledTimeline;

        // Always reassert the visual state. The logical style and the XAML
        // Opacity/Visibility can temporarily diverge after a settings/layout
        // refresh; gating these assignments behind "style changed" could leave
        // the native circular thumb stuck at Opacity=0 after leaving Filled.
        PositionSlider().Opacity(filledTimeline ? 0.0 : 1.0);
        // Minimal mode must honor the exact same progress-bar preference as
        // the normal transport/PiP. Windows 11 keeps the native circular
        // Slider thumb; HC Player hides only that visual and draws the same
        // vertical capsule language used by the established filled timeline.
        MinimalPositionSlider().Opacity(filledTimeline ? 0.0 : 1.0);
        MinimalFilledTimelineOverlay().Visibility(filledTimeline
            ? Microsoft::UI::Xaml::Visibility::Visible
            : Microsoft::UI::Xaml::Visibility::Collapsed);
        if (!filledTimeline)
        {
            MinimalFilledTimelinePositionMarker().Visibility(
                Microsoft::UI::Xaml::Visibility::Collapsed);
        }
        UpdateMinimalTimelineVisual();

        ChapterMarkers().Margin(filledTimeline
            ? Microsoft::UI::Xaml::Thickness{ 10.0, 0.0, 10.0, 0.0 }
            : Microsoft::UI::Xaml::Thickness{ 9.0, 0.0, 9.0, 0.0 });
        FilledTimelineOverlay().Visibility(filledTimeline
            ? Microsoft::UI::Xaml::Visibility::Visible
            : Microsoft::UI::Xaml::Visibility::Collapsed);
        StandardVolumeTrackHost().Visibility(filledTimeline
            ? Microsoft::UI::Xaml::Visibility::Collapsed
            : Microsoft::UI::Xaml::Visibility::Visible);
        FilledVolumeTrackHost().Visibility(filledTimeline
            ? Microsoft::UI::Xaml::Visibility::Visible
            : Microsoft::UI::Xaml::Visibility::Collapsed);
        StandardVolumeSlider().Visibility(filledTimeline
            ? Microsoft::UI::Xaml::Visibility::Collapsed
            : Microsoft::UI::Xaml::Visibility::Visible);
        VolumeSlider().Visibility(filledTimeline
            ? Microsoft::UI::Xaml::Visibility::Visible
            : Microsoft::UI::Xaml::Visibility::Collapsed);
        VolumeSliderOverlay().Visibility(filledTimeline
            ? Microsoft::UI::Xaml::Visibility::Visible
            : Microsoft::UI::Xaml::Visibility::Collapsed);

        if (!filledTimeline)
        {
            SetVolumeSliderHovered(false);
        }

        if (timelineStyleChanged)
        {
            FilledTimelinePositionMarker().Visibility(
                Microsoft::UI::Xaml::Visibility::Collapsed);

            // Force a geometry rebuild only when the style really changes.
            m_chapterMarkerDuration = 0.0;
            m_chapterMarkerWidth = 0.0;
            m_minimalChapterMarkerDuration = 0.0;
            m_minimalChapterMarkerWidth = 0.0;
            m_minimalChapterSegments.clear();
            double elapsed{};
            double duration{};
            if (PlayerGetPlaybackTimes(elapsed, duration))
            {
                RenderChapterMarkers(duration, elapsed);
            }
        }

        // Images never expose a seek timeline, regardless of whether the
        // selected style is "Faixa preenchida" or "Indicador circular".
        if (m_mediaControlsExpanded)
        {
            bool const imageWithoutTimeline =
                PlayerIsCurrentMediaImage();

            TimelineRow().Visibility(
                imageWithoutTimeline
                    ? Microsoft::UI::Xaml::Visibility::Collapsed
                    : Microsoft::UI::Xaml::Visibility::Visible);

            PlayerSetTransportImageMode(imageWithoutTimeline);

            if (!m_pictureInPicture)
            {
                PlaybackControlsRow().Translation(
                    Windows::Foundation::Numerics::float3{
                        0.0f,
                        imageWithoutTimeline ? 6.0f : 0.0f,
                        0.0f });
            }
        }

        if (!m_pictureInPicture)
        {
            CaptureButton().Visibility(
                m_videoOnlyActionsAllowed &&
                m_showCaptureButton && m_mediaControlsExpanded
                    ? Microsoft::UI::Xaml::Visibility::Visible
                    : Microsoft::UI::Xaml::Visibility::Collapsed);
            UpdateResponsiveControls(PlaybackControlsRow().ActualWidth());
        }

        ApplyTransportStyleVisuals();

        // Badge appearance is a shell preference just like the transport
        // visibility choices above. Re-apply it after Settings saves so a
        // custom set can become active without reopening the current media.
        ApplyMediaBadgeVisuals();
    }

    void MainPage::UpdateResponsiveControls(double width)
    {
        if (width <= 0.0) return;

        auto visibility = [](bool visible)
            {
                return visible ? Microsoft::UI::Xaml::Visibility::Visible
                    : Microsoft::UI::Xaml::Visibility::Collapsed;
            };

        if (m_pictureInPicture)
        {
            // A janela PiP pode chegar a 320 DIP de largura. Depois dos
            // paddings laterais, PlaybackControlsRow fica perto de 288 DIP.
            // Vídeo continua usando o limiar já aprovado. O PiP de áudio tem
            // um botão extra (Anterior | Play | Próxima | Loop), então o
            // relógio cede por mais alguns DIP para nunca comprimir/clippá-los.
            constexpr double PiPVideoTimeThreshold = 300.0;
            constexpr double PiPAudioTimeThreshold = 350.0;

            bool const pipAudio = PlayerIsCurrentMediaAudio();
            double const timeThreshold = pipAudio
                ? PiPAudioTimeThreshold
                : PiPVideoTimeThreshold;

            TimeDisplayHost().Visibility(
                visibility(width > timeThreshold));
            return;
        }

        // In the two-row Bar, badges share the flexible center column with
        // the time readout. They yield only in narrow windows; Capture remains
        // available because it is a direct action rather than decoration.
        constexpr double CompactBadgeThreshold = 1180.0;
        CompactBadgeSlot().Visibility(visibility(
            m_compactBarLayout &&
            m_mediaControlsExpanded &&
            width >= CompactBadgeThreshold));
        if (m_compactBarLayout && m_mediaControlsExpanded)
        {
            CaptureButton().Visibility(visibility(
                m_videoOnlyActionsAllowed && m_showCaptureButton));
        }

        // Preserve the core playback controls at every size. Secondary items
        // yield progressively instead of being clipped by the window edge.
        bool showFileNavigation = width >= 680.0;
        PreviousChapterButton().Visibility(visibility(showFileNavigation));
        NextChapterButton().Visibility(visibility(showFileNavigation));

        // Bar Compact: with the full optional toolbar enabled, a ~1718-DIP
        // normal window is the first width where the +/-10 buttons make the
        // second row feel crowded. Let BOTH seek-step buttons yield there while
        // keeping every saved preference intact; they return automatically once
        // the row grows past 1700 DIP (roughly a 1720-DIP outer window with the
        // current 10-DIP side padding). The frozen Full bar keeps its established
        // responsive behavior.
        constexpr double CompactSeekButtonsThreshold = 1700.0;
        constexpr double FullSeekBackwardButtonThreshold = 560.0;

        bool const compactSeekButtonsHaveRoom =
            !m_compactBarLayout ||
            !m_mediaControlsExpanded ||
            width >= CompactSeekButtonsThreshold;

        SeekBackwardButton().Visibility(visibility(
            m_showSeekButtons &&
            compactSeekButtonsHaveRoom &&
            (m_compactBarLayout ||
                width >= FullSeekBackwardButtonThreshold)));

        SeekForwardButton().Visibility(visibility(
            m_showSeekButtons && compactSeekButtonsHaveRoom));

        // The compact clock is a high-priority transport element.
        // Secondary actions already collapse before the minimum-window range,
        // and real layout testing shows enough center space for the normal
        // clock even with the ±10-second controls enabled.
        constexpr double NormalTimeDisplayThreshold = 500.0;

        TimeDisplayHost().Visibility(
            visibility(width >= NormalTimeDisplayThreshold));

        // Profiles are secondary. Preserve the core controls in narrow
        // windows and only reveal this opt-in button when profiles exist.
        constexpr double ProfilesButtonThreshold = 780.0;
        ProfilesButton().Visibility(visibility(
            m_videoOnlyActionsAllowed &&
            m_showProfilesButton &&
            m_hasImportedProfiles &&
            width >= ProfilesButtonThreshold));

        StatsButton().Visibility(visibility(
            m_showStatsButton && width >= 780.0));

        // Secondary transport actions yield before the compact time readout.
        // Their saved preferences remain untouched; they automatically return
        // as soon as enough horizontal room is available.
        constexpr double SecondaryActionThreshold = 720.0;

        ShuffleButton().Visibility(visibility(
            m_showShuffleButton && width >= SecondaryActionThreshold));

        MediaInfoButton().Visibility(visibility(
            m_showMediaInfoButton && width >= SecondaryActionThreshold));

        // In a narrow snapped window, hide the whole volume cluster (icon +
        // horizontal slider) so the time readout keeps the available space.
        // Both return automatically when the window becomes wider again.
        bool showVolumeControls = width >= 900.0;
        VolumeIcon().Visibility(visibility(showVolumeControls));
        VolumeSliderHost().Visibility(visibility(showVolumeControls));

        double volumeWidth = width < 680.0 ? 64.0 : 92.0;
        VolumeSliderHost().Width(volumeWidth);
        VolumeSlider().Width(volumeWidth);
        StandardVolumeSlider().Width(volumeWidth);
    }

    void MainPage::SetTransportVisible(bool visible, bool animate)
    {
        if (visible && m_consoleOpen)
        {
            return;
        }

        // Restore the real XAML island only when the transport is actually
        // transitioning from hidden to visible. Reasserting it while already
        // visible causes redundant native relayouts, especially in PiP and
        // borderless mode where the bottom hot-zone is polled frequently.
        if (visible && !m_transportVisible)
        {
            PlayerSetTransportHostVisible(true);
        }

        if (m_transportVisible == visible && animate)
            return;

        m_transportVisible = visible;

        if (!visible)
        {
            MinimalTimelineHoverPopup().IsOpen(false);
        }

        auto root = TransportRoot();
        auto visual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(root);
        m_transportCollapseTimer.Stop();
        if (!animate)
        {
            visual.StopAnimation(L"Opacity");
            visual.StopAnimation(L"Offset.Y");
            visual.Opacity(visible ? 1.0f : 0.0f);
            visual.Offset({ 0.0f, visible ? 0.0f : 18.0f, 0.0f });
            if (!visible) PlayerSetTransportHostVisible(false);
        }
        else
        {
            // Run opacity directly in the Windows compositor. This stays smooth
            // even while libmpv is presenting frames on the UI thread.
            auto compositor = visual.Compositor();
            auto animation = compositor.CreateScalarKeyFrameAnimation();
            auto easing = compositor.CreateCubicBezierEasingFunction(visible
                ? Windows::Foundation::Numerics::float2{ 0.16f, 1.0f }
                : Windows::Foundation::Numerics::float2{ 0.4f, 0.0f },
                visible
                ? Windows::Foundation::Numerics::float2{ 0.30f, 1.0f }
            : Windows::Foundation::Numerics::float2{ 1.0f, 1.0f });
            animation.InsertKeyFrame(1.0f, visible ? 1.0f : 0.0f, easing);
            animation.Duration(std::chrono::milliseconds(visible ? 170 : 210));
            visual.StartAnimation(L"Opacity", animation);

            auto slide = compositor.CreateScalarKeyFrameAnimation();
            slide.InsertKeyFrame(1.0f, visible ? 0.0f : 18.0f, easing);
            slide.Duration(std::chrono::milliseconds(visible ? 190 : 210));
            visual.StartAnimation(L"Offset.Y", slide);
            if (!visible) m_transportCollapseTimer.Start();
        }

        // Keep subtitles raised for the entire fade-out.  They return to their
        // normal position only after the bar has completely disappeared.
        if (visible || !animate)
        {
            PlayerSetTransportVisible(visible && !m_settingsOverlayOpen);
        }
    }

    void MainPage::TransportCollapseTimerTick(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        m_transportCollapseTimer.Stop();
        if (!m_transportVisible && !m_settingsOverlayOpen)
        {
            // Collapse the native island only after the 300 ms fade finishes.
            // The MPV video is already full-height underneath it.
            PlayerSetTransportHostVisible(false);
            PlayerSetTransportVisible(false);
        }
    }

    void MainPage::ScheduleTransportHide(std::chrono::milliseconds delay)
    {
        // While the player is still in its initial "Nenhuma mídia aberta"
        // state, the bottom bar is part of the empty-state UI and must remain
        // visible. Once a media item is successfully opened, m_ready becomes
        // true and the normal auto-hide behavior resumes unchanged.
        if (!m_ready)
        {
            m_transportHideTimer.Stop();
            return;
        }

        auto const now = std::chrono::steady_clock::now();
        auto const requestedHide = now + delay;
        if (requestedHide < m_transportHideNotBefore)
        {
            delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                m_transportHideNotBefore - now);
        }
        m_transportHideTimer.Stop();
        m_transportHideTimer.Interval(delay);
        m_transportHideTimer.Start();
    }

    void MainPage::TransportPointerEntered(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        // A pointer can enter the XAML transport because the host moved under
        // a stationary cursor while a new item is still starting.  Do not let
        // that bypass the same startup/real-movement gate used by the video
        // surface and native hot-zone polling.
        if (m_settingsOverlayOpen ||
            m_transportStartupGuard ||
            !m_transportRevealArmed)
        {
            return;
        }
        m_transportHideTimer.Stop();
        SetTransportVisible(true);
    }

    void MainPage::TransportHostPointerEntered()
    {
        // This entry point is also called by HCPlayer.cpp's native 50-ms hot
        // zone timer and by the transport-host HWND subclass.  Both routes must
        // obey the startup guard; otherwise they can reveal the bar even while
        // TransportVideoPointerMoved() is correctly rejecting startup motion.
        if (m_settingsOverlayOpen ||
            m_transportStartupGuard ||
            !m_transportRevealArmed)
        {
            return;
        }
        m_transportHideTimer.Stop();
        SetTransportVisible(true);
    }

    void MainPage::TransportPointerExited(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        if (m_settingsOverlayOpen || m_transportFlyoutOpen) return;
        ScheduleTransportHide(std::chrono::milliseconds(180));
    }

    void MainPage::TransportHostPointerExited()
    {
        if (m_settingsOverlayOpen || m_transportFlyoutOpen) return;
        ScheduleTransportHide(std::chrono::milliseconds(180));
    }

    void MainPage::TransportVideoPointerMoved(bool overControls)
    {
        if (m_settingsOverlayOpen || m_transportStartupGuard) return;

        // HCPlayer.cpp filters layout-generated WM_MOUSEMOVE events before this
        // call. Reaching here therefore means the user actually moved the mouse.
        // The startup guard above intentionally discards movement that happened
        // after ShowWindow() but before mpv had a presentable current item.
        m_transportRevealArmed = true;

        if (overControls)
        {
            SetTransportVisible(true);
            m_transportHideTimer.Stop();
        }
        else if (m_transportVisible && !m_transportFlyoutOpen)
        {
            ScheduleTransportHide(std::chrono::milliseconds(180));
        }
    }

    void MainPage::TransportHideTimerTick(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        m_transportHideTimer.Stop();
        if (!m_settingsOverlayOpen && !m_transportFlyoutOpen)
        {
            SetTransportVisible(false);
        }
    }

    void MainPage::MainPageLoaded(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        std::wstring savedVolume;
        if (PlayerTryGetSavedMpvOption(L"volume", savedVolume))
        {
            try
            {
                double value = std::stod(savedVolume);
                VolumeSlider().Value(value);
                StandardVolumeSlider().Value(value);
            }
            catch (...) {}
        }
        m_controlsReady = true;
        ShowVolumeFeedback();
        UpdateSpeedLabel(PlayerGetPlaybackSpeed());
        m_loopEnabled = PlayerGetLooping();
        UpdateLoopButtonState();
        m_shuffleEnabled = PlayerGetPlaylistShuffle();
        UpdateShuffleButtonState();
        RefreshInterfacePreferences();


        // Normal-window placement from the very first frame. PiP has its own
        // unchanged placement in SetPictureInPictureMode().
        if (!m_pictureInPicture)
        {
            TimeDisplayHost().Margin(
                Microsoft::UI::Xaml::Thickness{ 84.0, 0.0, 0.0, 0.0 });
            TimeDisplayHost().Translation(
                Windows::Foundation::Numerics::float3{ 0.0f, 1.5f, 0.0f });
        }

        ScheduleTransportHide(std::chrono::milliseconds(1800));
    }

    bool MainPage::OpenPath(std::wstring const& path)
    {
        if (m_thumbnailController)
        {
            m_thumbnailController->Cancel();
        }
        HideThumbnailPreview();

        NowPlayingText().Text(std::filesystem::path(path).filename().wstring());
        ClearMediaBadges();
        m_chapters.clear();
        m_chapterRefreshCountdown = 0;
        m_chapterMarkerDuration = 0.0;
        m_minimalChapterMarkerDuration = 0.0;
        m_minimalChapterMarkerWidth = 0.0;
        m_minimalChapterSegments.clear();
        ChapterMarkers().Children().Clear();
        MinimalChapterMarkers().Children().Clear();

        // A fresh explicit open begins visually at zero immediately instead of
        // showing the previous movie's seek position until the next timer tick.
        m_isUpdatingPosition = true;
        PositionSlider().Value(0.0);
        MinimalPositionSlider().Value(0.0);
        m_isUpdatingPosition = false;
        ElapsedTimeText().Text(L"0:00 / --:--");
        MinimalCurrentTimeText().Text(L"0:00");
        MinimalDurationTimeText().Text(L"--:--");
        FilledTimelinePositionMarker().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);

        // Never carry video-only actions from the outgoing item into the new
        // asynchronous load. They return only after the new media is classified.
        m_videoOnlyActionsAllowed = false;
        UpdateVideoOnlyActionVisibility();

        m_ready = PlayerLoadFile(path);
        m_shuffleEnabled = PlayerGetPlaylistShuffle();
        UpdateShuffleButtonState();
        EngineStatusText().Text(
            m_ready ? std::wstring{} : MainPageString(
                L"MainPageDynPlaybackComponentMissing",
                L"Componente de reprodução não encontrado"));
        EngineStatusText().Visibility(m_ready
            ? Microsoft::UI::Xaml::Visibility::Collapsed
            : Microsoft::UI::Xaml::Visibility::Visible);

        if (m_ready)
        {
            SetMediaControlsExpanded(true);

            // Fresh media starts clean: no transport is shown automatically.
            // The native bottom hot-zone will reveal it on genuine pointer
            // movement, while the periodic fallback stays disarmed until then.
            m_transportRevealArmed = false;
            m_transportStartupGuard = true;
            m_transportStartupGuardTicks = 0;
            m_transportStartupReadySamples = 0;
            m_transportHideTimer.Stop();
            m_transportCollapseTimer.Stop();
            m_transportHideNotBefore = {};
            SetTransportVisible(false, false);

            m_isPlaying = true;
            m_isReplay = false;
            m_lastEofReached = false;
            UpdatePlayButtonState();
        }

        return m_ready;
    }

    void MainPage::RestorePlayerState(std::wstring const& path)
    {
        m_ready = PlayerEngineReady();
        m_videoOnlyActionsAllowed = false;
        m_shuffleEnabled = PlayerGetPlaylistShuffle();
        UpdateShuffleButtonState();
        SetMediaControlsExpanded(!path.empty());
        if (!path.empty())
        {
            RefreshVideoOnlyActionEligibility();
        }
        if (path.empty())
        {
            NowPlayingText().Text(MainPageString(
                L"MainPageDynNoMediaOpen", L"Nenhuma mídia aberta"));
            EngineStatusText().Text(L"");
            EngineStatusText().Visibility(
                Microsoft::UI::Xaml::Visibility::Collapsed);
        }
        else
        {
            NowPlayingText().Text(std::filesystem::path(path).filename().wstring());
            EngineStatusText().Text(
                m_ready ? std::wstring{} : MainPageString(
                    L"MainPageDynPreparingPlayer", L"Preparando player..."));
            EngineStatusText().Visibility(m_ready
                ? Microsoft::UI::Xaml::Visibility::Collapsed
                : Microsoft::UI::Xaml::Visibility::Visible);
        }
        if (m_ready)
        {
            m_isPlaying = true;
            m_isReplay = false;
            m_lastEofReached = false;
            UpdatePlayButtonState();
        }
    }

    void MainPage::ClearPlayerState()
    {
        using Microsoft::UI::Xaml::Visibility;

        if (m_thumbnailController)
        {
            m_thumbnailController->Cancel();
        }
        HideThumbnailPreview();

        // The mpv engine itself remains alive (idle=yes), but HC Player returns
        // to the same compact state it has before any media is opened.
        m_ready = false;
        m_isPlaying = false;
        m_isReplay = false;
        m_shuffleEnabled = false;
        m_webCacheEnd = 0.0;
        m_progressTickCount = 0;
        m_chapterRefreshCountdown = 0;
        m_chapterMarkerDuration = 0.0;
        m_chapterMarkerWidth = 0.0;
        m_minimalChapterMarkerDuration = 0.0;
        m_minimalChapterMarkerWidth = 0.0;
        m_hoveredChapterSegment = -1;

        m_transportHideTimer.Stop();
        m_transportCollapseTimer.Stop();

        m_chapters.clear();
        m_chapterSegments.clear();
        m_minimalChapterSegments.clear();

        ChapterMarkers().Children().Clear();
        MinimalChapterMarkers().Children().Clear();
        ChapterHoverCard().Visibility(Visibility::Collapsed);
        ChapterHoverPopup().IsOpen(false);
        FilledTimelinePositionMarker().Visibility(Visibility::Collapsed);
        SetFilledTimelineHovered(false);

        m_timelineUserInteraction = false;
        m_timelineProgressHoldTicks = 0;
        m_timelineInteractionHasTarget = false;
        m_timelineInteractionSeconds = 0.0;

        m_isUpdatingPosition = true;
        PositionSlider().Value(0.0);
        MinimalPositionSlider().Value(0.0);
        m_isUpdatingPosition = false;

        ElapsedTimeText().Text(L"0:00 / --:--");
        MinimalCurrentTimeText().Text(L"0:00");
        MinimalDurationTimeText().Text(L"--:--");
        NowPlayingText().Text(MainPageString(
                L"MainPageDynNoMediaOpen", L"Nenhuma mídia aberta"));
        EngineStatusText().Text(L"");
        EngineStatusText().Visibility(Visibility::Collapsed);

        ClearMediaBadges();

        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            PreviousChapterButton(),
            MainPageBoxString(
                L"MainPageDynPreviousFile", L"Arquivo anterior"));
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            NextChapterButton(),
            MainPageBoxString(
                L"MainPageDynNextFile", L"Próximo arquivo"));

        // Collapses timeline + playback row and restores the approved compact
        // no-media transport height. This also clears image-only positioning.
        m_videoOnlyActionsAllowed = false;
        SetMediaControlsExpanded(false);
        UpdateVideoOnlyActionVisibility();

        UpdateShuffleButtonState();
        UpdatePlayButtonState();

        // The empty-state bar stays visible, matching application startup.
        m_transportStartupGuard = false;
        m_transportStartupGuardTicks = 0;
        m_transportStartupReadySamples = 0;
        m_transportRevealArmed = true;
        SetTransportVisible(true, false);
        ScheduleTransportHide(std::chrono::milliseconds(1800));
    }

    void MainPage::OpenClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        OpenFromDialog();
    }

    void MainPage::OpenFolderClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Reuse the already established native folder/DVD/Blu-ray picker.
        // The normal Open button click remains exactly OpenFromDialog().
        PlayerShowOpenFolderDialog();
    }

    void MainPage::OpenFromDialog()
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        winrt::check_hresult(CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.put())));

        FILEOPENDIALOGOPTIONS options{};
        winrt::check_hresult(dialog->GetOptions(&options));
        options |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        winrt::check_hresult(dialog->SetOptions(options));

        auto mediaFilesLabel = MainPageString(
            L"MainPageDynMediaFiles", L"Arquivos de mídia");
        auto videoFilesLabel = MainPageString(
            L"MainPageDynVideoFiles", L"Arquivos de vídeo");
        auto audioFilesLabel = MainPageString(
            L"MainPageDynAudioFiles", L"Arquivos de áudio");
        auto playlistsDiscLabel = MainPageString(
            L"MainPageDynPlaylistsDiscImages", L"Playlists e imagens de disco");
        auto imageFilesLabel = MainPageString(
            L"MainPageDynImageFiles", L"Arquivos de imagem");
        auto allFilesLabel = MainPageString(
            L"MainPageDynAllFiles", L"Todos os arquivos");

        COMDLG_FILTERSPEC filters[] = {
            { mediaFilesLabel.c_str(), L"*.mkv;*.mk3d;*.mp4;*.m4v;*.mov;*.webm;*.avi;*.wmv;*.asf;*.flv;*.ts;*.mts;*.m2ts;*.mpg;*.mpeg;*.vob;*.ogv;*.rm;*.rmvb;*.3gp;*.3g2;*.divx;*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.oga;*.opus;*.wav;*.wma;*.aiff;*.aif;*.ape;*.mka;*.ac3;*.eac3;*.dts;*.dtshd;*.m3u;*.m3u8;*.pls;*.cue;*.iso;*.avif;*.bmp;*.gif;*.jpeg;*.jpg;*.jxl;*.png;*.svg;*.tga;*.tif;*.tiff;*.webp" },
            { videoFilesLabel.c_str(), L"*.mkv;*.mk3d;*.mp4;*.m4v;*.mov;*.webm;*.avi;*.wmv;*.asf;*.flv;*.ts;*.mts;*.m2ts;*.mpg;*.mpeg;*.vob;*.ogv;*.rm;*.rmvb;*.3gp;*.3g2;*.divx" },
            { audioFilesLabel.c_str(), L"*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.oga;*.opus;*.wav;*.wma;*.aiff;*.aif;*.ape;*.mka;*.ac3;*.eac3;*.dts;*.dtshd" },
            { playlistsDiscLabel.c_str(), L"*.m3u;*.m3u8;*.pls;*.cue;*.iso" },
            { imageFilesLabel.c_str(), L"*.avif;*.bmp;*.gif;*.jpeg;*.jpg;*.jxl;*.png;*.svg;*.tga;*.tif;*.tiff;*.webp" },
            { allFilesLabel.c_str(), L"*.*" }
        };
        winrt::check_hresult(dialog->SetFileTypes(ARRAYSIZE(filters), filters));
        winrt::check_hresult(dialog->SetFileTypeIndex(1));
        auto dialogTitle = MainPageString(L"MainPageDynOpenMedia", L"Abrir mídia");
        winrt::check_hresult(dialog->SetTitle(dialogTitle.c_str()));

        HWND owner = static_cast<HWND>(PlayerGetMainWindowHandle());
        if (SUCCEEDED(dialog->Show(owner)))
        {
            winrt::com_ptr<IShellItem> item;
            winrt::check_hresult(dialog->GetResult(item.put()));

            PWSTR rawPath{};
            winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
            std::wstring path{ rawPath };
            CoTaskMemFree(rawPath);

            NowPlayingText().Text(std::filesystem::path(path).filename().wstring());
            m_chapters.clear();
            m_chapterRefreshCountdown = 0;
            m_chapterMarkerDuration = 0.0;
            ChapterMarkers().Children().Clear();

            // OpenFromDialog has its own load path; apply the same conservative
            // reset used by OpenPath so an outgoing video's actions cannot leak.
            m_videoOnlyActionsAllowed = false;
            UpdateVideoOnlyActionVisibility();

            m_ready = PlayerLoadFile(path);
            m_shuffleEnabled = PlayerGetPlaylistShuffle();
            UpdateShuffleButtonState();
            EngineStatusText().Text(
                m_ready ? std::wstring{} : MainPageString(
                L"MainPageDynPlaybackComponentMissing",
                L"Componente de reprodução não encontrado"));
            EngineStatusText().Visibility(m_ready
                ? Microsoft::UI::Xaml::Visibility::Collapsed
                : Microsoft::UI::Xaml::Visibility::Visible);

            if (m_ready)
            {
                SetMediaControlsExpanded(true);

                // Match every other explicit media open: the video starts with
                // a clean surface and controls appear only after real pointer
                // movement reaches the bottom hot-zone.
                m_transportRevealArmed = false;
                m_transportStartupGuard = true;
                m_transportStartupGuardTicks = 0;
                m_transportStartupReadySamples = 0;
                m_transportHideTimer.Stop();
                m_transportCollapseTimer.Stop();
                m_transportHideNotBefore = {};
                SetTransportVisible(false, false);

                m_isPlaying = true;
                m_isReplay = false;
                m_lastEofReached = false;
                // Keep both the normal and Minimal play glyphs synchronized on
                // the very first frame opened through the file dialog.
                UpdatePlayButtonState();
            }
        }
    }

    void MainPage::PlayClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        bool paused{};
        bool eofReached{};
        PlayerGetPlaybackState(paused, eofReached);
        if (eofReached || m_isReplay)
        {
            PlayerReplay();
            m_isReplay = false;
            m_isPlaying = true;
        }
        else
        {
            PlayerTogglePause();
            m_isPlaying = paused;
        }
        UpdatePlayButtonState();
    }

    void MainPage::SeekBackwardClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        PlayerSeekRelative(-10.0);
    }

    void MainPage::SeekForwardClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        PlayerSeekRelative(10.0);
    }

    void MainPage::PreviousChapterClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // No PiP de áudio estes botões são navegação de faixa, mesmo se o
        // arquivo atual possuir capítulos internos. Fora desse caso, mantém
        // exatamente a semântica capítulo/arquivo já existente.
        if (m_pictureInPicture && PlayerIsCurrentMediaAudio())
            PlayerChangePlaylistItem(-1);
        else
            PlayerChangeChapter(-1);
    }

    void MainPage::NextChapterClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_pictureInPicture && PlayerIsCurrentMediaAudio())
            PlayerChangePlaylistItem(1);
        else
            PlayerChangeChapter(1);
    }

    void MainPage::LoopClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        m_loopEnabled = !m_loopEnabled;
        PlayerSetLooping(m_loopEnabled);
        UpdateLoopButtonState();
    }

    void MainPage::ShuffleClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        bool const requested = !m_shuffleEnabled;
        if (PlayerSetPlaylistShuffle(requested))
        {
            m_shuffleEnabled = requested;
            UpdateShuffleButtonState();
        }
    }

    void MainPage::UpdatePlayButtonState()
    {
        auto glyph = m_isReplay ? L"\uE72C" :
            (m_isPlaying ? L"\uE769" : L"\uE768");
        if (PlayIcon().Glyph() != glyph) PlayIcon().Glyph(glyph);
        if (MinimalPlayIcon().Glyph() != glyph) MinimalPlayIcon().Glyph(glyph);
        auto const tooltip = m_isReplay
            ? MainPageBoxString(L"MainPageDynPlayAgain", L"Reproduzir novamente")
            : (m_isPlaying
                ? MainPageBoxString(L"MainPageDynPause", L"Pausar")
                : MainPageBoxString(L"MainPageDynPlay", L"Reproduzir"));
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            PlayButton(), tooltip);
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            MinimalPlayButton(), tooltip);
    }

    void MainPage::UpdateLoopButtonState()
    {
        // Reuse the same live ThemeResource brush as the timeline/volume.
        // TimelineProgressBrush resolves to the Windows SystemAccentColor,
        // so an active Loop follows the user's accent instead of a fixed blue.
        auto accent = Resources().Lookup(
            winrt::box_value(L"TimelineProgressBrush"))
            .as<Microsoft::UI::Xaml::Media::Brush>();
        auto inactive = Microsoft::UI::Xaml::Media::SolidColorBrush{
            PlayerIsLightTheme()
                ? Windows::UI::Color{ 255, 28, 28, 28 }
                : Windows::UI::Color{ 255, 245, 245, 245 } };
        LoopIcon().Foreground(m_loopEnabled ? accent : inactive);
        LoopButton().Foreground(m_loopEnabled ? accent : inactive);
        LoopIcon().Opacity(1.0);
        MinimalLoopIcon().Foreground(m_loopEnabled ? accent : inactive);
        MinimalLoopButton().Foreground(m_loopEnabled ? accent : inactive);
        MinimalLoopIcon().Opacity(1.0);
        auto const loopTooltip = m_loopEnabled
            ? MainPageBoxString(L"MainPageDynDisableRepeat", L"Desativar repetição")
            : MainPageBoxString(L"MainPageDynEnableRepeat", L"Ativar repetição");
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            LoopButton(), loopTooltip);
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            MinimalLoopButton(), loopTooltip);
    }

    void MainPage::UpdateShuffleButtonState()
    {
        // Same live Windows accent used by the player timeline/volume.
        auto accent = Resources().Lookup(
            winrt::box_value(L"TimelineProgressBrush"))
            .as<Microsoft::UI::Xaml::Media::Brush>();
        auto inactive = Microsoft::UI::Xaml::Media::SolidColorBrush{
            PlayerIsLightTheme()
                ? Windows::UI::Color{ 255, 28, 28, 28 }
                : Windows::UI::Color{ 255, 245, 245, 245 } };

        ShuffleIcon().Foreground(m_shuffleEnabled ? accent : inactive);
        ShuffleButton().Foreground(m_shuffleEnabled ? accent : inactive);
        ShuffleIcon().Opacity(1.0);

        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            ShuffleButton(), m_shuffleEnabled
                ? MainPageBoxString(
                    L"MainPageDynDisableShuffle", L"Desativar reprodução aleatória")
                : MainPageBoxString(
                    L"MainPageDynEnableShuffle", L"Ativar reprodução aleatória"));
    }

    void MainPage::RefreshThemeVisuals()
    {
        m_chapterThemeInitialized = false;
        m_minimalChapterMarkerDuration = 0.0;
        m_minimalChapterMarkerWidth = 0.0;
        m_minimalChapterSegments.clear();
        UpdateLoopButtonState();
        UpdateShuffleButtonState();
        ApplyTransportStyleVisuals();

        // O tema mudou: recarrega apenas a aparência dos badges
        // usando as versões Light/Dark correspondentes.
        ApplyMediaBadgeVisuals();

        double elapsed{};
        double duration{};
        if (PlayerGetPlaybackTimes(elapsed, duration) && duration > 0.0)
            RenderChapterMarkers(duration, elapsed);
    }

    void MainPage::ClearMediaBadges()
    {
        m_sourceBadge = MediaSourceBadge::None;
        m_videoBadge = MediaVideoBadge::None;
        m_audioBadge = MediaAudioBadge::None;
        m_badgeVideoTrackId = -1;
        m_badgeAudioTrackId = -1;
        m_hdr10PlusDetected = false;
        m_hdr10PlusHitCount = 0;
        m_hdr10PlusMissCount = 0;
        ++m_badgeVisualGeneration;

        SourceBadgeImage().Source(nullptr);
        VideoBadgeImage().Source(nullptr);
        AudioBadgeImage().Source(nullptr);

        HlsSourceBadge().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);
        LiveSourceBadge().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);
        SourceBadgeImage().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);
        VideoBadgeImage().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);
        AudioBadgeImage().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);
        MediaBadgesHost().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);
    }

    void MainPage::ApplyMediaBadgeVisuals()
    {
        using Microsoft::UI::Xaml::Visibility;
        using Microsoft::UI::Xaml::Media::Imaging::BitmapImage;

        bool const lightTheme = PlayerIsLightTheme();
        std::wstring savedBadgeStyle;
        bool const customStyle =
            PlayerTryGetSavedMpvOption(
                L"ui-media-badge-style", savedBadgeStyle) &&
            savedBadgeStyle == L"custom";

        std::uint64_t const generation = ++m_badgeVisualGeneration;

        // HC Player's built-in typography badges remain the absolute default.
        // These are the already-approved sizes and offsets.
        SourceBadgeImage().Height(22.0);
        SourceBadgeImage().MaxWidth(132.0);
        VideoBadgeImage().Height(22.0);
        VideoBadgeImage().MaxWidth(132.0);
        AudioBadgeImage().Height(22.0);
        AudioBadgeImage().MaxWidth(132.0);

        SourceBadgeImage().Margin(Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 0 });
        VideoBadgeImage().Margin(Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 0 });
        AudioBadgeImage().Margin(Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 0 });

        auto setPackagedBadge =
            [](Microsoft::UI::Xaml::Controls::Image const& image,
                wchar_t const* fileName)
            {
                if (!fileName || !*fileName)
                {
                    image.Source(nullptr);
                    image.Visibility(Visibility::Collapsed);
                    return;
                }

                std::wstring uri = L"ms-appx:///Assets/MediaBadges/";
                uri += fileName;

                BitmapImage bitmap{};
                image.Source(bitmap);
                bitmap.UriSource(
                    Windows::Foundation::Uri{ winrt::hstring{ uri } });

                image.Visibility(Visibility::Visible);
            };

        wchar_t const* sourceFile = nullptr;
        std::wstring sourceCustomPath;

        bool const hasHlsSourceBadge =
            m_sourceBadge == MediaSourceBadge::HLS ||
            m_sourceBadge == MediaSourceBadge::HLSLive;
        bool const hasLiveSourceBadge =
            m_sourceBadge == MediaSourceBadge::HLSLive ||
            m_sourceBadge == MediaSourceBadge::YouTubeLive;

        HlsSourceBadge().Visibility(
            hasHlsSourceBadge ? Visibility::Visible : Visibility::Collapsed);
        LiveSourceBadge().Visibility(
            hasLiveSourceBadge ? Visibility::Visible : Visibility::Collapsed);

        switch (m_sourceBadge)
        {
        case MediaSourceBadge::DVD:
            // Keep the approved optical badge geometry exactly as-is. DVD
            // artwork is never shipped by HC Player, so an imported user asset
            // is the only source and must not depend on the global custom-style
            // toggle used by badges that still have packaged fallbacks.
            SourceBadgeImage().Height(18.0);
            SourceBadgeImage().MaxWidth(74.0);
            sourceCustomPath =
                PlayerGetCustomBadgePath(L"DVD", lightTheme);
            break;

        case MediaSourceBadge::BluRay:
            SourceBadgeImage().Height(19.0);
            SourceBadgeImage().MaxWidth(78.0);
            sourceCustomPath =
                PlayerGetCustomBadgePath(L"BluRay", lightTheme);
            break;

        case MediaSourceBadge::YouTube:
        case MediaSourceBadge::YouTubeLive:
            sourceFile = lightTheme
                ? L"HC.YouTube.Light.png"
                : L"HC.YouTube.png";
            if (customStyle)
            {
                sourceCustomPath =
                    PlayerGetCustomBadgePath(L"YouTube", lightTheme);
            }
            break;

        default:
            break;
        }

        wchar_t const* videoFile = nullptr;
        std::wstring videoCustomPath;

        switch (m_videoBadge)
        {
        case MediaVideoBadge::DolbyVision:
            VideoBadgeImage().Height(24.0);
            VideoBadgeImage().Margin(
                Microsoft::UI::Xaml::Thickness{ 0, 2, 0, -2 });
            videoFile = lightTheme
                ? L"HC.DolbyVision.Light.png"
                : L"HC.DolbyVision.png";
            if (customStyle)
            {
                videoCustomPath =
                    PlayerGetCustomBadgePath(L"DolbyVision", lightTheme);
            }
            break;

        case MediaVideoBadge::HDR10Plus:
            VideoBadgeImage().Height(22.0);
            videoFile = lightTheme
                ? L"HC.HDR10Plus.Light.png"
                : L"HC.HDR10Plus.png";
            if (customStyle)
            {
                videoCustomPath =
                    PlayerGetCustomBadgePath(L"HDR10Plus", lightTheme);
            }
            break;

        case MediaVideoBadge::HDR:
            VideoBadgeImage().Height(9.0);
            videoFile = lightTheme
                ? L"HC.HDR.Light.png"
                : L"HC.HDR.png";
            if (customStyle)
            {
                videoCustomPath =
                    PlayerGetCustomBadgePath(L"HDR", lightTheme);
            }
            break;

        default:
            break;
        }

        wchar_t const* audioFile = nullptr;
        std::wstring audioCustomPath;

        switch (m_audioBadge)
        {
        case MediaAudioBadge::DolbyAudio:
            AudioBadgeImage().Height(25.0);
            AudioBadgeImage().Margin(
                Microsoft::UI::Xaml::Thickness{ 0, 2, 0, -2 });
            audioFile = lightTheme
                ? L"HC.DolbyAudio.Light.png"
                : L"HC.DolbyAudio.png";
            if (customStyle)
            {
                audioCustomPath =
                    PlayerGetCustomBadgePath(L"DolbyAudio", lightTheme);
            }
            break;

        case MediaAudioBadge::DolbyAtmos:
            AudioBadgeImage().Height(22.0);
            audioFile = lightTheme
                ? L"HC.DolbyAtmos.Light.png"
                : L"HC.DolbyAtmos.png";
            if (customStyle)
            {
                audioCustomPath =
                    PlayerGetCustomBadgePath(L"DolbyAtmos", lightTheme);
            }
            break;

        case MediaAudioBadge::DTS:
            AudioBadgeImage().Height(9.5);
            audioFile = lightTheme
                ? L"HC.DTS.Light.png"
                : L"HC.DTS.png";
            if (customStyle)
            {
                audioCustomPath =
                    PlayerGetCustomBadgePath(L"DTS", lightTheme);
            }
            break;

        case MediaAudioBadge::DTSX:
            AudioBadgeImage().Height(19.0);
            audioFile = lightTheme
                ? L"HC.DTSX.Light.png"
                : L"HC.DTSX.png";
            if (customStyle)
            {
                audioCustomPath =
                    PlayerGetCustomBadgePath(L"DTSX", lightTheme);
            }
            break;

        default:
            break;
        }

        // When a custom asset exists, restore the exact visual boxes used by
        // HC Player's former logo badges. This makes the user's own artwork
        // occupy the same size and position while leaving the modern built-in
        // typography badges completely untouched.
        if (!sourceCustomPath.empty() &&
            m_sourceBadge != MediaSourceBadge::DVD &&
            m_sourceBadge != MediaSourceBadge::BluRay)
        {
            SourceBadgeImage().Height(14.0);
            SourceBadgeImage().MaxWidth(100.0);
        }

        if (!videoCustomPath.empty())
        {
            VideoBadgeImage().Height(15.0);
            VideoBadgeImage().MaxWidth(100.0);
            VideoBadgeImage().Margin(
                Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 0 });
        }

        if (!audioCustomPath.empty())
        {
            AudioBadgeImage().Height(
                m_audioBadge == MediaAudioBadge::DTSX
                    ? 17.0
                    : 15.0);
            AudioBadgeImage().MaxWidth(100.0);
            AudioBadgeImage().Margin(
                Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 0 });
        }

        // Put the HC Player badge in place first. If a custom file is corrupt
        // or cannot be opened, the user still sees the safe built-in fallback.
        setPackagedBadge(SourceBadgeImage(), sourceFile);
        setPackagedBadge(VideoBadgeImage(), videoFile);
        setPackagedBadge(AudioBadgeImage(), audioFile);

        if (!sourceCustomPath.empty())
        {
            LoadCustomBadgeImageAsync(
                SourceBadgeImage(), sourceCustomPath, generation);
        }
        if (!videoCustomPath.empty())
        {
            LoadCustomBadgeImageAsync(
                VideoBadgeImage(), videoCustomPath, generation);
        }
        if (!audioCustomPath.empty())
        {
            LoadCustomBadgeImageAsync(
                AudioBadgeImage(), audioCustomPath, generation);
        }

        bool const hasSourceBadge =
            hasHlsSourceBadge ||
            sourceFile != nullptr ||
            !sourceCustomPath.empty();
        bool const hasVideoBadge = videoFile != nullptr;
        bool const hasAudioBadge = audioFile != nullptr;
        bool const hasBadge = hasSourceBadge || hasVideoBadge || hasAudioBadge;

        SourceBadgeDivider().Visibility(
            hasSourceBadge && (hasVideoBadge || hasAudioBadge)
            ? Visibility::Visible
            : Visibility::Collapsed);

        MediaBadgeDivider().Visibility(
            hasVideoBadge && hasAudioBadge
            ? Visibility::Visible
            : Visibility::Collapsed);

        MediaBadgesHost().Visibility(
            hasBadge
            ? Visibility::Visible
            : Visibility::Collapsed);
    }

    winrt::fire_and_forget MainPage::LoadCustomBadgeImageAsync(
        Microsoft::UI::Xaml::Controls::Image image,
        std::wstring path,
        std::uint64_t generation)
    {
        auto lifetime = get_strong();
        (void)lifetime;

        try
        {
            auto file = co_await Windows::Storage::StorageFile::
                GetFileFromPathAsync(winrt::hstring{ path });
            auto stream = co_await file.OpenAsync(
                Windows::Storage::FileAccessMode::Read);

            if (generation != m_badgeVisualGeneration)
            {
                co_return;
            }

            std::wstring extension =
                std::filesystem::path{ path }.extension().wstring();
            std::transform(
                extension.begin(),
                extension.end(),
                extension.begin(),
                towlower);

            if (extension == L".svg")
            {
                Microsoft::UI::Xaml::Media::Imaging::SvgImageSource svg{};

                // Match HC Player's pre-license YouTube path as closely as
                // possible. The original packaged SVG was displayed in a 14
                // logical-pixel Image box and WinUI could derive its decode size
                // from that live layout. The V7 external-file path decodes the
                // stream before attaching the SvgImageSource to the Image, so
                // give YouTube the same explicit logical rasterization height.
                // Leave every other SVG badge on the existing V7 behavior.
                std::wstring const svgStem =
                    std::filesystem::path{ path }.stem().wstring();
                if (svgStem == L"YouTube.Dark" ||
                    svgStem == L"YouTube.Light")
                {
                    double rasterizeHeight = image.Height();
                    if (std::isfinite(rasterizeHeight) && rasterizeHeight > 0.0)
                    {
                        svg.RasterizePixelHeight(rasterizeHeight);
                    }
                }

                auto status = co_await svg.SetSourceAsync(stream);

                if (generation == m_badgeVisualGeneration &&
                    status == decltype(status)::Success)
                {
                    image.Source(svg);
                    image.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
                }
            }
            else
            {
                Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap{};

                // Stream-backed images do not get XAML's automatic right-sized
                // decoding in this code path because the BitmapImage is decoded
                // before it is attached to the live Image element. Decode the
                // user's PNG explicitly at the same logical height that the badge
                // will occupy. DecodePixelType::Logical lets WinUI account for
                // the current display scale (100%, 125%, 150%, ...), matching the
                // behavior of the former packaged badge assets much more closely.
                double decodeHeight = image.Height();
                if (!std::isfinite(decodeHeight) || decodeHeight <= 0.0)
                {
                    decodeHeight = image.ActualHeight();
                }

                if (std::isfinite(decodeHeight) && decodeHeight > 0.0)
                {
                    bitmap.DecodePixelType(
                        Microsoft::UI::Xaml::Media::Imaging::DecodePixelType::Logical);
                    bitmap.DecodePixelHeight(
                        static_cast<std::int32_t>(
                            (std::max)(1.0, std::ceil(decodeHeight))));
                }

                co_await bitmap.SetSourceAsync(stream);

                if (generation == m_badgeVisualGeneration)
                {
                    image.Source(bitmap);
                    image.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
                }
            }
        }
        catch (...)
        {
            // When a packaged fallback exists it was installed before this
            // load. Optional DVD/Blu-ray artwork has no packaged fallback, so
            // a bad external asset simply remains hidden.
        }
    }

    void MainPage::RefreshMediaBadges()
    {
        if (!m_ready)
        {
            return;
        }

        MediaBadgeInfo const info = PlayerGetMediaBadgeInfo();

        bool changed = false;

        // ------------------------------------------------------------
        // SOURCE
        // ------------------------------------------------------------

        if (info.sourceReady &&
            info.source != m_sourceBadge)
        {
            m_sourceBadge = info.source;
            changed = true;
        }

        // ------------------------------------------------------------
        // VIDEO
        // ------------------------------------------------------------

        if (info.videoTrackId >= 0 &&
            info.videoTrackId != m_badgeVideoTrackId)
        {
            m_badgeVideoTrackId = info.videoTrackId;
            m_videoBadge = MediaVideoBadge::None;

            m_hdr10PlusDetected = false;
            m_hdr10PlusHitCount = 0;
            m_hdr10PlusMissCount = 0;

            changed = true;
        }

        if (info.videoReady)
        {
            MediaVideoBadge detectedVideo = info.video;

            // Dolby Vision is definitive and also invalidates any HDR10+
            // state that may have existed before.
            if (detectedVideo == MediaVideoBadge::DolbyVision)
            {
                m_hdr10PlusDetected = false;
                m_hdr10PlusHitCount = 0;
                m_hdr10PlusMissCount = 0;
            }
            else if (detectedVideo == MediaVideoBadge::HDR10Plus)
            {
                m_hdr10PlusMissCount = 0;

                // Require two consecutive positive samples before confirming
                // HDR10+. This prevents stale metadata from the previous file
                // from immediately restoring the old badge.
                if (!m_hdr10PlusDetected)
                {
                    if (m_hdr10PlusHitCount < 2)
                    {
                        ++m_hdr10PlusHitCount;
                    }

                    if (m_hdr10PlusHitCount >= 2)
                    {
                        m_hdr10PlusDetected = true;
                    }
                }

                detectedVideo = m_hdr10PlusDetected
                    ? MediaVideoBadge::HDR10Plus
                    : MediaVideoBadge::None;
            }
            else
            {
                m_hdr10PlusHitCount = 0;

                // One missing sample is tolerated so real HDR10+ metadata
                // cannot make the badge flicker. Two consecutive misses clear it.
                if (m_hdr10PlusDetected)
                {
                    if (m_hdr10PlusMissCount < 2)
                    {
                        ++m_hdr10PlusMissCount;
                    }

                    if (m_hdr10PlusMissCount < 2)
                    {
                        detectedVideo = MediaVideoBadge::HDR10Plus;
                    }
                    else
                    {
                        m_hdr10PlusDetected = false;
                        m_hdr10PlusMissCount = 0;
                    }
                }
                else
                {
                    m_hdr10PlusMissCount = 0;
                }
            }

            if (detectedVideo != m_videoBadge)
            {
                m_videoBadge = detectedVideo;
                changed = true;
            }
        }

        // ------------------------------------------------------------
        // AUDIO
        // ------------------------------------------------------------

        if (info.audioTrackId >= 0 &&
            info.audioTrackId != m_badgeAudioTrackId)
        {
            m_badgeAudioTrackId = info.audioTrackId;

            if (m_audioBadge != MediaAudioBadge::None)
            {
                m_audioBadge = MediaAudioBadge::None;
                changed = true;
            }
        }

        if (info.audioReady &&
            info.audio != m_audioBadge)
        {
            m_audioBadge = info.audio;
            changed = true;
        }

        if (info.audioTrackId < 0)
        {
            bool selectedAudioExists = false;

            for (auto const& track : PlayerGetMediaTracks())
            {
                if (track.type == L"audio" && track.selected)
                {
                    selectedAudioExists = true;
                    break;
                }
            }

            if (!selectedAudioExists)
            {
                m_badgeAudioTrackId = -1;

                if (m_audioBadge != MediaAudioBadge::None)
                {
                    m_audioBadge = MediaAudioBadge::None;
                    changed = true;
                }
            }
        }

        if (changed)
        {
            ApplyMediaBadgeVisuals();
        }
    }

    std::wstring MainPage::FormatPlaybackTime(
        double seconds, bool highPrecision)
    {
        if (!std::isfinite(seconds) || seconds < 0.0) return L"--:--";
        auto totalMilliseconds = static_cast<long long>(
            std::floor(seconds * 1000.0));
        auto total = totalMilliseconds / 1000;
        auto hours = total / 3600;
        auto minutes = (total / 60) % 60;
        auto remainingSeconds = total % 60;

        std::wostringstream text;
        text << std::setfill(L'0');
        if (hours > 0)
        {
            text << hours << L":" << std::setw(2) << minutes;
        }
        else
        {
            text << std::setw(2) << minutes;
        }
        text << L":" << std::setw(2) << remainingSeconds;
        if (highPrecision)
        {
            text << L"." << std::setw(3) << (totalMilliseconds % 1000);
        }
        return text.str();
    }

    void MainPage::UpdateTimeDisplay(double elapsed, double duration)
    {
        bool const imageMedia = PlayerIsCurrentMediaImage();
        MinimalCurrentTimeText().Text(imageMedia
            ? L"--:--"
            : FormatPlaybackTime(elapsed, false));
        MinimalDurationTimeText().Text(
            imageMedia ? L"--:--"
                : (duration > 0.0
                    ? FormatPlaybackTime(duration, false)
                    : L"--:--"));

        // A still image has no meaningful playback clock. Ask mpv explicitly
        // whether the selected video track is an image instead of inferring it
        // from duration == 0, because live streams can also have no duration.
        // PiP intentionally keeps its existing time-display behavior untouched.
        if (!m_pictureInPicture && PlayerIsCurrentMediaImage())
        {
            ElapsedTimeText().Text(L"--:-- / --:--");
            return;
        }

        if (m_pictureInPicture)
        {
            std::wstring const current =
                FormatPlaybackTime(elapsed, false);

            std::wstring const total =
                duration > 0.0
                ? FormatPlaybackTime(duration, false)
                : L"--:--";

            ElapsedTimeText().Text(current + L" / " + total);
            return;
        }

        // Time-display preferences remain saved even when the current window
        // is too narrow to render them safely. The normal compact clock is
        // allowed first; remaining time needs a little more room; percentage
        // and millisecond precision keep the wider 890-DIP requirement.
        bool allowRemainingTime = true;
        bool allowExtendedTimeDetails = true;

        HWND const mainWindow =
            static_cast<HWND>(PlayerGetMainWindowHandle());

        RECT windowRect{};
        if (mainWindow && GetWindowRect(mainWindow, &windowRect))
        {
            UINT dpi = GetDpiForWindow(mainWindow);
            if (dpi == 0)
            {
                dpi = 96;
            }

            double const widthDip =
                static_cast<double>(windowRect.right - windowRect.left) *
                96.0 / static_cast<double>(dpi);

            constexpr double RemainingTimeThreshold = 800.0;
            constexpr double ExtendedTimeThreshold = 890.0;

            allowRemainingTime =
                widthDip >= RemainingTimeThreshold;

            allowExtendedTimeDetails =
                widthDip >= ExtendedTimeThreshold;
        }

        // Measure the REAL horizontal room left for the time pill after the
        // controls that are actually visible have taken their space. This is
        // independent of video resolution/aspect ratio and reacts naturally
        // when the volume cluster or any optional transport control appears.
        bool remainingTimeHasRoom = true;
        bool fineTimeDetailsHaveRoom = true;
        bool percentageHasRoom = true;

        double const rowWidth = PlaybackControlsRow().ActualWidth();
        double const leftControlsWidth = PlaybackButtons().ActualWidth();
        double const rightControlsWidth = RightControls().ActualWidth();

        if (rowWidth > 0.0)
        {
            constexpr double PlaybackColumnSpacingTotal = 20.0;

            // The compact Remaining Time form needs less room than the finer
            // details, but it must also yield when the wide-layout controls
            // (notably the volume cluster) squeeze the center column.
            //
            // With the tested ~924x353 layout and every interface control
            // visible, the real center space falls below this threshold, so
            // the clock falls back to the normal compact presentation.
            constexpr double RemainingTimeContentWidthThreshold = 170.0;

            // High Precision requires a little more horizontal room.
            constexpr double FineTimeContentWidthThreshold = 190.0;

            // When Remaining + High Precision + Percentage are all requested,
            // Percentage remains the first extra detail to yield in the
            // intermediate-width layouts validated in Passo 28.
            constexpr double AllDetailsContentWidthThreshold = 235.0;

            auto const timeMargin = TimeDisplayHost().Margin();

            double const timeContentWidth = (std::max)(
                0.0,
                rowWidth
                - leftControlsWidth
                - rightControlsWidth
                - PlaybackColumnSpacingTotal
                - timeMargin.Left
                - timeMargin.Right);

            remainingTimeHasRoom =
                timeContentWidth >= RemainingTimeContentWidthThreshold;

            fineTimeDetailsHaveRoom =
                timeContentWidth >= FineTimeContentWidthThreshold;

            bool const allTimeDetailsRequested =
                m_showRemainingTime &&
                m_highPrecisionTime &&
                m_showTimePercentage;

            double const percentageThreshold =
                allTimeDetailsRequested
                    ? AllDetailsContentWidthThreshold
                    : FineTimeContentWidthThreshold;

            percentageHasRoom =
                timeContentWidth >= percentageThreshold;
        }

        bool const effectiveRemainingTime =
            m_showRemainingTime &&
            allowRemainingTime &&
            remainingTimeHasRoom;

        bool const effectiveHighPrecision =
            m_highPrecisionTime &&
            allowExtendedTimeDetails &&
            fineTimeDetailsHaveRoom;

        bool const effectivePercentage =
            m_showTimePercentage &&
            allowExtendedTimeDetails &&
            fineTimeDetailsHaveRoom &&
            percentageHasRoom;

        // Bar Compact: when every optional time detail is actually visible and
        // the +/-10-second controls are enabled, the wide clock and the media
        // badges can look visually cramped even though the Grid still fits.
        // Pull only that fully-expanded clock 14 DIP to the left. If the seek
        // buttons are disabled, any time detail yields, the badges are hidden,
        // or the user switches back to Full/PiP, preserve the frozen 84-DIP
        // placement exactly.
        bool const compactFullClockNeedsBreathingRoom =
            m_compactBarLayout &&
            m_mediaControlsExpanded &&
            CompactBadgeSlot().Visibility() ==
                Microsoft::UI::Xaml::Visibility::Visible &&
            MediaBadgesHost().Visibility() ==
                Microsoft::UI::Xaml::Visibility::Visible &&
            SeekBackwardButton().Visibility() ==
                Microsoft::UI::Xaml::Visibility::Visible &&
            SeekForwardButton().Visibility() ==
                Microsoft::UI::Xaml::Visibility::Visible &&
            effectiveRemainingTime &&
            effectiveHighPrecision &&
            effectivePercentage;

        TimeDisplayHost().Margin(Microsoft::UI::Xaml::Thickness{
            compactFullClockNeedsBreathingRoom ? 70.0 : 84.0,
            0.0, 0.0, 0.0 });

        std::wstring left;
        double percentageSource = elapsed;

        if (effectiveRemainingTime && duration > 0.0)
        {
            percentageSource = (std::max)(0.0, duration - elapsed);
            left = L"− " + FormatPlaybackTime(
                percentageSource, effectiveHighPrecision);
        }
        else
        {
            left = FormatPlaybackTime(
                elapsed, effectiveHighPrecision);
        }

        if (effectivePercentage && duration > 0.0)
        {
            double percentage = (std::max)(0.0, (std::min)(
                100.0, percentageSource * 100.0 / duration));

            std::wostringstream percentageText;
            percentageText << std::fixed << std::setprecision(1) << percentage;

            auto value = percentageText.str();
            std::replace(value.begin(), value.end(), L'.', L',');

            left += L" (" + value + L"%)";
        }

        std::wstring total = duration > 0.0
            ? FormatPlaybackTime(duration, effectiveHighPrecision)
            : L"--:--";

        ElapsedTimeText().Text(left + L" / " + total);
    }

    void MainPage::UpdateSpeedLabel(double speed)
    {
        if (!std::isfinite(speed) || speed <= 0.0) speed = 1.0;
        std::wostringstream text;
        text << std::fixed << std::setprecision(2) << speed;
        std::wstring value = text.str();
        while (!value.empty() && value.back() == L'0') value.pop_back();
        if (!value.empty() && value.back() == L'.') value.pop_back();
        std::replace(value.begin(), value.end(), L'.', L',');
        SpeedText().Text(value + L"\u00d7");
        MinimalSpeedText().Text(value + L"\u00d7");
    }

    void MainPage::SpeedFlyoutOpening(
        Windows::Foundation::IInspectable const& sender,
        Windows::Foundation::IInspectable const&)
    {
        using namespace Microsoft::UI::Xaml::Controls;
        auto menu = sender.try_as<MenuFlyout>();
        if (!menu) menu = SpeedFlyout();
        menu.Items().Clear();
        double current = PlayerGetPlaybackSpeed();

        static constexpr double speeds[] = {
            0.5, 0.75, 1.0, 1.25, 1.5, 2.0
        };
        for (double speed : speeds)
        {
            ToggleMenuFlyoutItem item;
            std::wostringstream label;
            label << std::fixed << std::setprecision(2) << speed;
            std::wstring value = label.str();
            while (!value.empty() && value.back() == L'0') value.pop_back();
            if (!value.empty() && value.back() == L'.') value.pop_back();
            std::replace(value.begin(), value.end(), L'.', L',');
            item.Text(value + L"\u00d7");
            item.IsChecked(std::abs(current - speed) < 0.001);
            item.Click([this, speed](auto const&, auto const&)
                {
                    PlayerSetPlaybackSpeed(speed);
                    UpdateSpeedLabel(speed);
                });
            menu.Items().Append(item);
        }
    }

    void MainPage::ProfilesFlyoutOpening(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        using namespace Microsoft::UI::Xaml::Controls;

        auto menu = ProfilesFlyout();
        menu.Items().Clear();

        auto profiles = PlayerGetImportedProfileNames();
        m_hasImportedProfiles = !profiles.empty();

        if (profiles.empty() || !m_videoOnlyActionsAllowed)
        {
            ProfilesButton().Visibility(
                Microsoft::UI::Xaml::Visibility::Collapsed);
            return;
        }

        std::wstring const active = PlayerGetActiveImportedProfile();

        for (auto const& profile : profiles)
        {
            ToggleMenuFlyoutItem item;
            item.Text(profile);
            item.IsChecked(profile == active);
            item.Click(
                [profile](auto const&, auto const&)
                {
                    PlayerApplyImportedProfile(profile);
                });
            menu.Items().Append(item);
        }

        menu.Items().Append(MenuFlyoutSeparator{});

        MenuFlyoutItem deactivate;
        deactivate.Text(
            MainPageString(
                L"MainPageDynDeactivateProfile",
                L"Desativar perfil"));
        deactivate.IsEnabled(!active.empty());
        deactivate.Click(
            [](auto const&, auto const&)
            {
                PlayerDeactivateImportedProfile();
            });
        menu.Items().Append(deactivate);
    }

    void MainPage::TracksFlyoutOpening(
        Windows::Foundation::IInspectable const& sender,
        Windows::Foundation::IInspectable const&)
    {
        using namespace Microsoft::UI::Xaml::Controls;
        auto menu = sender.try_as<MenuFlyout>();
        if (!menu) menu = TracksFlyout();
        menu.Items().Clear();
        auto tracks = PlayerGetMediaTracks();

        auto appendHeader = [&menu](std::wstring const& text)
            {
                MenuFlyoutItem header;
                header.Text(text);
                header.IsEnabled(false);
                menu.Items().Append(header);
            };

        auto appendTracks = [&menu, &tracks](
            std::wstring const& type, std::wstring const& property,
            std::wstring const& fallback, int selectionIndex)
            {
                int ordinal = 0;
                bool anySelected = false;
                for (auto const& track : tracks)
                {
                    if (track.type != type) continue;
                    ++ordinal;
                    bool selected = track.selected &&
                        (type != L"sub" || track.mainSelection == selectionIndex);
                    anySelected = anySelected || selected;

                    // Match the complete context menu's useful track title and
                    // flags. Only the codec is omitted from this compact flyout.
                    std::wstring label = track.title.empty()
                        ? std::wstring(fallback) + L" " + std::to_wstring(ordinal)
                        : track.title;
                    if (track.forced) label += MainPageString(
                        L"MainPageDynForcedSuffix", L"  \u00b7  for\u00e7ada");
                    if (track.external) label += MainPageString(
                        L"MainPageDynExternalSuffix", L"  \u00b7  externa");
                    if (track.defaultTrack) label += MainPageString(
                        L"MainPageDynDefaultSuffix", L"  \u00b7  padr\u00e3o");

                    // Keep the compact flyout readable. The complete technical
                    // value remains available in the full context menu.
                    constexpr size_t maxLabelLength = 46;
                    if (label.size() > maxLabelLength)
                    {
                        label.resize(maxLabelLength - 1);
                        label += L"\u2026";
                    }

                    ToggleMenuFlyoutItem item;
                    item.Text(label);
                    item.IsChecked(selected);
                    item.KeyboardAcceleratorTextOverride(track.language);
                    auto value = std::to_wstring(track.id);
                    item.Click([property, value](auto const&, auto const&)
                        {
                            PlayerSelectMediaTrack(property, value);
                        });
                    menu.Items().Append(item);
                }

                ToggleMenuFlyoutItem disabled;
                disabled.Text(type == L"audio"
                    ? MainPageString(L"MainPageDynNoAudio", L"Sem \u00e1udio")
                    : MainPageString(L"MainPageDynDisabled", L"Desativadas"));
                disabled.IsChecked(!anySelected);
                disabled.Click([property](auto const&, auto const&)
                    {
                        PlayerSelectMediaTrack(property, L"no");
                    });
                menu.Items().Append(disabled);
                return ordinal;
            };

        appendHeader(MainPageString(L"MainPageDynAudioHeader", L"\u00c1UDIO"));
        appendTracks(L"audio", L"aid",
            MainPageString(L"MainPageDynAudioBase", L"\u00c1udio"), -1);
        menu.Items().Append(MenuFlyoutSeparator{});
        appendHeader(MainPageString(L"MainPageDynSubtitlesHeader", L"LEGENDAS"));
        appendTracks(L"sub", L"sid",
            MainPageString(L"MainPageDynSubtitleBase", L"Legenda"), 0);
    }

    void MainPage::MinimalMoreFlyoutOpening(
        Windows::Foundation::IInspectable const& sender,
        Windows::Foundation::IInspectable const&)
    {
        using namespace Microsoft::UI::Xaml::Controls;

        auto menu = sender.try_as<MenuFlyout>();
        if (!menu) menu = MinimalMoreFlyout();
        menu.Items().Clear();

        // Keep the compact menu visually consistent with the established HC Player
        // context menu/transport. These are the same Fluent symbols/glyphs already
        // used elsewhere in the app, not a second icon language for Minimal mode.
        auto addSymbolItem = [&menu](
            std::wstring const& text,
            Symbol symbol,
            auto action)
            {
                MenuFlyoutItem item;
                item.Text(text);
                item.Icon(SymbolIcon{ symbol });
                item.Click(action);
                menu.Items().Append(item);
            };

        auto addFontItem = [&menu](
            std::wstring const& text,
            wchar_t const* glyph,
            auto action)
            {
                MenuFlyoutItem item;
                item.Text(text);
                FontIcon icon;
                icon.Glyph(glyph);
                icon.FontSize(16.0);
                item.Icon(icon);
                item.Click(action);
                menu.Items().Append(item);
            };

        addSymbolItem(
            MainPageString(L"MainPageDynMinimalOpenFile", L"Abrir arquivo"),
            Symbol::OpenFile,
            [](auto const&, auto const&) { PlayerShowOpenDialog(); });
        addFontItem(
            MainPageString(L"MainPageDynMinimalOpenFolder", L"Abrir pasta"),
            L"\uE8B7",
            [](auto const&, auto const&) { PlayerShowOpenFolderDialog(); });
        addSymbolItem(
            MainPageString(L"MainPageDynMinimalPlaylist", L"Playlist"),
            Symbol::List,
            [](auto const&, auto const&) { PlayerShowPlaylist(); });

        menu.Items().Append(MenuFlyoutSeparator{});

        addFontItem(
            MainPageString(L"MainPageDynPreviousChapter", L"Capítulo anterior"),
            L"\uE892",
            [](auto const&, auto const&) { PlayerChangeChapter(-1); });
        addFontItem(
            MainPageString(L"MainPageDynNextChapter", L"Próximo capítulo"),
            L"\uE893",
            [](auto const&, auto const&) { PlayerChangeChapter(1); });

        // Repeat already has a dedicated button in the Minimal pill. Put Profiles in
        // its former menu slot so the flyout does not duplicate the same command.
        auto profiles = PlayerGetImportedProfileNames();
        if (!profiles.empty() && m_videoOnlyActionsAllowed)
        {
            MenuFlyoutSubItem profilesMenu;
            profilesMenu.Text(MainPageString(L"MainPageDynMinimalProfiles", L"Perfis"));
            FontIcon profilesIcon;
            profilesIcon.Glyph(L"\uE9E9");
            profilesIcon.FontSize(16.0);
            profilesMenu.Icon(profilesIcon);

            std::wstring const active = PlayerGetActiveImportedProfile();
            for (auto const& profile : profiles)
            {
                // Keep profile entries as regular MenuFlyoutItems. A
                // ToggleMenuFlyoutItem makes WinUI reserve its dedicated
                // checkmark lane for the entire submenu, which leaves a large
                // empty gutter on the left. Mirror the context-menu behavior:
                // only the active profile receives an Accept icon.
                MenuFlyoutItem item;
                item.Text(profile);
                if (profile == active)
                {
                    item.Icon(SymbolIcon{ Symbol::Accept });
                }
                item.Click([profile](auto const&, auto const&)
                    { PlayerApplyImportedProfile(profile); });
                profilesMenu.Items().Append(item);
            }
            profilesMenu.Items().Append(MenuFlyoutSeparator{});
            MenuFlyoutItem deactivate;
            deactivate.Text(MainPageString(
                L"MainPageDynDeactivateProfile", L"Desativar perfil"));
            deactivate.Icon(SymbolIcon{ Symbol::Undo });
            deactivate.IsEnabled(!active.empty());
            deactivate.Click([](auto const&, auto const&)
                { PlayerDeactivateImportedProfile(); });
            profilesMenu.Items().Append(deactivate);
            menu.Items().Append(profilesMenu);
        }

        // Keep this as a regular MenuFlyoutItem instead of a ToggleMenuFlyoutItem.
        // A top-level toggle forces WinUI to reserve a checkmark column for every
        // item in the presenter, which created the empty lane to the left of the
        // real icons. The command still toggles the same shuffle state.
        MenuFlyoutItem shuffle;
        shuffle.Text(MainPageString(L"MainPageDynMinimalShuffle", L"Reprodução aleatória"));
        FontIcon shuffleIcon;
        shuffleIcon.Glyph(L"\uE8B1");
        shuffleIcon.FontSize(16.0);
        shuffle.Icon(shuffleIcon);
        shuffle.Click([this](auto const&, auto const&)
            {
                bool const requested = !m_shuffleEnabled;
                if (PlayerSetPlaylistShuffle(requested))
                {
                    m_shuffleEnabled = requested;
                    UpdateShuffleButtonState();
                }
            });
        menu.Items().Append(shuffle);

        menu.Items().Append(MenuFlyoutSeparator{});

        addFontItem(
            MainPageString(L"MainPageDynMinimalMediaInfo", L"Informações de mídia"),
            L"\uE8A5",
            [](auto const&, auto const&) { PlayerShowMediaInfo(); });
        addFontItem(
            MainPageString(L"MainPageDynMinimalStats", L"Estatísticas de reprodução"),
            L"\uE9D9",
            [](auto const&, auto const&) { PlayerToggleStats(); });

        if (m_videoOnlyActionsAllowed)
        {
            MenuFlyoutSubItem captureMenu;
            captureMenu.Text(MainPageString(L"MainPageDynMinimalCapture", L"Captura de tela"));
            FontIcon captureIcon;
            captureIcon.Glyph(L"\uE722");
            captureIcon.FontSize(16.0);
            captureMenu.Icon(captureIcon);

            MenuFlyoutItem noSubtitles;
            noSubtitles.Text(MainPageString(
                L"MainPageDynMinimalCaptureNoSubtitles", L"Capturar sem legendas"));
            noSubtitles.Click([](auto const&, auto const&)
                { PlayerCaptureScreenshot(false); });
            captureMenu.Items().Append(noSubtitles);

            MenuFlyoutItem withSubtitles;
            withSubtitles.Text(MainPageString(
                L"MainPageDynMinimalCaptureWithSubtitles", L"Capturar com legendas"));
            FontIcon withSubtitlesIcon;
            withSubtitlesIcon.Glyph(L"\uE722");
            withSubtitles.Icon(withSubtitlesIcon);
            withSubtitles.Click([](auto const&, auto const&)
                { PlayerCaptureScreenshot(true); });
            captureMenu.Items().Append(withSubtitles);

            captureMenu.Items().Append(MenuFlyoutSeparator{});
            MenuFlyoutItem folder;
            folder.Text(MainPageString(
                L"MainPageDynMinimalCaptureFolder", L"Abrir pasta de capturas"));
            FontIcon folderIcon;
            folderIcon.Glyph(L"\uE8B7");
            folder.Icon(folderIcon);
            folder.Click([](auto const&, auto const&)
                { PlayerOpenScreenshotDirectory(); });
            captureMenu.Items().Append(folder);

            menu.Items().Append(captureMenu);
        }

        addFontItem(
            MainPageString(L"MainPageDynPictureInPicture", L"Picture-in-Picture"),
            L"\uE8A7",
            [](auto const&, auto const&) { PlayerTogglePictureInPicture(); });
        addSymbolItem(
            MainPageString(L"MainPageDynMinimalSettings", L"Configurações"),
            Symbol::Setting,
            [](auto const&, auto const&) { PlayerShowSettings(); });
    }

    void MainPage::TransportFlyoutOpened(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        m_transportFlyoutOpen = true;
        m_transportHideTimer.Stop();
        PlayerSetTransportFlyoutOpen(true);
    }

    void MainPage::TransportFlyoutClosed(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        m_transportFlyoutOpen = false;
        PlayerSetTransportFlyoutOpen(false);
        if (!m_settingsOverlayOpen)
        {
            ScheduleTransportHide(std::chrono::milliseconds(900));
        }
    }

    double MainPage::TimelineTimeFromPointerX(
        double pointerX,
        double duration)
    {
        if (!std::isfinite(pointerX) ||
            !std::isfinite(duration) ||
            duration <= 0.0)
        {
            return 0.0;
        }

        // Use the ACTUAL visible track for the active style. The filled
        // overlay has Margin=10 while the chapter canvas has Margin=9; using
        // ChapterMarkers width for both styles created a 2-DIP range mismatch
        // that becomes several seconds on long media.
        double const trackWidth =
            m_filledTimelineStyle
                ? FilledTimelineOverlay().ActualWidth()
                : ChapterMarkers().ActualWidth();

        if (trackWidth <= 0.0)
        {
            return 0.0;
        }

        // Derive the left inset from the actual layout instead of hardcoding
        // a margin constant. The WinUI Slider template has SliderPreContentMargin
        // = 14 px on each side, while ChapterMarkers/FilledTimelineOverlay only
        // have a 9–10 px XAML margin. Using the hardcoded XAML margin as the
        // inset produced a ~5 px error that translates to ~11 s on a 2h40m film.
        // Computing (sliderWidth - trackWidth) / 2 gives the true left padding
        // regardless of the theme resource value.
        double const sliderWidth = PositionSlider().ActualWidth();
        double const trackInset =
            sliderWidth > trackWidth
                ? (sliderWidth - trackWidth) / 2.0
                : 0.0;

        double const trackX = (std::max)(
            0.0,
            (std::min)(
                trackWidth,
                pointerX - trackInset));

        return (std::max)(
            0.0,
            (std::min)(
                duration,
                trackX * duration / trackWidth));
    }

    bool MainPage::ApplyTimelinePointerPosition(
        double pointerX,
        bool exact)
    {
        if (!m_ready)
        {
            return false;
        }

        double elapsed{};
        double duration{};

        if (!PlayerGetPlaybackTimes(elapsed, duration) ||
            duration <= 0.0)
        {
            return false;
        }

        double const targetSeconds =
            TimelineTimeFromPointerX(pointerX, duration);

        bool const targetChanged =
            !m_timelineInteractionHasTarget ||
            std::abs(
                targetSeconds - m_timelineInteractionSeconds) > 0.001;

        double const targetPercent =
            (std::max)(
                0.0,
                (std::min)(
                    100.0,
                    targetSeconds * 100.0 / duration));

        // One mouse coordinate owns every timeline representation. The native
        // Slider is updated only as a visual/value mirror; its internal WinUI
        // track geometry never decides a mouse seek.
        m_isUpdatingPosition = true;
        PositionSlider().Value(targetPercent);
        m_isUpdatingPosition = false;

        m_timelineInteractionSeconds = targetSeconds;
        m_timelineInteractionHasTarget = true;

        // Do not ask mpv to seek twice to the same target. A stationary click
        // already performs one exact seek on PointerPressed; PointerReleased at
        // the same X must not flush/re-prime A/V a second time. During a drag,
        // however, the last seek at the final X is normally non-exact, so release
        // still upgrades that same target to one final exact seek.
        bool const needsSeek =
            targetChanged ||
            (exact && !m_timelineInteractionLastSeekWasExact);

        if (!needsSeek)
        {
            return true;
        }

        // Keep both visual timeline styles under the pointer immediately.
        // The regular playback timer remains suspended while the user owns
        // the timeline, so it cannot pull the marker back to stale playback.
        RenderChapterMarkers(duration, targetSeconds);

        if (exact)
        {
            PlayerSeekAbsoluteExact(targetSeconds);
        }
        else
        {
            PlayerSeekAbsolute(targetSeconds);
        }

        m_timelineInteractionLastSeekWasExact = exact;
        return true;
    }

    void MainPage::PositionChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        // ValueChanged also fires for timer-driven updates while
        // m_isUpdatingPosition is true. Refresh the compact HC Player marker
        // before the seek guard so it stays clock-synchronised in both cases.
        UpdateMinimalTimelineVisual();

        if (m_ready &&
            !m_isUpdatingPosition &&
            !m_timelineUserInteraction)
        {
            // Mouse seeking is owned by TimelineInputSurface and never reaches
            // this path. Keep ValueChanged for keyboard/accessibility changes.
            PlayerSeek(args.NewValue());
        }
    }

    void MainPage::MinimalTimelineSizeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::SizeChangedEventArgs const&)
    {
        UpdateMinimalTimelineVisual();
    }

    void MainPage::MinimalTimelinePointerEntered(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        // Match the established HC Player timeline hover exactly. The native
        // Windows 11 Slider style keeps its own pointer-over behavior; this
        // composition animation is active only for the HC Player filled style.
        SetFilledTimelineHovered(true);
    }

    void MainPage::MinimalTimelinePointerPressed(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        // Hover preview only: once the user starts dragging the seek thumb,
        // hide the chapter/time card and let the Slider seek without any
        // tooltip surface. The native thumb tooltip is disabled in XAML.
        MinimalTimelineHoverPopup().IsOpen(false);
    }

    void MainPage::MinimalTimelinePointerMoved(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (!UsesMinimalTransportStyle() ||
            PlayerIsCurrentMediaImage() ||
            !PlayerGetChapterHoverCardEnabled())
        {
            MinimalTimelineHoverPopup().IsOpen(false);
            return;
        }

        double elapsed{};
        double duration{};
        if (!PlayerGetPlaybackTimes(elapsed, duration) ||
            !std::isfinite(duration) || duration <= 0.0)
        {
            MinimalTimelineHoverPopup().IsOpen(false);
            return;
        }

        auto slider = MinimalPositionSlider();
        auto const currentPoint = args.GetCurrentPoint(slider);

        // Do not show any time/chapter tooltip while dragging. The hover card
        // returns naturally on the next pointer move after the button is released.
        if (currentPoint.Properties().IsLeftButtonPressed())
        {
            MinimalTimelineHoverPopup().IsOpen(false);
            return;
        }

        double const sliderWidth = slider.ActualWidth();
        if (sliderWidth <= 28.0)
        {
            MinimalTimelineHoverPopup().IsOpen(false);
            return;
        }

        auto const point = currentPoint.Position();

        // Both Minimal timeline styles use the same 14-DIP Slider track inset.
        // The HC Player custom track is explicitly Margin=14, while the native
        // Windows 11 Slider template uses the same pre/post content margin.
        constexpr double TrackInset = 14.0;
        double const trackWidth = sliderWidth - TrackInset * 2.0;
        double const trackX = (std::max)(
            0.0,
            (std::min)(trackWidth,
                static_cast<double>(point.X) - TrackInset));
        double const hoveredTime = (std::max)(
            0.0,
            (std::min)(duration, trackX * duration / trackWidth));

        std::wstring title;
        if (!m_chapters.empty())
        {
            size_t selected{};
            for (size_t index = 1; index < m_chapters.size(); ++index)
            {
                if (m_chapters[index].time > hoveredTime) break;
                selected = index;
            }

            title = m_chapters[selected].title.empty()
                ? MainPageString(L"MainPageDynChapterBase", L"Capítulo ") +
                    std::to_wstring(selected + 1)
                : m_chapters[selected].title;

            constexpr size_t MaximumTitleLength = 44;
            if (title.size() > MaximumTitleLength)
            {
                title.resize(MaximumTitleLength - 1);
                title += L"…";
            }
        }

        auto const hoveredTimeText = FormatPlaybackTime(
            hoveredTime, m_highPrecisionTime);
        MinimalTimelineHoverText().Text(
            title.empty()
                ? hoveredTimeText
                : title + L"  \u2022  " + hoveredTimeText);

        auto popup = MinimalTimelineHoverPopup();
        auto card = MinimalTimelineHoverCard();
        auto overlay = MinimalTimelineHoverOverlay();

        card.Measure({ 330.0f, 80.0f });
        double const cardWidth = (std::max)(
            1.0, static_cast<double>(card.DesiredSize().Width));
        double const cardHeight = (std::max)(
            1.0, static_cast<double>(card.DesiredSize().Height));

        auto const sliderTransform = slider.TransformToVisual(overlay);
        auto const sliderOrigin = sliderTransform.TransformPoint({ 0.0f, 0.0f });
        auto const barTransform = MinimalTransportBar().TransformToVisual(overlay);
        auto const barOrigin = barTransform.TransformPoint({ 0.0f, 0.0f });

        double left = static_cast<double>(sliderOrigin.X) +
            static_cast<double>(point.X) - cardWidth / 2.0;

        constexpr double HorizontalEdgeGap = 6.0;
        double const barLeft = static_cast<double>(barOrigin.X);
        double const barRight = barLeft + MinimalTransportBar().ActualWidth();
        if (barRight - barLeft > cardWidth + HorizontalEdgeGap * 2.0)
        {
            left = (std::max)(
                barLeft + HorizontalEdgeGap,
                (std::min)(
                    barRight - cardWidth - HorizontalEdgeGap,
                    left));
        }

        // Match the normal bar tooltip, but leave a slightly roomier visual
        // gap because the Minimal pill itself floats over the video.
        constexpr double VerticalGapAbovePill = 8.0;

        // Popup is rendered in its own composition surface. Even with
        // UseLayoutRounding on the card, a fractional popup translation can
        // resample the entire rounded rectangle between physical pixels and
        // make the corners look stair-stepped. Snap the popup translation to
        // the current XamlRoot pixel grid, while keeping the card itself fully
        // XAML/vector rendered.
        double rasterizationScale = 1.0;
        if (auto const xamlRoot = overlay.XamlRoot())
        {
            rasterizationScale = (std::max)(
                0.01, static_cast<double>(xamlRoot.RasterizationScale()));
        }

        auto const snapToPhysicalPixel = [rasterizationScale](double value)
        {
            return std::round(value * rasterizationScale) / rasterizationScale;
        };

        popup.HorizontalOffset(snapToPhysicalPixel(left));
        popup.VerticalOffset(snapToPhysicalPixel(
            static_cast<double>(barOrigin.Y) -
            cardHeight - VerticalGapAbovePill));

        if (!popup.IsOpen()) popup.IsOpen(true);
    }

    void MainPage::MinimalTimelinePointerExited(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        SetFilledTimelineHovered(false);
        MinimalTimelineHoverPopup().IsOpen(false);
    }

    void MainPage::TimelinePointerPressed(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (!m_ready)
        {
            return;
        }

        auto const currentPoint =
            args.GetCurrentPoint(PositionSlider());

        if (!currentPoint.Properties().IsLeftButtonPressed())
        {
            return;
        }

        auto const point = currentPoint.Position();

        m_timelineUserInteraction = true;
        m_timelineProgressHoldTicks = 0;
        m_timelineInteractionHasTarget = false;
        m_timelineInteractionLastSeekWasExact = false;

        if (auto const surface =
            sender.try_as<Microsoft::UI::Xaml::UIElement>())
        {
            surface.CapturePointer(args.Pointer());
        }

        // A stationary press must already be precise; do not wait for release
        // to correct a keyframe/percentage approximation.
        ApplyTimelinePointerPosition(
            static_cast<double>(point.X),
            true);

        args.Handled(true);
    }

    void MainPage::TimelinePointerReleased(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (!m_timelineUserInteraction)
        {
            return;
        }

        auto const point =
            args.GetCurrentPoint(PositionSlider()).Position();

        // The final target always receives an absolute+exact seek. During the
        // drag we use normal absolute seeks for responsiveness, but they use
        // this exact same X -> seconds conversion.
        ApplyTimelinePointerPosition(
            static_cast<double>(point.X),
            true);

        m_timelineUserInteraction = false;
        m_timelineProgressHoldTicks = 2;
        m_timelineInteractionHasTarget = false;
        m_timelineInteractionLastSeekWasExact = false;

        if (auto const surface =
            sender.try_as<Microsoft::UI::Xaml::UIElement>())
        {
            surface.ReleasePointerCapture(args.Pointer());
        }

        args.Handled(true);
    }

    void MainPage::TimelinePointerCaptureLost(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        if (!m_timelineUserInteraction)
        {
            return;
        }

        // Capture can be lost by window/layout transitions. Preserve the last
        // pointer-owned target instead of allowing a stale progress tick to win.
        if (m_timelineInteractionHasTarget &&
            !m_timelineInteractionLastSeekWasExact)
        {
            PlayerSeekAbsoluteExact(m_timelineInteractionSeconds);
        }

        m_timelineUserInteraction = false;
        m_timelineProgressHoldTicks = 2;
        m_timelineInteractionHasTarget = false;
        m_timelineInteractionLastSeekWasExact = false;
    }

    void MainPage::TimelinePointerMoved(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (!m_timelinePointerArmed)
        {
            POINT cursor{};
            if (!GetCursorPos(&cursor) ||
                (cursor.x == m_timelineResumeCursorX &&
                    cursor.y == m_timelineResumeCursorY))
            {
                SetFilledTimelineHovered(false);
                ChapterHoverCard().Visibility(
                    Microsoft::UI::Xaml::Visibility::Collapsed);
                ChapterHoverPopup().IsOpen(false);
                SetHoveredChapterSegment(-1);
                return;
            }

            m_timelinePointerArmed = true;
        }

        // O PiP usa exatamente o mesmo hover visual da timeline principal,
        // mas não cria nem exibe tooltip.
        SetFilledTimelineHovered(true);

        auto const point =
            args.GetCurrentPoint(PositionSlider()).Position();

        // While captured, drag seeking uses the exact same X -> seconds ruler
        // as tooltip/thumbnail. This removes the native Slider's second,
        // slightly different internal track from mouse seeking entirely.
        if (m_timelineUserInteraction)
        {
            ApplyTimelinePointerPosition(
                static_cast<double>(point.X),
                false);
        }

        if (m_pictureInPicture)
        {
            HideThumbnailPreview();
            ChapterHoverCard().Visibility(
                Microsoft::UI::Xaml::Visibility::Collapsed);
            ChapterHoverPopup().IsOpen(false);
            SetHoveredChapterSegment(-1);
            return;
        }

        if (PositionSlider().ActualWidth() <= 1.0)
        {
            ChapterHoverCard().Visibility(
                Microsoft::UI::Xaml::Visibility::Collapsed);
            ChapterHoverPopup().IsOpen(false);
            return;
        }

        double elapsed{};
        double duration{};

        if (!PlayerGetPlaybackTimes(elapsed, duration) || duration <= 0.0)
        {
            ChapterHoverCard().Visibility(
                Microsoft::UI::Xaml::Visibility::Collapsed);
            ChapterHoverPopup().IsOpen(false);
            return;
        }

        double const hoveredTime =
            TimelineTimeFromPointerX(
                static_cast<double>(point.X),
                duration);

        // Thumbnail image preview is independent from the chapter/time tooltip.
        // Default is OFF; when disabled we do not request frames, so the lazy
        // private libmpv thumbnail worker never starts. Audio items are excluded
        // even when MPV exposes album art as a one-frame video track: cover art
        // belongs to the song and must never become a seek thumbnail.
        bool const allowVideoThumbnail =
            m_videoThumbnailsEnabled && !PlayerIsCurrentMediaAudio();

        if (allowVideoThumbnail)
        {
            m_thumbnailHoverActive = true;
            m_thumbnailHoverPointX = static_cast<double>(point.X);

            // Only invalidate the current generation when the hovered time has
            // moved beyond the thumbnail cache tolerance (~50 ms). Sub-pixel jitter
            // and synthetic WinUI events can fire PointerMoved without any
            // meaningful position change; bumping the generation on every such
            // event can churn the displayed/hover generation identity unnecessarily.
            // Keep generation changes tied to meaningful timeline movement instead.
            static constexpr double kGenerationChangeThresholdSeconds = 0.050;
            bool const timeChanged =
                std::abs(hoveredTime - m_thumbnailHoveredTime) >
                kGenerationChangeThresholdSeconds;

            if (timeChanged)
            {
                m_thumbnailHoveredTime = hoveredTime;
            }

            auto const generation =
                timeChanged
                    ? ++m_thumbnailHoverGeneration
                    : m_thumbnailHoverGeneration;

            // ThumbFast-style visual policy: the shell follows the mouse
            // immediately even while a replacement frame is still being decoded.
            // Frame identity remains separate through generation, so visual motion
            // can never make an old frame authoritative for a newer hover request.
            m_thumbnailMotionTargetX = m_thumbnailHoverPointX;

            if (ThumbnailPreviewPopup().IsOpen())
            {
                if (!m_thumbnailMotionInitialized)
                {
                    m_thumbnailMotionCurrentX = m_thumbnailDisplayedPointX;
                    m_thumbnailMotionInitialized = true;
                }

                // Keep the old image visually present while FAST catches up. A
                // replacement frame itself receives the subtle fade below.
                m_thumbnailOpacityTarget = 1.0;
                m_thumbnailMotionTimer.Start();
            }

            // Cheap request only. PointerMoved never decodes or seeks here.
            // Give this hover an explicit logical identity and carry that identity
            // all the way back with the asynchronous result.
            if (m_thumbnailController)
            {
                auto weakThis = get_weak();
                double const requestPointX =
                    m_thumbnailHoverPointX;

                bool const thumbnailScrubbing =
                    m_timelineUserInteraction;

                m_thumbnailController->Request(
                    hoveredTime,
                    thumbnailScrubbing,
                    [weakThis, generation, requestPointX](
                        ThumbnailController::Result&& result)
                    {
                        auto page = weakThis.get();
                        if (!page)
                        {
                            return;
                        }

                        auto payload =
                            std::make_shared<ThumbnailController::Result>(
                                std::move(result));

                        page->DispatcherQueue().TryEnqueue(
                            [weakThis, payload, generation, requestPointX]() mutable
                            {
                                if (auto strong = weakThis.get())
                                {
                                    strong->ApplyThumbnailResult(
                                        std::move(*payload),
                                        generation,
                                        requestPointX);
                                }
                            });
                    });
            }

        }
        else if (m_thumbnailHoverActive || ThumbnailPreviewPopup().IsOpen())
        {
            if (m_thumbnailController)
            {
                m_thumbnailController->Cancel();
            }
            HideThumbnailPreview();
        }

        std::wstring title;

        if (!m_chapters.empty())
        {
            size_t selected{};

            for (size_t index = 1; index < m_chapters.size(); ++index)
            {
                if (m_chapters[index].time > hoveredTime)
                {
                    break;
                }

                selected = index;
            }

            title = m_chapters[selected].title.empty()
                ? MainPageString(L"MainPageDynChapterBase", L"Capítulo ") +
                    std::to_wstring(selected + 1)
                : m_chapters[selected].title;

            constexpr size_t maximumTitleLength = 44;

            if (title.size() > maximumTitleLength)
            {
                title.resize(maximumTitleLength - 1);
                title += L"…";
            }
        }

        auto const hoveredTimeText = FormatPlaybackTime(
            hoveredTime,
            m_highPrecisionTime);

        ChapterHoverText().Text(
            title.empty()
            ? hoveredTimeText
            : title + L"  \u2022  " + hoveredTimeText);

        int32_t hoveredSegment = -1;

        for (size_t index = 0;
            index < m_chapterSegments.size();
            ++index)
        {
            auto const& segment = m_chapterSegments[index];

            if (hoveredTime >= segment.start &&
                hoveredTime <= segment.end)
            {
                hoveredSegment =
                    static_cast<int32_t>(index);
                break;
            }
        }

        SetHoveredChapterSegment(hoveredSegment);

        auto card = ChapterHoverCard();

        if (!PlayerGetChapterHoverCardEnabled())
        {
            card.Visibility(
                Microsoft::UI::Xaml::Visibility::Collapsed);
            ChapterHoverPopup().IsOpen(false);
            return;
        }

        // Keep the lightweight chapter/time tooltip visible together with
        // the image preview. In Bar Compact it must be allowed to escape the
        // 94-DIP transport bounds, so it lives in a Popup just like thumbnails.
        card.Visibility(
            Microsoft::UI::Xaml::Visibility::Visible);

        card.Measure({ 330.0f, 80.0f });

        double const cardWidth = (std::max)(
            1.0,
            static_cast<double>(card.DesiredSize().Width));

        double const cardHeight = (std::max)(
            1.0,
            static_cast<double>(card.DesiredSize().Height));

        auto overlay = TimelineHoverOverlay();
        auto const sliderTransform =
            PositionSlider().TransformToVisual(overlay);
        auto const sliderOrigin =
            sliderTransform.TransformPoint({ 0.0f, 0.0f });
        auto const transportTransform =
            TransportRoot().TransformToVisual(overlay);
        auto const transportOrigin =
            transportTransform.TransformPoint({ 0.0f, 0.0f });

        double left =
            static_cast<double>(sliderOrigin.X) +
            static_cast<double>(point.X) - cardWidth / 2.0;

        constexpr double HorizontalEdgeGap = 6.0;
        double const overlayWidth = overlay.ActualWidth();
        if (overlayWidth > cardWidth + HorizontalEdgeGap * 2.0)
        {
            left = (std::max)(
                HorizontalEdgeGap,
                (std::min)(
                    overlayWidth - cardWidth - HorizontalEdgeGap,
                    left));
        }
        else
        {
            left = 0.0;
        }

        constexpr double TooltipGapAboveTransport = 7.0;
        bool const compactBarActive =
            m_compactBarLayout &&
            m_mediaControlsExpanded &&
            !m_minimalTransportStyle &&
            !m_pictureInPicture;

        // Preserve the frozen three-row Bar's original tooltip position. Only
        // Bar Compact needs the Popup lifted above the entire 94-DIP host.
        double const top = compactBarActive
            ? static_cast<double>(transportOrigin.Y) -
                cardHeight - TooltipGapAboveTransport
            : -cardHeight - TooltipGapAboveTransport;

        auto popup = ChapterHoverPopup();
        bool const tooltipWasOpen = popup.IsOpen();
        popup.HorizontalOffset(left);
        popup.VerticalOffset(top);
        popup.IsOpen(true);

        // If an already-visible thumbnail was positioned before the tooltip
        // opened, refresh its cached Y once so the two popups form a clean
        // vertical stack instead of overlapping.
        if (!tooltipWasOpen && ThumbnailPreviewPopup().IsOpen())
        {
            RefreshThumbnailPreviewGeometry();
            PositionThumbnailPreview(
                m_thumbnailMotionInitialized
                    ? m_thumbnailMotionCurrentX
                    : m_thumbnailHoverPointX);
        }
    }

    void MainPage::TimelinePointerExited(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        if (m_thumbnailController)
        {
            m_thumbnailController->Cancel();
        }
        HideThumbnailPreview();

        SetFilledTimelineHovered(false);
        ChapterHoverCard().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);
        ChapterHoverPopup().IsOpen(false);
        SetHoveredChapterSegment(-1);
    }

    void MainPage::HideThumbnailPreview()
    {
        // Invalidate any UI-side request that may still be in flight.
        ++m_thumbnailHoverGeneration;
        m_thumbnailHoverActive = false;
        m_thumbnailDisplayedValid = false;
        m_thumbnailDisplayedGeneration = 0;

        m_thumbnailMotionTimer.Stop();
        m_thumbnailMotionInitialized = false;
        m_thumbnailMotionCurrentX = 0.0;
        m_thumbnailMotionTargetX = 0.0;
        m_thumbnailOpacityCurrent = 1.0;
        m_thumbnailOpacityTarget = 1.0;
        m_thumbnailGeometryValid = false;
        m_thumbnailGeometrySliderOriginX = 0.0;
        m_thumbnailGeometryTop = 0.0;
        m_thumbnailGeometryCardWidth = 0.0;
        m_thumbnailGeometryOverlayWidth = 0.0;
        m_thumbnailGeometrySliderWidth = 0.0;

        if (ThumbnailPreviewPopup().IsOpen())
        {
            ThumbnailPreviewPopup().IsOpen(false);
        }

        ThumbnailPreviewImage().Opacity(1.0);
        ThumbnailPreviewImage().Source(nullptr);
        m_thumbnailBitmap = nullptr;
        m_thumbnailBitmapWidth = 0;
        m_thumbnailBitmapHeight = 0;
    }

    void MainPage::ThumbnailMotionTimerTick(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        if (!m_thumbnailHoverActive ||
            !ThumbnailPreviewPopup().IsOpen())
        {
            m_thumbnailMotionTimer.Stop();
            return;
        }

        if (!m_thumbnailMotionInitialized)
        {
            m_thumbnailMotionCurrentX = m_thumbnailMotionTargetX;
            m_thumbnailMotionInitialized = true;
        }

        // Exponential ease-out. 38P runs this light visual loop at 50 Hz instead
        // of 60 Hz; 0.44 keeps approximately the same perceived settling time
        // while reducing UI-thread popup updates.
        double const positionDelta =
            m_thumbnailMotionTargetX - m_thumbnailMotionCurrentX;

        bool const positionSettled =
            std::abs(positionDelta) <= 0.20;

        if (positionSettled)
        {
            m_thumbnailMotionCurrentX = m_thumbnailMotionTargetX;
        }
        else
        {
            m_thumbnailMotionCurrentX += positionDelta * 0.44;
        }

        PositionThumbnailPreview(m_thumbnailMotionCurrentX);

        double const opacityDelta =
            m_thumbnailOpacityTarget - m_thumbnailOpacityCurrent;

        bool const opacitySettled =
            std::abs(opacityDelta) <= 0.01;

        if (opacitySettled)
        {
            m_thumbnailOpacityCurrent = m_thumbnailOpacityTarget;
        }
        else
        {
            m_thumbnailOpacityCurrent += opacityDelta * 0.48;
        }

        ThumbnailPreviewImage().Opacity(
            (std::max)(0.0, (std::min)(1.0, m_thumbnailOpacityCurrent)));

        if (positionSettled && opacitySettled)
        {
            m_thumbnailMotionTimer.Stop();
        }
    }

    void MainPage::RefreshThumbnailPreviewGeometry()
    {
        auto popup = ThumbnailPreviewPopup();
        auto card = ThumbnailPreviewCard();
        auto overlay = TimelineHoverOverlay();

        if (!popup.IsOpen())
        {
            m_thumbnailGeometryValid = false;
            return;
        }

        // This is intentionally the ONLY place that performs Measure and
        // TransformToVisual for thumbnail motion. The 16-ms animation tick only
        // does cheap arithmetic + HorizontalOffset after this cache is ready.
        card.Measure({ 330.0f, 260.0f });

        double const cardWidth = (std::max)(
            1.0,
            static_cast<double>(card.DesiredSize().Width));

        double const cardHeight = (std::max)(
            1.0,
            static_cast<double>(card.DesiredSize().Height));

        auto const sliderTransform =
            PositionSlider().TransformToVisual(overlay);
        auto const sliderOrigin =
            sliderTransform.TransformPoint({ 0.0f, 0.0f });

        auto const transportTransform =
            TransportRoot().TransformToVisual(overlay);
        auto const transportOrigin =
            transportTransform.TransformPoint({ 0.0f, 0.0f });

        constexpr double VerticalGapAboveTransport = 8.0;
        constexpr double TooltipGapAboveTransport = 7.0;
        constexpr double GapBetweenThumbnailAndTooltip = 6.0;

        double tooltipStackHeight = 0.0;
        bool const compactBarActive =
            m_compactBarLayout &&
            m_mediaControlsExpanded &&
            !m_minimalTransportStyle &&
            !m_pictureInPicture;

        // Full Bar already had a stable thumbnail/tooltip relationship. Only
        // the new two-row layout stacks the thumbnail above the Popup tooltip.
        if (compactBarActive &&
            ChapterHoverPopup().IsOpen() &&
            ChapterHoverCard().Visibility() ==
                Microsoft::UI::Xaml::Visibility::Visible)
        {
            auto tooltipCard = ChapterHoverCard();
            tooltipCard.Measure({ 330.0f, 80.0f });
            tooltipStackHeight =
                (std::max)(
                    1.0,
                    static_cast<double>(tooltipCard.DesiredSize().Height)) +
                TooltipGapAboveTransport +
                GapBetweenThumbnailAndTooltip;
        }

        m_thumbnailGeometrySliderOriginX =
            static_cast<double>(sliderOrigin.X);
        m_thumbnailGeometryTop =
            static_cast<double>(transportOrigin.Y) -
            cardHeight -
            (tooltipStackHeight > 0.0
                ? tooltipStackHeight
                : VerticalGapAboveTransport);
        m_thumbnailGeometryCardWidth = cardWidth;
        m_thumbnailGeometryOverlayWidth = overlay.ActualWidth();
        m_thumbnailGeometrySliderWidth = PositionSlider().ActualWidth();
        m_thumbnailGeometryValid = true;

        popup.VerticalOffset(m_thumbnailGeometryTop);
    }

    void MainPage::PositionThumbnailPreview(double pointerX)
    {
        auto popup = ThumbnailPreviewPopup();

        if (!popup.IsOpen())
        {
            return;
        }

        auto overlay = TimelineHoverOverlay();

        // Window/layout resize invalidates cached transforms. Comparing only
        // ActualWidth here is cheap and avoids forcing layout work per frame.
        if (!m_thumbnailGeometryValid ||
            std::abs(
                overlay.ActualWidth() -
                m_thumbnailGeometryOverlayWidth) > 0.5 ||
            std::abs(
                PositionSlider().ActualWidth() -
                m_thumbnailGeometrySliderWidth) > 0.5)
        {
            RefreshThumbnailPreviewGeometry();
        }

        if (!m_thumbnailGeometryValid)
        {
            return;
        }

        double left =
            m_thumbnailGeometrySliderOriginX +
            pointerX -
            m_thumbnailGeometryCardWidth / 2.0;

        constexpr double HorizontalEdgeGap = 6.0;

        if (m_thumbnailGeometryOverlayWidth >
            m_thumbnailGeometryCardWidth +
            HorizontalEdgeGap * 2.0)
        {
            left = (std::max)(
                HorizontalEdgeGap,
                (std::min)(
                    m_thumbnailGeometryOverlayWidth -
                    m_thumbnailGeometryCardWidth -
                    HorizontalEdgeGap,
                    left));
        }

        popup.HorizontalOffset(left);
    }

    void MainPage::ApplyThumbnailResult(
        ThumbnailController::Result&& result,
        std::uint64_t generation,
        double requestPointX)
    {
        // Only the logical hover request that is still current may become the
        // displayed thumbnail. The controller may deliver FAST first and EXACT
        // second for this same generation; either frame carries its real mpv
        // timestamp, so a click always seeks to the frame actually on screen.
        if (!m_videoThumbnailsEnabled ||
            PlayerIsCurrentMediaAudio() ||
            !m_thumbnailHoverActive ||
            m_pictureInPicture ||
            !result ||
            generation == 0 ||
            generation != m_thumbnailHoverGeneration)
        {
            return;
        }

        if (result.frame.width <= 0 ||
            result.frame.height <= 0 ||
            result.frame.stride !=
                result.frame.width * 4)
        {
            return;
        }

        size_t const expectedSize =
            static_cast<size_t>(
                result.frame.stride) *
            static_cast<size_t>(
                result.frame.height);

        if (result.frame.pixels.size() <
            expectedSize)
        {
            return;
        }

        using Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap;

        bool const bitmapSizeChanged =
            !m_thumbnailBitmap ||
            m_thumbnailBitmapWidth != result.frame.width ||
            m_thumbnailBitmapHeight != result.frame.height;

        if (bitmapSizeChanged)
        {
            m_thumbnailBitmap = WriteableBitmap{
                result.frame.width,
                result.frame.height };
            m_thumbnailBitmapWidth = result.frame.width;
            m_thumbnailBitmapHeight = result.frame.height;
            m_thumbnailGeometryValid = false;
        }

        auto pixelBuffer = m_thumbnailBitmap.PixelBuffer();

        winrt::com_ptr<HcBufferByteAccess> byteAccess;

        winrt::check_hresult(
            winrt::get_unknown(pixelBuffer)->QueryInterface(
                __uuidof(HcBufferByteAccess),
                byteAccess.put_void()));

        uint8_t* destination{};

        winrt::check_hresult(
            byteAccess->Buffer(&destination));

        if (!destination)
        {
            return;
        }

        std::memcpy(
            destination,
            result.frame.pixels.data(),
            expectedSize);

        m_thumbnailBitmap.Invalidate();

        double const imageWidth =
            static_cast<double>(
                result.frame.width);

        double const imageHeight =
            static_cast<double>(
                result.frame.height);

        // 38S: keep the decoder/cache at its established resolution and only
        // reduce the presentation size. This preserves thumbnail quality, FAST/
        // EXACT timing and click sync while making the hover card less dominant.
        constexpr double previewMaxWidth = 300.0;
        constexpr double previewMaxHeight = 169.0;

        double const presentationScale = (std::min)(
            1.0,
            (std::min)(
                previewMaxWidth / imageWidth,
                previewMaxHeight / imageHeight));

        double const presentationWidth =
            imageWidth * presentationScale;
        double const presentationHeight =
            imageHeight * presentationScale;

        ThumbnailPreviewCard().Width(
            (std::max)(200.0, presentationWidth));

        ThumbnailPreviewImage().Width(presentationWidth);
        ThumbnailPreviewImage().Height(presentationHeight);

        // Source assignment itself can invalidate XAML image state. Do it only
        // when a new backing bitmap was allocated; normal FAST/EXACT updates
        // simply mutate and Invalidate the existing buffer.
        if (bitmapSizeChanged)
        {
            ThumbnailPreviewImage().Source(m_thumbnailBitmap);
        }

        m_thumbnailDisplayedValid = true;
        m_thumbnailDisplayedGeneration = generation;
        m_thumbnailDisplayedRequestTime =
            result.requestedSeconds;
        m_thumbnailDisplayedFrameTime =
            result.seconds;
        m_thumbnailDisplayedPointX =
            requestPointX;

        bool const wasOpen =
            ThumbnailPreviewPopup().IsOpen();

        if (!wasOpen)
        {
            ThumbnailPreviewPopup().IsOpen(true);
            m_thumbnailGeometryValid = false;

            // First appearance: no lateral travel from an invented origin.
            // Place it correctly, then fade the decoded image in.
            m_thumbnailMotionCurrentX = requestPointX;
            m_thumbnailMotionTargetX = requestPointX;
            m_thumbnailMotionInitialized = true;
            PositionThumbnailPreview(requestPointX);

            m_thumbnailOpacityCurrent = 0.0;
            ThumbnailPreviewImage().Opacity(0.0);
        }
        else
        {
            if (bitmapSizeChanged)
            {
                RefreshThumbnailPreviewGeometry();
            }

            // The shell may already be following the pointer. Keep its current
            // eased position and simply converge on the point that generated
            // this authoritative frame.
            if (!m_thumbnailMotionInitialized)
            {
                m_thumbnailMotionCurrentX = requestPointX;
                m_thumbnailMotionInitialized = true;
            }

            m_thumbnailMotionTargetX = requestPointX;

            // Do NOT pre-dip opacity here. The bitmap pixels were already
            // updated above (memcpy + Invalidate), so the very next compositor
            // frame already shows the new content. Forcing an opacity drop
            // before that composited frame lands produces a visible flicker:
            // the old image dims for one frame, then the new image appears and
            // fades back up. Letting the timer handle opacity means it rises
            // smoothly from wherever it already is, with no artificial dip.
        }

        m_thumbnailOpacityTarget = 1.0;
        m_thumbnailMotionTimer.Start();
    }

    void MainPage::TimeDisplayFlyoutOpening(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        RemainingTimeItem().IsChecked(m_showRemainingTime);
        HighPrecisionTimeItem().IsChecked(m_highPrecisionTime);
        ShowPercentageItem().IsChecked(m_showTimePercentage);
    }

    void MainPage::RemainingTimeClicked(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        m_showRemainingTime = sender.as<
            Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem>().IsChecked();
        double elapsed{};
        double duration{};
        if (PlayerGetPlaybackTimes(elapsed, duration))
            UpdateTimeDisplay(elapsed, duration);
    }

    void MainPage::HighPrecisionTimeClicked(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        m_highPrecisionTime = sender.as<
            Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem>().IsChecked();
        double elapsed{};
        double duration{};
        if (PlayerGetPlaybackTimes(elapsed, duration))
            UpdateTimeDisplay(elapsed, duration);
    }

    void MainPage::ShowPercentageClicked(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        m_showTimePercentage = sender.as<
            Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem>().IsChecked();
        double elapsed{};
        double duration{};
        if (PlayerGetPlaybackTimes(elapsed, duration))
            UpdateTimeDisplay(elapsed, duration);
    }

    void MainPage::VolumeIconTapped(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& args)
    {
        PlayerExecuteMpvCommand(L"no-osd cycle mute");
        ShowVolumeFeedback();
        args.Handled(true);
    }

    void MainPage::VolumeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        // ValueChanged can run while XAML is still constructing one of the
        // volume sliders. MainPageLoaded initializes all presentations
        // explicitly, so cross-control synchronization starts only afterwards.
        if (!m_controlsReady)
        {
            UpdateVolumeMarker();
            return;
        }
        bool internalUpdate = m_isUpdatingVolume;
        if (!internalUpdate)
        {
            m_isUpdatingVolume = true;
            VolumeSlider().Value(args.NewValue());
            StandardVolumeSlider().Value(args.NewValue());
            m_isUpdatingVolume = false;
        }
        UpdateVolumeMarker();
        if (internalUpdate) return;
        PlayerSetVolume(args.NewValue());
        ShowVolumeFeedback();
    }

    void MainPage::VolumeSliderSizeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::SizeChangedEventArgs const&)
    {
        UpdateVolumeMarker();
    }

    void MainPage::VolumeSliderPointerEntered(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        SetVolumeSliderHovered(true);
    }

    void MainPage::VolumeSliderPointerExited(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        SetVolumeSliderHovered(false);
    }

    void MainPage::UpdateVolumeMarker()
    {
        auto standardTrackHost = StandardVolumeTrackHost();
        auto standardValueTrack = StandardVolumeValueTrack();
        if (standardTrackHost && standardValueTrack)
        {
            double standardWidth = standardTrackHost.ActualWidth();
            double standardRange = StandardVolumeSlider().Maximum() -
                StandardVolumeSlider().Minimum();
            double standardRatio = standardRange > 0.0
                ? (StandardVolumeSlider().Value() -
                    StandardVolumeSlider().Minimum()) / standardRange
                : 0.0;
            standardRatio = (std::max)(0.0,
                (std::min)(1.0, standardRatio));
            standardValueTrack.Width(standardWidth * standardRatio);
        }

        auto filledTrackHost = FilledVolumeTrackHost();
        auto filledValueTrack = FilledVolumeValueTrack();
        if (filledTrackHost && filledValueTrack)
        {
            double filledWidth = filledTrackHost.ActualWidth();
            double filledRange = VolumeSlider().Maximum() - VolumeSlider().Minimum();
            double filledRatio = filledRange > 0.0
                ? (VolumeSlider().Value() - VolumeSlider().Minimum()) / filledRange
                : 0.0;
            filledRatio = (std::max)(0.0, (std::min)(1.0, filledRatio));
            filledValueTrack.Width(filledWidth * filledRatio);
        }

        auto overlay = VolumeSliderOverlay();
        auto marker = VolumeSliderPositionMarker();
        if (!overlay || !marker) return;
        double width = overlay.ActualWidth();
        if (width <= 1.0) return;

        double range = VolumeSlider().Maximum() - VolumeSlider().Minimum();
        double ratio = range > 0.0
            ? (VolumeSlider().Value() - VolumeSlider().Minimum()) / range
            : 0.0;
        ratio = (std::max)(0.0, (std::min)(1.0, ratio));
        double left = ratio * width - marker.Width() / 2.0;
        left = (std::max)(0.0, (std::min)(width - marker.Width(), left));
        Microsoft::UI::Xaml::Controls::Canvas::SetLeft(marker, left);
        Microsoft::UI::Xaml::Controls::Canvas::SetTop(marker, 0.0);
    }

    void MainPage::SetVolumeSliderHovered(bool hovered)
    {
        if (hovered == m_volumeSliderHovered) return;
        m_volumeSliderHovered = hovered;

        auto marker = VolumeSliderPositionMarker();
        if (!marker) return;
        auto visual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(marker);
        visual.CenterPoint({
            static_cast<float>(marker.ActualWidth() / 2.0),
            static_cast<float>(marker.ActualHeight() / 2.0), 0.0f });
        auto compositor = visual.Compositor();
        auto easing = compositor.CreateCubicBezierEasingFunction(
            { 0.16f, 1.0f }, { 0.30f, 1.0f });
        auto animation = compositor.CreateVector3KeyFrameAnimation();
        animation.InsertKeyFrame(1.0f,
            { 1.0f, hovered ? 1.22f : 1.0f, 1.0f }, easing);
        animation.Duration(std::chrono::milliseconds(120));
        visual.StartAnimation(L"Scale", animation);
    }

    void MainPage::ShowVolumeFeedback()
    {
        // Segoe Fluent's volume family progressively adds sound waves. Keep
        // the mute glyph authoritative when mpv is muted, without changing the
        // remembered volume level.
        std::wstring muteValue;
        bool const muted =
            PlayerTryGetMpvRuntimeOption(L"mute", muteValue) && muteValue == L"yes";
        double volume = VolumeSlider().Value();
        wchar_t const* glyph = muted || volume <= 0.5 ? L"\uE74F" :
            (volume < 34.0 ? L"\uE993" :
                (volume < 67.0 ? L"\uE994" : L"\uE995"));
        if (VolumeIcon().Glyph() != glyph) VolumeIcon().Glyph(glyph);
    }

    void MainPage::FullscreenClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        PlayerToggleFullscreen();
    }

    void MainPage::TimelineSizeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::SizeChangedEventArgs const&)
    {
        // Rebuild during the resize layout pass instead of waiting for the
        // playback timer. This prevents a brief old-width timeline when the
        // window enters or leaves fullscreen.
        m_chapterMarkerWidth = -1.0;
        double elapsed{};
        double duration{};
        if (m_ready && PlayerGetPlaybackTimes(elapsed, duration))
        {
            RenderChapterMarkers(duration, elapsed);
        }
    }

    void MainPage::PlaybackControlsSizeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::SizeChangedEventArgs const& args)
    {
        UpdateResponsiveControls(args.NewSize().Width);
    }

    void MainPage::MinimalTransportBarSizeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::SizeChangedEventArgs const&)
    {
        if (!UsesMinimalTransportStyle())
        {
            return;
        }

        // SiteBridge resize can precede the XAML measure pass. Re-clip the
        // native transport HWND after the pill has its final measured bounds,
        // keeping the approved XAML geometry untouched at every window size/DPI.
        auto queue = DispatcherQueue();
        if (queue)
        {
            queue.TryEnqueue([]() { PlayerRefreshTransportLayout(); });
        }
    }

    void MainPage::TransportRootSizeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::SizeChangedEventArgs const&)
    {
        if (!UsesMinimalTransportStyle())
        {
            return;
        }

        // The Minimal pill has a capped width, so fullscreen can change its
        // horizontal POSITION without changing the pill's ActualWidth. The old
        // SizeChanged hook on MinimalTransportBar therefore never fired and the
        // native popup kept clipping at the pre-fullscreen X coordinate. Queue
        // one second layout pass after the XAML root has adopted the new client
        // width; MinimalTransportRegion() then reports the new centered position.
        auto queue = DispatcherQueue();
        if (queue)
        {
            queue.TryEnqueue([]() { PlayerRefreshTransportLayout(); });
        }
    }

    void MainPage::PipClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        PlayerTogglePictureInPicture();
    }

    void MainPage::StatsClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        PlayerToggleStats();
    }

    void MainPage::CaptureClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_videoOnlyActionsAllowed) return;

        // Visual-only shutter feedback. The primary button now uses mpv's
        // normal video screenshot (without subtitles); animation is unchanged.
        auto iconVisual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(CaptureIcon());
        auto pulseVisual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(CapturePulse());
        auto compositor = iconVisual.Compositor();

        float iconCenterX = static_cast<float>(CaptureIcon().ActualWidth() * 0.5);
        float iconCenterY = static_cast<float>(CaptureIcon().ActualHeight() * 0.5);
        if (iconCenterX <= 0.0f) iconCenterX = 7.0f;
        if (iconCenterY <= 0.0f) iconCenterY = 7.0f;
        iconVisual.CenterPoint({ iconCenterX, iconCenterY, 0.0f });

        auto shutter = compositor.CreateVector3KeyFrameAnimation();
        shutter.InsertKeyFrame(0.00f, { 1.00f, 1.00f, 1.00f });
        shutter.InsertKeyFrame(0.30f, { 0.88f, 0.88f, 1.00f });
        shutter.InsertKeyFrame(0.68f, { 1.070f, 1.070f, 1.00f });
        shutter.InsertKeyFrame(1.00f, { 1.00f, 1.00f, 1.00f });
        shutter.Duration(std::chrono::milliseconds(180));
        iconVisual.StartAnimation(L"Scale", shutter);

        auto shutterOpacity = compositor.CreateScalarKeyFrameAnimation();
        shutterOpacity.InsertKeyFrame(0.00f, 1.00f);
        shutterOpacity.InsertKeyFrame(0.30f, 0.64f);
        shutterOpacity.InsertKeyFrame(1.00f, 1.00f);
        shutterOpacity.Duration(std::chrono::milliseconds(160));
        iconVisual.StartAnimation(L"Opacity", shutterOpacity);

        float pulseCenterX = static_cast<float>(CapturePulse().ActualWidth() * 0.5);
        float pulseCenterY = static_cast<float>(CapturePulse().ActualHeight() * 0.5);
        if (pulseCenterX <= 0.0f) pulseCenterX = 8.0f;
        if (pulseCenterY <= 0.0f) pulseCenterY = 8.0f;
        pulseVisual.CenterPoint({ pulseCenterX, pulseCenterY, 0.0f });

        auto pulseScale = compositor.CreateVector3KeyFrameAnimation();
        pulseScale.InsertKeyFrame(0.00f, { 0.78f, 0.78f, 1.00f });
        pulseScale.InsertKeyFrame(1.00f, { 1.38f, 1.38f, 1.00f });
        pulseScale.Duration(std::chrono::milliseconds(225));
        pulseVisual.StartAnimation(L"Scale", pulseScale);

        auto pulseOpacity = compositor.CreateScalarKeyFrameAnimation();
        pulseOpacity.InsertKeyFrame(0.00f, 0.00f);
        pulseOpacity.InsertKeyFrame(0.18f, 0.60f);
        pulseOpacity.InsertKeyFrame(1.00f, 0.00f);
        pulseOpacity.Duration(std::chrono::milliseconds(225));
        pulseVisual.StartAnimation(L"Opacity", pulseOpacity);

        PlayerCaptureScreenshot(false);
    }

    void MainPage::CaptureWithSubtitlesClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_videoOnlyActionsAllowed) return;
        PlayerCaptureScreenshot(true);
    }

    void MainPage::CaptureVideoOnlyClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_videoOnlyActionsAllowed) return;
        PlayerCaptureScreenshot(false);
    }

    void MainPage::OpenCaptureFolderClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        PlayerOpenScreenshotDirectory();
    }

    void MainPage::MediaInfoClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        PlayerShowMediaInfo();
    }

    void MainPage::SettingsClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        PlayerShowSettings();
    }

    void MainPage::SettingsPointerEntered(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        Microsoft::UI::Xaml::Controls::AnimatedIcon::SetState(
            SettingsButton(), L"PointerOver");
    }

    void MainPage::SettingsPointerExited(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
    {
        Microsoft::UI::Xaml::Controls::AnimatedIcon::SetState(
            SettingsButton(), L"Normal");
    }

    void MainPage::RefreshChapterData(double duration, double elapsed)
    {
        auto chapters = PlayerGetMediaChapters();
        std::vector<ChapterView> refreshed;
        refreshed.reserve(chapters.size());
        for (auto const& chapter : chapters)
        {
            refreshed.push_back({ chapter.time, chapter.title });
        }

        bool changed = refreshed.size() != m_chapters.size();
        if (!changed)
        {
            for (size_t index = 0; index < refreshed.size(); ++index)
            {
                if (std::abs(refreshed[index].time - m_chapters[index].time) >= 0.01 ||
                    refreshed[index].title != m_chapters[index].title)
                {
                    changed = true;
                    break;
                }
            }
        }
        if (changed)
        {
            m_chapters = std::move(refreshed);
            m_chapterMarkerDuration = 0.0;
            m_minimalChapterMarkerDuration = 0.0;
            m_minimalChapterMarkerWidth = 0.0;
            m_minimalChapterSegments.clear();
            m_hoveredChapterSegment = -1;

            bool hasChapters = !m_chapters.empty();
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                PreviousChapterButton(), hasChapters
                    ? MainPageBoxString(L"MainPageDynPreviousChapter", L"Capítulo anterior")
                    : MainPageBoxString(L"MainPageDynPreviousFile", L"Arquivo anterior"));
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                NextChapterButton(), hasChapters
                    ? MainPageBoxString(L"MainPageDynNextChapter", L"Próximo capítulo")
                    : MainPageBoxString(L"MainPageDynNextFile", L"Próximo arquivo"));
        }
        RenderChapterMarkers(duration, elapsed);
    }

    void MainPage::UpdateChapterTitle(double elapsed)
    {
        // Reuse the exact same secondary text line and spacing already used by
        // video chapter titles. For audio-only media, including audio with
        // embedded/external cover art, this line becomes the Artist metadata.
        std::wstring artist;
        if (PlayerGetCurrentAudioArtist(artist))
        {
            EngineStatusText().Text(artist);
            EngineStatusText().Visibility(artist.empty()
                ? Microsoft::UI::Xaml::Visibility::Collapsed
                : Microsoft::UI::Xaml::Visibility::Visible);
            return;
        }

        std::wstring title;
        for (auto const& chapter : m_chapters)
        {
            if (chapter.time > elapsed + 0.05) break;
            title = chapter.title;
        }

        // Ordinary video keeps the established chapter-title behavior.
        // A file without a named current chapter leaves no placeholder behind.
        EngineStatusText().Text(title);
        EngineStatusText().Visibility(title.empty()
            ? Microsoft::UI::Xaml::Visibility::Collapsed
            : Microsoft::UI::Xaml::Visibility::Visible);
    }

    void MainPage::RenderChapterMarkers(double duration, double elapsed)
    {
        double width = ChapterMarkers().ActualWidth();
        if (duration <= 0.0 || width <= 1.0)
        {
            ChapterMarkers().Children().Clear();
            m_chapterSegments.clear();
            FilledTimelinePositionMarker().Visibility(
                Microsoft::UI::Xaml::Visibility::Collapsed);
            return;
        }

        bool rebuild = std::abs(duration - m_chapterMarkerDuration) >= 0.01 ||
            std::abs(width - m_chapterMarkerWidth) >= 0.5 ||
            m_chapterSegments.empty();
        if (rebuild)
        {
            auto children = ChapterMarkers().Children();
            children.Clear();
            m_chapterSegments.clear();
            m_hoveredChapterSegment = -1;

            std::vector<double> starts{ 0.0 };
            for (auto const& chapter : m_chapters)
            {
                if (chapter.time > 0.5 && chapter.time < duration - 0.5)
                {
                    starts.push_back(chapter.time);
                }
            }
            auto themeResources = Resources().ThemeDictionaries().Lookup(
                winrt::box_value(PlayerIsLightTheme() ? L"Light" : L"Dark"))
                .as<Microsoft::UI::Xaml::ResourceDictionary>();
            auto trackBrush = themeResources.Lookup(
                winrt::box_value(L"TimelineTrackBrush"))
                .as<Microsoft::UI::Xaml::Media::Brush>();
            auto cacheBrush = Resources().Lookup(
                winrt::box_value(L"TimelineCacheBrush"))
                .as<Microsoft::UI::Xaml::Media::Brush>();
            auto progressBrush = Resources().Lookup(
                winrt::box_value(L"TimelineProgressBrush"))
                .as<Microsoft::UI::Xaml::Media::Brush>();
            double const gap = m_filledTimelineStyle ? 1.0 : 4.0;
            double const segmentHeight = m_filledTimelineStyle ? 6.0 : 4.0;
            double const segmentTop = m_filledTimelineStyle ? 1.0 : 2.0;
            double const cornerRadius = m_filledTimelineStyle ? 1.5 : 2.0;

            for (size_t index = 0; index < starts.size(); ++index)
            {
                double start = starts[index];
                double end = index + 1 < starts.size()
                    ? starts[index + 1] : duration;
                double left = start * width / duration +
                    (index == 0 ? 0.0 : gap / 2.0);
                double right = end * width / duration -
                    (index + 1 == starts.size() ? 0.0 : gap / 2.0);
                double segmentWidth = (std::max)(1.0, right - left);

                Microsoft::UI::Xaml::Controls::Border segment;
                segment.Width(segmentWidth);
                segment.Height(segmentHeight);
                segment.CornerRadius({ cornerRadius, cornerRadius,
                    cornerRadius, cornerRadius });
                segment.Background(trackBrush);

                Microsoft::UI::Xaml::Controls::Grid layers;
                Microsoft::UI::Xaml::Controls::Border cache;
                cache.Width(0.0);
                cache.Height(segmentHeight);
                cache.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Left);
                cache.CornerRadius({ cornerRadius, cornerRadius,
                    cornerRadius, cornerRadius });
                cache.Background(cacheBrush);

                Microsoft::UI::Xaml::Controls::Border fill;
                fill.Width(0.0);
                fill.Height(segmentHeight);
                fill.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Left);
                fill.CornerRadius({ cornerRadius, cornerRadius,
                    cornerRadius, cornerRadius });
                fill.Background(progressBrush);
                layers.Children().Append(cache);
                layers.Children().Append(fill);
                segment.Child(layers);

                Microsoft::UI::Xaml::Controls::Canvas::SetLeft(segment, left);
                Microsoft::UI::Xaml::Controls::Canvas::SetTop(segment, segmentTop);
                children.Append(segment);
                auto visual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                    GetElementVisual(segment);
                visual.CenterPoint({ static_cast<float>(segmentWidth / 2.0),
                    static_cast<float>(segmentHeight / 2.0), 0.0f });
                visual.Scale({ 1.0f, 1.0f, 1.0f });

                m_chapterSegments.push_back({
                    start, end, segmentWidth, segment, cache, fill });
            }
            m_chapterMarkerDuration = duration;
            m_chapterMarkerWidth = width;
        }

        bool lightTheme = PlayerIsLightTheme();
        if (rebuild || !m_chapterThemeInitialized ||
            m_chapterThemeWasLight != lightTheme)
        {
            // Keep the same ThemeResource brush instance on every segment.
            // Its SystemAccentColor binding then follows Windows immediately,
            // just like the native slider thumb and volume track.
            auto themeResources = Resources().ThemeDictionaries().Lookup(
                winrt::box_value(lightTheme ? L"Light" : L"Dark"))
                .as<Microsoft::UI::Xaml::ResourceDictionary>();
            auto currentTrackBrush = themeResources.Lookup(
                winrt::box_value(L"TimelineTrackBrush"))
                .as<Microsoft::UI::Xaml::Media::Brush>();
            auto currentProgressBrush = Resources().Lookup(
                winrt::box_value(L"TimelineProgressBrush"))
                .as<Microsoft::UI::Xaml::Media::Brush>();
            auto currentCacheBrush = Resources().Lookup(
                winrt::box_value(L"TimelineCacheBrush"))
                .as<Microsoft::UI::Xaml::Media::Brush>();
            for (auto const& segment : m_chapterSegments)
            {
                segment.root.Background(currentTrackBrush);
                segment.cache.Background(currentCacheBrush);
                segment.fill.Background(currentProgressBrush);
            }
            m_chapterThemeInitialized = true;
            m_chapterThemeWasLight = lightTheme;
        }
        // mpv's final time-pos can stop a fraction before duration. Once EOF
        // has been confirmed, render the visual clock exactly at duration so
        // neither the filled track nor the capsule leaves a tiny tail at 100%.
        double const visualElapsed = m_isReplay ? duration : elapsed;

        for (auto const& segment : m_chapterSegments)
        {
            double cacheRatio{};
            if (m_webCacheEnd >= segment.end) cacheRatio = 1.0;
            else if (m_webCacheEnd > segment.start)
                cacheRatio = (m_webCacheEnd - segment.start) /
                (segment.end - segment.start);

            cacheRatio = (std::max)(
                0.0, (std::min)(1.0, cacheRatio));

            double cacheWidth = segment.width * cacheRatio;

            // mpv can report cache-end a tiny fraction before the media's
            // absolute end even when the stream is effectively buffered to
            // the end. On the LAST timeline segment this appears as a small
            // unfilled tail. Snap only that tiny final visual remainder.
            bool const lastSegment =
                segment.end >= duration - 0.01;

            constexpr double FinalCacheSnapDip = 6.0;
            double const remainingWidth =
                segment.width - cacheWidth;

            if (lastSegment &&
                cacheWidth > 0.0 &&
                remainingWidth > 0.0 &&
                remainingWidth <= FinalCacheSnapDip)
            {
                cacheWidth = segment.width;
            }

            segment.cache.Width(cacheWidth);

            double ratio{};
            if (visualElapsed >= segment.end) ratio = 1.0;
            else if (visualElapsed > segment.start)
                ratio = (visualElapsed - segment.start) / (segment.end - segment.start);
            segment.fill.Width(segment.width * (std::max)(0.0, (std::min)(1.0, ratio)));
        }

        if (m_filledTimelineStyle)
        {
            auto marker = FilledTimelinePositionMarker();
            double markerWidth = marker.Width();
            double ratio = (std::max)(0.0, (std::min)(
                1.0, visualElapsed / duration));
            // Keep the marker centred on the real 0..100% track endpoints.
            // FilledTimelineOverlay already has a 10 px outer margin, so the
            // 12 px capsule can safely overhang the track by 6 px at either end.
            // Clamping it inside the Canvas moved its centre inward and exposed
            // a small loose tail of track at 0%/100%.
            double left = ratio * width - markerWidth / 2.0;
            Microsoft::UI::Xaml::Controls::Canvas::SetLeft(marker, left);
            Microsoft::UI::Xaml::Controls::Canvas::SetTop(marker, 0.0);
            marker.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
        }
        else
        {
            FilledTimelinePositionMarker().Visibility(
                Microsoft::UI::Xaml::Visibility::Collapsed);
        }

    }

    void MainPage::RenderMinimalChapterMarkers(double duration, double elapsed)
    {
        using Microsoft::UI::Xaml::Controls::Border;
        using Microsoft::UI::Xaml::Controls::Canvas;
        using Microsoft::UI::Xaml::Controls::Grid;
        using Microsoft::UI::Xaml::HorizontalAlignment;

        auto canvas = MinimalChapterMarkers();
        double const width = canvas.ActualWidth();
        if (duration <= 0.0 || width <= 1.0)
        {
            canvas.Children().Clear();
            m_minimalChapterSegments.clear();
            return;
        }

        bool const lightTheme = PlayerIsLightTheme();
        bool const rebuild =
            std::abs(duration - m_minimalChapterMarkerDuration) >= 0.01 ||
            std::abs(width - m_minimalChapterMarkerWidth) >= 0.5 ||
            m_minimalChapterSegments.empty() ||
            m_minimalChapterMarkerFilledStyle != m_filledTimelineStyle ||
            m_minimalChapterThemeWasLight != lightTheme;

        if (rebuild)
        {
            auto children = canvas.Children();
            children.Clear();
            m_minimalChapterSegments.clear();

            std::vector<double> starts{ 0.0 };
            for (auto const& chapter : m_chapters)
            {
                if (chapter.time > 0.5 && chapter.time < duration - 0.5)
                {
                    starts.push_back(chapter.time);
                }
            }

            auto themeResources = Resources().ThemeDictionaries().Lookup(
                winrt::box_value(lightTheme ? L"Light" : L"Dark"))
                .as<Microsoft::UI::Xaml::ResourceDictionary>();
            auto trackBrush = themeResources.Lookup(
                winrt::box_value(L"TimelineTrackBrush"))
                .as<Microsoft::UI::Xaml::Media::Brush>();
            auto cacheBrush = Resources().Lookup(
                winrt::box_value(L"TimelineCacheBrush"))
                .as<Microsoft::UI::Xaml::Media::Brush>();
            auto progressBrush = Resources().Lookup(
                winrt::box_value(L"TimelineProgressBrush"))
                .as<Microsoft::UI::Xaml::Media::Brush>();

            // Same segment geometry used by the normal transport. Minimal has a
            // fixed 14-DIP track inset, but the chapter gaps/heights/radii are
            // intentionally identical to the selected normal-bar style.
            double const gap = m_filledTimelineStyle ? 1.0 : 4.0;
            double const segmentHeight = m_filledTimelineStyle ? 6.0 : 4.0;
            double const segmentTop = m_filledTimelineStyle ? 1.0 : 2.0;
            double const cornerRadius = m_filledTimelineStyle ? 1.5 : 2.0;

            for (size_t index = 0; index < starts.size(); ++index)
            {
                double const segmentStart = starts[index];
                double const segmentEnd = index + 1 < starts.size()
                    ? starts[index + 1] : duration;
                double const left = segmentStart * width / duration +
                    (index == 0 ? 0.0 : gap / 2.0);
                double const right = segmentEnd * width / duration -
                    (index + 1 == starts.size() ? 0.0 : gap / 2.0);
                double const segmentWidth = (std::max)(1.0, right - left);

                Border segment;
                segment.Width(segmentWidth);
                segment.Height(segmentHeight);
                segment.CornerRadius({ cornerRadius, cornerRadius,
                    cornerRadius, cornerRadius });
                segment.Background(trackBrush);

                Grid layers;
                Border cache;
                cache.Width(0.0);
                cache.Height(segmentHeight);
                cache.HorizontalAlignment(HorizontalAlignment::Left);
                cache.CornerRadius({ cornerRadius, cornerRadius,
                    cornerRadius, cornerRadius });
                cache.Background(cacheBrush);

                Border fill;
                fill.Width(0.0);
                fill.Height(segmentHeight);
                fill.HorizontalAlignment(HorizontalAlignment::Left);
                fill.CornerRadius({ cornerRadius, cornerRadius,
                    cornerRadius, cornerRadius });
                fill.Background(progressBrush);

                layers.Children().Append(cache);
                layers.Children().Append(fill);
                segment.Child(layers);
                Canvas::SetLeft(segment, left);
                Canvas::SetTop(segment, segmentTop);
                children.Append(segment);

                m_minimalChapterSegments.push_back({
                    segmentStart, segmentEnd, segmentWidth, segment, cache, fill });
            }

            m_minimalChapterMarkerDuration = duration;
            m_minimalChapterMarkerWidth = width;
            m_minimalChapterMarkerFilledStyle = m_filledTimelineStyle;
            m_minimalChapterThemeWasLight = lightTheme;
        }

        double const visualElapsed = m_isReplay ? duration : elapsed;
        for (auto const& segment : m_minimalChapterSegments)
        {
            double cacheRatio{};
            if (m_webCacheEnd >= segment.end) cacheRatio = 1.0;
            else if (m_webCacheEnd > segment.start)
                cacheRatio = (m_webCacheEnd - segment.start) /
                (segment.end - segment.start);
            cacheRatio = (std::max)(0.0, (std::min)(1.0, cacheRatio));
            segment.cache.Width(segment.width * cacheRatio);

            double fillRatio{};
            if (visualElapsed >= segment.end) fillRatio = 1.0;
            else if (visualElapsed > segment.start)
                fillRatio = (visualElapsed - segment.start) /
                (segment.end - segment.start);
            fillRatio = (std::max)(0.0, (std::min)(1.0, fillRatio));
            segment.fill.Width(segment.width * fillRatio);
        }
    }

    void MainPage::UpdateMinimalTimelineVisual()
    {
        using Microsoft::UI::Xaml::Visibility;
        using Microsoft::UI::Xaml::Controls::Canvas;

        auto marker = MinimalFilledTimelinePositionMarker();

        double elapsed{};
        double duration{};
        if (!PlayerGetPlaybackTimes(elapsed, duration) ||
            !std::isfinite(duration) || duration <= 0.0)
        {
            MinimalChapterMarkers().Children().Clear();
            m_minimalChapterSegments.clear();
            marker.Visibility(Visibility::Collapsed);
            return;
        }

        RenderMinimalChapterMarkers(duration, elapsed);

        if (!m_filledTimelineStyle)
        {
            marker.Visibility(Visibility::Collapsed);
            return;
        }

        double const width = MinimalChapterMarkers().ActualWidth();
        if (width <= 1.0)
        {
            marker.Visibility(Visibility::Collapsed);
            return;
        }

        auto slider = MinimalPositionSlider();
        double const range = slider.Maximum() - slider.Minimum();
        double const ratio = range > 0.0
            ? (slider.Value() - slider.Minimum()) / range
            : 0.0;
        double const clamped = (std::max)(0.0, (std::min)(1.0, ratio));

        // Exactly the same 12x24 outer capsule and 4x16 accent core used by
        // FilledTimelinePositionMarker. Let its centre overhang by 6 DIP at
        // 0/100%, matching the established HC Player/PiP endpoint geometry.
        double const markerWidth = marker.Width();
        Canvas::SetLeft(marker, clamped * width - markerWidth / 2.0);
        Canvas::SetTop(marker, 0.0);
        marker.Visibility(Visibility::Visible);
    }

    void MainPage::SetHoveredChapterSegment(int32_t index)
    {
        if (index == m_hoveredChapterSegment) return;

        // The filled style reads as one continuous control. Chapter titles
        // still update in the tooltip, but individual sections do not pop out.
        if (m_filledTimelineStyle)
        {
            m_hoveredChapterSegment = index;
            return;
        }

        auto animate = [](Microsoft::UI::Xaml::Controls::Border const& segment,
            float verticalScale)
            {
                if (!segment) return;
                auto visual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                    GetElementVisual(segment);
                auto compositor = visual.Compositor();
                auto easing = compositor.CreateCubicBezierEasingFunction(
                    { 0.16f, 1.0f }, { 0.30f, 1.0f });
                auto animation = compositor.CreateVector3KeyFrameAnimation();
                animation.InsertKeyFrame(1.0f, { 1.0f, verticalScale, 1.0f }, easing);
                animation.Duration(std::chrono::milliseconds(140));
                visual.StartAnimation(L"Scale", animation);
            };

        if (m_hoveredChapterSegment >= 0 &&
            static_cast<size_t>(m_hoveredChapterSegment) < m_chapterSegments.size())
        {
            animate(m_chapterSegments[m_hoveredChapterSegment].root, 1.0f);
        }
        if (index >= 0 && static_cast<size_t>(index) < m_chapterSegments.size())
        {
            animate(m_chapterSegments[index].root, 1.55f);
        }
        m_hoveredChapterSegment = index;
    }

    void MainPage::SetFilledTimelineHovered(bool hovered)
    {
        if (!m_filledTimelineStyle || hovered == m_filledTimelineHovered)
            return;

        m_filledTimelineHovered = hovered;

        auto animate = [hovered](Microsoft::UI::Xaml::UIElement const& element)
            {
                auto visual = Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                    GetElementVisual(element);
                visual.CenterPoint({ static_cast<float>(
                    element.as<Microsoft::UI::Xaml::FrameworkElement>().ActualWidth() / 2.0),
                    static_cast<float>(
                    element.as<Microsoft::UI::Xaml::FrameworkElement>().ActualHeight() / 2.0),
                    0.0f });

                auto compositor = visual.Compositor();
                auto easing = compositor.CreateCubicBezierEasingFunction(
                    { 0.16f, 1.0f }, { 0.30f, 1.0f });
                auto animation = compositor.CreateVector3KeyFrameAnimation();
                animation.InsertKeyFrame(1.0f,
                    { 1.0f, hovered ? 1.22f : 1.0f, 1.0f }, easing);
                animation.Duration(std::chrono::milliseconds(120));
                visual.StartAnimation(L"Scale", animation);
            };

        // The normal bar/PiP and Minimal mode now share the exact same
        // composition hover language: the track and position indicator grow
        // vertically by 22% with the same easing and duration. Hidden elements
        // are harmless to animate and keep one authoritative hover state.
        animate(ChapterMarkers());
        animate(FilledTimelineOverlay());
        animate(MinimalChapterMarkers());
        animate(MinimalFilledTimelineOverlay());
    }

    void MainPage::ProgressTimerTick(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        bool consoleOpen = PlayerIsConsoleOpen();

        if (consoleOpen != m_consoleOpen)
        {
            m_consoleOpen = consoleOpen;
            m_transportHideTimer.Stop();

            if (consoleOpen)
            {
                // O campo de entrada do console fica na parte inferior.
                // Escondemos a barra para não cobri-lo.
                SetTransportVisible(false, false);
            }
            else if (!m_settingsOverlayOpen)
            {
                if (!m_ready)
                {
                    // Sem mídia aberta, a barra da tela inicial continua visível.
                    SetTransportVisible(true, false);
                }
                else if (PlayerIsCursorInTransportHotZone())
                {
                    // Com vídeo, só mostra a barra se o mouse realmente estiver
                    // na região dos controles.
                    SetTransportVisible(true, false);
                }
                else
                {
                    // Mouse longe dos controles: volta diretamente ao vídeo limpo.
                    SetTransportVisible(false, false);
                }
            }
        }

        // ShowWindow() can precede mpv's first presentable frame. Keep genuine
        // pointer movement from arming the transport during that opening gap.
        // Reuse the existing 250-ms UI poll: no new timer, wait, or playback
        // synchronization is introduced. The fallback is defensive only, so a
        // malformed/failed item can never leave transport input gated forever.
        if (m_transportStartupGuard)
        {
            constexpr uint32_t TransportStartupGuardFallbackTicks = 80; // 20 s
            constexpr uint32_t TransportStartupReadySamples = 2;

            ++m_transportStartupGuardTicks;
            if (m_ready && PlayerIsMediaPresentationReady())
            {
                ++m_transportStartupReadySamples;
            }
            else
            {
                m_transportStartupReadySamples = 0;
            }

            // Require two consecutive ready samples. Besides ensuring the video
            // output is stable, this prevents an outgoing file's final mpv
            // properties from ending the guard during an asynchronous replace.
            if (m_transportStartupReadySamples >=
                    TransportStartupReadySamples ||
                m_transportStartupGuardTicks >=
                    TransportStartupGuardFallbackTicks)
            {
                m_transportStartupGuard = false;
                m_transportStartupGuardTicks = 0;
                m_transportStartupReadySamples = 0;

                // Movement made while the gate was active must not carry over.
                // HCPlayer.cpp has already kept its physical cursor baseline up
                // to date, so the next real movement after this point is the
                // first one allowed to arm the normal hot-zone behavior.
                m_transportRevealArmed = false;

                // The same presentation boundary is a safe point to expose
                // capture/profiles only when this item is genuinely video.
                RefreshVideoOnlyActionEligibility();
            }
        }

        // This dispatcher-backed fallback remains reliable even when the MPV
        // video surface consumes native mouse messages during presentation.
        // After a fresh media open, do not let a stationary cursor already in
        // the hot-zone reveal controls; genuine WM_MOUSEMOVE arms it first.
        if (m_transportRevealArmed && PlayerIsCursorInTransportHotZone())
        {
            TransportHostPointerEntered();
        }

        if (!m_ready)
        {
            return;
        }

        // The current item can change without a new OpenPath() call (playlist,
        // Previous/Next, Shuffle). Images have no timeline in either style, and
        // the native transport host shrinks/expands together with that state.
        bool const imageWithoutTimeline =
            m_mediaControlsExpanded &&
            PlayerIsCurrentMediaImage();

        bool const minimalTransportActive =
            m_minimalTransportStyle &&
            m_mediaControlsExpanded &&
            !m_pictureInPicture;

        if (minimalTransportActive)
        {
            auto const minimalTimelineVisibility = imageWithoutTimeline
                ? Microsoft::UI::Xaml::Visibility::Collapsed
                : Microsoft::UI::Xaml::Visibility::Visible;
            MinimalTimelineHost().Visibility(minimalTimelineVisibility);
            MinimalCurrentTimeText().Visibility(minimalTimelineVisibility);
            MinimalDurationTimeText().Visibility(minimalTimelineVisibility);
        }

        auto const desiredTimelineVisibility =
            (m_mediaControlsExpanded && !imageWithoutTimeline &&
                !minimalTransportActive)
                ? Microsoft::UI::Xaml::Visibility::Visible
                : Microsoft::UI::Xaml::Visibility::Collapsed;

        if (TimelineRow().Visibility() != desiredTimelineVisibility)
        {
            TimelineRow().Visibility(desiredTimelineVisibility);
            PlayerSetTransportImageMode(imageWithoutTimeline);

            if (!m_pictureInPicture)
            {
                PlaybackControlsRow().Translation(
                    Windows::Foundation::Numerics::float3{
                        0.0f,
                        imageWithoutTimeline ? 6.0f : 0.0f,
                        0.0f });
            }

            if (imageWithoutTimeline)
            {
                ChapterHoverCard().Visibility(
                    Microsoft::UI::Xaml::Visibility::Collapsed);
                ChapterHoverPopup().IsOpen(false);
                SetHoveredChapterSegment(-1);
                SetFilledTimelineHovered(false);
            }
        }

        PlayerUpdateWebBufferingIndicator();

        double elapsed{};
        double duration{};
        if (PlayerGetPlaybackTimes(elapsed, duration))
        {
            UpdateTimeDisplay(elapsed, duration);
            double cacheEnd{};
            m_webCacheEnd = PlayerGetWebCacheEnd(cacheEnd)
                ? (std::max)(elapsed, cacheEnd) : 0.0;
            double position = duration > 0.0
                ? (std::min)(100.0, elapsed * 100.0 / duration)
                : PlayerGetPositionPercent();

            // Safety net: if Windows reports the physical button released but
            // PointerReleased/CaptureLost was missed, finalize the last
            // pointer-owned timestamp exactly before returning timer ownership.
            if (m_timelineUserInteraction &&
                (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0)
            {
                if (m_timelineInteractionHasTarget)
                {
                    PlayerSeekAbsoluteExact(
                        m_timelineInteractionSeconds);
                }

                m_timelineUserInteraction = false;
                m_timelineProgressHoldTicks = 2;
                m_timelineInteractionHasTarget = false;
            }

            bool const userOwnsSlider =
                m_timelineUserInteraction ||
                m_timelineProgressHoldTicks > 0;

            if (!userOwnsSlider)
            {
                m_isUpdatingPosition = true;
                if (position >= 0.0)
                {
                    PositionSlider().Value(position);
                    MinimalPositionSlider().Value(position);
                }
                m_isUpdatingPosition = false;
            }
            else if (!m_timelineUserInteraction &&
                m_timelineProgressHoldTicks > 0)
            {
                --m_timelineProgressHoldTicks;
            }

            if (m_chapterRefreshCountdown == 0)
            {
                RefreshChapterData(duration, elapsed);
                m_chapterRefreshCountdown = 4;
            }
            else
            {
                --m_chapterRefreshCountdown;
                RenderChapterMarkers(duration, elapsed);
            }
            UpdateChapterTitle(elapsed);
        }
        // Keep the toolbar Loop indicator synchronized with mpv itself. The
        // default L binding continues to execute unchanged inside mpv; this is
        // read-only UI synchronization on the existing 250-ms progress poll.
        bool const liveLoopEnabled = PlayerGetLooping();
        if (liveLoopEnabled != m_loopEnabled)
        {
            m_loopEnabled = liveLoopEnabled;
            UpdateLoopButtonState();
        }

        bool paused{};
        bool eofReached{};
        if (PlayerGetPlaybackState(paused, eofReached))
        {
            hc::system_media_controls::UpdatePlaybackState(paused, eofReached);
            bool autoAdvanced = false;

            // Only react to the transition into EOF. If the last item remains
            // parked at the end, never issue playlist-next repeatedly.
            // File loop keeps priority over continuous playback.
            if (eofReached &&
                !m_lastEofReached &&
                m_continuousPlayback &&
                !m_loopEnabled)
            {
                m_lastEofReached = true;
                autoAdvanced = PlayerAdvanceContinuousPlayback();
            }
            else if (!eofReached)
            {
                m_lastEofReached = false;
            }

            if (autoAdvanced)
            {
                m_isReplay = false;
                m_isPlaying = true;
                UpdatePlayButtonState();
            }
            else
            {
                bool stateChanged = m_isReplay != eofReached ||
                    m_isPlaying != (!paused && !eofReached);
                m_isReplay = eofReached;
                m_isPlaying = !paused && !eofReached;
                if (stateChanged) UpdatePlayButtonState();
            }
        }
        PlayerUpdateTaskbarProgress();
        if ((++m_progressTickCount % 4) == 0)
        {
            // Playlist navigation can replace the current item without OpenPath().
            // Reclassify once per second so video-only actions cannot leak into
            // audio or still-image items, without adding a new timer.
            RefreshVideoOnlyActionEligibility();

            // Keep album art on audio-only files fitted to the current video
            // surface. The MPV options are file-local and disappear with the
            // audio item, so photos and real videos keep their existing scale.
            PlayerApplyAudioCoverScalingPolicy();

            // Online metadata arrives after yt-dlp resolves the URL. Polling
            // once per second is enough to replace the temporary URL label
            // without adding work to MPV's rendering path.
            auto const mediaTitle = PlayerGetMediaTitle();
            if (!mediaTitle.empty() && NowPlayingText().Text() != mediaTitle)
            {
                NowPlayingText().Text(mediaTitle);
            }

            if (PlayerIsCurrentMediaImage())
            {
                hc::system_media_controls::Clear();
            }
            else
            {
                std::wstring systemMediaArtist;
                bool const audioOnly =
                    PlayerGetCurrentAudioArtist(systemMediaArtist);
                std::wstring systemMediaTitle = mediaTitle;
                if (systemMediaTitle.empty())
                {
                    systemMediaTitle = NowPlayingText().Text().c_str();
                }
                hc::system_media_controls::UpdateMetadata(
                    systemMediaTitle, systemMediaArtist, audioOnly);
            }
            // Reflect [ and ] keyboard shortcuts without querying MPV on every
            // 250 ms timeline update.
            UpdateSpeedLabel(PlayerGetPlaybackSpeed());
            m_isUpdatingVolume = true;
            double volume = PlayerGetVolume();
            VolumeSlider().Value(volume);
            StandardVolumeSlider().Value(volume);
            m_isUpdatingVolume = false;
            UpdateVolumeMarker();
            RefreshMediaBadges();
        }
    }
}


