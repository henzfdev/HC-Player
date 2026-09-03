#include "pch.h"
#include "SystemMediaControlsManager.h"
#include "PlayerBridge.h"

#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cstdint>
#include <string>

namespace hc::system_media_controls
{
    namespace
    {
        using namespace winrt::Windows::Media;

        SystemMediaTransportControls g_controls{ nullptr };
        winrt::event_token g_buttonPressedToken{};
        bool g_hasButtonPressedToken{};
        bool g_enabled{};
        bool g_hasPlaybackStatus{};
        MediaPlaybackStatus g_lastPlaybackStatus{ MediaPlaybackStatus::Closed };
        std::wstring g_lastTitle;
        std::wstring g_lastArtist;
        std::wstring g_lastMediaPath;
        bool g_lastAudioOnly{};
        bool g_hasMetadata{};
        std::uint64_t g_metadataGeneration{};

        void ResetCache() noexcept
        {
            // Invalidate any album-art lookup that may still be completing.
            ++g_metadataGeneration;
            g_enabled = false;
            g_hasPlaybackStatus = false;
            g_lastPlaybackStatus = MediaPlaybackStatus::Closed;
            g_lastTitle.clear();
            g_lastArtist.clear();
            g_lastMediaPath.clear();
            g_lastAudioOnly = false;
            g_hasMetadata = false;
        }

        winrt::fire_and_forget UpdateAudioArtworkAsync(
            std::wstring mediaPath,
            std::uint64_t generation)
        {
            try
            {
                // Keep all SMTC/global-state access on the apartment that
                // requested the artwork. The file/thumbnail work itself stays
                // asynchronous and never blocks playback or the WinUI thread.
                winrt::apartment_context callerApartment;

                auto file = co_await
                    winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(
                        mediaPath);

                auto thumbnail = co_await file.GetThumbnailAsync(
                    winrt::Windows::Storage::FileProperties::ThumbnailMode::MusicView,
                    512,
                    winrt::Windows::Storage::FileProperties::ThumbnailOptions::UseCurrentScale);

                co_await callerApartment;

                // A playlist change can complete while Windows is still
                // extracting the previous file's cover. Never publish stale art.
                if (generation != g_metadataGeneration ||
                    !g_controls ||
                    !g_hasMetadata ||
                    !g_lastAudioOnly ||
                    g_lastMediaPath != mediaPath)
                {
                    co_return;
                }

                // MusicView may fall back to a generic file icon. Publish only
                // a real image so "no cover" remains genuinely cover-free.
                if (!thumbnail ||
                    thumbnail.Type() !=
                        winrt::Windows::Storage::FileProperties::ThumbnailType::Image ||
                    thumbnail.Size() == 0)
                {
                    co_return;
                }

                auto updater = g_controls.DisplayUpdater();
                updater.Thumbnail(
                    winrt::Windows::Storage::Streams::RandomAccessStreamReference::
                        CreateFromStream(
                            thumbnail.as<
                                winrt::Windows::Storage::Streams::IRandomAccessStream>()));
                updater.Update();
            }
            catch (...)
            {
                // Album art is optional shell decoration. Failure must never
                // affect SMTC transport controls or media playback.
            }
        }

        void EnableIfNeeded()
        {
            if (!g_controls || g_enabled) return;
            g_controls.IsEnabled(true);
            g_enabled = true;
            if (g_hasPlaybackStatus)
            {
                g_controls.PlaybackStatus(g_lastPlaybackStatus);
            }
        }
    }

