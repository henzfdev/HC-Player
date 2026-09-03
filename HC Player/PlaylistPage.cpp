#include "pch.h"
#include "PlaylistPage.h"
#include "LocalizationManager.h"

#if __has_include("PlaylistPage.g.cpp")
#include "PlaylistPage.g.cpp"
#endif

#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

using namespace winrt;

namespace
{
    std::wstring PlaylistString(
        std::wstring_view resourceId,
        std::wstring_view fallback)
    {
        return hc::localization::GetString(resourceId, fallback);
    }
}

namespace winrt::HCPlayer::implementation
{
    PlaylistPage::PlaylistPage()
    {
        RequestedTheme(PlayerIsLightTheme()
            ? Microsoft::UI::Xaml::ElementTheme::Light
            : Microsoft::UI::Xaml::ElementTheme::Dark);

        // Read-only UI refresh while the queue is visible. It never sends a
        // playback command and is stopped as soon as the panel closes.
        m_refreshTimer = Microsoft::UI::Xaml::DispatcherTimer{};
        m_refreshTimer.Interval(std::chrono::milliseconds(700));
        m_refreshTimer.Tick({ this, &PlaylistPage::RefreshTimerTick });
    }

    void PlaylistPage::PlaylistLoaded(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
    }

    void PlaylistPage::PrepareForOpen()
    {
        m_closing = false;

        RequestedTheme(PlayerIsLightTheme()
            ? Microsoft::UI::Xaml::ElementTheme::Light
            : Microsoft::UI::Xaml::ElementTheme::Dark);

        PlaylistRoot().IsHitTestVisible(true);
        PlaylistTranslate().X(0.0);
        PlaylistRoot().Opacity(1.0);
        SetExternalDropActive(false);

        auto visual =
            Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                GetElementVisual(PlaylistRoot());
        visual.StopAnimation(L"Offset");
        visual.Offset({ -112.0f, 0.0f, 0.0f });

        auto headerVisual =
            Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                GetElementVisual(PlaylistHeader());
        headerVisual.StopAnimation(L"Opacity");
        headerVisual.Opacity(0.76f);

        auto contentVisual =
            Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                GetElementVisual(PlaylistScrollViewer());
        contentVisual.StopAnimation(L"Opacity");
        contentVisual.Opacity(0.88f);

        m_hasSnapshot = false;
        m_lastSignature.clear();
        RefreshList();
        m_refreshTimer.Start();
    }

    void PlaylistPage::PrepareForClose()
    {
        m_refreshTimer.Stop();
        SetExternalDropActive(false);
        CancelReorderDrag();
    }

    void PlaylistPage::SetExternalDropActive(bool active)
    {
        // Explorer/OLE drag is independent from the pointer-capture drag used
        // to reorder rows. If an old internal drag is still armed, cancel it
        // before showing the external-drop affordance.
        if (active && m_reorderDragging) CancelReorderDrag();
        if (m_closing) active = false;

        ExternalDropOverlay().Visibility(
            active
                ? Microsoft::UI::Xaml::Visibility::Visible
                : Microsoft::UI::Xaml::Visibility::Collapsed);
    }

    void PlaylistPage::CompleteExternalDrop(bool queueChanged)
    {
        SetExternalDropActive(false);
        if (!queueChanged || m_closing) return;

        // mpv has already mutated its native playlist. Invalidate the cached
        // visual signature so the newly appended rows appear immediately rather
        // than waiting for the periodic read-only refresh tick.
        m_hasSnapshot = false;
        RefreshList();
    }

