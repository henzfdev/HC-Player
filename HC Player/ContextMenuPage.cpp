#include "pch.h"
#include "ContextMenuPage.h"
#include "PlayerBridge.h"
#include "LocalizationManager.h"


namespace
{
    std::wstring ContextMenuString(
        std::wstring_view resourceId,
        std::wstring_view fallback)
    {
        return hc::localization::GetString(resourceId, fallback);
    }

    // Keep every mpv command and argument exactly as authored. Only replace
    // the user-visible text inside the known show-text payloads.
    bool UseCompactContextMenu()
    {
        std::wstring value;
        if (!PlayerTryGetSavedMpvOption(L"ui-context-menu-compact", value))
            return false;

        std::transform(value.begin(), value.end(), value.begin(), towlower);
        return value == L"yes" || value == L"true" ||
            value == L"1" || value == L"on";
    }

    std::wstring LocalizeContextMenuOsdCommand(std::wstring command)
    {
        auto replaceOnce = [&](
            wchar_t const* source,
            std::wstring_view resourceId,
            std::wstring_view fallback)
        {
            std::wstring sourceText{ source };
            auto const position = command.find(sourceText);
            if (position == std::wstring::npos) return false;

            command.replace(
                position,
                sourceText.size(),
                ContextMenuString(resourceId, fallback));
            return true;
        };

        if (replaceOnce(
            L"Playlist: ordem embaralhada",
            L"ContextMenuOsdPlaylistShuffled",
            L"Playlist: ordem embaralhada")) return command;

        if (replaceOnce(
            L"Proporção: Original",
            L"ContextMenuOsdAspectOriginal",
            L"Proporção: Original")) return command;

        if (replaceOnce(
            L"Proporção: 16:9",
            L"ContextMenuOsdAspect169",
            L"Proporção: 16:9")) return command;

        if (replaceOnce(
            L"Proporção: 4:3",
            L"ContextMenuOsdAspect43",
            L"Proporção: 4:3")) return command;

        replaceOnce(
            L"Proporção: 2,35:1",
            L"ContextMenuOsdAspect235",
            L"Proporção: 2,35:1");
        return command;
    }
}

namespace winrt::HCPlayer::implementation
{
    ContextMenuPage::ContextMenuPage()
    {
        RequestedTheme(PlayerIsLightTheme()
            ? Microsoft::UI::Xaml::ElementTheme::Light
            : Microsoft::UI::Xaml::ElementTheme::Dark);
    }

    void ContextMenuPage::RefreshProfilesMenu()
    {
        auto menu = ProfilesMenu();
        auto profiles = PlayerGetImportedProfileNames();
        auto activeProfile = PlayerGetActiveImportedProfile();
        menu.Visibility(profiles.empty()
            ? Microsoft::UI::Xaml::Visibility::Collapsed
            : Microsoft::UI::Xaml::Visibility::Visible);

        // This page is reused. Avoid destroying and recreating the same WinUI
        // menu-item tree on every right-click while mpv is presenting video.
        if (profiles == m_renderedProfiles &&
            activeProfile == m_renderedActiveProfile)
        {
            return;
        }

        menu.Items().Clear();
        m_renderedProfiles = profiles;
        m_renderedActiveProfile = activeProfile;

        if (!profiles.empty())
        {
            Microsoft::UI::Xaml::Controls::MenuFlyoutItem reset;
            reset.Text(ContextMenuString(L"ContextMenuDynDeactivateProfile", L"Desativar perfil"));
            reset.Icon(Microsoft::UI::Xaml::Controls::SymbolIcon{
                Microsoft::UI::Xaml::Controls::Symbol::Undo });
            reset.Tag(winrt::box_value(L"action:deactivate-profile"));
            reset.Click({ this, &ContextMenuPage::MenuItemClicked });
            menu.Items().Append(reset);
            menu.Items().Append(Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator{});
        }

        for (size_t index = 0; index < profiles.size(); ++index)
        {
            Microsoft::UI::Xaml::Controls::MenuFlyoutItem item;
            item.Text(profiles[index]);
            item.Tag(winrt::box_value(L"profile:" + profiles[index]));
            if (profiles[index] == activeProfile)
            {
                item.Icon(Microsoft::UI::Xaml::Controls::SymbolIcon{
                    Microsoft::UI::Xaml::Controls::Symbol::Accept });
            }
            if (index < 12)
            {
                item.KeyboardAcceleratorTextOverride(
                    L"F" + std::to_wstring(index + 1));
            }
            item.Click({ this, &ContextMenuPage::MenuItemClicked });
            menu.Items().Append(item);
        }
    }