    bool Initialize(HWND window, unsigned int commandMessage) noexcept
    {
        try
        {
            if (!window || !commandMessage) return false;
            if (g_controls) return true;

            auto activationFactory = winrt::get_activation_factory<
                SystemMediaTransportControls>();
            auto interopFactory =
                activationFactory.as<ISystemMediaTransportControlsInterop>();

            SystemMediaTransportControls controls{ nullptr };
            winrt::check_hresult(interopFactory->GetForWindow(
                window,
                winrt::guid_of<SystemMediaTransportControls>(),
                winrt::put_abi(controls)));

            controls.IsEnabled(false);
            controls.IsPlayEnabled(true);
            controls.IsPauseEnabled(true);
            controls.IsStopEnabled(false);
            controls.IsNextEnabled(true);
            controls.IsPreviousEnabled(true);
            controls.IsFastForwardEnabled(false);
            controls.IsRewindEnabled(false);

            g_buttonPressedToken = controls.ButtonPressed(
                [window, commandMessage](
                    SystemMediaTransportControls const&,
                    SystemMediaTransportControlsButtonPressedEventArgs const& args) noexcept
                {
                    try
                    {
                        Command command{};
                        switch (args.Button())
                        {
                        case SystemMediaTransportControlsButton::Play:
                            command = Command::Play;
                            break;
                        case SystemMediaTransportControlsButton::Pause:
                            command = Command::Pause;
                            break;
                        case SystemMediaTransportControlsButton::Previous:
                            command = Command::Previous;
                            break;
                        case SystemMediaTransportControlsButton::Next:
                            command = Command::Next;
                            break;
                        default:
                            return;
                        }

                        PostMessageW(
                            window,
                            commandMessage,
                            static_cast<WPARAM>(command),
                            0);
                    }
                    catch (...)
                    {
                        // SMTC is optional shell integration.
                    }
                });
            g_hasButtonPressedToken = true;
            g_controls = std::move(controls);
            ResetCache();
            return true;
        }
        catch (...)
        {
            g_controls = nullptr;
            g_hasButtonPressedToken = false;
            ResetCache();
            return false;
        }
    }

    void Shutdown() noexcept
    {
        try
        {
            if (g_controls)
            {
                if (g_hasButtonPressedToken)
                {
                    g_controls.ButtonPressed(g_buttonPressedToken);
                }
                g_controls.IsEnabled(false);
            }
        }
        catch (...)
        {
        }

        g_controls = nullptr;
        g_hasButtonPressedToken = false;
        ResetCache();
    }

    void UpdatePlaybackState(bool paused, bool eofReached) noexcept
    {
        try
        {
            if (!g_controls) return;

            MediaPlaybackStatus const status = eofReached
                ? MediaPlaybackStatus::Stopped
                : (paused
                    ? MediaPlaybackStatus::Paused
                    : MediaPlaybackStatus::Playing);

            if (g_hasPlaybackStatus && g_lastPlaybackStatus == status)
            {
                return;
            }

            g_lastPlaybackStatus = status;
            g_hasPlaybackStatus = true;
            if (g_enabled)
            {
                g_controls.PlaybackStatus(status);
            }
        }
        catch (...)
        {
        }
    }

    void UpdateMetadata(
        std::wstring_view title,
        std::wstring_view artist,
        bool audioOnly) noexcept
    {
        try
        {
            if (!g_controls || title.empty()) return;

            std::wstring mediaPath;
            if (audioOnly)
            {
                // Best-effort local path lookup. Web audio keeps the existing
                // metadata-only SMTC behavior and simply has no thumbnail.
                PlayerGetCurrentLocalMediaPath(mediaPath);
            }

            if (g_hasMetadata &&
                g_lastTitle == title &&
                g_lastArtist == artist &&
                g_lastMediaPath == mediaPath &&
                g_lastAudioOnly == audioOnly)
            {
                EnableIfNeeded();
                return;
            }

            // Clearing first removes the previous track's artwork immediately.
            // The new cover, if one exists, is added asynchronously afterward.
            std::uint64_t const generation = ++g_metadataGeneration;
            auto updater = g_controls.DisplayUpdater();
            updater.ClearAll();

            winrt::hstring const titleText{ std::wstring(title) };
            if (audioOnly)
            {
                updater.Type(MediaPlaybackType::Music);
                auto music = updater.MusicProperties();
                music.Title(titleText);
                music.Artist(winrt::hstring{ std::wstring(artist) });
            }
            else
            {
                updater.Type(MediaPlaybackType::Video);
                updater.VideoProperties().Title(titleText);
            }

            updater.Update();
            EnableIfNeeded();

            g_lastTitle.assign(title.data(), title.size());
            g_lastArtist.assign(artist.data(), artist.size());
            g_lastMediaPath = mediaPath;
            g_lastAudioOnly = audioOnly;
            g_hasMetadata = true;

            if (audioOnly && !mediaPath.empty())
            {
                UpdateAudioArtworkAsync(std::move(mediaPath), generation);
            }
        }
        catch (...)
        {
        }
    }

    void Clear() noexcept
    {
        try
        {
            if (!g_controls) return;
            if (g_enabled || g_hasMetadata)
            {
                auto updater = g_controls.DisplayUpdater();
                updater.ClearAll();
                updater.Update();
                g_controls.PlaybackStatus(MediaPlaybackStatus::Closed);
                g_controls.IsEnabled(false);
            }
        }
        catch (...)
        {
        }

        ResetCache();
    }
}