    void PlaylistPage::BeginOpenAnimation()
    {
        auto root = PlaylistRoot();
        auto header = PlaylistHeader();
        auto content = PlaylistScrollViewer();

        root.DispatcherQueue().TryEnqueue([root, header, content]()
        {
            auto visual =
                Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                    GetElementVisual(root);
            auto compositor = visual.Compositor();

            auto arrival = compositor.CreateCubicBezierEasingFunction(
                { 0.16f, 1.0f }, { 0.30f, 1.0f });
            auto settle = compositor.CreateCubicBezierEasingFunction(
                { 0.20f, 0.0f }, { 0.20f, 1.0f });

            auto slide = compositor.CreateVector3KeyFrameAnimation();
            slide.InsertKeyFrame(0.82f, { 3.0f, 0.0f, 0.0f }, arrival);
            slide.InsertKeyFrame(1.0f, { 0.0f, 0.0f, 0.0f }, settle);
            slide.Duration(std::chrono::milliseconds(360));
            slide.StopBehavior(
                Microsoft::UI::Composition::AnimationStopBehavior::SetToFinalValue);
            visual.StartAnimation(L"Offset", slide);

            auto animateOpacity = [compositor, arrival](
                auto const& target,
                std::chrono::milliseconds delay,
                std::chrono::milliseconds duration)
            {
                auto targetVisual =
                    Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                        GetElementVisual(target);
                auto opacity = compositor.CreateScalarKeyFrameAnimation();
                opacity.InsertKeyFrame(1.0f, 1.0f, arrival);
                opacity.DelayTime(delay);
                opacity.Duration(duration);
                opacity.StopBehavior(
                    Microsoft::UI::Composition::AnimationStopBehavior::SetToFinalValue);
                targetVisual.StartAnimation(L"Opacity", opacity);
            };

            animateOpacity(header,
                std::chrono::milliseconds(45),
                std::chrono::milliseconds(235));
            animateOpacity(content,
                std::chrono::milliseconds(75),
                std::chrono::milliseconds(285));
        });
    }

    void PlaylistPage::RequestClose()
    {
        if (m_closing)
        {
            return;
        }

        m_closing = true;
        PrepareForClose();

        using namespace Microsoft::UI::Xaml::Media::Animation;

        auto slide = DoubleAnimation{};
        slide.To(-520.0);
        slide.Duration(Microsoft::UI::Xaml::DurationHelper::FromTimeSpan(
            std::chrono::milliseconds(190)));
        slide.EnableDependentAnimation(true);

        auto ease = CubicEase{};
        ease.EasingMode(EasingMode::EaseIn);
        slide.EasingFunction(ease);

        Storyboard::SetTarget(slide, PlaylistTranslate());
        Storyboard::SetTargetProperty(slide, L"X");

        auto fade = DoubleAnimation{};
        fade.To(0.0);
        fade.Duration(Microsoft::UI::Xaml::DurationHelper::FromTimeSpan(
            std::chrono::milliseconds(145)));
        Storyboard::SetTarget(fade, PlaylistRoot());
        Storyboard::SetTargetProperty(fade, L"Opacity");

        auto storyboard = Storyboard{};
        storyboard.Children().Append(slide);
        storyboard.Children().Append(fade);

        auto root = PlaylistRoot();
        storyboard.Completed([root](auto&&, auto&&)
        {
            root.IsHitTestVisible(false);
            PlayerClosePlaylist();
        });
        storyboard.Begin();
    }

    void PlaylistPage::ScrollBy(int wheelDelta)
    {
        double const target =
            PlaylistScrollViewer().VerticalOffset() - wheelDelta;
        PlaylistScrollViewer().ChangeView(nullptr, target, nullptr, false);
    }

    void PlaylistPage::CancelReorderDrag()
    {
        for (auto const& indicator : m_dropTopIndicators)
        {
            if (indicator) indicator.Opacity(0.0);
        }
        for (auto const& indicator : m_dropBottomIndicators)
        {
            if (indicator) indicator.Opacity(0.0);
        }

        m_reorderDragging = false;
        m_dragSourceIndex = -1;
        m_dragSourceFilename.clear();
        m_dragSnapshotFilenames.clear();
        m_dragDropSlot = -1;
    }

    int PlaylistPage::CalculateDropSlot(double pointerY)
    {
        auto host = PlaylistItemsHost();
        auto children = host.Children();
        uint32_t const count = children.Size();
        if (count == 0) return 0;

        double top = 0.0;
        double const spacing = host.Spacing();
        for (uint32_t index = 0; index < count; ++index)
        {
            auto row = children.GetAt(index)
                .try_as<Microsoft::UI::Xaml::FrameworkElement>();
            double const height = row ? row.ActualHeight() : 0.0;
            if (pointerY < top + (height * 0.5))
            {
                return static_cast<int>(index);
            }
            top += height + spacing;
        }
        return static_cast<int>(count);
    }