    void ContextMenuPage::RefreshRecentMenu()
    {
        auto menu = RecentMenu();
        menu.Items().Clear();
        auto items = PlayerGetRecentFiles();
        menu.IsEnabled(!items.empty());

        for (auto const& recent : items)
        {
            Microsoft::UI::Xaml::Controls::MenuFlyoutItem item;
            item.Text(recent.title);
            item.KeyboardAcceleratorTextOverride(recent.hint);
            item.Tag(winrt::box_value(L"recent:" + recent.path));
            item.Click({ this, &ContextMenuPage::MenuItemClicked });
            menu.Items().Append(item);
        }
        if (!items.empty())
        {
            menu.Items().Append(Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator{});
            Microsoft::UI::Xaml::Controls::MenuFlyoutItem clear;
            clear.Text(ContextMenuString(L"ContextMenuDynClearHistory", L"Limpar histórico"));
            clear.Icon(Microsoft::UI::Xaml::Controls::SymbolIcon{
                Microsoft::UI::Xaml::Controls::Symbol::Delete });
            clear.Tag(winrt::box_value(L"action:clear-recent"));
            clear.Click({ this, &ContextMenuPage::MenuItemClicked });
            menu.Items().Append(clear);
        }
    }

    void ContextMenuPage::FillTrackMenu(
        Microsoft::UI::Xaml::Controls::MenuFlyoutSubItem const& menu,
        std::vector<MediaTrackOption> const& tracks,
        std::wstring const& type,
        std::wstring const& property,
        int selectionIndex,
        bool allowDisabled)
    {
        menu.Items().Clear();
        int ordinal = 0;
        bool anySelected = false;

        for (auto const& track : tracks)
        {
            if (track.type != type) continue;
            ++ordinal;

            bool selected = track.selected &&
                (type != L"sub" || track.mainSelection == selectionIndex);
            anySelected = anySelected || selected;

            std::wstring label = track.title.empty()
                ? (type == L"audio"
                    ? ContextMenuString(L"ContextMenuDynAudioBase", L"Áudio ")
                    : type == L"video"
                        ? ContextMenuString(L"ContextMenuDynVideoBase", L"Vídeo ")
                        : ContextMenuString(L"ContextMenuDynSubtitleBase", L"Legenda "))
                    + std::to_wstring(ordinal)
                : track.title;
            if (track.forced) label += ContextMenuString(L"ContextMenuDynForcedSuffix", L"  ·  forçada");
            if (track.external) label += ContextMenuString(L"ContextMenuDynExternalSuffix", L"  ·  externa");
            if (track.defaultTrack) label += ContextMenuString(L"ContextMenuDynDefaultSuffix", L"  ·  padrão");

            Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem item;
            item.Text(label);
            item.IsChecked(selected);
            std::wstring hint = track.language;
            if (!track.codec.empty())
            {
                if (!hint.empty()) hint += L"  ·  ";
                hint += track.codec;
            }
            item.KeyboardAcceleratorTextOverride(hint);
            item.Tag(winrt::box_value(
                L"track:" + property + L":" + std::to_wstring(track.id)));
            item.Click({ this, &ContextMenuPage::MenuItemClicked });
            menu.Items().Append(item);
        }

        if (allowDisabled)
        {
            if (ordinal > 0)
            {
                menu.Items().Append(Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator{});
            }
            Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem disabled;
            disabled.Text(type == L"audio"
                ? ContextMenuString(L"ContextMenuDynNoAudio", L"Sem áudio")
                : ContextMenuString(L"ContextMenuDynDisabled", L"Desativado"));
            disabled.IsChecked(!anySelected);
            disabled.Tag(winrt::box_value(L"track:" + property + L":no"));
            disabled.Click({ this, &ContextMenuPage::MenuItemClicked });
            menu.Items().Append(disabled);
        }

        menu.IsEnabled(ordinal > 0 || allowDisabled);
    }

