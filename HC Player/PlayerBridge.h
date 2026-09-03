#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ShaderManager.h"

namespace MediaInfoBridge
{
    struct Analysis;
}

struct ImportedMpvOption
{
    std::wstring section;
    std::wstring name;
    std::wstring value;
    bool profile{};
    bool builtIn{};
};

struct ImportedMpvConfig
{
    std::vector<ImportedMpvOption> options;
    int invalidCount{};
    int ignoredNativeCount{};
    int appliedBuiltInCount{};
    int additionalCount{};
    int sectionCount{};
    bool success{};
    std::wstring message;
};

struct AudioDeviceOption
{
    std::wstring name;
    std::wstring description;
};

struct RecentMediaItem
{
    std::wstring path;
    std::wstring title;
    std::wstring hint;
};

enum class MediaSourceBadge
{
    None,
    DVD,
    BluRay,
    YouTube,
    YouTubeLive,
    HLS,
    HLSLive,
};

enum class MediaVideoBadge
{
    None,
    DolbyVision,
    HDR10Plus,
    HDR,
};

enum class MediaAudioBadge
{
    None,
    DolbyAudio,
    DolbyAtmos,
    DTS,
    DTSX,
};

struct MediaBadgeInfo
{
    MediaSourceBadge source{ MediaSourceBadge::None };
    MediaVideoBadge video{ MediaVideoBadge::None };
    MediaAudioBadge audio{ MediaAudioBadge::None };
    int64_t videoTrackId{ -1 };
    int64_t audioTrackId{ -1 };
    bool sourceReady{};
    bool videoReady{};
    bool audioReady{};
};

struct MediaTrackOption
{
    int64_t id{};
    std::wstring type;
    std::wstring title;
    std::wstring language;
    std::wstring codec;
    bool selected{};
    bool external{};
    bool forced{};
    bool defaultTrack{};
    int mainSelection{ -1 };
};

struct MediaChapterOption
{
    double time{};
    std::wstring title;
};

struct MediaEditionOption
{
    int64_t id{};
    std::wstring title;
    double duration{};
    bool selected{};
    bool defaultEdition{};
    bool discTitle{};
};

struct MediaPlaylistItem
{
    int64_t index{};
    std::wstring title;
    std::wstring filename;
    std::wstring format;
    bool current{};
};


struct PlayerEngineVersionInfo
{
    std::wstring mpv;
    std::wstring ffmpeg;
    std::wstring libplacebo;
    std::wstring libass;
};

struct YtdlpStatus
{
    bool available{};
    bool imported{};
    bool jsRuntimeAvailable{};
    bool jsRuntimeImported{};
    bool jsRuntimeInvalid{};
    std::wstring path;
    std::wstring jsRuntimePath;
    std::wstring message;
};

using PlayerShaderInfo = hc::shaders::ShaderInfo;
using PlayerAnime4KModeInfo = hc::shaders::Anime4KModeInfo;
using PlayerAnime4KStatus = hc::shaders::Anime4KStatus;

bool PlayerLoadFile(std::wstring const& path);
void PlayerTogglePause();
void PlayerReplay();
bool PlayerGetPlaybackState(bool& paused, bool& eofReached);
void PlayerSetLooping(bool enabled);
bool PlayerGetLooping();
bool PlayerSetPlaylistShuffle(bool enabled);
bool PlayerGetPlaylistShuffle();
bool PlayerGetChapterHoverCardEnabled();
void PlayerToggleStats();
void PlayerSeek(double percent);
void PlayerSeekRelative(double seconds);
void PlayerSeekAbsolute(double seconds);
void PlayerSeekAbsoluteExact(double seconds);
void PlayerChangePlaylistItem(int delta);
void PlayerChangeChapter(int delta);
bool PlayerAdvanceContinuousPlayback();
void PlayerSetVolume(double volume);
bool PlayerAdjustVolumeFromWheel(int wheelDelta);
double PlayerGetVolume();
bool PlayerGetPlaybackTimes(double& positionSeconds, double& durationSeconds);
bool PlayerGetCurrentLocalMediaPath(std::wstring& path);
bool PlayerIsCurrentMediaImage();
bool PlayerIsCurrentMediaAudio();
void PlayerApplyAudioCoverScalingPolicy();
void PlayerUpdateTaskbarProgress();
bool PlayerGetWebCacheEnd(double& cacheEndSeconds);
void PlayerUpdateWebBufferingIndicator();
bool PlayerGetCurrentAudioArtist(std::wstring& artist);
std::wstring PlayerGetMediaTitle();
bool PlayerGetMediaInfoReport(std::wstring& report, std::wstring& error);
bool PlayerGetMediaInfoAnalysis(
    MediaInfoBridge::Analysis& analysis,
    std::wstring& error);