    void PlaylistPage::ShowDropSlot(int slot)
    {
        int const count = static_cast<int>(m_dropTopIndicators.size());
        slot = (std::max)(0, (std::min)(slot, count));
        if (slot == m_dragDropSlot) return;

        // PointerMoved can fire frequently. Touch only the previous and next
        // indicator instead of rewriting every row in a long playlist.
        if (m_dragDropSlot >= 0)
        {
            if (m_dragDropSlot < count)
            {
                m_dropTopIndicators[m_dragDropSlot].Opacity(0.0);
            }
            else if (!m_dropBottomIndicators.empty())
            {
                m_dropBottomIndicators.back().Opacity(0.0);
            }
        }

        m_dragDropSlot = slot;
        if (slot < count)
        {
            m_dropTopIndicators[slot].Opacity(1.0);
        }
        else if (!m_dropBottomIndicators.empty())
        {
            m_dropBottomIndicators.back().Opacity(1.0);
        }
    }

    bool PlaylistPage::CommitReorderDrag()
    {
        if (!m_reorderDragging || m_dragSourceIndex < 0 ||
            m_dragDropSlot < 0 || m_dragSnapshotFilenames.empty())
        {
            return false;
        }

        auto const livePlaylist = PlayerGetPlaylistItems();
        if (livePlaylist.size() != m_dragSnapshotFilenames.size()) return false;

        for (size_t index = 0; index < livePlaylist.size(); ++index)
        {
            if (livePlaylist[index].filename != m_dragSnapshotFilenames[index])
            {
                return false;
            }
        }

        if (m_dragSourceIndex >= static_cast<int64_t>(livePlaylist.size()) ||
            livePlaylist[static_cast<size_t>(m_dragSourceIndex)].filename !=
                m_dragSourceFilename)
        {
            return false;
        }

        // m_dragDropSlot is a boundary in the original list: 0 is before the
        // first row and count is after the last. Removing a source above that
        // boundary shifts the desired final index one position to the left.
        int64_t finalIndex = static_cast<int64_t>(m_dragDropSlot);
        if (m_dragSourceIndex < finalIndex) --finalIndex;
        finalIndex = (std::max)(int64_t{ 0 },
            (std::min)(finalIndex,
                static_cast<int64_t>(livePlaylist.size()) - 1));

        if (finalIndex == m_dragSourceIndex) return true;
        return PlayerMovePlaylistItem(m_dragSourceIndex, finalIndex);
    }