    void ContextMenuPage::RefreshTracksMenus()
    {
        auto tracks = PlayerGetMediaTracks();
        FillTrackMenu(TracksVideoMenu(), tracks, L"video", L"vid", -1, true);
        FillTrackMenu(TracksAudioMenu(), tracks, L"audio", L"aid", -1, true);
        FillTrackMenu(TracksSubtitleMenu(), tracks, L"sub", L"sid", 0, true);
        FillTrackMenu(TracksSecondarySubtitleMenu(), tracks, L"sub", L"secondary-sid", 1, true);
        FillTrackMenu(AudioTracksMenu(), tracks, L"audio", L"aid", -1, true);
        FillTrackMenu(SubtitlePrimaryTracksMenu(), tracks, L"sub", L"sid", 0, true);
        FillTrackMenu(SubtitleSecondaryTracksMenu(), tracks, L"sub", L"secondary-sid", 1, true);
        bool const canAddExternalTrack = PlayerIsMediaPresentationReady();
        AddExternalAudioItem().IsEnabled(canAddExternalTrack);
        AddExternalSubtitleItem().IsEnabled(canAddExternalTrack);
        TracksMenu().IsEnabled(!tracks.empty());
    }

    void ContextMenuPage::RefreshChaptersMenu()
    {
        auto menu = ChaptersMenu();
        menu.Items().Clear();
        auto chapters = PlayerGetMediaChapters();
        menu.IsEnabled(!chapters.empty());
        if (chapters.empty()) return;

        Microsoft::UI::Xaml::Controls::MenuFlyoutItem next;
        next.Text(ContextMenuString(L"ContextMenuDynNext", L"Próximo"));
        next.KeyboardAcceleratorTextOverride(L"PgUp");
        next.Tag(winrt::box_value(L"action:chapter-next"));
        next.Click({ this, &ContextMenuPage::MenuItemClicked });
        menu.Items().Append(next);

        Microsoft::UI::Xaml::Controls::MenuFlyoutItem previous;
        previous.Text(ContextMenuString(L"ContextMenuDynPrevious", L"Anterior"));
        previous.KeyboardAcceleratorTextOverride(L"PgDn");
        previous.Tag(winrt::box_value(L"action:chapter-previous"));
        previous.Click({ this, &ContextMenuPage::MenuItemClicked });
        menu.Items().Append(previous);
        menu.Items().Append(Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator{});

        double elapsed{};
        double duration{};
        PlayerGetPlaybackTimes(elapsed, duration);
        size_t current{};
        for (size_t index = 1; index < chapters.size(); ++index)
        {
            if (chapters[index].time > elapsed + 0.05) break;
            current = index;
        }

        auto formatTime = [](double seconds)
        {
            auto total = static_cast<long long>((std::max)(0.0, seconds));
            auto hours = total / 3600;
            auto minutes = (total / 60) % 60;
            auto remaining = total % 60;
            wchar_t text[24]{};
            if (hours > 0)
                swprintf_s(text, L"%lld:%02lld:%02lld", hours, minutes, remaining);
            else
                swprintf_s(text, L"%02lld:%02lld", minutes, remaining);
            return std::wstring{ text };
        };

        for (size_t index = 0; index < chapters.size(); ++index)
        {
            std::wstring title = chapters[index].title.empty()
                ? ContextMenuString(L"ContextMenuDynChapterBase", L"Capítulo ") + std::to_wstring(index + 1)
                : chapters[index].title;
            constexpr size_t maximumTitleLength = 58;
            if (title.size() > maximumTitleLength)
            {
                title.resize(maximumTitleLength - 1);
                title += L"…";
            }
            Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem item;
            item.Text(title);
            item.KeyboardAcceleratorTextOverride(formatTime(chapters[index].time));
            item.IsChecked(index == current);
            item.Tag(winrt::box_value(
                L"chapter:" + std::to_wstring(chapters[index].time)));
            item.Click({ this, &ContextMenuPage::MenuItemClicked });
            menu.Items().Append(item);
        }
    }