std::vector<MediaChapterOption> PlayerGetMediaChapters();
std::vector<MediaEditionOption> PlayerGetMediaEditions();
bool PlayerSelectMediaEdition(bool discTitle, int64_t id);
std::vector<MediaPlaylistItem> PlayerGetPlaylistItems();
bool PlayerPlayPlaylistItem(int64_t index);
bool PlayerRemovePlaylistItem(int64_t index);
bool PlayerClearPlaylistExceptCurrent();
bool PlayerMovePlaylistItem(int64_t fromIndex, int64_t finalIndex);
bool PlayerAddPlaylistFiles(std::vector<std::wstring> const& paths);
bool PlayerAddPlaylistFilesFromDialog();
bool PlayerAddPlaylistFolderFromDialog();
double PlayerGetPlaybackSpeed();
void PlayerSetPlaybackSpeed(double speed);
void PlayerToggleFullscreen();
void PlayerToggleBorderless();
void PlayerTogglePictureInPicture();
bool PlayerEngineReady();
// Read-only startup signal used only by the transport UI. For video/image
// media this becomes true once mpv has a current video track and valid
// video-out-params; audio-only media becomes ready once its audio track exists.
bool PlayerIsMediaPresentationReady();
PlayerEngineVersionInfo PlayerGetEngineVersionInfo();
bool PlayerIsConsoleOpen();
double PlayerGetPositionPercent();
void PlayerShowSettings();
void PlayerCloseSettings();
void PlayerShowMediaInfo();
void PlayerCloseMediaInfo();
void PlayerShowPlaylist();
void PlayerClosePlaylist();
void* PlayerGetMainWindowHandle();
void PlayerShowOpenDialog();
void PlayerShowOpenFolderDialog();
void PlayerShowOpenDiscImageDialog(bool bluray);
void PlayerShowAddExternalAudioDialog();
void PlayerShowAddExternalSubtitleDialog();
bool PlayerOpenClipboardMedia();
bool PlayerOpenDroppedMedia(std::vector<std::wstring> const& items);
void PlayerCloseContextMenu();
void PlayerQuitApp();
bool PlayerOpenRecentFile(std::wstring const& path);
std::vector<RecentMediaItem> PlayerGetRecentFiles();
void PlayerClearRecentFiles();
void PlayerSendMpvKey(std::wstring const& key);
void PlayerExecuteMpvCommand(std::wstring const& command);
void PlayerCaptureScreenshot(bool withSubtitles);
void PlayerOpenScreenshotDirectory();
bool PlayerDeactivateImportedProfile();
bool PlayerApplyImportedProfile(std::wstring const& name);
std::wstring PlayerGetActiveImportedProfile();
ImportedMpvConfig PlayerGetImportedConfig();
void PlayerSetTransportVisible(bool visible);
void PlayerSetTransportHostVisible(bool visible);
void PlayerRefreshTransportLayout();
void PlayerSetTransportFlyoutOpen(bool open);
void PlayerSetTransportCompact(bool compact);
void PlayerSetTransportBarCompactLayout(bool compact);
void PlayerSetTransportImageMode(bool imageMode);
void PlayerSetTransportMinimal(bool minimal);
void PlayerReleaseTransportFocus();
bool PlayerIsCursorInTransportHotZone();
bool PlayerIsLightTheme();
void PlayerSetLightTheme(bool light);
std::vector<MediaTrackOption> PlayerGetMediaTracks();
MediaBadgeInfo PlayerGetMediaBadgeInfo();
int PlayerGetCustomBadgeVariantMask(std::wstring const& badgeName);
int PlayerGetCustomBadgeFileCount();
bool PlayerImportCustomBadgeSet(
    std::wstring const& sourceFolder,
    int& importedCount,
    std::wstring& error);
bool PlayerResetCustomBadgeSet(std::wstring& error);
bool PlayerImportCustomBadgeFile(
    std::wstring const& sourceFile,
    std::wstring& importedBadgeName,
    std::wstring& importedVariant,
    std::wstring& error);
bool PlayerRemoveCustomBadgeFile(
    std::wstring const& badgeName,
    std::wstring const& variant,
    int& removedCount,
    std::wstring& error);
std::wstring PlayerGetCustomBadgePath(
    std::wstring const& badgeName,
    bool lightTheme);
bool PlayerSelectMediaTrack(std::wstring const& property, std::wstring const& value);
void PlayerToggleAlwaysOnTop();
void PlayerSetAlwaysOnTop(bool enabled);
void PlayerSetHardwareDecoding(bool enabled);
void PlayerSetSubtitleSize(double size);
bool PlayerSetMpvOption(std::wstring const& name, std::wstring const& value);
bool PlayerApplyMpvOptions(
    std::vector<std::pair<std::wstring, std::wstring>> const& options,
    std::wstring& error);
bool PlayerTryGetSavedMpvOption(std::wstring const& name, std::wstring& value);
bool PlayerTryGetMpvRuntimeOption(std::wstring const& name, std::wstring& value);
std::vector<AudioDeviceOption> PlayerGetAudioDevices();
std::vector<std::wstring> PlayerGetMpvOptionChoices(std::wstring const& name);
std::vector<std::wstring> PlayerGetImportedProfileNames();
std::vector<PlayerShaderInfo> PlayerGetShaders();
PlayerAnime4KStatus PlayerGetAnime4KStatus();
bool PlayerSetAnime4KMode(
    std::wstring const& profile,
    std::wstring const& mode,
    std::wstring& error);
bool PlayerDisableAnime4K(std::wstring& error);
bool PlayerImportShader(std::wstring const& sourcePath, std::wstring& error);
bool PlayerSetShaderEnabled(
    std::wstring const& shaderPath,
    bool enabled,
    std::wstring& error);
bool PlayerMoveShader(
    std::wstring const& shaderPath,
    int direction,
    std::wstring& error);
bool PlayerRemoveShader(
    std::wstring const& shaderPath,
    std::wstring& error);
bool PlayerRemoveAllShaders(std::wstring& error);
YtdlpStatus PlayerGetYtdlpStatus();
bool PlayerImportYtdlpBinary(std::wstring const& sourcePath, std::wstring& error);
bool PlayerResetImportedYtdlp(std::wstring& error);
bool PlayerImportDenoBinary(std::wstring const& sourcePath, std::wstring& error);
bool PlayerResetImportedDeno(std::wstring& error);
ImportedMpvConfig PlayerImportMpvConfig(std::wstring const& path);
bool PlayerResetImportedConfig();
bool PlayerResetAllSettingsToDefaults(std::wstring& error);
bool PlayerUpdateImportedOption(
    std::wstring const& section,
    std::wstring const& name,
    std::wstring const& value,
    bool profile);