    Microsoft::UI::Xaml::Controls::Grid PlaylistPage::CreateItemButton(
        MediaPlaylistItem const& item,
        int displayIndex,
        bool paused,
        bool eofReached)
    {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        using namespace Microsoft::UI::Xaml::Media;

        auto button = Button{};
        button.HorizontalAlignment(HorizontalAlignment::Stretch);
        button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        button.Padding(Thickness{ 0.0, 0.0, 0.0, 0.0 });
        button.BorderThickness(Thickness{ 0.0, 0.0, 0.0, 0.0 });
        button.Background(SolidColorBrush{ Windows::UI::Color{ 0, 0, 0, 0 } });

        // The stock Button template adds its own hover/pressed wash around the
        // content.  Playlist cards own their visual feedback instead, so keep
        // that outer layer fully transparent and animate only the card below.
        auto transparentButtonBrush =
            SolidColorBrush{ Windows::UI::Color{ 0, 0, 0, 0 } };
        button.Resources().Insert(
            box_value(L"ButtonBackgroundPointerOver"), transparentButtonBrush);
        button.Resources().Insert(
            box_value(L"ButtonBackgroundPressed"), transparentButtonBrush);
        button.Resources().Insert(
            box_value(L"ButtonBorderBrushPointerOver"), transparentButtonBrush);
        button.Resources().Insert(
            box_value(L"ButtonBorderBrushPressed"), transparentButtonBrush);

        auto card = Border{};
        card.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 10.0 });

        auto theme = Resources().ThemeDictionaries().Lookup(
            box_value(PlayerIsLightTheme() ? L"Light" : L"Dark"))
            .as<ResourceDictionary>();
        card.Background(
            item.current
                ? Resources().Lookup(box_value(L"PlaylistCurrentBrush")).as<Brush>()
                : theme.Lookup(box_value(L"PlaylistCardBrush")).as<Brush>());

        // The hover surface must cover the whole card, not only its padded
        // content area. Keep the card itself unpadded and put the spacing on a
        // dedicated content container above the overlay. This makes the hover
        // read as one Fluent surface from corner to corner.
        auto hoverLayer = Border{};
        hoverLayer.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 10.0 });
        hoverLayer.Background(
            item.current
                ? theme.Lookup(box_value(L"PlaylistButtonPointerOverBrush")).as<Brush>()
                : theme.Lookup(box_value(L"PlaylistCardPointerOverBrush")).as<Brush>());
        hoverLayer.Opacity(0.0);
        hoverLayer.IsHitTestVisible(false);

        auto grid = Grid{};
        grid.ColumnSpacing(12.0);
        grid.ColumnDefinitions().Append(ColumnDefinition{});
        grid.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromPixels(34.0));
        grid.ColumnDefinitions().Append(ColumnDefinition{});
        grid.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::FromValueAndType(1.0, GridUnitType::Star));
        grid.ColumnDefinitions().Append(ColumnDefinition{});
        grid.ColumnDefinitions().GetAt(2).Width(GridLengthHelper::Auto());

        auto indexText = TextBlock{};
        std::wstring indexLabel =
            item.current ? L"\u25B6" : std::to_wstring(displayIndex);
        indexText.Text(indexLabel);
        indexText.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
        indexText.FontSize(item.current ? 12.0 : 12.5);
        indexText.Opacity(item.current ? 0.95 : 0.52);
        indexText.VerticalAlignment(VerticalAlignment::Center);
        indexText.HorizontalAlignment(HorizontalAlignment::Center);
        Grid::SetColumn(indexText, 0);
        grid.Children().Append(indexText);

        auto textStack = StackPanel{};
        textStack.Spacing(3.0);
        Grid::SetColumn(textStack, 1);

        auto title = TextBlock{};
        title.Text(item.title);
        title.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
        title.FontSize(14.0);
        if (item.current)
        {
            title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        }
        title.TextTrimming(TextTrimming::CharacterEllipsis);
        textStack.Children().Append(title);

        auto detail = TextBlock{};
        std::wstring detailText;
        if (item.current)
        {
            if (eofReached)
            {
                detailText = PlaylistString(L"PlaylistFinishedLabel", L"Concluído");
            }
            else if (paused)
            {
                detailText = PlaylistString(L"PlaylistPausedLabel", L"Pausado");
            }
            else
            {
                detailText = PlaylistString(L"PlaylistCurrentLabel", L"Reproduzindo");
            }

            if (!item.format.empty())
            {
                detailText += L"  •  ";
                detailText += item.format;
            }
        }
        else if (!item.format.empty())
        {
            detailText = item.format;
        }
        else
        {
            detailText = PlaylistString(L"PlaylistItemLabel", L"Item da playlist");
        }
        detail.Text(detailText);
        detail.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
        detail.FontSize(11.5);
        detail.Opacity(0.55);
        detail.TextTrimming(TextTrimming::CharacterEllipsis);
        textStack.Children().Append(detail);

        grid.Children().Append(textStack);

        // Give the play affordance its own fixed slot instead of pinning the
        // glyph directly against the card edge.  The whole row is still the
        // click target; this slot is visual spacing only and never intercepts
        // pointer input independently.
        auto playSlot = Grid{};
        playSlot.Width(38.0);
        playSlot.Height(30.0);
        playSlot.HorizontalAlignment(HorizontalAlignment::Right);
        playSlot.VerticalAlignment(VerticalAlignment::Center);
        playSlot.IsHitTestVisible(false);
        Grid::SetColumn(playSlot, 2);

        auto playGlyph = FontIcon{};
        // The current row becomes a true play/pause affordance. Other rows keep
        // the play glyph because clicking them switches playback to that item.
        bool const showPause = item.current && !paused && !eofReached;
        playGlyph.Glyph(showPause ? L"\uE769" : L"\uE768");
        playGlyph.FontSize(13.5);
        playGlyph.Opacity(item.current ? 0.78 : 0.52);
        playGlyph.HorizontalAlignment(HorizontalAlignment::Center);
        playGlyph.VerticalAlignment(VerticalAlignment::Center);
        playSlot.Children().Append(playGlyph);
        grid.Children().Append(playSlot);

        // The content keeps the original inset while the hover layer underneath
        // spans the entire rounded card. No nested hover rectangle remains.
        auto contentHost = Border{};
        // Reserve a calm action gutter at the far right. The play/pause glyph
        // stays visible while the remove affordance appears independently on
        // hover, so neither action jumps or overlaps the other.
        contentHost.Padding(Thickness{ 42.0, 11.0, 42.0, 11.0 });
        contentHost.Background(SolidColorBrush{ Windows::UI::Color{ 0, 0, 0, 0 } });
        contentHost.Child(grid);

        auto cardSurface = Grid{};
        cardSurface.Children().Append(hoverLayer);
        cardSurface.Children().Append(contentHost);
        card.Child(cardSurface);
        button.Content(card);

        // Keep the destructive action separate from the row Button. Nesting a
        // Button inside another Button creates ambiguous routed input, so a
        // sibling overlay owns only the small remove target at the far right.
        auto rowHost = Grid{};
        rowHost.HorizontalAlignment(HorizontalAlignment::Stretch);
        // Give the whole row an explicit transparent hit-test surface. With a
        // null Grid background WinUI can route entry through whichever child is
        // currently hit, which made PointerEntered noticeably late/irregular
        // once the remove button became a sibling overlay in Patch 33.6.
        rowHost.Background(SolidColorBrush{ Windows::UI::Color{ 0, 0, 0, 0 } });
        rowHost.Children().Append(button);

        // A thin Accent Color line marks the exact insertion boundary without
        // moving any row. Each row owns top/bottom indicators; ShowDropSlot only
        // exposes one of them while a handle drag is active.
        auto dropBrush =
            Resources().Lookup(box_value(L"PlaylistDropIndicatorBrush")).as<Brush>();
        auto dropTop = Border{};
        dropTop.Height(2.0);
        dropTop.Margin(Thickness{ 8.0, -4.0, 8.0, 0.0 });
        dropTop.HorizontalAlignment(HorizontalAlignment::Stretch);
        dropTop.VerticalAlignment(VerticalAlignment::Top);
        dropTop.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 1.0 });
        dropTop.Background(dropBrush);
        dropTop.Opacity(0.0);
        dropTop.IsHitTestVisible(false);
        rowHost.Children().Append(dropTop);

        auto dropBottom = Border{};
        dropBottom.Height(2.0);
        dropBottom.Margin(Thickness{ 8.0, 0.0, 8.0, -4.0 });
        dropBottom.HorizontalAlignment(HorizontalAlignment::Stretch);
        dropBottom.VerticalAlignment(VerticalAlignment::Bottom);
        dropBottom.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 1.0 });
        dropBottom.Background(dropBrush);
        dropBottom.Opacity(0.0);
        dropBottom.IsHitTestVisible(false);
        rowHost.Children().Append(dropBottom);
        m_dropTopIndicators.push_back(dropTop);
        m_dropBottomIndicators.push_back(dropBottom);

        int64_t const index = item.index;
        bool const current = item.current;
        std::wstring const expectedFilename = item.filename;

        // Reordering starts only from this dedicated grip. It is a sibling of
        // the row Button, so dragging never steals the row's Play/Pause click.
        auto dragHandle = Grid{};
        dragHandle.Width(28.0);
        dragHandle.Height(30.0);
        dragHandle.Margin(Thickness{ 6.0, 0.0, 0.0, 0.0 });
        dragHandle.HorizontalAlignment(HorizontalAlignment::Left);
        dragHandle.VerticalAlignment(VerticalAlignment::Center);
        dragHandle.Background(transparentButtonBrush);

        auto dragGlyph = TextBlock{};
        dragGlyph.Text(L"\u22EE\u22EE");
        dragGlyph.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Symbol" });
        dragGlyph.FontSize(13.0);
        dragGlyph.Opacity(0.38);
        dragGlyph.HorizontalAlignment(HorizontalAlignment::Center);
        dragGlyph.VerticalAlignment(VerticalAlignment::Center);
        dragHandle.Children().Append(dragGlyph);

        std::wstring const reorderLabel =
            PlaylistString(L"PlaylistReorderLabel", L"Arrastar para reordenar");
        ToolTipService::SetToolTip(dragHandle, box_value(reorderLabel));
        rowHost.Children().Append(dragHandle);

        dragHandle.PointerEntered([this, dragGlyph](auto const&, auto const&)
        {
            if (!m_reorderDragging) dragGlyph.Opacity(0.78);
        });
        dragHandle.PointerExited([this, dragGlyph](auto const&, auto const&)
        {
            if (!m_reorderDragging) dragGlyph.Opacity(0.38);
        });

        dragHandle.PointerPressed(
            [this, index, expectedFilename, dragHandle, dragGlyph](
                auto const&,
                Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            if (m_closing) return;

            auto const point = args.GetCurrentPoint(dragHandle);
            if (!point.Properties().IsLeftButtonPressed()) return;

            auto const livePlaylist = PlayerGetPlaylistItems();
            if (index < 0 || index >= static_cast<int64_t>(livePlaylist.size()) ||
                livePlaylist[static_cast<size_t>(index)].filename != expectedFilename)
            {
                m_hasSnapshot = false;
                RefreshList();
                return;
            }

            CancelReorderDrag();
            m_reorderDragging = true;
            m_dragSourceIndex = index;
            m_dragSourceFilename = expectedFilename;
            m_dragSnapshotFilenames.reserve(livePlaylist.size());
            for (auto const& entry : livePlaylist)
            {
                m_dragSnapshotFilenames.push_back(entry.filename);
            }

            dragGlyph.Opacity(1.0);
            if (!dragHandle.CapturePointer(args.Pointer()))
            {
                CancelReorderDrag();
                dragGlyph.Opacity(0.38);
                return;
            }

            auto const hostPoint =
                args.GetCurrentPoint(PlaylistItemsHost()).Position();
            ShowDropSlot(CalculateDropSlot(hostPoint.Y));
            args.Handled(true);
        });

        dragHandle.PointerMoved(
            [this, index, expectedFilename](
                auto const&,
                Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            if (!m_reorderDragging || m_dragSourceIndex != index ||
                m_dragSourceFilename != expectedFilename)
            {
                return;
            }

            // Small edge assist keeps long queues usable without introducing a
            // background autoscroll timer. It only scrolls while the pointer is
            // actively moving near the viewport edge.
            auto scroll = PlaylistScrollViewer();
            auto const scrollPoint = args.GetCurrentPoint(scroll).Position();
            constexpr double Edge = 32.0;
            constexpr double Step = 20.0;
            if (scrollPoint.Y < Edge && scroll.VerticalOffset() > 0.0)
            {
                scroll.ChangeView(nullptr,
                    (std::max)(0.0, scroll.VerticalOffset() - Step),
                    nullptr, false);
            }
            else if (scrollPoint.Y > scroll.ActualHeight() - Edge &&
                scroll.VerticalOffset() < scroll.ScrollableHeight())
            {
                scroll.ChangeView(nullptr,
                    (std::min)(scroll.ScrollableHeight(),
                        scroll.VerticalOffset() + Step),
                    nullptr, false);
            }

            auto const hostPoint =
                args.GetCurrentPoint(PlaylistItemsHost()).Position();
            ShowDropSlot(CalculateDropSlot(hostPoint.Y));
            args.Handled(true);
        });

        dragHandle.PointerReleased(
            [this, index, expectedFilename, dragHandle, dragGlyph](
                auto const&,
                Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            if (!m_reorderDragging || m_dragSourceIndex != index ||
                m_dragSourceFilename != expectedFilename)
            {
                return;
            }

            CommitReorderDrag();
            CancelReorderDrag();
            dragGlyph.Opacity(0.38);
            dragHandle.ReleasePointerCapture(args.Pointer());

            // Re-read mpv even for a no-op or a rejected stale drag. This also
            // catches any playback transition that occurred while refreshes were
            // intentionally suspended during pointer capture.
            m_hasSnapshot = false;
            RefreshList();
            args.Handled(true);
        });

        dragHandle.PointerCaptureLost(
            [this, index, expectedFilename, dragGlyph](
                auto const&,
                Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)
        {
            if (!m_reorderDragging || m_dragSourceIndex != index ||
                m_dragSourceFilename != expectedFilename)
            {
                return;
            }

            CancelReorderDrag();
            dragGlyph.Opacity(0.38);
            m_hasSnapshot = false;
            RefreshList();
        });

        auto removeButton = Button{};
        removeButton.Width(30.0);
        removeButton.Height(30.0);
        removeButton.Padding(Thickness{ 0.0, 0.0, 0.0, 0.0 });
        removeButton.Margin(Thickness{ 0.0, 0.0, 8.0, 0.0 });
        removeButton.HorizontalAlignment(HorizontalAlignment::Right);
        removeButton.VerticalAlignment(VerticalAlignment::Center);
        removeButton.Background(SolidColorBrush{ Windows::UI::Color{ 0, 0, 0, 0 } });
        removeButton.BorderThickness(Thickness{ 0.0, 0.0, 0.0, 0.0 });
        removeButton.Opacity(0.0);
        removeButton.IsHitTestVisible(false);

        // No miniature hover tile inside the card: only the X itself brightens.
        removeButton.Resources().Insert(
            box_value(L"ButtonBackgroundPointerOver"), transparentButtonBrush);
        removeButton.Resources().Insert(
            box_value(L"ButtonBackgroundPressed"), transparentButtonBrush);
        removeButton.Resources().Insert(
            box_value(L"ButtonBorderBrushPointerOver"), transparentButtonBrush);
        removeButton.Resources().Insert(
            box_value(L"ButtonBorderBrushPressed"), transparentButtonBrush);

        auto removeGlyph = FontIcon{};
        removeGlyph.Glyph(L"\uE711");
        removeGlyph.FontSize(11.5);
        removeGlyph.Opacity(0.72);
        removeButton.Content(removeGlyph);

        std::wstring const removeLabel =
            PlaylistString(L"PlaylistRemoveLabel", L"Remover da fila");
        ToolTipService::SetToolTip(removeButton, box_value(removeLabel));
        rowHost.Children().Append(removeButton);

        // Deterministic desktop hover. Do not animate the hover itself: the first
        // pointer movement over the row updates XAML opacity immediately. This
        // avoids both compositor timing and relying solely on PointerEntered,
        // which can be delayed when a XAML island/overlay is revealed under the
        // cursor. PointerExited clears the state immediately as well.
        auto setHover =
            [hoverLayer, removeButton, current = item.current](bool entered)
        {
            float const hoverTarget = current ? 1.0f : 0.92f;
            constexpr double RemoveTarget = 0.78;

            // Stop any opacity animation left from an older build/state before
            // setting the dependency properties themselves. Keeping XAML and the
            // compositor visual in agreement prevents a later layout/render pass
            // from restoring stale opacity.
            auto hoverVisual =
                Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                    GetElementVisual(hoverLayer);
            auto removeVisual =
                Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                    GetElementVisual(removeButton);
            hoverVisual.StopAnimation(L"Opacity");
            removeVisual.StopAnimation(L"Opacity");

            hoverLayer.Opacity(entered ? hoverTarget : 0.0);
            removeButton.Opacity(entered ? RemoveTarget : 0.0);
            removeButton.IsHitTestVisible(entered);
        };

        // PointerEntered is the normal fast path. PointerMoved is intentional: it
        // guarantees immediate feedback even when the panel or a rebuilt row was
        // placed underneath a cursor that was already inside its bounds. The work
        // here is only a few property assignments and has no layout/mpv cost.
        rowHost.PointerEntered([setHover](auto const&, auto const&)
        {
            setHover(true);
        });
        rowHost.PointerMoved([setHover](auto const&, auto const&)
        {
            setHover(true);
        });
        rowHost.PointerExited([setHover](auto const&, auto const&)
        {
            setHover(false);
        });

        removeButton.PointerEntered([removeGlyph](auto const&, auto const&)
        {
            removeGlyph.Opacity(1.0);
        });
        removeButton.PointerExited([removeGlyph](auto const&, auto const&)
        {
            removeGlyph.Opacity(0.72);
        });

        button.Click([this, index, current](auto const&, auto const&)
        {
            if (current)
            {
                // Reuse HC Player's established playback controls instead of
                // introducing playlist-local pause state. At EOF, the same click
                // restarts the item; otherwise it simply toggles mpv's pause.
                bool pausedNow{};
                bool eofNow{};
                if (PlayerGetPlaybackState(pausedNow, eofNow))
                {
                    if (eofNow)
                    {
                        PlayerReplay();
                    }
                    else
                    {
                        PlayerTogglePause();
                    }

                    // Force an immediate visual refresh; the normal 700 ms
                    // read-only poll remains as the synchronization fallback.
                    m_hasSnapshot = false;
                    RefreshList();
                }
                return;
            }

            if (PlayerPlayPlaylistItem(index))
            {
                m_hasSnapshot = false;
                RefreshList();
            }
        });

        removeButton.Click([this, index, expectedFilename](auto const&, auto const&)
        {
            // A row may be up to 700 ms old if mpv's playlist changed through an
            // external binding. Verify the captured index still names the same
            // entry before issuing a destructive command; otherwise just resync.
            auto livePlaylist = PlayerGetPlaylistItems();
            auto const liveItem = std::find_if(
                livePlaylist.begin(), livePlaylist.end(),
                [index, &expectedFilename](MediaPlaylistItem const& candidate)
                {
                    return candidate.index == index &&
                        candidate.filename == expectedFilename;
                });

            if (liveItem == livePlaylist.end())
            {
                m_hasSnapshot = false;
                RefreshList();
                return;
            }

            if (PlayerRemovePlaylistItem(index))
            {
                // mpv owns the post-remove behavior, including advancing when
                // the current item is removed. Re-read its playlist immediately.
                m_hasSnapshot = false;
                RefreshList();
            }
        });

        return rowHost;
    }

    void PlaylistPage::RefreshList()
    {
        // Never rebuild the visual tree while a grip owns pointer capture. A
        // playback-state change can wait until release; the mpv playlist itself
        // remains authoritative and is validated again before any move command.
        if (m_reorderDragging) return;

        auto playlist = PlayerGetPlaylistItems();

        // The overflow now contains both a non-destructive folder append and the
        // queue-clear action. Keep the menu itself available at all times, but
        // enable clearing only when mpv reports a current item plus another entry.
        bool const hasCurrentItem = std::any_of(
            playlist.begin(), playlist.end(),
            [](MediaPlaylistItem const& item) { return item.current; });
        ClearQueueMenuItem().IsEnabled(playlist.size() > 1 && hasCurrentItem);

        // Playback state is also part of the visible queue state now: the current
        // row changes between Play/Pause and Playing/Paused/Finished without any
        // playlist mutation. Read it once per refresh, never once per row.
        bool paused{};
        bool eofReached{};
        bool const hasPlaybackState =
            PlayerGetPlaybackState(paused, eofReached);
        if (!hasPlaybackState)
        {
            paused = false;
            eofReached = false;
        }

        // Rebuild XAML only when mpv's playlist snapshot actually changes.
        // This keeps hover/scroll stable while still reflecting Next/Previous,
        // shell launches and other playlist changes made outside this panel.
        std::wstring signature;
        signature += paused ? L"pause=1|" : L"pause=0|";
        signature += eofReached ? L"eof=1\n" : L"eof=0\n";
        for (auto const& item : playlist)
        {
            signature += std::to_wstring(item.index);
            signature += item.current ? L"|1|" : L"|0|";
            signature += item.title;
            signature += L"|";
            signature += item.filename;
            signature += L"\n";
        }

        if (m_hasSnapshot && signature == m_lastSignature)
        {
            return;
        }
        m_hasSnapshot = true;
        m_lastSignature = std::move(signature);

        auto host = PlaylistItemsHost();
        m_dropTopIndicators.clear();
        m_dropBottomIndicators.clear();
        host.Children().Clear();

        if (playlist.empty())
        {
            auto empty = Microsoft::UI::Xaml::Controls::TextBlock{};
            empty.Text(PlaylistString(L"PlaylistEmptyLabel", L"Nenhum item na fila"));
            empty.Margin(Microsoft::UI::Xaml::Thickness{ 0.0, 14.0, 0.0, 0.0 });
            empty.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Center);
            empty.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{ L"Segoe UI Variable Text" });
            empty.FontSize(13.0);
            empty.Opacity(0.58);
            host.Children().Append(empty);
            return;
        }

        int displayIndex = 1;
        for (auto const& item : playlist)
        {
            host.Children().Append(CreateItemButton(
                item, displayIndex++, paused, eofReached));
        }
    }

    void PlaylistPage::RefreshTimerTick(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        if (!m_closing && !m_reorderDragging)
        {
            RefreshList();
        }
    }

    void PlaylistPage::AddClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // The native picker and mpv append operation live in the player bridge.
        // This page only requests the action and refreshes its read-only snapshot
        // afterwards, keeping queue UI state separate from playback ownership.
        if (PlayerAddPlaylistFilesFromDialog())
        {
            m_hasSnapshot = false;
            RefreshList();
        }
    }

    void PlaylistPage::AddFolderClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_closing || m_reorderDragging) return;

        // Folder enumeration and natural ordering stay in the bridge. The page
        // only requests the append and refreshes its read-only mpv snapshot.
        if (PlayerAddPlaylistFolderFromDialog())
        {
            m_hasSnapshot = false;
            RefreshList();
        }
    }

    void PlaylistPage::ClearQueueClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_closing || m_reorderDragging) return;

        // playlist-clear is deliberately exposed as one atomic bridge operation:
        // mpv preserves the current entry itself, so the UI never loops through
        // stale indexes or risks removing/reloading the file that is playing.
        if (PlayerClearPlaylistExceptCurrent())
        {
            m_hasSnapshot = false;
            RefreshList();
        }
    }

    void PlaylistPage::CloseClicked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        RequestClose();
    }
}