    void ContextMenuPage::RefreshEditionsMenu()
    {
        auto menu = EditionsMenu();
        menu.Items().Clear();
        auto editions = PlayerGetMediaEditions();
        menu.IsEnabled(!editions.empty());

        auto formatDuration = [](double seconds)
        {
            if (seconds <= 0.0 || !std::isfinite(seconds)) return std::wstring{};
            auto milliseconds = static_cast<long long>(seconds * 1000.0 + 0.5);
            auto hours = milliseconds / 3600000;
            auto minutes = (milliseconds / 60000) % 60;
            auto remaining = (milliseconds / 1000) % 60;
            auto fraction = milliseconds % 1000;
            wchar_t text[32]{};
            swprintf_s(text, L"%02lld:%02lld:%02lld.%03lld",
                hours, minutes, remaining, fraction);
            return std::wstring{ text };
        };

        for (size_t index = 0; index < editions.size(); ++index)
        {
            auto const& edition = editions[index];
            std::wstring label = edition.title;

            // Optical-disc playback opened from folders/ISOs can expose the
            // same "title: N" presentation through either disc-title-list or
            // edition-list. Localize only an actual prefix at the final UI
            // layer; IDs, selection type and native mpv properties stay intact.
            if (!label.empty())
            {
                std::wstring lowerLabel = label;
                std::transform(
                    lowerLabel.begin(),
                    lowerLabel.end(),
                    lowerLabel.begin(),
                    towlower);

                if (lowerLabel.starts_with(L"title:"))
                {
                    label.replace(0, 6, ContextMenuString(L"ContextMenuDynTitlePrefix", L"Título:"));
                }
            }

            if (label.empty())
            {
                label = edition.discTitle
                    ? ContextMenuString(L"ContextMenuDynTitleBase", L"Título: ")
                    : ContextMenuString(L"ContextMenuDynEditionBase", L"Edição: ");
                label += std::to_wstring(index + 1);
            }
            auto duration = formatDuration(edition.duration);
            if (!duration.empty()) label += L" (" + duration + L")";
            if (edition.defaultEdition && !edition.selected)
                label += ContextMenuString(L"ContextMenuDynDefaultSuffix", L"  ·  padrão");

            Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem item;
            item.Text(label);
            item.IsChecked(edition.selected);
            item.Tag(winrt::box_value(std::wstring{
                edition.discTitle ? L"disc-title:" : L"edition:" } +
                std::to_wstring(edition.id)));
            item.Click({ this, &ContextMenuPage::MenuItemClicked });
            menu.Items().Append(item);
        }
    }

    void ContextMenuPage::RefreshPlaylistMenu()
    {
        auto menu = PlaylistMenu();
        menu.Items().Clear();
        auto playlist = PlayerGetPlaylistItems();
        menu.IsEnabled(!playlist.empty());
        if (playlist.empty()) return;

        Microsoft::UI::Xaml::Controls::MenuFlyoutItem next;
        next.Text(ContextMenuString(L"ContextMenuDynNextFile", L"Próximo arquivo"));
        next.KeyboardAcceleratorTextOverride(L"F12");
        next.Tag(winrt::box_value(L"key:F12"));
        next.Click({ this, &ContextMenuPage::MenuItemClicked });
        menu.Items().Append(next);

        Microsoft::UI::Xaml::Controls::MenuFlyoutItem previous;
        previous.Text(ContextMenuString(L"ContextMenuDynPreviousFile", L"Arquivo anterior"));
        previous.KeyboardAcceleratorTextOverride(L"F11");
        previous.Tag(winrt::box_value(L"key:F11"));
        previous.Click({ this, &ContextMenuPage::MenuItemClicked });
        menu.Items().Append(previous);

        Microsoft::UI::Xaml::Controls::MenuFlyoutItem shuffle;
        shuffle.Text(ContextMenuString(L"ContextMenuDynShuffleOrder", L"Embaralhar ordem"));
        shuffle.Tag(winrt::box_value(
            L"cmd:no-osd playlist-shuffle; show-text \"Playlist: ordem embaralhada\""));
        shuffle.Click({ this, &ContextMenuPage::MenuItemClicked });
        menu.Items().Append(shuffle);
        menu.Items().Append(Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator{});

        for (auto const& entry : playlist)
        {
            Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem item;
            item.Text(entry.title);
            item.KeyboardAcceleratorTextOverride(entry.format);
            item.IsChecked(entry.current);
            item.Tag(winrt::box_value(
                L"playlist:" + std::to_wstring(entry.index)));
            item.Click({ this, &ContextMenuPage::MenuItemClicked });
            menu.Items().Append(item);
        }
    }

    void ContextMenuPage::RefreshSpeedMenu()
    {
        double current = PlayerGetPlaybackSpeed();
        for (auto const& entry : SpeedMenu().Items())
        {
            auto item = entry.try_as<
                Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem>();
            if (!item || !item.Tag()) continue;
            std::wstring tag = winrt::unbox_value<hstring>(item.Tag()).c_str();
            if (!tag.starts_with(L"speed:")) continue;
            try
            {
                item.IsChecked(std::abs(current - std::stod(tag.substr(6))) < 0.001);
            }
            catch (...) { item.IsChecked(false); }
        }
    }

    void ContextMenuPage::ResetTopLevelMenuVisibility()
    {
        using Microsoft::UI::Xaml::Visibility;
        for (auto const& item : PlayerMenu().Items())
        {
            item.Visibility(Visibility::Visible);
        }
    }

    void ContextMenuPage::ApplyCompactMenuVisibility(bool compact)
    {
        if (!compact) return;

        using Microsoft::UI::Xaml::Visibility;

        // RefreshProfilesMenu() runs before this mask and is the authority on
        // whether imported profiles actually exist. Preserve that dynamic state
        // before hiding the complete menu, then expose Perfis only when useful.
        bool const profilesAvailable =
            ProfilesMenu().Visibility() == Visibility::Visible;

        for (auto const& item : PlayerMenu().Items())
        {
            item.Visibility(Visibility::Collapsed);
        }

        OpenMenu().Visibility(Visibility::Visible);
        NavigateMenu().Visibility(Visibility::Visible);
        ChaptersMenu().Visibility(ChaptersMenu().IsEnabled()
            ? Visibility::Visible : Visibility::Collapsed);
        TracksMenu().Visibility(Visibility::Visible);
        EditionsMenu().Visibility(EditionsMenu().IsEnabled()
            ? Visibility::Visible : Visibility::Collapsed);
        ProfilesMenu().Visibility(profilesAvailable
            ? Visibility::Visible : Visibility::Collapsed);
        PanScanMenu().Visibility(Visibility::Visible);
        CompactSettingsSeparator().Visibility(Visibility::Visible);
        SettingsMenuItem().Visibility(Visibility::Visible);
    }

    void ContextMenuPage::ShowAt(double x, double y)
    {
        // This Page is reused between openings. Undo the previous compact mask
        // before rebuilding dynamic state, then apply the selected presentation.
        ResetTopLevelMenuVisibility();
        RefreshProfilesMenu();
        RefreshRecentMenu();
        RefreshTracksMenus();
        RefreshChaptersMenu();
        RefreshEditionsMenu();
        RefreshPlaylistMenu();
        RefreshSpeedMenu();
        ApplyCompactMenuVisibility(UseCompactContextMenu());
        // The complete menu is intentionally tall. Keep its horizontal origin
        // at the cursor, but anchor it near the top so Settings/Exit never fall
        // outside the client area on regular laptop displays.
        y = (std::min)(y, 12.0);
        Microsoft::UI::Xaml::Controls::Primitives::FlyoutShowOptions options;
        auto position = winrt::box_value(Windows::Foundation::Point{
            static_cast<float>(x), static_cast<float>(y) })
            .as<Windows::Foundation::IReference<Windows::Foundation::Point>>();
        options.Position(position);
        options.Placement(Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Auto);
        PlayerMenu().ShowAt(MenuAnchor(), options);
    }

    void ContextMenuPage::MenuItemClicked(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto item = sender.try_as<Microsoft::UI::Xaml::FrameworkElement>();
        if (!item || !item.Tag()) return;
        std::wstring tag = winrt::unbox_value<hstring>(item.Tag()).c_str();

        if (tag == L"action:open") PlayerShowOpenDialog();
        else if (tag == L"action:open-folder") PlayerShowOpenFolderDialog();
        else if (tag == L"action:open-dvd-iso") PlayerShowOpenDiscImageDialog(false);
        else if (tag == L"action:open-bluray-iso") PlayerShowOpenDiscImageDialog(true);
        else if (tag == L"action:add-external-audio") PlayerShowAddExternalAudioDialog();
        else if (tag == L"action:add-external-subtitle") PlayerShowAddExternalSubtitleDialog();
        else if (tag == L"action:open-clipboard") PlayerOpenClipboardMedia();
        else if (tag == L"action:clear-recent") PlayerClearRecentFiles();
        else if (tag == L"action:capture-subtitles") PlayerCaptureScreenshot(true);
        else if (tag == L"action:capture-video") PlayerCaptureScreenshot(false);
        else if (tag == L"action:deactivate-profile") PlayerDeactivateImportedProfile();
        else if (tag == L"action:chapter-next") PlayerChangeChapter(1);
        else if (tag == L"action:chapter-previous") PlayerChangeChapter(-1);
        else if (tag.starts_with(L"profile:")) PlayerApplyImportedProfile(tag.substr(8));
        else if (tag.starts_with(L"chapter:"))
        {
            try { PlayerSeekAbsolute(std::stod(tag.substr(8))); } catch (...) {}
        }
        else if (tag.starts_with(L"edition:"))
        {
            try { PlayerSelectMediaEdition(false, std::stoll(tag.substr(8))); } catch (...) {}
        }
        else if (tag.starts_with(L"disc-title:"))
        {
            try { PlayerSelectMediaEdition(true, std::stoll(tag.substr(11))); } catch (...) {}
        }
        else if (tag.starts_with(L"playlist:"))
        {
            try { PlayerPlayPlaylistItem(std::stoll(tag.substr(9))); } catch (...) {}
        }
        else if (tag.starts_with(L"recent:")) PlayerOpenRecentFile(tag.substr(7));
        else if (tag.starts_with(L"speed:"))
        {
            try { PlayerSetPlaybackSpeed(std::stod(tag.substr(6))); } catch (...) {}
        }
        else if (tag.starts_with(L"track:"))
        {
            auto separator = tag.find(L':', 6);
            if (separator != std::wstring::npos)
            {
                PlayerSelectMediaTrack(
                    tag.substr(6, separator - 6), tag.substr(separator + 1));
            }
        }
        else if (tag == L"action:pause") PlayerTogglePause();
        else if (tag == L"action:fullscreen") PlayerToggleFullscreen();
        else if (tag == L"action:borderless") PlayerToggleBorderless();
        else if (tag == L"action:stats") PlayerToggleStats();
        else if (tag == L"action:console")
            PlayerExecuteMpvCommand(L"script-binding console/enable");
        else if (tag == L"action:ontop") PlayerToggleAlwaysOnTop();
        else if (tag == L"action:quit") PlayerQuitApp();
        // Wait for the flyout and its XAML island to finish closing before the
        // settings island is created. Creating both in the same click callback
        // can leave stale composition surfaces and can crash Microsoft.UI.Xaml.
        else if (tag == L"action:settings") m_openSettingsAfterClose = true;
        else if (tag == L"action:mediainfo") m_openMediaInfoAfterClose = true;
        else if (tag.starts_with(L"key:")) PlayerSendMpvKey(tag.substr(4));
        else if (tag.starts_with(L"cmd:"))
            PlayerExecuteMpvCommand(LocalizeContextMenuOsdCommand(tag.substr(4)));
    }

    void ContextMenuPage::MenuClosed(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        bool const openSettings = m_openSettingsAfterClose;
        bool const openMediaInfo = m_openMediaInfoAfterClose;
        m_openSettingsAfterClose = false;
        m_openMediaInfoAfterClose = false;

        PlayerCloseContextMenu();

        if (openSettings)
            PlayerShowSettings();
        else if (openMediaInfo)
            PlayerShowMediaInfo();
    }
}
