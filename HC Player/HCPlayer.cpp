#include "pch.h"
#include "HCPlayer.h"
#include "App.xaml.h"
#include "MainPage.h"
#include "SettingsPage.h"
#include "MediaInfoPage.h"
#include "PlaylistPage.h"
#include "ContextMenuPage.h"
#include "PlayerBridge.h"
#include "ShaderManager.h"
#include "RecentMediaManager.h"
#include "ExternalToolsManager.h"
#include "MpvSettingsManager.h"
#include "LocalizationManager.h"
#include "MediaInfoBridge.h"
#include "FileAssociationManager.h"
#include "SystemMediaControlsManager.h"
#include "StoragePaths.h"
#include "PortableRegistrationManager.h"

#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <Microsoft.UI.Dispatching.Interop.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Text.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

#define MAX_LOADSTRING 100

namespace winrt
{
    using namespace Microsoft::UI;
    using namespace Microsoft::UI::Dispatching;
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Hosting;
}

namespace
{
    // Three deliberately roomy transport rows: media information, timeline,
    // and playback controls. Keeping this in one constant also keeps the MPV
    // video surface, mouse hot-zone and subtitle offset perfectly aligned.
    constexpr int ControlsHeight = 132;
    // TimelineRow = 24 DIP + 4 DIP top margin. Images have no seek timeline,
    // so the transport removes exactly those 28 DIP.
    constexpr int ImageControlsHeight = 104;
    // Two-row Bar removes the 38-DIP information row while preserving the
    // approved timeline and playback rows. Still images additionally remove
    // the 28-DIP timeline, leaving only the playback row.
    constexpr int CompactBarControlsHeight = 94;
    constexpr int CompactBarImageControlsHeight = 66;
    constexpr int CompactControlsHeight = 54;
    constexpr int MinimalControlsHeight = 66;
    constexpr int PictureInPictureControlsHeight = 82;
    constexpr int PictureInPictureWidth = 440;
    constexpr int PictureInPictureHeight = 332;
    constexpr int NormalMinimumWidth = 568;
    constexpr int NormalMinimumHeight = 360;
    constexpr UINT TaskbarPreviousButtonId = 6101;
    constexpr UINT TaskbarPlayPauseButtonId = 6102;
    constexpr UINT TaskbarNextButtonId = 6103;
    constexpr UINT MpvWakeupMessage = WM_APP + 1;
    constexpr UINT ShowSettingsMessage = WM_APP + 2;
    constexpr UINT CloseSettingsMessage = WM_APP + 3;
    constexpr UINT ShowContextMenuMessage = WM_APP + 4;
    constexpr UINT CloseContextMenuMessage = WM_APP + 5;
    constexpr UINT OpenFileMessage = WM_APP + 6;
    constexpr UINT OpenFolderMessage = WM_APP + 7;
    constexpr UINT OpenDiscImageMessage = WM_APP + 8;
    constexpr UINT TransportVisibilityMessage = WM_APP + 9;
    constexpr UINT ReleaseTransportFocusMessage = WM_APP + 10;
    constexpr UINT ShowMediaInfoMessage = WM_APP + 11;
    constexpr UINT CloseMediaInfoMessage = WM_APP + 12;
    constexpr UINT SystemMediaControlsCommandMessage = WM_APP + 13;
    constexpr UINT ShowPlaylistMessage = WM_APP + 14;
    constexpr UINT ClosePlaylistMessage = WM_APP + 15;
    constexpr UINT AddExternalAudioMessage = WM_APP + 16;
    constexpr UINT AddExternalSubtitleMessage = WM_APP + 17;
    constexpr UINT_PTR NativeSettingsSaveTimer = 41;
    constexpr UINT_PTR SettingsTransitionTimer = 42;
    constexpr UINT_PTR TransportPointerTimer = 43;
    constexpr UINT_PTR AutofitWindowTimer = 44;
    constexpr UINT_PTR VideoSingleClickTimer = 45;
    constexpr UINT_PTR DynamicWindowFitTimer = 46;
    // Presentation-only one-shot poll used only after an immersive 16:9 ->
    // 16:9 fullscreen entry. It keeps the already-existing frozen bridge over
    // the real window until mpv reports its fullscreen video margins settled.
    constexpr UINT_PTR FullscreenVideoSettleTimer = 47;
    // 34.20.8.36 TEST: cold shell launches create the main HWND hidden only
    // long enough to place a black child shield over the client area. The
    // top-level window is then shown immediately while mpv resolves the first
    // media geometry underneath the shield.
    constexpr UINT_PTR InitialMediaRevealTimer = 48;
    constexpr ULONGLONG InitialMediaRevealTimeoutMs = 1500;
    constexpr int SettingsPanelWidth = 520;
    constexpr int MediaInfoPanelWidth = 520;
    constexpr int PlaylistPanelWidth = 520;
    constexpr wchar_t VideoWindowClassName[] = L"HCPlayer.VideoSurface";
    constexpr wchar_t SettingsHostClassName[] = L"HCPlayer.SettingsHost";
    constexpr wchar_t TransportHostClassName[] = L"HCPlayer.MinimalTransportHost";
    constexpr wchar_t PipResizeGripClassName[] = L"HCPlayer.PipResizeGrip";
    constexpr wchar_t BorderlessCaptionClassName[] =
        L"HCPlayer.BorderlessCaptionControls";
    constexpr wchar_t FullscreenTransitionShieldClassName[] =
        L"HCPlayer.FullscreenTransitionShield";
    constexpr wchar_t MediaFullscreenTransitionShieldClassName[] =
        L"HCPlayer.MediaFullscreenTransitionShield";
    constexpr wchar_t InitialMediaRevealShieldClassName[] =
        L"HCPlayer.InitialMediaRevealShield";
    constexpr wchar_t InstalledSingleInstanceMutexName[] =
        L"Local\\HCPlayer.SingleInstance.v1";
    constexpr wchar_t InstalledSingleInstancePrimaryPropertyName[] =
        L"HCPlayer.SingleInstancePrimary.v1";
    constexpr ULONG_PTR SingleInstanceCopyDataId = 0x4843504Cu; // "HCPL"

    HINSTANCE g_instance{};
    WCHAR g_title[MAX_LOADSTRING]{};
    WCHAR g_windowClass[MAX_LOADSTRING]{};
    HWND g_mainWindow{};
    HWND g_videoWindow{};
    HWND g_transportDropWindow{};
    HWND g_playlistDropWindow{};
    HWND g_pipBottomResizeWindow{};
    HWND g_borderlessCaptionWindow{};
    int g_captionHotButton{ -1 };
    int g_captionPressedButton{ -1 };
    bool g_fullscreen{};
    // While the top-level HWND is switching between windowed and fullscreen,
    // keep WinUI overlay islands hidden. WM_SIZE can otherwise let DWM present
    // one intermediate frame with the old transport width inside the new client
    // size. The video child stays live and continues receiving its final size.
    bool g_fullscreenLayoutTransition{};
    // One-shot request used by keyboard/double-click fullscreen transitions.
    // Enter, Escape and video double-click keep the transport hidden across the
    // resize; the toolbar button and other fullscreen callers preserve the
    // established reveal behavior. PlayerToggleFullscreen consumes this flag.
    bool g_suppressFullscreenEntryTransportReveal{};
    WINDOWPLACEMENT g_previousPlacement{ sizeof(WINDOWPLACEMENT) };
    LONG_PTR g_previousStyle{};
    LONG_PTR g_previousExStyle{};
    bool g_borderless{};
    LONG_PTR g_borderedStyle{};
    LONG_PTR g_borderedExStyle{};
    HWND g_borderlessDragSource{};
    POINT g_borderlessDragStart{};
    bool g_pictureInPicture{};
    // Returning from PiP to the normal window can synchronously emit WM_SIZE /
    // WM_MOVE while the XAML transport still has PiP geometry.  Minimal mode
    // then needs one dispatcher turn to move the page back into its dedicated
    // rounded popup.  Keep the transport completely hidden across that short
    // handoff so DWM never presents a mixed PiP/windowed frame.
    bool g_pipReturnLayoutTransition{};
    // Entering PiP from fullscreen can expose the restored normal window and
    // half-built PiP layout for a few compositor frames. Keep the top-level
    // player DWM-cloaked across that visual handoff only.
    bool g_pipEntryLayoutTransition{};

    // Presentation-only bridge for rapid Enter -> fullscreen transitions. It
    // owns a frozen copy of the last visible video frame; no libmpv/D3D11 or
    // fullscreen style state is stored here.
    struct FullscreenTransitionSnapshot
    {
        HBITMAP bitmap{};
        int width{};
        int height{};

        // 34.20.8.19: only the frozen transition bridge uses these fields.
        // They never alter mpv, its HWND, keepaspect, D3D11 or the final
        // fullscreen geometry. The crop is enabled only when both the current
        // video and destination monitor are effectively 16:9.
        bool settleMatchingFullscreenVideo{};
        int sourceX{};
        int sourceY{};
        int sourceWidth{};
        int sourceHeight{};
        int targetWidth{};
        int targetHeight{};
    };

    HWND g_pendingFullscreenVideoSettleShield{};
    ULONGLONG g_fullscreenVideoSettleStartedTick{};

    WINDOWPLACEMENT g_pipPreviousPlacement{ sizeof(WINDOWPLACEMENT) };
    LONG_PTR g_pipPreviousStyle{};
    LONG_PTR g_pipPreviousExStyle{};
    int g_pipResizeEdges{};
    POINT g_pipResizeStart{};
    RECT g_pipResizeWindowStart{};
    HWND g_pipResizeCaptureWindow{};
    POINT g_lastVideoMouseScreenPoint{};
    bool g_hasLastVideoMouseScreenPoint{};
    POINT g_transportHiddenCursor{};
    bool g_hasTransportHiddenCursor{};
    std::wstring g_currentMediaPath;
    bool g_currentMediaIsDisc{};
    bool g_currentDiscIsBluray{};
    bool g_loopPlayback{};
    bool g_shufflePlayback{};
    winrt::com_ptr<ITaskbarList3> g_taskbarList;
    UINT g_taskbarButtonCreatedMessage{};
    bool g_taskbarButtonsAdded{};
    HICON g_taskbarPreviousIcon{};
    HICON g_taskbarPlayIcon{};
    HICON g_taskbarPauseIcon{};
    HICON g_taskbarNextIcon{};
    bool g_taskbarIconThemeKnown{};
    bool g_taskbarIconsForLightBackground{};
    bool g_cursorHidden{};
    POINT g_lastCursorActivityPoint{};
    bool g_hasLastCursorActivityPoint{};
    ULONGLONG g_lastCursorActivityTick{};
    bool g_settingsOpen{};
    bool g_mediaInfoOpen{};
    bool g_playlistOpen{};
    bool g_contextMenuOpen{};
    bool g_transportFlyoutOpen{};
    bool g_transportHostVisible{ true };
    bool g_transportCompact{ true };
    bool g_transportBarCompactLayout{ true };
    bool g_transportImageMode{};
    bool g_transportMinimal{};
    // Minimal uses an owned top-level popup. Track the owner lifecycle and the
    // physical cursor independently so a lost XAML/WM_MOUSELEAVE cannot strand
    // the pill on screen, and a restore cannot present the popup before its
    // owner has produced the first visible frame.
    bool g_mainWindowWasMinimized{};
    bool g_minimalCursorOutsideOwner{};
    bool g_lightTheme{};
    bool g_themeLoaded{};
    bool g_suppressAutoload{};
    bool g_videoClickCandidate{};
    bool g_suppressNextVideoClickUp{};
    POINT g_videoClickStart{};
    int g_autofitAttemptsRemaining{};
    bool g_deferredStartupMediaReveal{};
    HWND g_initialMediaRevealShield{};
    ULONGLONG g_deferredStartupRevealStartedTick{};
    int64_t g_dynamicFitObservedWidth{};
    int64_t g_dynamicFitObservedHeight{};
    int64_t g_dynamicFitAppliedWidth{};
    int64_t g_dynamicFitAppliedHeight{};
    int g_dynamicFitStableSamples{};
    HANDLE g_singleInstanceMutex{};
    hc::recent::Manager g_recentMediaManager;
    hc::settings::Manager g_mpvSettingsManager;

    // Resume data deliberately stores no file names, URLs or titles. The
    // media identity is a compact hash and only three records are retained.
    // This keeps the feature private, tiny and completely outside mpv's
    // decoder/cache pipeline.
    struct ResumeRecord
    {
        std::string key;
        double position{};
        double duration{};
        std::int64_t savedAt{};
    };
    std::vector<ResumeRecord> g_resumeRecords;
    bool g_resumeRecordsLoaded{};

    // One-shot compatibility fallback for resume. The bundled mpv expands
    // local files through --autocreate-playlist, and older builds can lose
    // loadfile's file-local start option during that expansion. Keep the
    // intended target until mpv confirms that exact media item is active, then
    // perform one normal absolute+exact seek and immediately forget it.
    std::string g_pendingResumeKey;
    double g_pendingResumePosition{};
    int g_pendingResumePollsRemaining{};

    bool IsSidePanelOpen();

    std::wstring PlayerUiString(
        wchar_t const* resourceId,
        wchar_t const* fallback) noexcept
    {
        return hc::localization::GetString(resourceId, fallback);
    }

    std::string LocalizedBaseMpvOptionValue(
        char const* name,
        char const* fallback)
    {
        if (strcmp(name, "osd-msg3") != 0) return fallback;

        return winrt::to_string(PlayerUiString(
            L"OsdTimeUnknownDuration",
            L"${playback-time:--:--}${?duration: / ${duration}}${!duration: / duração desconhecida}"));
    }

    bool UiToggleEnabled(char const* name, bool fallback = false)
    {
        auto const found = g_mpvSettingsManager.Overrides().find(name);
        if (found == g_mpvSettingsManager.Overrides().end())
            return fallback;
        return found->second == "yes";
    }

    bool SingleInstanceModeEnabled()
    {
        auto const found =
            g_mpvSettingsManager.Overrides().find("ui-instance-mode");
        return found == g_mpvSettingsManager.Overrides().end() ||
            found->second != "multiple";
    }

    std::uint64_t PortableInstanceScopeHash()
    {
        // FNV-1a over the normalized executable directory keeps each portable
        // copy isolated from the installed app and from other portable copies.
        // This hash is only a local coordination name; it is not security data.
        std::wstring path = hc::storage::ExecutableDirectory()
            .lexically_normal().wstring();
        std::transform(path.begin(), path.end(), path.begin(), towlower);

        std::uint64_t hash = 14695981039346656037ull;
        for (wchar_t character : path)
        {
            hash ^= static_cast<std::uint16_t>(character);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    std::wstring const& SingleInstanceMutexName()
    {
        static std::wstring const name = []
            {
                if (!hc::storage::IsPortableMode())
                    return std::wstring{ InstalledSingleInstanceMutexName };

                std::wstringstream stream;
                stream << L"Local\\HCPlayer.Portable.SingleInstance.v1."
                    << std::hex << PortableInstanceScopeHash();
                return stream.str();
            }();
        return name;
    }

    std::wstring const& SingleInstancePrimaryPropertyName()
    {
        static std::wstring const name = []
            {
                if (!hc::storage::IsPortableMode())
                    return std::wstring{ InstalledSingleInstancePrimaryPropertyName };

                std::wstringstream stream;
                stream << L"HCPlayer.Portable.SingleInstancePrimary.v1."
                    << std::hex << PortableInstanceScopeHash();
                return stream.str();
            }();
        return name;
    }

    HWND FindSingleInstancePrimaryWindow()
    {
        HWND primary{};
        EnumWindows(
            [](HWND candidate, LPARAM parameter) -> BOOL
            {
                wchar_t className[MAX_LOADSTRING]{};
                if (!GetClassNameW(candidate, className, ARRAYSIZE(className)) ||
                    _wcsicmp(className, g_windowClass) != 0 ||
                    !GetPropW(candidate, SingleInstancePrimaryPropertyName().c_str()))
                {
                    return TRUE;
                }

                *reinterpret_cast<HWND*>(parameter) = candidate;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&primary));
        return primary;
    }

    void ReleaseSingleInstanceGate()
    {
        if (g_mainWindow)
        {
            RemovePropW(
                g_mainWindow, SingleInstancePrimaryPropertyName().c_str());
        }
        if (g_singleInstanceMutex)
        {
            ReleaseMutex(g_singleInstanceMutex);
            CloseHandle(g_singleInstanceMutex);
            g_singleInstanceMutex = nullptr;
        }
    }

    void ApplySingleInstanceModeRuntime(std::wstring const& value)
    {
        if (_wcsicmp(value.c_str(), L"multiple") == 0)
        {
            ReleaseSingleInstanceGate();
            return;
        }

        if (g_singleInstanceMutex)
        {
            if (g_mainWindow)
            {
                SetPropW(
                    g_mainWindow,
                    SingleInstancePrimaryPropertyName().c_str(),
                    reinterpret_cast<HANDLE>(1));
            }
            return;
        }

        HANDLE mutex = CreateMutexW(nullptr, TRUE, SingleInstanceMutexName().c_str());
        if (!mutex)
        {
            return;
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(mutex);
            return;
        }

        g_singleInstanceMutex = mutex;
        if (g_mainWindow)
        {
            SetPropW(
                g_mainWindow,
                SingleInstancePrimaryPropertyName().c_str(),
                reinterpret_cast<HANDLE>(1));
        }
    }

    bool BecomeSingleInstancePrimaryOrForward()
    {
        HANDLE mutex = CreateMutexW(nullptr, TRUE, SingleInstanceMutexName().c_str());
        if (!mutex)
        {
            // Fail open: inability to create the coordination primitive must
            // never prevent the player itself from starting.
            return true;
        }

        if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            g_singleInstanceMutex = mutex;
            return true;
        }

        for (int attempt = 0; attempt < 60; ++attempt)
        {
            if (HWND primary = FindSingleInstancePrimaryWindow())
            {
                DWORD primaryProcessId{};
                GetWindowThreadProcessId(primary, &primaryProcessId);
                if (primaryProcessId)
                {
                    AllowSetForegroundWindow(primaryProcessId);
                }

                std::wstring commandLine = GetCommandLineW();
                COPYDATASTRUCT data{};
                data.dwData = SingleInstanceCopyDataId;
                data.cbData = static_cast<DWORD>(
                    (commandLine.size() + 1) * sizeof(wchar_t));
                data.lpData = commandLine.data();

                DWORD_PTR ignored{};
                if (SendMessageTimeoutW(
                    primary,
                    WM_COPYDATA,
                    0,
                    reinterpret_cast<LPARAM>(&data),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    3000,
                    &ignored))
                {
                    CloseHandle(mutex);
                    return false;
                }
            }

            DWORD const wait = WaitForSingleObject(mutex, 0);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED)
            {
                g_singleInstanceMutex = mutex;
                return true;
            }

            Sleep(100);
        }

        // Another healthy process still owns the single-instance gate. Do not
        // create a duplicate merely because its UI did not answer in time.
        CloseHandle(mutex);
        return false;
    }

    void ArmVideoClickCandidate()
    {
        if (!UiToggleEnabled("ui-click-video-play-pause", false) ||
            IsSidePanelOpen())
        {
            g_videoClickCandidate = false;
            return;
        }

        g_videoClickCandidate =
            GetCursorPos(&g_videoClickStart) != FALSE;
    }

    void UpdateVideoClickCandidateForMovement(WPARAM mouseState)
    {
        if (!g_videoClickCandidate || !(mouseState & MK_LBUTTON))
            return;

        POINT cursor{};
        if (!GetCursorPos(&cursor))
        {
            g_videoClickCandidate = false;
            return;
        }

        int const thresholdX =
            (std::max)(2, GetSystemMetrics(SM_CXDRAG) / 2);
        int const thresholdY =
            (std::max)(2, GetSystemMetrics(SM_CYDRAG) / 2);

        if (abs(cursor.x - g_videoClickStart.x) >= thresholdX ||
            abs(cursor.y - g_videoClickStart.y) >= thresholdY)
        {
            g_videoClickCandidate = false;
        }
    }

    std::wstring Trim(std::wstring value);
    std::wstring ResolveInternetShortcut(std::wstring const& path);
    bool IsPlayableFolderFile(std::filesystem::path const& path);
    int PipResizeEdgesAt(POINT screenPoint);
    int BorderlessResizeEdgesAt(POINT screenPoint);
    HCURSOR PipResizeCursor(int edges);

    FORMATETC DropFormat(CLIPFORMAT format)
    {
        return { format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    }

    bool DropDataHasFormat(IDataObject* data, CLIPFORMAT format)
    {
        auto request = DropFormat(format);
        return data && SUCCEEDED(data->QueryGetData(&request));
    }

    std::wstring ReadDroppedText(IDataObject* data, CLIPFORMAT format)
    {
        auto request = DropFormat(format);
        STGMEDIUM medium{};
        if (!data || FAILED(data->GetData(&request, &medium))) return {};
        wchar_t const* raw = static_cast<wchar_t const*>(GlobalLock(medium.hGlobal));
        std::wstring value = raw ? raw : L"";
        if (raw) GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        return Trim(value);
    }

    std::vector<std::wstring> ReadDroppedMedia(IDataObject* data)
    {
        std::vector<std::wstring> items;
        auto filesFormat = DropFormat(CF_HDROP);
        STGMEDIUM medium{};
        if (data && SUCCEEDED(data->GetData(&filesFormat, &medium)))
        {
            auto drop = reinterpret_cast<HDROP>(medium.hGlobal);
            if (drop)
            {
                UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT index = 0; index < count; ++index)
                {
                    UINT length = DragQueryFileW(drop, index, nullptr, 0);
                    std::wstring path(length + 1, L'\0');
                    DragQueryFileW(drop, index, path.data(), length + 1);
                    path.resize(length);
                    path = ResolveInternetShortcut(path);
                    if (!path.empty()) items.push_back(std::move(path));
                }
            }
            ReleaseStgMedium(&medium);
            if (!items.empty()) return items;
        }

        // Browsers normally expose dragged links as Unicode text. Chromium
        // also publishes UniformResourceLocatorW, which is checked first.
        CLIPFORMAT urlFormat = static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(L"UniformResourceLocatorW"));
        std::wstring text = ReadDroppedText(data,
            DropDataHasFormat(data, urlFormat) ? urlFormat : CF_UNICODETEXT);
        text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());
        if (auto newline = text.find(L'\n'); newline != std::wstring::npos)
            text.resize(newline);
        text = Trim(text);
        if (!text.empty()) items.push_back(std::move(text));
        return items;
    }

    class MediaDropTarget final : public IDropTarget
    {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid == IID_IUnknown || iid == IID_IDropTarget)
            {
                *result = static_cast<IDropTarget*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return static_cast<ULONG>(InterlockedIncrement(&m_references));
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            ULONG remaining = static_cast<ULONG>(
                InterlockedDecrement(&m_references));
            if (!remaining) delete this;
            return remaining;
        }

        HRESULT STDMETHODCALLTYPE DragEnter(
            IDataObject* data, DWORD, POINTL, DWORD* effect) override
        {
            m_accepts = Accepts(data);
            if (effect) *effect = m_accepts ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
        {
            if (effect) *effect = m_accepts ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DragLeave() override
        {
            m_accepts = false;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Drop(
            IDataObject* data, DWORD, POINTL, DWORD* effect) override
        {
            auto items = ReadDroppedMedia(data);
            bool opened = PlayerOpenDroppedMedia(items);
            if (effect) *effect = opened ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            m_accepts = false;
            return S_OK;
        }

    private:
        static bool Accepts(IDataObject* data)
        {
            CLIPFORMAT urlFormat = static_cast<CLIPFORMAT>(
                RegisterClipboardFormatW(L"UniformResourceLocatorW"));
            return DropDataHasFormat(data, CF_HDROP) ||
                DropDataHasFormat(data, CF_UNICODETEXT) ||
                DropDataHasFormat(data, urlFormat);
        }

        LONG m_references{ 1 };
        bool m_accepts{};
    };

    void RegisterMediaDropTarget(HWND window)
    {
        if (!window) return;
        auto* target = new (std::nothrow) MediaDropTarget{};
        if (!target) return;
        RegisterDragDrop(window, target);
        target->Release();
    }

    std::vector<std::wstring> ReadDroppedQueueFiles(IDataObject* data)
    {
        // Queue drops are intentionally narrower than HC Player's global drop
        // target: only real, supported filesystem media from Explorer may be
        // appended here. URLs/text keep their established global-open behavior.
        auto items = ReadDroppedMedia(data);
        std::vector<std::wstring> files;
        files.reserve(items.size());
        for (auto& item : items)
        {
            std::error_code error;
            std::filesystem::path path{ item };
            if (std::filesystem::is_regular_file(path, error) &&
                IsPlayableFolderFile(path))
            {
                files.push_back(std::move(item));
            }
        }
        return files;
    }

    class PlaylistDropTarget final : public IDropTarget
    {
    public:
        explicit PlaylistDropTarget(
            winrt::HCPlayer::implementation::PlaylistPage* page) noexcept
            : m_page(page)
        {
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (iid == IID_IUnknown || iid == IID_IDropTarget)
            {
                *result = static_cast<IDropTarget*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return static_cast<ULONG>(InterlockedIncrement(&m_references));
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            ULONG remaining = static_cast<ULONG>(
                InterlockedDecrement(&m_references));
            if (!remaining) delete this;
            return remaining;
        }

        HRESULT STDMETHODCALLTYPE DragEnter(
            IDataObject* data, DWORD, POINTL, DWORD* effect) override
        {
            m_accepts = !ReadDroppedQueueFiles(data).empty();
            SetVisual(m_accepts);
            if (effect) *effect = m_accepts ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
        {
            if (effect) *effect = m_accepts ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DragLeave() override
        {
            m_accepts = false;
            SetVisual(false);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Drop(
            IDataObject* data, DWORD, POINTL, DWORD* effect) override
        {
            auto files = ReadDroppedQueueFiles(data);
            bool appended = PlayerAddPlaylistFiles(files);
            if (effect) *effect = appended ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            m_accepts = false;
            CompleteDrop(appended);
            return S_OK;
        }

    private:
        void SetVisual(bool active) noexcept
        {
            if (!m_page) return;
            try
            {
                m_page->SetExternalDropActive(active);
            }
            catch (...)
            {
            }
        }

        void CompleteDrop(bool queueChanged) noexcept
        {
            if (!m_page) return;
            try
            {
                m_page->CompleteExternalDrop(queueChanged);
            }
            catch (...)
            {
            }
        }

        LONG m_references{ 1 };
        winrt::HCPlayer::implementation::PlaylistPage* m_page{};
        bool m_accepts{};
    };

    void RegisterPlaylistDropTarget(
        HWND window, winrt::HCPlayer::implementation::PlaylistPage* page)
    {
        if (!window || !page) return;

        auto* target = new (std::nothrow) PlaylistDropTarget{ page };
        if (!target) return;

        HRESULT const result = RegisterDragDrop(window, target);
        target->Release();
        if (SUCCEEDED(result))
        {
            g_playlistDropWindow = window;
        }
    }

    hc::shaders::Manager g_shaderManager;
    hc::tools::Manager g_externalToolsManager;

    bool IsWebUrl(std::wstring const& value)
    {
        std::wstring lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        return lower.starts_with(L"http://") || lower.starts_with(L"https://");
    }

    bool IsYouTubeUrl(std::wstring value)
    {
        if (value.starts_with(L"ytdl://"))
        {
            value.erase(0, 7);
        }

        std::transform(value.begin(), value.end(), value.begin(), towlower);

        size_t schemeEnd = value.find(L"://");
        if (schemeEnd == std::wstring::npos) return false;

        size_t authorityStart = schemeEnd + 3;
        size_t authorityEnd = value.find_first_of(L"/?#", authorityStart);
        std::wstring host = value.substr(
            authorityStart,
            authorityEnd == std::wstring::npos
                ? std::wstring::npos
                : authorityEnd - authorityStart);

        if (auto at = host.rfind(L'@'); at != std::wstring::npos)
        {
            host.erase(0, at + 1);
        }

        // YouTube hosts are DNS names, so a trailing numeric port can be
        // removed without affecting legitimate source detection.
        if (!host.empty() && host.front() != L'[')
        {
            if (auto colon = host.rfind(L':'); colon != std::wstring::npos)
            {
                host.resize(colon);
            }
        }

        while (!host.empty() && host.back() == L'.') host.pop_back();

        auto isHostOrSubdomain =
            [&host](std::wstring const& domain)
            {
                if (host == domain) return true;
                return host.size() > domain.size() &&
                    host.ends_with(domain) &&
                    host[host.size() - domain.size() - 1] == L'.';
            };

        return isHostOrSubdomain(L"youtube.com") ||
            host == L"youtu.be" ||
            isHostOrSubdomain(L"youtube-nocookie.com");
    }

    bool IsLikelyHlsSource(std::wstring const& value)
    {
        if (value.empty()) return false;

        std::wstring lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);

        // Keep the query string for HLS detection. Signed/CDN URLs frequently
        // carry the manifest name in a query parameter instead of the path
        // itself (for example ?file=master.m3u8). URL fragments are irrelevant
        // to the HTTP resource and can safely be ignored here.
        auto fragment = lower.find(L'#');
        if (fragment != std::wstring::npos) lower.resize(fragment);

        return lower.find(L".m3u8") != std::wstring::npos;
    }

    bool IsDirectMediaUrl(std::wstring const& value)
    {
        // HLS must go directly to FFmpeg/libavformat. Check it before stripping
        // the query so tokenized manifests such as ?url=master.m3u8 are not
        // accidentally redirected through yt-dlp.
        if (IsLikelyHlsSource(value)) return true;

        std::wstring lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        auto query = lower.find_first_of(L"?#");
        if (query != std::wstring::npos) lower.resize(query);
        static const std::set<std::wstring> extensions = {
            L".mpd", L".mp4", L".m4v", L".mkv", L".webm",
            L".mov", L".ts", L".m2ts", L".mp3", L".m4a", L".aac",
            L".flac", L".ogg", L".opus", L".wav", L".jpg", L".jpeg",
            L".png", L".webp", L".avif"
        };
        return std::any_of(extensions.begin(), extensions.end(),
            [&](std::wstring const& extension) { return lower.ends_with(extension); });
    }

    std::wstring MpvLoadTarget(std::wstring const& value)
    {
        if (value.starts_with(L"ytdl://")) return value;
        // Direct HLS/DASH/media streams stay with FFmpeg. Regular web pages are
        // explicitly handed to yt-dlp, which enables every extractor supported
        // by the imported binary rather than special-casing YouTube.
        if (IsWebUrl(value) && !IsDirectMediaUrl(value) && !g_externalToolsManager.ResolveYtdlpPath().empty())
            return L"ytdl://" + value;
        return value;
    }

    std::wstring ResolveInternetShortcut(std::wstring const& path)
    {
        std::filesystem::path file{ path };
        std::wstring extension = file.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        if (extension != L".url") return path;

        std::vector<wchar_t> url(32768);
        DWORD length = GetPrivateProfileStringW(
            L"InternetShortcut", L"URL", L"", url.data(),
            static_cast<DWORD>(url.size()), file.c_str());
        return length ? Trim(std::wstring{ url.data(), length }) : path;
    }

    int CurrentControlsHeight()
    {
        if (g_pictureInPicture) return PictureInPictureControlsHeight;
        if (g_transportMinimal) return MinimalControlsHeight;
        if (g_transportCompact) return CompactControlsHeight;
        if (g_transportImageMode)
            return g_transportBarCompactLayout
                ? CompactBarImageControlsHeight
                : ImageControlsHeight;
        return g_transportBarCompactLayout
            ? CompactBarControlsHeight
            : ControlsHeight;
    }

    UINT WindowDpi(HWND window)
    {
        UINT dpi = window ? GetDpiForWindow(window) : 0;
        return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    }

    int DipToPx(HWND window, int dip)
    {
        return MulDiv(dip, static_cast<int>(WindowDpi(window)),
            USER_DEFAULT_SCREEN_DPI);
    }

    int CurrentControlsHeightPx(HWND window)
    {
        return DipToPx(window, CurrentControlsHeight());
    }

    enum class TaskbarTransportIcon
    {
        Previous,
        Play,
        Pause,
        Next
    };

    bool TaskbarUsesLightBackground() noexcept
    {
        // The thumbnail-toolbar surface follows Windows mode (taskbar/Start),
        // not HC Player's independent app theme. SystemUsesLightTheme is the
        // per-user Windows setting behind that choice. If it cannot be read,
        // retain the long-standing dark-surface behavior as the safe fallback.
        DWORD light{};
        DWORD bytes = sizeof(light);
        if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"SystemUsesLightTheme",
            RRF_RT_REG_DWORD,
            nullptr,
            &light,
            &bytes) == ERROR_SUCCESS)
        {
            return light != 0;
        }
        return false;
    }

    HICON CreateTaskbarTransportIcon(
        TaskbarTransportIcon shape,
        bool lightBackground)
    {
        // The Windows thumbnail toolbar ultimately consumes a 32x32 HICON.
        // Draw the transport geometry ourselves instead of rasterizing a font
        // glyph: this keeps Previous / Play / Pause / Next visually stronger,
        // balanced and crisp at the tiny size used by the taskbar preview.
        constexpr int size = 32;
        constexpr int samplesPerAxis = 8;
        constexpr int samplesPerPixel = samplesPerAxis * samplesPerAxis;

        BITMAPV5HEADER bitmapInfo{};
        bitmapInfo.bV5Size = sizeof(bitmapInfo);
        bitmapInfo.bV5Width = size;
        bitmapInfo.bV5Height = -size;
        bitmapInfo.bV5Planes = 1;
        bitmapInfo.bV5BitCount = 32;
        bitmapInfo.bV5Compression = BI_BITFIELDS;
        bitmapInfo.bV5RedMask = 0x00FF0000;
        bitmapInfo.bV5GreenMask = 0x0000FF00;
        bitmapInfo.bV5BlueMask = 0x000000FF;
        bitmapInfo.bV5AlphaMask = 0xFF000000;

        void* rawPixels{};
        HDC screen = GetDC(nullptr);
        HBITMAP color = CreateDIBSection(screen,
            reinterpret_cast<BITMAPINFO*>(&bitmapInfo), DIB_RGB_COLORS,
            &rawPixels, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!color || !rawPixels) return nullptr;

        auto inTriangle = [](double px, double py,
            double ax, double ay, double bx, double by,
            double cx, double cy)
            {
                auto edge = [](double x1, double y1, double x2, double y2,
                    double x, double y)
                    {
                        return (x - x1) * (y2 - y1) -
                            (y - y1) * (x2 - x1);
                    };

                double e1 = edge(ax, ay, bx, by, px, py);
                double e2 = edge(bx, by, cx, cy, px, py);
                double e3 = edge(cx, cy, ax, ay, px, py);
                bool hasNegative = e1 < 0.0 || e2 < 0.0 || e3 < 0.0;
                bool hasPositive = e1 > 0.0 || e2 > 0.0 || e3 > 0.0;
                return !(hasNegative && hasPositive);
            };

        auto inRoundedRect = [](double px, double py,
            double left, double top, double right, double bottom,
            double radius)
            {
                if (px < left || px > right || py < top || py > bottom)
                    return false;

                double cx = (std::max)(left + radius,
                    (std::min)(right - radius, px));
                double cy = (std::max)(top + radius,
                    (std::min)(bottom - radius, py));
                double dx = px - cx;
                double dy = py - cy;
                return dx * dx + dy * dy <= radius * radius;
            };

        auto insideShape = [&](double x, double y)
            {
                switch (shape)
                {
                case TaskbarTransportIcon::Play:
                    return inTriangle(x, y,
                        10.8, 7.8, 10.8, 24.2, 23.2, 16.0);

                case TaskbarTransportIcon::Pause:
                    return inRoundedRect(x, y, 9.5, 7.8, 14.0, 24.2, 1.35) ||
                        inRoundedRect(x, y, 18.0, 7.8, 22.5, 24.2, 1.35);

                case TaskbarTransportIcon::Previous:
                    return inRoundedRect(x, y, 8.0, 8.0, 11.3, 24.0, 1.20) ||
                        inTriangle(x, y,
                            21.8, 8.4, 21.8, 23.6, 12.1, 16.0);

                case TaskbarTransportIcon::Next:
                    return inRoundedRect(x, y, 20.7, 8.0, 24.0, 24.0, 1.20) ||
                        inTriangle(x, y,
                            10.2, 8.4, 10.2, 23.6, 19.9, 16.0);
                }
                return false;
            };

        auto* pixels = static_cast<DWORD*>(rawPixels);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                int covered{};
                for (int sy = 0; sy < samplesPerAxis; ++sy)
                {
                    for (int sx = 0; sx < samplesPerAxis; ++sx)
                    {
                        double sampleX = static_cast<double>(x) +
                            (static_cast<double>(sx) + 0.5) / samplesPerAxis;
                        double sampleY = static_cast<double>(y) +
                            (static_cast<double>(sy) + 0.5) / samplesPerAxis;
                        if (insideShape(sampleX, sampleY)) ++covered;
                    }
                }

                BYTE alpha = static_cast<BYTE>(
                    (covered * 255 + samplesPerPixel / 2) / samplesPerPixel);
                DWORD const glyphRgb = lightBackground
                    ? 0x001B1B1B
                    : 0x00FFFFFF;
                pixels[y * size + x] = alpha == 0 ? 0
                    : (static_cast<DWORD>(alpha) << 24) | glyphRgb;
            }
        }

        constexpr int maskStride = ((size + 15) / 16) * 2;
        std::array<BYTE, maskStride * size> maskBits{};
        HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
        ICONINFO iconInfo{};
        iconInfo.fIcon = TRUE;
        iconInfo.hbmColor = color;
        iconInfo.hbmMask = mask;
        HICON icon = CreateIconIndirect(&iconInfo);
        DeleteObject(mask);
        DeleteObject(color);
        return icon;
    }

    void DestroyTaskbarTransportIconHandles()
    {
        for (HICON* icon : { &g_taskbarPreviousIcon, &g_taskbarPlayIcon,
                            &g_taskbarPauseIcon, &g_taskbarNextIcon })
        {
            if (*icon) DestroyIcon(std::exchange(*icon, nullptr));
        }
    }

    void EnsureTaskbarButtonIcons()
    {
        bool const lightBackground = TaskbarUsesLightBackground();
        if (g_taskbarIconThemeKnown &&
            g_taskbarIconsForLightBackground != lightBackground)
        {
            // Keep the toolbar registration itself intact. Only replace the
            // HICONs, then ThumbBarUpdateButtons publishes the new set.
            DestroyTaskbarTransportIconHandles();
        }

        g_taskbarIconThemeKnown = true;
        g_taskbarIconsForLightBackground = lightBackground;

        if (!g_taskbarPreviousIcon)
            g_taskbarPreviousIcon = CreateTaskbarTransportIcon(
                TaskbarTransportIcon::Previous, lightBackground);
        if (!g_taskbarPlayIcon)
            g_taskbarPlayIcon = CreateTaskbarTransportIcon(
                TaskbarTransportIcon::Play, lightBackground);
        if (!g_taskbarPauseIcon)
            g_taskbarPauseIcon = CreateTaskbarTransportIcon(
                TaskbarTransportIcon::Pause, lightBackground);
        if (!g_taskbarNextIcon)
            g_taskbarNextIcon = CreateTaskbarTransportIcon(
                TaskbarTransportIcon::Next, lightBackground);
    }

    void DestroyTaskbarButtonIcons()
    {
        DestroyTaskbarTransportIconHandles();
        g_taskbarIconThemeKnown = false;
        g_taskbarButtonsAdded = false;
    }

    void UpdateTaskbarThumbnailButtons(bool hasMedia, bool showPlay)
    {
        if (!g_taskbarList || !g_mainWindow) return;

        EnsureTaskbarButtonIcons();
        THUMBBUTTON buttons[3]{};
        auto configure = [hasMedia](THUMBBUTTON& button, UINT id,
            HICON icon, wchar_t const* tooltip)
            {
                button.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
                button.iId = id;
                button.hIcon = icon;
                button.dwFlags = hasMedia ? THBF_ENABLED : THBF_DISABLED;
                wcsncpy_s(button.szTip, ARRAYSIZE(button.szTip),
                    tooltip, _TRUNCATE);
            };

        auto previousTooltip = PlayerUiString(
            L"TaskbarPrevious", L"Anterior");
        auto playTooltip = PlayerUiString(
            L"TaskbarPlay", L"Reproduzir");
        auto pauseTooltip = PlayerUiString(
            L"TaskbarPause", L"Pausar");
        auto nextTooltip = PlayerUiString(
            L"TaskbarNext", L"Próximo");

        configure(buttons[0], TaskbarPreviousButtonId,
            g_taskbarPreviousIcon, previousTooltip.c_str());
        configure(buttons[1], TaskbarPlayPauseButtonId,
            showPlay ? g_taskbarPlayIcon : g_taskbarPauseIcon,
            showPlay ? playTooltip.c_str() : pauseTooltip.c_str());
        configure(buttons[2], TaskbarNextButtonId,
            g_taskbarNextIcon, nextTooltip.c_str());

        if (!g_taskbarButtonsAdded)
        {
            if (SUCCEEDED(g_taskbarList->ThumbBarAddButtons(
                g_mainWindow, ARRAYSIZE(buttons), buttons)))
            {
                g_taskbarButtonsAdded = true;
            }
        }
        else
        {
            g_taskbarList->ThumbBarUpdateButtons(
                g_mainWindow, ARRAYSIZE(buttons), buttons);
        }
    }

    const std::pair<const char*, const char*> BaseMpvOptions[] = {
        { "taskbar-progress", "yes" },
        { "force-seekable", "yes" },
        { "keep-open", "always" },
        { "reset-on-next-file", "pause" },
        { "autofit", "1216x714" },
        { "autofit-larger", "81%x81%" },
        { "title", "${media-title:HC Player}" },
        { "ytdl-raw-options", "" },
        { "hwdec", "d3d11va" },
        { "vo", "gpu-next" },
        { "gpu-api", "d3d11" },
        { "gpu-context", "d3d11" },
        { "icc-profile-auto", "no" },
        { "hr-seek", "yes" },
        { "scale", "spline36" },
        { "dscale", "hermite" },
        { "cscale", "bicubic_fast" },
        { "linear-upscaling", "no" },
        { "sigmoid-upscaling", "yes" },
        { "correct-downscaling", "yes" },
        { "dither-depth", "auto" },
        { "dither", "fruit" },
        { "deinterlace", "no" },
        { "deband", "no" },
        { "deband-iterations", "2" },
        { "deband-threshold", "56" },
        { "deband-range", "17" },
        { "deband-grain", "12" },
        { "tone-mapping", "auto" },
        { "target-colorspace-hint", "auto" },
        { "gamut-mapping-mode", "perceptual" },
        { "alang", "pt,por,en,eng,es,spa" },
        { "slang", "pt,por,en,eng,es,spa" },
        { "volume", "100" },
        { "audio-file-auto", "fuzzy" },
        { "volume-max", "100" },
        { "audio-pitch-correction", "yes" },
        { "ao", "wasapi" },
        { "audio-device", "auto" },
        { "audio-exclusive", "no" },
        { "sub-auto", "fuzzy" },
        { "demuxer-mkv-subtitle-preroll", "yes" },
        { "sub-fix-timing", "no" },
        { "sub-scale-by-window", "yes" },
        { "sub-use-margins", "yes" },
        { "sub-scale", "1.0" },
        { "sub-font", "Segoe UI" },
        { "sub-font-size", "48" },
        { "sub-color", "#FFF0F0F0" },
        { "sub-border-color", "#FF000000" },
        { "sub-border-size", "2.0" },
        { "sub-shadow-offset", "1.5" },
        { "sub-spacing", "0.5" },
        { "sub-blur", "0.4" },
        { "sub-gauss", "0.6" },
        { "screenshot-format", "png" },
        { "screenshot-high-bit-depth", "yes" },
        { "screenshot-png-compression", "1" },
        { "screenshot-directory", "~/Pictures/Capturas do HC Player" },
        { "screenshot-template", "%f-%wH.%wM.%wS.%wT-#%#00n" },
        { "cursor-autohide-fs-only", "no" },
        { "cursor-autohide", "780" },
        { "osd-level", "1" },
        { "osd-duration", "1000" },
        { "osd-msg3", "${playback-time:--:--}${?duration: / ${duration}}${!duration: / duração desconhecida}" },
        { "osd-font", "Verdana" },
        { "osd-font-size", "30" },
        { "osd-color", "#FFFFFF" },
        { "osd-border-color", "#000000" },
        { "osd-border-size", "0.6" },
        { "osd-blur", "0.2" },
        { "video-output-levels", "auto" },
        { "blend-subtitles", "no" },
        // Conservative timing defaults. An imported mpv.conf may override
        // them; the matching built-in cards are hidden to avoid duplicates.
        { "d3d11-sync-interval", "1" },
        { "video-sync", "audio" },
        { "interpolation", "no" }
    };

    bool IsHostManagedOption(std::string const& name)
    {
        return name == "taskbar-progress" ||
            name == "fullscreen" ||
            name == "cursor-autohide-fs-only" ||
            name == "cursor-autohide";
    }

    bool IsHostManagedOption(std::wstring const& name)
    {
        return name == L"taskbar-progress" ||
            name == L"fullscreen" ||
            name == L"cursor-autohide-fs-only" ||
            name == L"cursor-autohide";
    }

    std::string ConfiguredNativeValue(
        char const* name, char const* defaultValue)
    {
        auto saved = g_mpvSettingsManager.Overrides().find(name);
        return saved == g_mpvSettingsManager.Overrides().end()
            ? std::string{ defaultValue } : saved->second;
    }

    bool ConfiguredNativeToggle(char const* name, bool defaultValue)
    {
        std::string value = ConfiguredNativeValue(
            name, defaultValue ? "yes" : "no");
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char character) { return static_cast<char>(tolower(character)); });
        return value != "no" && value != "false" && value != "0";
    }

    void SetApplicationCursorHidden(bool hidden)
    {
        if (g_cursorHidden == hidden) return;
        g_cursorHidden = hidden;
        if (hidden)
        {
            while (ShowCursor(FALSE) >= 0) {}
            SetCursor(nullptr);
        }
        else
        {
            while (ShowCursor(TRUE) < 0) {}
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
    }

    void RegisterCursorActivity()
    {
        POINT cursor{};
        if (!GetCursorPos(&cursor)) return;
        if (g_hasLastCursorActivityPoint &&
            cursor.x == g_lastCursorActivityPoint.x &&
            cursor.y == g_lastCursorActivityPoint.y)
        {
            return;
        }
        g_lastCursorActivityPoint = cursor;
        g_hasLastCursorActivityPoint = true;
        g_lastCursorActivityTick = GetTickCount64();
        SetApplicationCursorHidden(false);
    }

    bool IsSidePanelOpen()
    {
        return g_settingsOpen || g_mediaInfoOpen || g_playlistOpen;
    }

    void UpdateCursorAutohide()
    {
        if (!g_mainWindow || IsSidePanelOpen() || g_contextMenuOpen ||
            g_transportFlyoutOpen || GetForegroundWindow() != g_mainWindow)
        {
            SetApplicationCursorHidden(false);
            return;
        }

        POINT cursor{};
        RECT client{};
        if (!GetCursorPos(&cursor) || !GetClientRect(g_mainWindow, &client))
        {
            SetApplicationCursorHidden(false);
            return;
        }

        if (g_pictureInPicture || g_borderless)
        {
            int const resizeEdges = g_pictureInPicture
                ? PipResizeEdgesAt(cursor)
                : BorderlessResizeEdgesAt(cursor);

            if (resizeEdges != 0)
            {
                // A native resize cursor is active. Never let the player's
                // normal cursor-autohide timer make it invisible.
                g_lastCursorActivityTick = GetTickCount64();
                SetApplicationCursorHidden(false);
                SetCursor(PipResizeCursor(resizeEdges));
                return;
            }
        }

        ScreenToClient(g_mainWindow, &cursor);
        if (!PtInRect(&client, cursor))
        {
            SetApplicationCursorHidden(false);
            return;
        }

        if (ConfiguredNativeToggle("cursor-autohide-fs-only", false) &&
            !g_fullscreen)
        {
            SetApplicationCursorHidden(false);
            return;
        }

        std::string setting = ConfiguredNativeValue("cursor-autohide", "780");
        std::transform(setting.begin(), setting.end(), setting.begin(),
            [](unsigned char character) { return static_cast<char>(tolower(character)); });
        if (setting == "no")
        {
            SetApplicationCursorHidden(false);
            return;
        }
        if (setting == "always")
        {
            SetApplicationCursorHidden(true);
            return;
        }

        try
        {
            unsigned long long timeout = std::stoull(setting);
            if (!g_lastCursorActivityTick)
                g_lastCursorActivityTick = GetTickCount64();
            SetApplicationCursorHidden(
                GetTickCount64() - g_lastCursorActivityTick >= timeout);
        }
        catch (...)
        {
            SetApplicationCursorHidden(false);
        }
    }

    // Presentation policy owned by the player. Apply it after mpv.conf so an
    // imported profile cannot accidentally disable Windows/DWM synchronization.
    const std::pair<const char*, const char*> PresentationMpvOptions[] = {
        { "vo", "gpu-next" },
        { "gpu-api", "d3d11" },
        { "gpu-context", "d3d11" },
        { "d3d11-output-mode", "window" },
        { "d3d11-flip", "yes" },
        // Timing choices remain outside this mandatory presentation policy,
        // so an imported mpv.conf can own them.
    };

    struct mpv_handle;
    struct MpvNode;
    using mpv_create_fn = mpv_handle * (*)();
    using mpv_initialize_fn = int (*)(mpv_handle*);
    using mpv_set_option_string_fn = int (*)(mpv_handle*, const char*, const char*);
    using mpv_command_fn = int (*)(mpv_handle*, const char* const*);
    using mpv_command_node_fn = int (*)(mpv_handle*, MpvNode*, MpvNode*);
    using mpv_command_string_fn = int (*)(mpv_handle*, const char*);
    using mpv_set_property_string_fn = int (*)(mpv_handle*, const char*, const char*);
    using mpv_get_property_fn = int (*)(mpv_handle*, const char*, int, void*);
    using mpv_load_config_file_fn = int (*)(mpv_handle*, const char*);
    using mpv_terminate_destroy_fn = void (*)(mpv_handle*);
    using mpv_free_node_contents_fn = void (*)(void*);

    constexpr int MpvFormatString = 1;
    constexpr int MpvFormatInt64 = 4;
    constexpr int MpvFormatDouble = 5;
    constexpr int MpvFormatFlag = 3;

    bool BuildRuntimeInputConfig(
        std::filesystem::path const& source,
        std::filesystem::path& generated);
    constexpr int MpvFormatNode = 6;
    constexpr int MpvFormatNodeArray = 7;
    constexpr int MpvFormatNodeMap = 8;

    struct MpvNode;
    struct MpvNodeList
    {
        int count{};
        MpvNode* values{};
        char** keys{};
    };

    struct MpvNode
    {
        union
        {
            char* string;
            int flag;
            int64_t integer;
            double number;
            MpvNodeList* list;
            void* bytes;
        } value{};
        int format{};
    };

    struct MpvEngine
    {
        HMODULE module{};
        mpv_handle* handle{};
        mpv_create_fn create{};
        mpv_initialize_fn initialize{};
        mpv_set_option_string_fn setOption{};
        mpv_command_fn command{};
        mpv_command_node_fn commandNode{};
        mpv_command_string_fn commandString{};
        mpv_set_property_string_fn setProperty{};
        mpv_get_property_fn getProperty{};
        mpv_load_config_file_fn loadConfig{};
        mpv_terminate_destroy_fn destroy{};
        mpv_free_node_contents_fn freeNodeContents{};

        static int ShaderCommandAdapter(
            void* context,
            const char* const* args)
        {
            auto* engine = static_cast<MpvEngine*>(context);
            if (!engine || !engine->handle || !engine->command)
            {
                return -1;
            }
            return engine->command(engine->handle, args);
        }

        bool LoadFunctions()
        {
            if (module)
            {
                return true;
            }

            module = LoadLibraryW(L"libmpv-2.dll");
            if (!module)
            {
                module = LoadLibraryW(L"mpv-2.dll");
            }
            if (!module)
            {
                return false;
            }

            create = reinterpret_cast<mpv_create_fn>(GetProcAddress(module, "mpv_create"));
            initialize = reinterpret_cast<mpv_initialize_fn>(GetProcAddress(module, "mpv_initialize"));
            setOption = reinterpret_cast<mpv_set_option_string_fn>(GetProcAddress(module, "mpv_set_option_string"));
            command = reinterpret_cast<mpv_command_fn>(GetProcAddress(module, "mpv_command"));
            commandNode = reinterpret_cast<mpv_command_node_fn>(GetProcAddress(module, "mpv_command_node"));
            commandString = reinterpret_cast<mpv_command_string_fn>(GetProcAddress(module, "mpv_command_string"));
            setProperty = reinterpret_cast<mpv_set_property_string_fn>(GetProcAddress(module, "mpv_set_property_string"));
            getProperty = reinterpret_cast<mpv_get_property_fn>(GetProcAddress(module, "mpv_get_property"));
            loadConfig = reinterpret_cast<mpv_load_config_file_fn>(GetProcAddress(module, "mpv_load_config_file"));
            destroy = reinterpret_cast<mpv_terminate_destroy_fn>(GetProcAddress(module, "mpv_terminate_destroy"));
            freeNodeContents = reinterpret_cast<mpv_free_node_contents_fn>(GetProcAddress(module, "mpv_free_node_contents"));

            if (!create || !initialize || !setOption || !command || !commandString ||
                !setProperty || !getProperty || !loadConfig || !destroy || !freeNodeContents)
            {
                Stop();
                return false;
            }

            return true;
        }

        bool Start(HWND videoWindow)
        {
            if (handle)
            {
                return true;
            }

            if (!LoadFunctions())
            {
                return false;
            }

            handle = create();
            if (!handle)
            {
                Stop();
                return false;
            }

            setOption(handle, "terminal", "no");
            setOption(handle, "config", "no");
            setOption(handle, "input-default-bindings", "no");
            setOption(handle, "input-builtin-bindings", "no");
            setOption(handle, "input-vo-keyboard", "no");
            // Keep a video output alive without loaded media. This lets the
            // native MPV console render inside the embedded HWND at any time.
            setOption(handle, "idle", "yes");
            setOption(handle, "force-window", "yes");
            // libmpv does not guarantee that built-in scripts are enabled by
            // every embedding host. Both the yt-dlp hook and the native
            // console are scripts, while the player's own UI replaces OSC.
            setOption(handle, "load-scripts", "yes");
            setOption(handle, "osc", "no");
            setOption(handle, "ytdl", "yes");
            if (auto ytdlpPath = g_externalToolsManager.ResolveYtdlpPath(); !ytdlpPath.empty())
            {
                g_externalToolsManager.AddExecutableDirectoryToPath(ytdlpPath);
                // Forward slashes avoid the script-options parser treating a
                // Windows backslash as an escape character.
                std::string scriptOption = "ytdl_hook-ytdl_path=" +
                    winrt::to_string(ytdlpPath.generic_wstring());
                setOption(handle, "script-opts-append", scriptOption.c_str());
            }
            wchar_t executablePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, executablePath, ARRAYSIZE(executablePath));
            std::filesystem::path inputPath =
                std::filesystem::path(executablePath).parent_path() / L"default-input.conf";
            if (std::filesystem::exists(inputPath))
            {
                std::filesystem::path runtimeInputPath;
                if (BuildRuntimeInputConfig(inputPath, runtimeInputPath))
                {
                    inputPath = std::move(runtimeInputPath);
                }
                std::string inputPathUtf8 = winrt::to_string(inputPath.wstring());
                setOption(handle, "input-conf", inputPathUtf8.c_str());
            }
            for (auto const& [name, value] : BaseMpvOptions)
            {
                if (!IsHostManagedOption(name))
                {
                    // An explicit empty autofit/autofit-larger override means
                    // the user deliberately cleared that built-in default.
                    // Do not reapply the base value when the engine starts.
                    auto const saved = g_mpvSettingsManager.Overrides().find(name);
                    bool const explicitlyClearedWindowFit =
                        (strcmp(name, "autofit") == 0 ||
                            strcmp(name, "autofit-larger") == 0) &&
                        saved != g_mpvSettingsManager.Overrides().end() &&
                        saved->second.empty();
                    if (explicitlyClearedWindowFit) continue;

                    std::string localizedValue =
                        LocalizedBaseMpvOptionValue(name, value);
                    setOption(handle, name, localizedValue.c_str());
                }
            }

            if (!g_mpvSettingsManager.ImportedConfigPath().empty())
            {
                std::string configPath = winrt::to_string(g_mpvSettingsManager.ImportedConfigPath());
                loadConfig(handle, configPath.c_str());
            }

            // The Win32 host owns the actual taskbar button and cursor. Apply
            // this policy after an imported file so libmpv never manipulates
            // its embedded child HWND independently.
            setOption(handle, "taskbar-progress", "no");
            setOption(handle, "cursor-autohide", "no");

            for (auto const& [name, value] : PresentationMpvOptions)
            {
                setOption(handle, name, value);
            }

            // Explicit choices made in the built-in settings panel have the
            // final word, while an imported mpv.conf still overrides the base.
            for (auto const& [name, value] : g_mpvSettingsManager.Overrides())
            {
                if (name.starts_with("ui-")) continue;
                // These three are deliberate application defaults rather than
                // exposed preferences. Ignore stale values saved by older UI.
                if (name == "title" || name == "ao") continue;
                if ((name == "autofit" || name == "autofit-larger") &&
                    value.empty())
                {
                    continue;
                }
                if (!IsHostManagedOption(name))
                    setOption(handle, name.c_str(), value.c_str());
            }

            // Use mpv's native directory playlist implementation instead of
            // constructing/reordering sibling entries in the host application.
            // "filter" keeps the same mixed-media behavior the app already had:
            // video, audio and image extensions recognized by mpv are eligible.
            setOption(handle, "autocreate-playlist", "filter");

            // Keep extraction and libavformat on the same address family. On
            // dual-stack Windows connections YouTube may bind a signed media
            // URL to the IPv6 address used by yt-dlp, while FFmpeg opens it via
            // IPv4; YouTube then answers 403. This changes no format/quality.
            if (!g_externalToolsManager.ResolveYtdlpPath().empty())
            {
                std::string rawOption = "force-ipv4=";
                if (auto cookieBrowser = g_mpvSettingsManager.Overrides().find(
                    "ui-ytdl-cookie-browser");
                    cookieBrowser != g_mpvSettingsManager.Overrides().end() &&
                    !cookieBrowser->second.empty() &&
                    cookieBrowser->second != "no")
                {
                    // yt-dlp reads the selected browser's cookie database at
                    // extraction time. No cookie value is copied or persisted
                    // by this application.
                    rawOption = "cookies-from-browser=" +
                        cookieBrowser->second + "," + rawOption;
                }
                // Current yt-dlp versions recommend a JS runtime for full
                // YouTube support. The official executable contains EJS; only
                // Deno's explicit path needs to be supplied here.
                if (auto denoPath = g_externalToolsManager.ResolveDenoPath(); !denoPath.empty())
                {
                    rawOption = "js-runtimes=deno:" +
                        winrt::to_string(denoPath.generic_wstring()) + "," + rawOption;
                }
                setOption(handle, "ytdl-raw-options-append", rawOption.c_str());
            }

            std::string windowId = std::to_string(reinterpret_cast<uintptr_t>(videoWindow));
            setOption(handle, "wid", windowId.c_str());

            if (initialize(handle) < 0)
            {
                Stop();
                return false;
            }

            // HC Player-managed shaders are deliberately applied only after
            // mpv is initialized. ShaderManager keeps ownership/persistence
            // isolated from the engine; startup application remains best-effort
            // so a broken third-party shader can never prevent engine startup.
            g_shaderManager.ApplyStartup({
                this, &MpvEngine::ShaderCommandAdapter });

            return true;
        }

        void Stop()
        {
            if (handle && destroy)
            {
                destroy(handle);
            }
            handle = nullptr;
            if (module)
            {
                FreeLibrary(module);
            }
            module = nullptr;
            create = nullptr;
            initialize = nullptr;
            setOption = nullptr;
            command = nullptr;
            commandNode = nullptr;
            commandString = nullptr;
            setProperty = nullptr;
            getProperty = nullptr;
            loadConfig = nullptr;
            destroy = nullptr;
        }
    };

    MpvEngine g_mpv;

    hc::shaders::RuntimeAccess ManagedShaderRuntime()
    {
        if (!g_mpv.handle || !g_mpv.command)
        {
            return {};
        }
        return { &g_mpv, &MpvEngine::ShaderCommandAdapter };
    }

    int LoadFileWithLocalOptions(
        std::string const& target,
        char const* flags,
        std::vector<std::pair<std::string, std::string>> const& localOptions)
    {
        if (!g_mpv.handle || !g_mpv.command) return -1;

        if (!localOptions.empty() && g_mpv.commandNode)
        {
            // The client API accepts the final loadfile argument as a native
            // map. This avoids escaping collisions between loadfile's own
            // option list and nested libavformat key/value lists.
            std::vector<MpvNode> optionValues(localOptions.size());
            std::vector<char*> optionKeys(localOptions.size());
            for (size_t index = 0; index < localOptions.size(); ++index)
            {
                optionKeys[index] =
                    const_cast<char*>(localOptions[index].first.c_str());
                optionValues[index].format = MpvFormatString;
                optionValues[index].value.string =
                    const_cast<char*>(localOptions[index].second.c_str());
            }

            MpvNodeList optionMap{};
            optionMap.count = static_cast<int>(localOptions.size());
            optionMap.values = optionValues.data();
            optionMap.keys = optionKeys.data();

            MpvNode commandValues[5]{};
            auto setString = [](MpvNode& node, char const* value)
            {
                node.format = MpvFormatString;
                node.value.string = const_cast<char*>(value);
            };
            setString(commandValues[0], "loadfile");
            setString(commandValues[1], target.c_str());
            setString(commandValues[2], flags ? flags : "replace");
            setString(commandValues[3], "-1");
            commandValues[4].format = MpvFormatNodeMap;
            commandValues[4].value.list = &optionMap;

            MpvNodeList commandArray{};
            commandArray.count = ARRAYSIZE(commandValues);
            commandArray.values = commandValues;
            commandArray.keys = nullptr;

            MpvNode root{};
            root.format = MpvFormatNodeArray;
            root.value.list = &commandArray;
            return g_mpv.commandNode(g_mpv.handle, &root, nullptr);
        }

        // Compatibility fallback. An unexpectedly old/custom libmpv should
        // still open media instead of turning an optional resilience feature
        // into a startup failure. The existing continuation point is preserved.
        auto startOption = std::find_if(
            localOptions.begin(), localOptions.end(),
            [](auto const& option) { return option.first == "start"; });
        if (startOption != localOptions.end())
        {
            std::string serialized = "start=" + startOption->second;
            const char* args[] = {
                "loadfile", target.c_str(), flags ? flags : "replace",
                "-1", serialized.c_str(), nullptr };
            return g_mpv.command(g_mpv.handle, args);
        }

        const char* args[] = {
            "loadfile", target.c_str(), flags ? flags : "replace", nullptr };
        return g_mpv.command(g_mpv.handle, args);
    }

    std::vector<std::pair<std::string, std::string>> HlsFileLocalOptions()
    {
        // Do not override HLS network/cache behavior. Native mpv/FFmpeg HLS handling
        // proved more stable than the former forced HTTP reconnect policy. Also do
        // not force force-seekable=no here: HC Player's normal configured value must
        // remain available so HLS/DVR streams that can seek on user request behave
        // like the standalone mpv player.
        return {};
    }

    std::wstring Trim(std::wstring value)
    {
        constexpr wchar_t whitespace[] = L" \t\r\n";
        auto first = value.find_first_not_of(whitespace);
        if (first == std::wstring::npos)
        {
            return {};
        }
        auto last = value.find_last_not_of(whitespace);
        return value.substr(first, last - first + 1);
    }





    std::filesystem::path ThemeStoragePath()
    {
        return hc::storage::UserDataRoot() / L"theme.dat";
    }

    std::filesystem::path ResumePointsStoragePath()
    {
        return hc::storage::UserDataRoot() / L"resume-points.dat";
    }

    std::filesystem::path CustomMediaBadgesDirectory()
    {
        return hc::storage::UserDataRoot() / L"CustomBadges";
    }

    std::wstring Lowercase(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), towlower);
        return value;
    }

    bool IsCustomBadgeImage(std::filesystem::path const& path)
    {
        std::wstring extension = Lowercase(path.extension().wstring());
        return extension == L".png" || extension == L".svg";
    }

    bool IsSupportedCustomBadgeName(std::wstring const& badgeName)
    {
        static constexpr wchar_t const* supported[] = {
            L"DolbyAudio",
            L"DolbyVision",
            L"DolbyAtmos",
            L"DTS",
            L"DTSX",
            L"YouTube",
            L"HDR10Plus",
            L"HDR",
            L"DVD",
            L"BluRay",
        };

        for (auto const* value : supported)
        {
            if (badgeName == value)
            {
                return true;
            }
        }
        return false;
    }

    bool IsSupportedCustomBadgeVariant(std::wstring const& variant)
    {
        return variant == L"both" ||
            variant == L"dark" ||
            variant == L"light";
    }

    std::wstring CanonicalCustomBadgeStem(std::wstring stem)
    {
        std::wstring lower = Lowercase(stem);

        // Normalize only at IMPORT time. This has no playback/rendering cost.
        //
        // Accepted separators are intentionally flexible:
        //   DolbyVision.Dark
        //   DolbyVisionDark
        //   dolby_vision_dark
        //   dolby-vision-dark
        //   "Dolby Vision Dark"
        //
        // '+' is normalized to "plus", so HDR10+Dark is also accepted.
        std::wstring normalized;
        normalized.reserve(lower.size() + 4);

        for (wchar_t ch : lower)
        {
            if (ch >= L'a' && ch <= L'z')
            {
                normalized.push_back(ch);
            }
            else if (ch >= L'0' && ch <= L'9')
            {
                normalized.push_back(ch);
            }
            else if (ch == L'+')
            {
                normalized += L"plus";
            }
            // All other separators/punctuation are ignored on purpose.
        }

        struct NamePair
        {
            wchar_t const* normalized;
            wchar_t const* canonical;
        };

        static constexpr NamePair accepted[] = {
            { L"dolbyaudiodark",       L"DolbyAudio.Dark" },
            { L"dolbyaudiolight",      L"DolbyAudio.Light" },
            { L"dolbyaudiodarknor",    L"DolbyAudio.Dark" },
            { L"dolbyaudiolightnor",   L"DolbyAudio.Light" },

            { L"dolbyvisiondark",      L"DolbyVision.Dark" },
            { L"dolbyvisionlight",     L"DolbyVision.Light" },

            { L"dolbyatmosdark",       L"DolbyAtmos.Dark" },
            { L"dolbyatmoslight",      L"DolbyAtmos.Light" },

            { L"dtsdark",               L"DTS.Dark" },
            { L"dtslight",              L"DTS.Light" },

            { L"dtsxdark",              L"DTSX.Dark" },
            { L"dtsxlight",             L"DTSX.Light" },

            { L"youtubedark",           L"YouTube.Dark" },
            { L"youtubelight",          L"YouTube.Light" },

            { L"hdr10plusdark",         L"HDR10Plus.Dark" },
            { L"hdr10pluslight",        L"HDR10Plus.Light" },

            { L"hdrdark",               L"HDR.Dark" },
            { L"hdrlight",              L"HDR.Light" },

            { L"dvddark",               L"DVD.Dark" },
            { L"dvdlight",              L"DVD.Light" },
            // Backward-compatible aliases for HC Player's former packaged
            // optical artwork. These names are accepted only at import time;
            // the files themselves are no longer distributed with the app.
            { L"hcdvd",                 L"DVD.Dark" },
            { L"hcdvdlight",            L"DVD.Light" },

            { L"bluraydark",            L"BluRay.Dark" },
            { L"bluraylight",           L"BluRay.Light" },
            { L"hcbluray",              L"BluRay.Dark" },
            { L"hcbluraydark",          L"BluRay.Dark" },
            { L"hcbluraylight",         L"BluRay.Light" },
        };

        for (auto const& item : accepted)
        {
            if (normalized == item.normalized)
            {
                return item.canonical;
            }
        }
        return {};
    }

    std::filesystem::path FindCustomBadgeFile(
        std::wstring const& badgeName,
        bool lightTheme)
    {
        auto directory = CustomMediaBadgesDirectory();
        std::wstring requestedStem =
            badgeName + (lightTheme ? L".Light" : L".Dark");

        // Strict theme behavior:
        // - current-theme custom file exists -> use it
        // - otherwise -> return no custom file, so MainPage keeps the
        //   native HC Player badge for that theme.
        //
        // We intentionally do not use neutral files and never fall back
        // to the opposite theme.
        for (auto const* extension : { L".png", L".svg" })
        {
            auto candidate = directory / (requestedStem + extension);
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error))
            {
                return candidate;
            }
        }
        return {};
    }

    std::string ResumeIdentityHash(std::wstring const& identity)
    {
        // FNV-1a is sufficient here: this is an opaque lookup key, not a
        // security primitive. With a maximum of three entries, collision risk
        // is negligible, while avoiding any additional crypto dependency.
        std::string bytes = winrt::to_string(identity);
        std::uint64_t hash = 14695981039346656037ull;
        for (unsigned char byte : bytes)
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        std::ostringstream text;
        text << std::hex << std::setw(16) << std::setfill('0') << hash;
        return text.str();
    }

    bool LooksLikeStillImage(std::wstring const& value)
    {
        std::wstring clean = value;
        auto query = clean.find_first_of(L"?#");
        if (query != std::wstring::npos) clean.resize(query);
        std::filesystem::path path{ clean };
        std::wstring extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        static const std::set<std::wstring> imageExtensions = {
            L".jpg", L".jpeg", L".png", L".webp", L".avif", L".bmp",
            L".gif", L".tif", L".tiff", L".jxl", L".heic", L".heif"
        };
        return imageExtensions.contains(extension);
    }

    bool BuildResumeKey(std::wstring const& media, std::string& key)
    {
        key.clear();
        // A live HLS window can look finite and seekable while still being a
        // moving timeline. Disable continuation for every HLS source in v1;
        // ordinary local media and non-HLS VOD remain unchanged.
        if (media.empty() || LooksLikeStillImage(media) ||
            IsLikelyHlsSource(media))
        {
            return false;
        }

        std::wstring identity;
        std::error_code error;
        std::filesystem::path file{ media };
        if (std::filesystem::is_regular_file(file, error))
        {
            auto absolute = std::filesystem::weakly_canonical(file, error);
            if (error)
            {
                error.clear();
                absolute = std::filesystem::absolute(file, error);
                if (error) absolute = file.lexically_normal();
            }

            std::wstring normalized = absolute.wstring();
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), towlower);

            error.clear();
            auto size = std::filesystem::file_size(file, error);
            if (error) size = 0;
            error.clear();
            auto modified = std::filesystem::last_write_time(file, error);
            auto modifiedTicks = error
                ? std::int64_t{}
                : static_cast<std::int64_t>(modified.time_since_epoch().count());

            // Size + write time make replacing a local file at the same path a
            // new identity, so stale positions are not applied to new content.
            identity = L"file|" + normalized + L"|" +
                std::to_wstring(size) + L"|" + std::to_wstring(modifiedTicks);
        }
        else
        {
            // Exact URL identity is intentional. Signed/temporary URLs that
            // change naturally stop matching, which is safer than guessing.
            identity = L"url|" + media;
        }

        key = ResumeIdentityHash(identity);
        return !key.empty();
    }

    void ClearPendingResumeSeek()
    {
        g_pendingResumeKey.clear();
        g_pendingResumePosition = 0.0;
        g_pendingResumePollsRemaining = 0;
    }

    void ArmPendingResumeSeek(std::wstring const& media, double position)
    {
        std::string key;
        if (!BuildResumeKey(media, key) || !std::isfinite(position) || position < 0.0)
        {
            ClearPendingResumeSeek();
            return;
        }

        g_pendingResumeKey = std::move(key);
        g_pendingResumePosition = position;
        // ProgressTimerTick polls every 250 ms. Thirty seconds is intentionally
        // generous for slow local/network VOD opens while remaining strictly
        // one-shot and completely idle when there is no resume request.
        g_pendingResumePollsRemaining = 120;
    }

    bool TryApplyPendingResumeSeek(double& reportedPosition)
    {
        if (g_pendingResumeKey.empty() || g_pendingResumePollsRemaining <= 0)
            return false;

        --g_pendingResumePollsRemaining;
        if (g_pendingResumePollsRemaining <= 0)
        {
            ClearPendingResumeSeek();
            return false;
        }

        if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents ||
            !g_mpv.command)
        {
            return false;
        }

        // loadfile is asynchronous. Never seek merely because the host has
        // requested a new path: verify mpv's actually playing path first so an
        // outgoing file cannot receive the incoming file's continuation seek.
        std::wstring currentMedia;
        MpvNode pathNode{};
        if (g_mpv.getProperty(
            g_mpv.handle, "path", MpvFormatNode, &pathNode) >= 0)
        {
            if (pathNode.format == MpvFormatString && pathNode.value.string)
                currentMedia = winrt::to_hstring(pathNode.value.string).c_str();
            g_mpv.freeNodeContents(&pathNode);
        }

        if (currentMedia.empty()) return false;

        // HC Player prefixes extractor-backed web pages with ytdl:// only for
        // mpv dispatch. Resume identity is based on the user-facing URL, so
        // remove that transport prefix before comparing the opaque keys.
        if (currentMedia.starts_with(L"ytdl://"))
            currentMedia.erase(0, 7);

        std::string currentKey;
        if (!BuildResumeKey(currentMedia, currentKey) ||
            currentKey != g_pendingResumeKey)
        {
            return false;
        }

        double const target = g_pendingResumePosition;

        // On newer mpv versions the loadfile start option may already have been
        // honored. Avoid issuing a redundant seek in that case.
        if (std::isfinite(reportedPosition) &&
            std::abs(reportedPosition - target) <= 1.5)
        {
            ClearPendingResumeSeek();
            return false;
        }

        std::ostringstream value;
        value << std::setprecision(12) << target;
        std::string seconds = value.str();
        const char* seek[] = {
            "seek", seconds.c_str(), "absolute+exact", nullptr
        };
        if (g_mpv.command(g_mpv.handle, seek) < 0)
        {
            return false;
        }

        // Mirror the accepted target immediately; the next 250-ms poll will
        // replace it with mpv's authoritative post-seek time-pos.
        reportedPosition = target;
        ClearPendingResumeSeek();
        return true;
    }

    bool SaveResumeRecords()
    {
        auto storage = ResumePointsStoragePath();
        std::error_code error;
        std::filesystem::create_directories(storage.parent_path(), error);
        if (error) return false;

        auto temporary = storage;
        temporary += L".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        for (auto const& item : g_resumeRecords)
        {
            output << std::quoted(item.key) << ' '
                << std::setprecision(17) << item.position << ' '
                << std::setprecision(17) << item.duration << ' '
                << item.savedAt << '\n';
        }
        output.flush();
        bool good = output.good();
        output.close();
        if (!good)
        {
            std::filesystem::remove(temporary, error);
            return false;
        }

        if (!MoveFileExW(
            temporary.c_str(), storage.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }

    void LoadResumeRecords()
    {
        if (g_resumeRecordsLoaded) return;
        g_resumeRecordsLoaded = true;
        g_resumeRecords.clear();

        std::ifstream input(ResumePointsStoragePath(), std::ios::binary);
        ResumeRecord item;
        while (input >> std::quoted(item.key) >> item.position >> item.duration >> item.savedAt)
        {
            if (item.key.empty() || !std::isfinite(item.position) ||
                !std::isfinite(item.duration) || item.position < 0.0 || item.duration <= 0.0)
            {
                continue;
            }
            g_resumeRecords.push_back(item);
            if (g_resumeRecords.size() == 3) break;
        }
    }

    void ClearResumePoints()
    {
        ClearPendingResumeSeek();
        g_resumeRecordsLoaded = true;
        g_resumeRecords.clear();
        std::error_code error;
        std::filesystem::remove(ResumePointsStoragePath(), error);
    }

    void RemoveResumePoint(std::wstring const& media)
    {
        std::string key;
        if (!BuildResumeKey(media, key)) return;
        LoadResumeRecords();
        auto before = g_resumeRecords.size();
        std::erase_if(g_resumeRecords,
            [&key](ResumeRecord const& item) { return item.key == key; });
        if (g_resumeRecords.size() != before)
        {
            SaveResumeRecords();
        }
    }

    bool TryGetResumePosition(std::wstring const& media, double& position)
    {
        position = 0.0;
        if (!UiToggleEnabled("ui-resume-playback", false)) return false;

        std::string key;
        if (!BuildResumeKey(media, key)) return false;
        LoadResumeRecords();
        auto found = std::find_if(
            g_resumeRecords.begin(), g_resumeRecords.end(),
            [&key](ResumeRecord const& item) { return item.key == key; });
        if (found == g_resumeRecords.end()) return false;

        // Guard against stale/corrupt entries. We never resume from an almost
        // finished item, and tiny positions are not worth restoring.
        if (found->position < 10.0 ||
            found->position >= found->duration * 0.95 ||
            found->duration - found->position <= 30.0)
        {
            g_resumeRecords.erase(found);
            SaveResumeRecords();
            return false;
        }

        position = found->position;
        return true;
    }

    void SaveCurrentResumePointOnExit()
    {
        if (!UiToggleEnabled("ui-resume-playback", false) ||
            g_currentMediaPath.empty() || g_currentMediaIsDisc ||
            !g_mpv.handle || !g_mpv.getProperty)
        {
            return;
        }

        // Real still images have no playback position. Album art is excluded
        // by PlayerIsCurrentMediaImage(), so audio-with-cover remains eligible.
        if (PlayerIsCurrentMediaImage())
        {
            RemoveResumePoint(g_currentMediaPath);
            return;
        }

        double position{};
        double duration{};
        if (g_mpv.getProperty(
            g_mpv.handle, "time-pos", MpvFormatDouble, &position) < 0 ||
            g_mpv.getProperty(
                g_mpv.handle, "duration", MpvFormatDouble, &duration) < 0 ||
            !std::isfinite(position) || !std::isfinite(duration) || duration <= 0.0)
        {
            // Live/non-duration streams are deliberately ignored.
            return;
        }

        int seekable{};
        if (g_mpv.getProperty(
            g_mpv.handle, "seekable", MpvFormatFlag, &seekable) >= 0 &&
            seekable == 0)
        {
            return;
        }

        // Treat a restart or a practically-complete item as finished. This
        // also removes a previously stored point if the user intentionally
        // returned to the beginning before closing the app.
        if (position < 10.0 || position >= duration * 0.95 ||
            duration - position <= 30.0)
        {
            RemoveResumePoint(g_currentMediaPath);
            return;
        }

        std::string key;
        if (!BuildResumeKey(g_currentMediaPath, key)) return;
        LoadResumeRecords();
        std::erase_if(g_resumeRecords,
            [&key](ResumeRecord const& item) { return item.key == key; });

        ResumeRecord record;
        record.key = std::move(key);
        record.position = position;
        record.duration = duration;
        record.savedAt = static_cast<std::int64_t>(std::time(nullptr));
        g_resumeRecords.insert(g_resumeRecords.begin(), std::move(record));
        if (g_resumeRecords.size() > 3) g_resumeRecords.resize(3);
        SaveResumeRecords();
    }

    void EnsureThemeLoaded()
    {
        if (g_themeLoaded) return;
        g_themeLoaded = true;
        std::ifstream input(ThemeStoragePath(), std::ios::binary);
        std::string value;
        if (input >> value) g_lightTheme = value == "light";
    }

    void SaveTheme()
    {
        auto path = ThemeStoragePath();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (output) output << (g_lightTheme ? "light" : "dark");
    }

    void AddRecentFile(std::wstring const& path)
    {
        g_recentMediaManager.Add(path);
    }

    bool CaptureCurrentRecentTitle()
    {
        if (g_currentMediaPath.empty()) return false;
        return g_recentMediaManager.UpdateProtocolTitle(
            g_currentMediaPath, PlayerGetMediaTitle());
    }

    bool PickFileSystemPath(
        std::wstring& path,
        bool folder,
        bool iso,
        wchar_t const* title)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
        {
            return false;
        }

        FILEOPENDIALOGOPTIONS options{};
        dialog->GetOptions(&options);
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (folder) options |= FOS_PICKFOLDERS;
        else options |= FOS_FILEMUSTEXIST;
        dialog->SetOptions(options);
        dialog->SetTitle(title);
        if (iso)
        {
            std::wstring isoFilter = PlayerUiString(
                L"PickerIsoImageFilter", L"Imagem de disco ISO");
            std::wstring allFilesFilter = PlayerUiString(
                L"PickerAllFilesFilter", L"Todos os arquivos");
            COMDLG_FILTERSPEC filters[] = {
                { isoFilter.c_str(), L"*.iso" },
                { allFilesFilter.c_str(), L"*.*" }
            };
            dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        }

        if (FAILED(dialog->Show(g_mainWindow))) return false;
        winrt::com_ptr<IShellItem> item;
        if (FAILED(dialog->GetResult(item.put()))) return false;
        PWSTR rawPath{};
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))) return false;
        path.assign(rawPath);
        CoTaskMemFree(rawPath);
        return !path.empty();
    }

    bool PickExternalTrackFile(std::wstring& path, bool audio)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
        {
            return false;
        }

        FILEOPENDIALOGOPTIONS options{};
        if (FAILED(dialog->GetOptions(&options))) return false;
        options |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        if (FAILED(dialog->SetOptions(options))) return false;

        std::wstring filesFilter = PlayerUiString(
            audio ? L"PickerExternalAudioFilesFilter" : L"PickerExternalSubtitleFilesFilter",
            audio ? L"Arquivos de áudio" : L"Arquivos de legenda");
        std::wstring allFilesFilter = PlayerUiString(
            L"PickerAllFilesFilter", L"Todos os arquivos");
        COMDLG_FILTERSPEC filters[] = {
            { filesFilter.c_str(), audio
                ? L"*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.oga;*.opus;*.wav;*.wma;*.aiff;*.aif;*.ape;*.mka;*.ac3;*.eac3;*.dts;*.dtshd;*.spx;*.tak;*.tta;*.wv"
                : L"*.srt;*.ass;*.ssa;*.sub;*.idx;*.vtt;*.smi;*.sami;*.lrc;*.sup;*.mks;*.ttml;*.dfxp;*.txt" },
            { allFilesFilter.c_str(), L"*.*" }
        };
        if (FAILED(dialog->SetFileTypes(ARRAYSIZE(filters), filters))) return false;
        dialog->SetFileTypeIndex(1);

        std::wstring title = PlayerUiString(
            audio ? L"PickerExternalAudioTitle" : L"PickerExternalSubtitleTitle",
            audio ? L"Adicionar faixa de áudio externa" : L"Adicionar legenda externa");
        dialog->SetTitle(title.c_str());

        if (FAILED(dialog->Show(g_mainWindow))) return false;
        winrt::com_ptr<IShellItem> item;
        if (FAILED(dialog->GetResult(item.put()))) return false;
        PWSTR rawPath{};
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))) return false;
        path.assign(rawPath ? rawPath : L"");
        CoTaskMemFree(rawPath);
        return !path.empty();
    }

    bool AddExternalTrack(std::wstring const& path, bool audio)
    {
        if (path.empty() || !g_mpv.handle || !g_mpv.command) return false;
        std::string target = winrt::to_string(path);
        const char* command[] = {
            audio ? "audio-add" : "sub-add",
            target.c_str(),
            "select",
            nullptr
        };
        return g_mpv.command(g_mpv.handle, command) >= 0;
    }

    bool IsPlayableFolderFile(std::filesystem::path const& path)
    {
        static const std::set<std::wstring> extensions = {
            L".mkv", L".mk3d", L".mp4", L".webm", L".avi", L".mov", L".m4v",
            L".ts", L".m2ts", L".mts", L".flv", L".wmv", L".asf", L".mpg",
            L".mpeg", L".vob", L".ogv", L".rm", L".rmvb", L".3gp", L".3g2",
            L".divx", L".mp3", L".flac", L".m4a", L".aac", L".ogg", L".oga",
            L".opus", L".wav", L".wma", L".aiff", L".aif", L".ape", L".mka",
            L".ac3", L".eac3", L".dts", L".dtshd", L".spx", L".tak", L".tta",
            L".wv", L".mid", L".midi", L".mod", L".xm", L".s3m", L".it",
            L".m3u", L".m3u8", L".pls", L".cue",
            L".avif", L".bmp", L".gif", L".jpeg", L".jpg", L".jxl",
            L".png", L".svg", L".tga", L".tif", L".tiff", L".webp"
        };
        std::wstring extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        return extensions.contains(extension);
    }

    bool TryGetOpticalDiscFolder(
        std::filesystem::path const& selected,
        std::filesystem::path& deviceRoot,
        bool& bluray)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(selected, error)) return false;

        auto normalized = selected.lexically_normal();
        std::wstring folderName = normalized.filename().wstring();
        std::transform(folderName.begin(), folderName.end(), folderName.begin(), towlower);
        if (folderName == L"bdmv")
        {
            deviceRoot = normalized.parent_path();
            bluray = true;
            return !deviceRoot.empty();
        }
        if (folderName == L"video_ts")
        {
            deviceRoot = normalized.parent_path();
            bluray = false;
            return !deviceRoot.empty();
        }

        error.clear();
        if (std::filesystem::is_directory(normalized / L"BDMV", error))
        {
            deviceRoot = normalized;
            bluray = true;
            return true;
        }
        error.clear();
        if (std::filesystem::is_directory(normalized / L"VIDEO_TS", error))
        {
            deviceRoot = normalized;
            bluray = false;
            return true;
        }
        return false;
    }

    std::string QuoteMpvInputArgument(std::wstring const& value)
    {
        std::string utf8 = winrt::to_string(value);
        std::string quoted{ "\"" };
        for (char character : utf8)
        {
            if (character == '\\' || character == '"') quoted.push_back('\\');
            quoted.push_back(character);
        }
        quoted.push_back('"');
        return quoted;
    }

    std::string LocalizeInputShowText(std::string line)
    {
        // default-input.conf is the canonical keyboard binding preset.  Keep
        // every key and mpv command byte-for-byte equivalent, but localize the
        // quoted payload of show-text while generating runtime-input.conf.
        // Unknown/missing resources fall back to the literal bundled text.
        static const std::regex showTextLiteral{
            R"HC(show-text\s+"((?:\\.|[^"\\])*)")HC" };

        size_t searchOffset{};
        while (searchOffset < line.size())
        {
            std::smatch match;
            std::string tail = line.substr(searchOffset);
            if (!std::regex_search(tail, match, showTextLiteral)) break;

            std::string escaped = match[1].str();
            std::string unescaped;
            unescaped.reserve(escaped.size());
            for (size_t index = 0; index < escaped.size(); ++index)
            {
                if (escaped[index] == '\\' && index + 1 < escaped.size() &&
                    (escaped[index + 1] == '\\' || escaped[index + 1] == '"'))
                {
                    unescaped.push_back(escaped[++index]);
                }
                else
                {
                    unescaped.push_back(escaped[index]);
                }
            }

            std::wstring fallback = winrt::to_hstring(unescaped).c_str();
            std::wstring localized = hc::localization::GetStringForFallback(
                L"InputOsd", fallback);
            std::string replacement = QuoteMpvInputArgument(localized);

            size_t quotedStart = searchOffset +
                static_cast<size_t>(match.position(1)) - 1;
            size_t quotedLength = static_cast<size_t>(match.length(1)) + 2;
            line.replace(quotedStart, quotedLength, replacement);
            searchOffset = quotedStart + replacement.size();
        }
        return line;
    }

    bool BuildRuntimeInputConfig(
        std::filesystem::path const& source,
        std::filesystem::path& generated)
    {
        std::ifstream input(source, std::ios::binary);
        if (!input) return false;

        generated = hc::storage::UserDataRoot() / L"runtime-input.conf";

        std::error_code error;
        std::filesystem::create_directories(generated.parent_path(), error);
        if (error) return false;

        std::ofstream output(generated, std::ios::binary | std::ios::trunc);
        if (!output) return false;

        std::vector<std::wstring> profiles;
        std::set<std::wstring> profileNames;
        for (auto const& option : g_mpvSettingsManager.ImportedOptions())
        {
            if (option.profile && profileNames.insert(option.section).second)
            {
                profiles.push_back(option.section);
            }
        }

        auto writeImportedProfiles = [&]()
            {
                for (size_t index = 0; index < profiles.size(); ++index)
                {
                    std::wstring label = profiles[index];
                    std::replace(label.begin(), label.end(), L'>', L'\u203a');
                    label.erase(std::remove(label.begin(), label.end(), L'\r'), label.end());
                    label.erase(std::remove(label.begin(), label.end(), L'\n'), label.end());

                    std::string key = index < 12
                        ? "F" + std::to_string(index + 1)
                        : "_";
                    output << key << " apply-profile "
                        << QuoteMpvInputArgument(profiles[index])
                        << "; show-text "
                        << QuoteMpvInputArgument(
                            PlayerUiString(L"OsdProfilePrefix", L"Perfil: ") + label)
                        << " #menu: Perfis > "
                        << winrt::to_string(label) << '\n';
                }
            };

        bool profilesWritten{};
        // A duration supplied directly to show-text overrides osd-duration.
        // Strip only that final numeric argument in the generated runtime
        // file so the setting consistently controls every built-in message.
        static const std::regex showTextDuration{
            R"((show-text\s+"(?:\\.|[^"\\])*")\s+[0-9]+(?=\s*(?:#|$)))" };
        std::string line;
        while (std::getline(input, line))
        {
            line = LocalizeInputShowText(std::move(line));
            line = std::regex_replace(line, showTextDuration, "$1");
            bool oldProfileMenuLine =
                line.find("#menu: Perfis >") != std::string::npos;
            bool marker = line.find("IMPORTED_PROFILES_MENU") != std::string::npos;
            if ((oldProfileMenuLine || marker) && !profilesWritten)
            {
                writeImportedProfiles();
                profilesWritten = true;
            }
            if (!oldProfileMenuLine && !marker)
            {
                output << line << '\n';
            }
        }
        if (!profilesWritten && !profiles.empty())
        {
            output << '\n';
            writeImportedProfiles();
        }

        output.close();
        return output.good();
    }



    void ScheduleNativeOptionsSave()
    {
        g_mpvSettingsManager.MarkDirty();
        if (g_mainWindow)
        {
            SetTimer(g_mainWindow, NativeSettingsSaveTimer, 650, nullptr);
        }
    }




    std::wstring QuoteWindowsProcessArgument(std::wstring const& value)
    {
        if (value.empty()) return L"\"\"";
        if (value.find_first_of(L" \t\"") == std::wstring::npos)
            return value;

        std::wstring quoted{ L'"' };
        size_t backslashes{};
        for (wchar_t ch : value)
        {
            if (ch == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (ch == L'"')
            {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(L'"');
                backslashes = 0;
                continue;
            }
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
        quoted.append(backslashes * 2, L'\\');
        quoted.push_back(L'"');
        return quoted;
    }

    bool ParseYtdlpRawOptions(
        std::wstring const& raw,
        std::vector<std::pair<std::wstring, std::wstring>>& options,
        std::wstring& error)
    {
        options.clear();
        error.clear();
        if (Trim(raw).empty()) return true;
        if (!g_mpv.LoadFunctions())
        {
            error = L"O mecanismo de reprodução não está disponível para validar as opções do yt-dlp.";
            return false;
        }

        mpv_handle* validator = g_mpv.create();
        if (!validator)
        {
            error = L"Não foi possível preparar a validação das opções do yt-dlp.";
            return false;
        }

        auto destroyValidator = [&]()
        {
            if (validator)
            {
                g_mpv.destroy(validator);
                validator = nullptr;
            }
        };

        g_mpv.setOption(validator, "terminal", "no");
        g_mpv.setOption(validator, "config", "no");
        g_mpv.setOption(validator, "load-scripts", "no");
        g_mpv.setOption(validator, "ytdl", "no");

        std::string utf8Raw = winrt::to_string(raw);
        if (g_mpv.setOption(
            validator, "ytdl-raw-options", utf8Raw.c_str()) < 0)
        {
            destroyValidator();
            error = L"A sintaxe de ytdl-raw-options não é válida.";
            return false;
        }
        if (g_mpv.initialize(validator) < 0)
        {
            destroyValidator();
            error = L"Não foi possível inicializar a validação das opções do yt-dlp.";
            return false;
        }

        MpvNode node{};
        if (g_mpv.getProperty(
            validator,
            "options/ytdl-raw-options",
            MpvFormatNode,
            &node) < 0 ||
            node.format != MpvFormatNodeMap ||
            !node.value.list)
        {
            if (node.format != 0) g_mpv.freeNodeContents(&node);
            destroyValidator();
            error = L"Não foi possível interpretar ytdl-raw-options.";
            return false;
        }

        auto* list = node.value.list;
        bool valid = true;
        for (int index = 0; index < list->count; ++index)
        {
            if (!list->keys || !list->values || !list->keys[index] ||
                list->values[index].format != MpvFormatString ||
                !list->values[index].value.string)
            {
                valid = false;
                break;
            }
            options.emplace_back(
                winrt::to_hstring(list->keys[index]).c_str(),
                winrt::to_hstring(list->values[index].value.string).c_str());
        }

        g_mpv.freeNodeContents(&node);
        destroyValidator();
        if (!valid)
        {
            options.clear();
            error = L"Não foi possível interpretar ytdl-raw-options.";
            return false;
        }
        return true;
    }

    bool LoadYtdlpSupportedOptions(
        std::filesystem::path const& executable,
        std::set<std::wstring>& supported,
        std::wstring& error)
    {
        error.clear();
        supported.clear();
        if (executable.empty()) return false;

        std::error_code timeError;
        auto writeTime = std::filesystem::last_write_time(executable, timeError);
        static std::filesystem::path cachedPath;
        static std::filesystem::file_time_type cachedWriteTime{};
        static std::set<std::wstring> cachedOptions;
        if (!timeError && executable == cachedPath &&
            writeTime == cachedWriteTime && !cachedOptions.empty())
        {
            supported = cachedOptions;
            return true;
        }

        wchar_t tempDirectory[MAX_PATH]{};
        wchar_t tempFile[MAX_PATH]{};
        if (!GetTempPathW(ARRAYSIZE(tempDirectory), tempDirectory) ||
            !GetTempFileNameW(tempDirectory, L"HCP", 0, tempFile))
        {
            error = L"Não foi possível preparar a consulta às opções do yt-dlp.";
            return false;
        }

        SECURITY_ATTRIBUTES security{
            sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE output = CreateFileW(
            tempFile,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY,
            nullptr);
        if (output == INVALID_HANDLE_VALUE)
        {
            DeleteFileW(tempFile);
            error = L"Não foi possível preparar a consulta às opções do yt-dlp.";
            return false;
        }

        std::wstring commandLine = QuoteWindowsProcessArgument(executable.wstring()) +
            L" --ignore-config --help";
        STARTUPINFOW startup{ sizeof(STARTUPINFOW) };
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = output;
        startup.hStdOutput = output;
        startup.hStdError = output;
        PROCESS_INFORMATION process{};
        std::wstring workingDirectory = executable.parent_path().wstring();
        BOOL created = CreateProcessW(
            executable.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startup,
            &process);
        if (!created)
        {
            CloseHandle(output);
            DeleteFileW(tempFile);
            error = L"Não foi possível executar o yt-dlp atual para consultar suas opções.";
            return false;
        }

        DWORD wait = WaitForSingleObject(process.hProcess, 8000);
        if (wait == WAIT_TIMEOUT)
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
        }
        DWORD exitCode = ERROR_GEN_FAILURE;
        if (wait == WAIT_OBJECT_0)
            GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(output);

        if (wait != WAIT_OBJECT_0 || exitCode != 0)
        {
            DeleteFileW(tempFile);
            error = L"Não foi possível consultar as opções suportadas pelo yt-dlp atual.";
            return false;
        }

        std::ifstream input(std::filesystem::path{ tempFile }, std::ios::binary);
        std::string help{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{} };
        input.close();
        DeleteFileW(tempFile);

        // yt-dlp's help lists option declarations on lines whose first
        // non-whitespace character is '-'. Parse long names only from those
        // declaration lines so examples in descriptive text cannot whitelist
        // an otherwise unknown custom key.
        std::istringstream lines{ help };
        std::string line;
        std::regex longOption{ R"(--([A-Za-z0-9][A-Za-z0-9-]*))" };
        while (std::getline(lines, line))
        {
            auto first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos || line[first] != '-') continue;
            for (std::sregex_iterator match(
                    line.cbegin() + static_cast<std::string::difference_type>(first),
                    line.cend(), longOption), end;
                match != end; ++match)
            {
                supported.insert(winrt::to_hstring((*match)[1].str()).c_str());
            }
        }

        if (supported.empty())
        {
            error = L"O yt-dlp atual não retornou uma lista de opções reconhecível.";
            return false;
        }

        if (!timeError)
        {
            cachedPath = executable;
            cachedWriteTime = writeTime;
            cachedOptions = supported;
        }
        return true;
    }

    bool ValidateYtdlpRawOptions(
        std::wstring const& raw,
        std::wstring& error)
    {
        error.clear();
        if (Trim(raw).empty()) return true;

        std::vector<std::pair<std::wstring, std::wstring>> options;
        if (!ParseYtdlpRawOptions(raw, options, error)) return false;
        if (options.empty()) return true;

        auto executable = g_externalToolsManager.ResolveYtdlpPath();
        if (executable.empty())
        {
            error = L"O yt-dlp não está disponível para validar as opções avançadas.";
            return false;
        }

        std::set<std::wstring> supported;
        if (!LoadYtdlpSupportedOptions(executable, supported, error))
            return false;

        for (auto const& [key, value] : options)
        {
            (void)value;
            if (key.empty() || key.starts_with(L"--") ||
                !supported.contains(key))
            {
                error = L"O yt-dlp atual não reconhece a opção: --" + key;
                return false;
            }
        }
        return true;
    }

    bool ParseSettingDouble(std::wstring value, double& result)
    {
        value = Trim(std::move(value));
        if (value.empty()) return false;
        std::replace(value.begin(), value.end(), L',', L'.');
        try
        {
            size_t consumed{};
            result = std::stod(value, &consumed);
            return consumed == value.size() && std::isfinite(result);
        }
        catch (...)
        {
            return false;
        }
    }


    bool ValidateImportedMpvOptionEdit(
        std::wstring const& section,
        std::wstring const& name,
        std::wstring const& value,
        bool profile)
    {
        // Keep profile metadata behavior identical to the proven importer.
        if (profile && (name == L"profile-desc" || name == L"profile-restore"))
        {
            return true;
        }
        if (!g_mpv.LoadFunctions())
        {
            return false;
        }

        mpv_handle* validator = g_mpv.create();
        if (!validator)
        {
            return false;
        }

        bool foundTarget{};
        bool valid{};

        // Mirror PlayerImportMpvConfig() as closely as possible.
        //
        // The importer uses ONE temporary mpv handle and validates every
        // accepted option in original file order. It does not filter out
        // options merely because they came from another profile/section.
        //
        // The previous edit fix filtered that context, so it was still not
        // equivalent to the importer. Here we replay EVERY already-accepted
        // option that precedes the row being edited, in the exact stored order,
        // then substitute only the edited value at the target row.
        for (auto const& option : g_mpvSettingsManager.ImportedOptions())
        {
            bool const sameTarget =
                option.profile == profile &&
                option.name == name &&
                (!profile || option.section == section);

            if (sameTarget)
            {
                std::string const utf8Name = winrt::to_string(name);
                std::string const utf8Value = winrt::to_string(value);
                valid = g_mpv.setOption(
                    validator,
                    utf8Name.c_str(),
                    utf8Value.c_str()) >= 0;
                foundTarget = true;
                break;
            }

            // PlayerImportMpvConfig() accepts these profile metadata entries
            // without sending them to mpv, so the edit validator must do the
            // same while rebuilding the sequence.
            if (option.profile &&
                (option.name == L"profile-desc" ||
                 option.name == L"profile-restore"))
            {
                continue;
            }

            std::string const utf8Name = winrt::to_string(option.name);
            std::string const utf8Value = winrt::to_string(option.value);

            // Every option in ImportedOptions() already passed the original
            // importer. If replaying that proven sequence unexpectedly fails,
            // fail closed rather than accepting an edit without validation.
            if (g_mpv.setOption(
                validator,
                utf8Name.c_str(),
                utf8Value.c_str()) < 0)
            {
                foundTarget = false;
                valid = false;
                break;
            }
        }

        g_mpv.destroy(validator);
        return foundTarget && valid;
    }

    bool ValidateMpvOption(std::wstring const& name, std::wstring const& value, bool profile)
    {
        if (profile && (name == L"profile-desc" || name == L"profile-restore"))
        {
            return true;
        }
        if (!g_mpv.LoadFunctions())
        {
            return false;
        }

        mpv_handle* validator = g_mpv.create();
        if (!validator)
        {
            return false;
        }

        std::string utf8Name = winrt::to_string(name);
        std::string utf8Value = winrt::to_string(value);
        bool valid = g_mpv.setOption(
            validator, utf8Name.c_str(), utf8Value.c_str()) >= 0;
        g_mpv.destroy(validator);
        return valid;
    }

    struct WindowInfo;
    void ApplyClientLayout(HWND window, WindowInfo* info, int width, int height);

    struct WindowInfo
    {
        // The stable/classic transport remains hosted directly by the main HWND.
        // Only Minimal mode is rehosted into this owned popup so the DWM can
        // produce native antialiased corners without changing the normal bar.
        HWND transportHostWindow{};
        bool transportHostedInPopup{};
        winrt::DesktopWindowXamlSource xamlSource{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::MicaController
            transportMicaController{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration
            transportMicaConfiguration{ nullptr };
        winrt::HCPlayer::MainPage page{ nullptr };
        winrt::DesktopWindowXamlSource bufferingSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ProgressRing bufferingRing{ nullptr };
        bool bufferingVisible{};
        // Independent, non-interactive branding island shown only while no
        // media is loaded. Keeping it separate from the transport prevents
        // the XAML surface from ever covering libmpv during playback.
        winrt::DesktopWindowXamlSource emptyStateSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid emptyStateRoot{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid emptyStateGlowLayer{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Image emptyStateIcon{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::TranslateTransform emptyStateGlowTranslate{ nullptr };
        bool emptyStateGlowHovered{};
        bool emptyStateVisible{};
        winrt::DesktopWindowXamlSource settingsSource{ nullptr };
        winrt::HCPlayer::SettingsPage settingsPage{ nullptr };
        winrt::DesktopWindowXamlSource mediaInfoSource{ nullptr };
        winrt::HCPlayer::MediaInfoPage mediaInfoPage{ nullptr };
        winrt::DesktopWindowXamlSource playlistSource{ nullptr };
        winrt::HCPlayer::PlaylistPage playlistPage{ nullptr };
        HWND settingsHostWindow{};
        HWND settingsTransitionWindow{};
        HBITMAP settingsTransitionBitmap{};
        winrt::DesktopWindowXamlSource contextSource{ nullptr };
        winrt::HCPlayer::ContextMenuPage contextPage{ nullptr };
    };

    void CloseMinimalTransportMica(WindowInfo* info)
    {
        if (!info) return;
        if (info->transportMicaController)
        {
            try
            {
                info->transportMicaController.RemoveAllSystemBackdropTargets();
                info->transportMicaController.Close();
            }
            catch (...)
            {
            }
        }
        info->transportMicaController = nullptr;
        info->transportMicaConfiguration = nullptr;
    }

    void UpdateMinimalTransportMica(WindowInfo* info, bool inputActive)
    {
        if (!info || !info->transportMicaConfiguration) return;
        using namespace winrt::Microsoft::UI::Composition::SystemBackdrops;
        info->transportMicaConfiguration.IsInputActive(inputActive);
        info->transportMicaConfiguration.Theme(
            g_lightTheme ? SystemBackdropTheme::Light : SystemBackdropTheme::Dark);
    }

    bool SetupMinimalTransportMica(WindowInfo* info)
    {
        if (!info || !info->xamlSource) return false;
        using namespace winrt::Microsoft::UI::Composition;
        using namespace winrt::Microsoft::UI::Composition::SystemBackdrops;

        CloseMinimalTransportMica(info);
        try
        {
            if (!MicaController::IsSupported()) return false;
            auto target = info->xamlSource.try_as<ICompositionSupportsSystemBackdrop>();
            if (!target) return false;

            info->transportMicaConfiguration = SystemBackdropConfiguration{};
            UpdateMinimalTransportMica(
                info,
                GetForegroundWindow() == g_mainWindow ||
                GetActiveWindow() == g_mainWindow);

            info->transportMicaController = MicaController{};
            info->transportMicaController.Kind(MicaKind::Base);
            if (!info->transportMicaController.AddSystemBackdropTarget(target))
            {
                CloseMinimalTransportMica(info);
                return false;
            }
            info->transportMicaController.SetSystemBackdropConfiguration(
                info->transportMicaConfiguration);
            return true;
        }
        catch (...)
        {
            CloseMinimalTransportMica(info);
            return false;
        }
    }

    void QueueClassicTransportMica(HWND window)
    {
        auto dispatcher =
            winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        if (!dispatcher) return;

        dispatcher.TryEnqueue([window]()
            {
                if (!IsWindow(window)) return;
                auto* info = reinterpret_cast<WindowInfo*>(
                    GetWindowLongPtrW(window, GWLP_USERDATA));
                if (!info || !info->xamlSource || info->transportHostedInPopup)
                    return;
                try
                {
                    info->xamlSource.SystemBackdrop(
                        winrt::Microsoft::UI::Xaml::Media::MicaBackdrop{});
                }
                catch (winrt::hresult_error const&)
                {
                    // Preserve the stable solid fallback if Mica is unavailable.
                }
            });
    }

    bool EnsureMinimalTransportHost(HWND owner, WindowInfo* info)
    {
        if (!owner || !info) return false;
        if (info->transportHostWindow && IsWindow(info->transportHostWindow))
            return true;

        info->transportHostWindow = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            TransportHostClassName,
            nullptr,
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 1, 1,
            owner,
            nullptr,
            g_instance,
            nullptr);
        if (!info->transportHostWindow) return false;

        constexpr DWORD noBorderColor = 0xFFFFFFFE; // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(
            info->transportHostWindow, DWMWA_BORDER_COLOR,
            &noBorderColor, sizeof(noBorderColor));
        BOOL dark = g_lightTheme ? FALSE : TRUE;
        DwmSetWindowAttribute(
            info->transportHostWindow, DWMWA_USE_IMMERSIVE_DARK_MODE,
            &dark, sizeof(dark));
        DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(
            info->transportHostWindow, DWMWA_WINDOW_CORNER_PREFERENCE,
            &corners, sizeof(corners));
        ShowWindow(info->transportHostWindow, SW_HIDE);
        return true;
    }

    bool RehostTransportXaml(HWND window, WindowInfo* info, bool minimal)
    {
        if (!window || !info || !info->page) return false;
        if (info->transportHostedInPopup == minimal && info->xamlSource)
            return true;

        HWND target = window;
        if (minimal)
        {
            if (!EnsureMinimalTransportHost(window, info)) return false;
            target = info->transportHostWindow;
        }

        // Hide the current native island before changing XamlRoot so no
        // intermediate rectangular surface can flash over the video.
        if (info->xamlSource)
        {
            try
            {
                HWND oldIsland = winrt::Microsoft::UI::GetWindowFromWindowId(
                    info->xamlSource.SiteBridge().WindowId());
                if (oldIsland) ShowWindow(oldIsland, SW_HIDE);
                info->xamlSource.Content(nullptr);
                CloseMinimalTransportMica(info);
                info->xamlSource.Close();
            }
            catch (...)
            {
                CloseMinimalTransportMica(info);
            }
        }

        if (info->transportHostWindow)
            ShowWindow(info->transportHostWindow, SW_HIDE);

        try
        {
            info->xamlSource = winrt::DesktopWindowXamlSource{};
            info->xamlSource.Initialize(
                winrt::Microsoft::UI::GetWindowIdFromWindow(target));
            info->xamlSource.Content(info->page);
            info->transportHostedInPopup = minimal;

            if (minimal)
            {
                if (!SetupMinimalTransportMica(info))
                {
                    try
                    {
                        info->xamlSource.SystemBackdrop(
                            winrt::Microsoft::UI::Xaml::Media::MicaBackdrop{});
                    }
                    catch (...)
                    {
                    }
                }
            }
            else
            {
                QueueClassicTransportMica(window);
            }
            return true;
        }
        catch (...)
        {
            info->transportHostedInPopup = false;
            return false;
        }
    }

    void SetPipEntryWindowCloaked(HWND window, bool cloaked)
    {
        if (!window || !IsWindow(window)) return;

        // DWMWA_CLOAK keeps the HWND, libmpv child and composition surfaces
        // alive while preventing DWM from presenting intermediate geometry.
        DwmFlush();
        BOOL value = cloaked ? TRUE : FALSE;
        DwmSetWindowAttribute(
            window, DWMWA_CLOAK, &value, sizeof(value));
        DwmFlush();
    }

    void FinishPipEntryLayoutTransition(HWND window, WindowInfo* info)
    {
        if (!g_pipEntryLayoutTransition) return;

        // Allow the final PiP transport to participate in the settled layout,
        // then expose the already-composed result in one DWM commit.
        g_pipEntryLayoutTransition = false;

        if (info)
        {
            RECT client{};
            if (GetClientRect(window, &client))
            {
                ApplyClientLayout(
                    window,
                    info,
                    client.right - client.left,
                    client.bottom - client.top);
            }
        }

        UpdateWindow(window);
        SetPipEntryWindowCloaked(window, false);
        SetForegroundWindow(window);
    }

    void ScheduleTransportRehost(HWND window)
    {
        auto dispatcher =
            winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        if (!dispatcher) return;
        dispatcher.TryEnqueue([window]()
            {
                if (!IsWindow(window)) return;
                auto* info = reinterpret_cast<WindowInfo*>(
                    GetWindowLongPtrW(window, GWLP_USERDATA));
                if (!info) return;
                if (!RehostTransportXaml(window, info, g_transportMinimal))
                {
                    // Never leave the player transport permanently suppressed
                    // if a one-off rehost fails. PiP entry may also have the
                    // owner cloaked, so release that guard as a fail-safe.
                    g_pipReturnLayoutTransition = false;
                    if (g_pipEntryLayoutTransition)
                    {
                        FinishPipEntryLayoutTransition(window, info);
                    }
                    return;
                }

                // PiP -> window in Minimal mode intentionally holds the old PiP
                // island hidden until this rehost has attached the page to the
                // final rounded popup. Release the guard only now, immediately
                // before laying out the settled window geometry.
                if (g_pipReturnLayoutTransition && g_transportMinimal &&
                    info->transportHostedInPopup)
                {
                    g_pipReturnLayoutTransition = false;
                }

                // Fullscreen -> PiP from Minimal mode does the inverse rehost:
                // the rounded Minimal popup is removed and the compact PiP
                // transport returns to the player HWND. Uncloak only after that
                // final host exists.
                if (g_pipEntryLayoutTransition &&
                    g_pictureInPicture &&
                    !g_transportMinimal &&
                    !info->transportHostedInPopup)
                {
                    FinishPipEntryLayoutTransition(window, info);
                    return;
                }

                RECT client{};
                if (GetClientRect(window, &client))
                {
                    ApplyClientLayout(window, info,
                        client.right - client.left,
                        client.bottom - client.top);
                }
            });
    }

    bool TryGetMinimalTransportPixelRect(
        HWND window, WindowInfo* info,
        int transportWidth, int transportHeight, RECT& result)
    {
        result = {};
        if (!window || !info || !info->page) return false;
        auto* page = winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
            info->page);
        if (!page->UsesMinimalTransportStyle()) return false;

        auto const region = page->MinimalTransportRegion();

        // The pill is deliberately centered in TransportRoot. During a native
        // fullscreen style swap the XAML tree can still report the previous
        // TransformToVisual X/Y (or briefly no transform at all) even though the
        // native client already has its final size. Position from CURRENT native
        // geometry and use XAML only for the measured pill dimensions. If the
        // one-frame XAML measurement is unavailable, fall back to the frozen
        // approved Minimal geometry: 18-DIP side margins, MaxWidth 860, Height 50.
        // This avoids a hidden rectangular "measure" state during fullscreen.
        double const scale = static_cast<double>(WindowDpi(window)) /
            static_cast<double>(USER_DEFAULT_SCREEN_DPI);
        double const rootWidthDip = transportWidth / scale;
        double const rootHeightDip = transportHeight / scale;
        double const fallbackWidthDip = (std::max)(1.0,
            (std::min)(860.0, rootWidthDip - 36.0));
        double const fallbackHeightDip = (std::max)(1.0,
            (std::min)(50.0, rootHeightDip));
        double const pillWidthDip = region.Width > 1.0f
            ? static_cast<double>(region.Width)
            : fallbackWidthDip;
        double const pillHeightDip = region.Height > 1.0f
            ? static_cast<double>(region.Height)
            : fallbackHeightDip;
        int const pillWidth = (std::max)(1,
            static_cast<int>(std::ceil(pillWidthDip * scale)));
        int const pillHeight = (std::max)(1,
            static_cast<int>(std::ceil(pillHeightDip * scale)));
        int left = (std::max)(0, (transportWidth - pillWidth) / 2);
        int top = (std::max)(0, (transportHeight - pillHeight) / 2);
        int right = (std::min)(transportWidth, left + pillWidth);
        int bottom = (std::min)(transportHeight, top + pillHeight);
        if (right <= left || bottom <= top) return false;
        result = { left, top, right, bottom };
        return true;
    }

    void ApplyTransportLayoutOnly(
        HWND window, WindowInfo* info, int width, int height,
        bool showControls, int transportLayoutHeight)
    {
        if (!window || !info || !info->xamlSource) return;

        HWND island = winrt::Microsoft::UI::GetWindowFromWindowId(
            info->xamlSource.SiteBridge().WindowId());
        if (!island) return;

        if (!info->transportHostedInPopup)
        {
            // Byte-for-byte behavior of the pre-Minimal stable bar path.
            int transportY = showControls
                ? max(0, height - transportLayoutHeight)
                : height;
            info->xamlSource.SiteBridge().MoveAndResize(
                { 0, transportY, width, transportLayoutHeight });
            bool visible = IsWindowVisible(island) != FALSE;
            if (visible != showControls)
                ShowWindow(island, showControls ? SW_SHOWNA : SW_HIDE);
            if (showControls)
            {
                SetWindowPos(island, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            return;
        }

        // Minimal-only popup path. It cannot become visible while the owner is
        // hidden/minimized, preventing an owned transport from ever floating on
        // the desktop by itself.
        bool const ownerPresent =
            IsWindowVisible(window) != FALSE && IsIconic(window) == FALSE;
        bool const canShow = showControls && ownerPresent && g_transportMinimal;

        RECT pill{};
        bool const pillReady = TryGetMinimalTransportPixelRect(
            window, info, width, transportLayoutHeight, pill);

        POINT origin{ 0, 0 };
        ClientToScreen(window, &origin);
        int const fullY = (std::max)(0, height - transportLayoutHeight);

        if (!pillReady)
        {
            // Keep the hidden popup large enough for one normal XAML measure
            // pass. SizeChanged then requests a second layout with exact pill
            // bounds. Never show this rectangular measuring state.
            SetWindowPos(info->transportHostWindow, nullptr,
                origin.x, origin.y + fullY,
                (std::max)(1, width), (std::max)(1, transportLayoutHeight),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
            info->xamlSource.SiteBridge().MoveAndResize(
                { 0, 0, width, transportLayoutHeight });
            ShowWindow(info->transportHostWindow, SW_HIDE);
            if (!IsWindowVisible(island)) ShowWindow(island, SW_SHOWNA);
            return;
        }

        int const hostWidth = (std::max)(1, static_cast<int>(pill.right - pill.left));
        int const hostHeight = (std::max)(1, static_cast<int>(pill.bottom - pill.top));

        // The host itself is positioned from current native geometry so it
        // survives window <-> fullscreen transitions.  The XAML content still
        // needs to be clipped from its *actual* measured origin, however.  The
        // 34.20.8.4 fullscreen recovery reused the native-centered pill.left/top
        // for both jobs; at fractional DPI that can leave a 1-2 px strip of the
        // popup backdrop exposed along an edge.  Keep the native host placement,
        // but restore the measured XAML origin for the island offset.
        int contentLeft = pill.left;
        int contentTop = pill.top;
        auto* page = winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
            info->page);
        auto const measuredRegion = page->MinimalTransportRegion();
        if (measuredRegion.Width > 1.0f && measuredRegion.Height > 1.0f)
        {
            double const scale = static_cast<double>(WindowDpi(window)) /
                static_cast<double>(USER_DEFAULT_SCREEN_DPI);
            contentLeft = std::clamp(
                static_cast<int>(std::floor(measuredRegion.X * scale)),
                0, width);
            contentTop = std::clamp(
                static_cast<int>(std::floor(measuredRegion.Y * scale)),
                0, transportLayoutHeight);
        }
        info->xamlSource.SiteBridge().MoveAndResize(
            { -contentLeft, -contentTop, width, transportLayoutHeight });

        BOOL dark = g_lightTheme ? FALSE : TRUE;
        DwmSetWindowAttribute(info->transportHostWindow,
            DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(info->transportHostWindow,
            DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        SetWindowRgn(info->transportHostWindow, nullptr, FALSE);

        // A fullscreen WS_POPUP/frame rebuild can reorder an owned NOACTIVATE
        // popup even though its logical visibility never changed. The classic
        // transport already reasserts its child island at HWND_TOP when shown;
        // do the equivalent for Minimal while the HC Player owner is active.
        bool const ownerActive =
            GetForegroundWindow() == window || GetActiveWindow() == window;
        bool const raiseHost = canShow && (ownerActive || g_fullscreen);
        UINT hostFlags = SWP_NOACTIVATE | SWP_NOSENDCHANGING |
            SWP_NOOWNERZORDER | (canShow ? SWP_SHOWWINDOW : 0);
        if (!raiseHost) hostFlags |= SWP_NOZORDER;
        SetWindowPos(info->transportHostWindow,
            raiseHost ? HWND_TOP : nullptr,
            origin.x + pill.left,
            origin.y + fullY + pill.top,
            hostWidth, hostHeight,
            hostFlags);
        if (!canShow) ShowWindow(info->transportHostWindow, SW_HIDE);
        if (!IsWindowVisible(island)) ShowWindow(island, SW_SHOWNA);
    }

    void RefreshCustomBadgeVisuals()
    {
        if (!g_mainWindow)
        {
            return;
        }

        auto* windowInfo = reinterpret_cast<WindowInfo*>(
            GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
        if (windowInfo && windowInfo->page)
        {
            winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
                windowInfo->page)->RefreshInterfacePreferences();
        }
    }


    void ActivateMainWindowForForwardedLaunch()
    {
        if (!g_mainWindow) return;

        if (IsIconic(g_mainWindow))
        {
            ShowWindow(g_mainWindow, SW_RESTORE);
        }
        else
        {
            ShowWindow(g_mainWindow, SW_SHOW);
        }

        SetForegroundWindow(g_mainWindow);
        BringWindowToTop(g_mainWindow);
    }

    void EnsureUserDataOwnershipMarker() noexcept
    {
        try
        {
            auto const userDataRoot =
                g_mpvSettingsManager.ImportedConfigStoragePath().parent_path();
            if (userDataRoot.empty()) return;

            std::error_code error;
            std::filesystem::create_directories(userDataRoot, error);
            if (error) return;

            auto const marker = userDataRoot / L"hc-player-owned.marker";
            if (std::filesystem::exists(marker, error)) return;
            if (error) return;

            std::ofstream output(marker, std::ios::binary | std::ios::trunc);
            if (!output) return;
            output << "HC Player user data\n";
        }
        catch (...)
        {
            // Ownership marking is only for safe uninstall cleanup. It must
            // never block startup or affect playback if storage is unavailable.
        }
    }

    bool TryApplyInstallerInitialLanguage(
        std::wstring const& fullCommandLine,
        int& exitCode)
    {
        exitCode = 0;
        if (fullCommandLine.empty()) return false;

        int argumentCount{};
        LPWSTR* arguments = CommandLineToArgvW(
            fullCommandLine.c_str(), &argumentCount);
        if (!arguments) return false;

        bool const requested =
            argumentCount > 1 &&
            _wcsicmp(arguments[1], L"--hc-initial-language") == 0;
        if (!requested)
        {
            LocalFree(arguments);
            return false;
        }

        if (argumentCount != 3 ||
            (_wcsicmp(arguments[2], L"pt-BR") != 0 &&
             _wcsicmp(arguments[2], L"en-US") != 0))
        {
            LocalFree(arguments);
            exitCode = 2;
            return true;
        }

        std::string const requestedLanguage =
            _wcsicmp(arguments[2], L"pt-BR") == 0 ? "pt-BR" : "en-US";
        LocalFree(arguments);

        // Installer language is only an initial preference. Never overwrite a
        // language that already exists in the user's HC Player settings.
        auto& overrides = g_mpvSettingsManager.Overrides();
        if (overrides.contains("ui-language"))
        {
            return true;
        }

        overrides["ui-language"] = requestedLanguage;
        g_mpvSettingsManager.MarkDirty();
        if (!g_mpvSettingsManager.SaveNativeOptions())
        {
            exitCode = 3;
        }
        return true;
    }

    bool HasStartupMediaArgument(std::wstring const& fullCommandLine)
    {
        if (fullCommandLine.empty()) return false;

        int argumentCount{};
        LPWSTR* arguments = CommandLineToArgvW(
            fullCommandLine.c_str(), &argumentCount);
        if (!arguments) return false;

        bool const hasMediaArgument =
            argumentCount > 1 &&
            _wcsicmp(arguments[1], L"--import-config") != 0;

        LocalFree(arguments);
        return hasMediaArgument;
    }

    void HandleLaunchCommandLine(
        std::wstring const& fullCommandLine,
        bool forwarded)
    {
        if (!g_mainWindow || fullCommandLine.empty()) return;

        if (forwarded)
        {
            ActivateMainWindowForForwardedLaunch();
        }

        int argumentCount{};
        LPWSTR* arguments = CommandLineToArgvW(
            fullCommandLine.c_str(), &argumentCount);
        if (!arguments) return;

        if (argumentCount > 1)
        {
            if (_wcsicmp(arguments[1], L"--import-config") == 0 &&
                argumentCount > 2)
            {
                SendMessageW(g_mainWindow, ShowSettingsMessage, 0, 0);
                auto* info = reinterpret_cast<WindowInfo*>(
                    GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
                if (info && info->settingsPage)
                {
                    winrt::get_self<
                        winrt::HCPlayer::implementation::SettingsPage>(
                            info->settingsPage)->ImportPath(arguments[2]);
                }
            }
            else
            {
                auto* info = reinterpret_cast<WindowInfo*>(
                    GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
                if (info && info->page)
                {
                    auto* page = winrt::get_self<
                        winrt::HCPlayer::implementation::MainPage>(
                            info->page);
                    if (page->OpenPath(arguments[1]) && !forwarded)
                    {
                        // Showing the HWND after a cold shell launch can synthesize
                        // WM_MOUSEMOVE at the cursor's unchanged screen position.
                        // Seed the existing movement guard so only real pointer
                        // movement can reveal the newly hidden transport.
                        POINT cursor{};
                        if (GetCursorPos(&cursor))
                        {
                            g_lastVideoMouseScreenPoint = cursor;
                            g_hasLastVideoMouseScreenPoint = true;
                        }
                    }
                }
            }
        }

        LocalFree(arguments);
    }

    void RemoveSettingsTransition(WindowInfo* info)
    {
        if (!info) return;
        if (info->settingsTransitionWindow)
        {
            DestroyWindow(info->settingsTransitionWindow);
            info->settingsTransitionWindow = nullptr;
        }
        if (info->settingsTransitionBitmap)
        {
            DeleteObject(info->settingsTransitionBitmap);
            info->settingsTransitionBitmap = nullptr;
        }
    }

    void ShowSettingsTransition(HWND window, WindowInfo* info, int width, int height)
    {
        if (!info || width <= 0 || height <= 0) return;
        RemoveSettingsTransition(info);

        POINT origin{};
        ClientToScreen(window, &origin);
        RECT client{};
        GetClientRect(window, &client);
        int sourceWidth = max(1, client.right - client.left);
        int sourceHeight = max(1,
            client.bottom - client.top - min(
                CurrentControlsHeightPx(window), client.bottom - client.top));

        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
        HGDIOBJ previous = SelectObject(memory, bitmap);
        SetStretchBltMode(memory, HALFTONE);
        SetBrushOrgEx(memory, 0, 0, nullptr);
        StretchBlt(memory, 0, 0, width, height, screen,
            origin.x, origin.y, sourceWidth, sourceHeight,
            SRCCOPY | CAPTUREBLT);
        SelectObject(memory, previous);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);

        HWND cover = CreateWindowExW(0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_BITMAP,
            0, 0, width, height, window, nullptr, g_instance, nullptr);
        if (!cover)
        {
            DeleteObject(bitmap);
            return;
        }
        SendMessageW(cover, STM_SETIMAGE, IMAGE_BITMAP,
            reinterpret_cast<LPARAM>(bitmap));
        SetWindowPos(cover, HWND_TOP, 0, 0, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        info->settingsTransitionWindow = cover;
        info->settingsTransitionBitmap = bitmap;
        // gpu-next can need a little over one compositor frame to rebuild its
        // D3D11 swap chain after the video child changes width. Keep the last
        // fully presented frame visible until that resize has settled.
        SetTimer(window, SettingsTransitionTimer, 360, nullptr);
    }

    void AnimateEmptyStateGlow(WindowInfo* info, bool hovered)
    {
        if (!info || !info->emptyStateGlowLayer || !info->emptyStateIcon ||
            info->emptyStateGlowHovered == hovered)
        {
            return;
        }

        info->emptyStateGlowHovered = hovered;

        auto glowVisual =
            winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(info->emptyStateGlowLayer);
        auto iconVisual =
            winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(info->emptyStateIcon);
        auto compositor = glowVisual.Compositor();
        auto easing = compositor.CreateCubicBezierEasingFunction(
            winrt::Windows::Foundation::Numerics::float2{ 0.16f, 1.0f },
            winrt::Windows::Foundation::Numerics::float2{ 0.30f, 1.0f });

        // The glow is deliberately restrained: it brightens and opens behind
        // the logo without turning the idle screen into a neon animation.
        auto opacityAnimation = compositor.CreateScalarKeyFrameAnimation();
        opacityAnimation.InsertKeyFrame(
            1.0f, hovered ? 0.86f : 0.0f, easing);
        opacityAnimation.Duration(std::chrono::milliseconds(
            hovered ? 190 : 280));
        glowVisual.StartAnimation(L"Opacity", opacityAnimation);

        auto glowScaleAnimation = compositor.CreateVector3KeyFrameAnimation();
        float glowScale = hovered ? 1.0f : 0.88f;
        glowScaleAnimation.InsertKeyFrame(
            1.0f,
            winrt::Windows::Foundation::Numerics::float3{
                glowScale, glowScale, 1.0f },
            easing);
        glowScaleAnimation.Duration(std::chrono::milliseconds(
            hovered ? 210 : 300));
        glowVisual.StartAnimation(L"Scale", glowScaleAnimation);

        // A 1.2% lift gives the icon a tiny sense of depth while keeping the
        // original artwork visually stable.
        auto iconScaleAnimation = compositor.CreateVector3KeyFrameAnimation();
        float iconScale = hovered ? 1.012f : 1.0f;
        iconScaleAnimation.InsertKeyFrame(
            1.0f,
            winrt::Windows::Foundation::Numerics::float3{
                iconScale, iconScale, 1.0f },
            easing);
        iconScaleAnimation.Duration(std::chrono::milliseconds(
            hovered ? 190 : 260));
        iconVisual.StartAnimation(L"Scale", iconScaleAnimation);
    }

    void UpdateEmptyStateGlow(WindowInfo* info)
    {
        if (!info || !info->emptyStateSource || !info->emptyStateRoot ||
            !info->emptyStateGlowLayer || !info->emptyStateIcon ||
            !info->emptyStateGlowTranslate)
        {
            return;
        }

        // The glow belongs exclusively to the idle/empty screen. ApplyClientLayout
        // tracks that visibility, so playback pays only this boolean branch on
        // the existing 50 ms transport tick: no cursor query, XAML transform,
        // composition lookup, or animation work occurs while media is loaded.
        if (!info->emptyStateVisible)
        {
            return;
        }

        HWND emptyWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
            info->emptyStateSource.SiteBridge().WindowId());
        bool visible = emptyWindow && IsWindowVisible(emptyWindow) != FALSE;
        bool hovered = false;
        double targetX = 0.0;
        double targetY = 0.0;

        POINT cursor{};
        if (visible && GetCursorPos(&cursor))
        {
            try
            {
                double iconWidth = info->emptyStateIcon.ActualWidth();
                double iconHeight = info->emptyStateIcon.ActualHeight();
                if (iconWidth > 1.0 && iconHeight > 1.0)
                {
                    auto transform = info->emptyStateIcon.TransformToVisual(
                        info->emptyStateRoot);
                    auto origin = transform.TransformPoint(
                        winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
                    RECT islandRect{};
                    if (GetWindowRect(emptyWindow, &islandRect))
                    {
                        double scale = static_cast<double>(
                            GetDpiForWindow(emptyWindow)) / 96.0;
                        double centerX = islandRect.left +
                            (origin.X + iconWidth * 0.5) * scale;
                        double centerY = islandRect.top +
                            (origin.Y + iconHeight * 0.5) * scale;
                        double halfWidth = iconWidth * scale * 0.5;
                        double halfHeight = iconHeight * scale * 0.5;
                        double padding = 12.0 * scale;

                        hovered =
                            cursor.x >= centerX - halfWidth - padding &&
                            cursor.x <= centerX + halfWidth + padding &&
                            cursor.y >= centerY - halfHeight - padding &&
                            cursor.y <= centerY + halfHeight + padding;

                        if (hovered)
                        {
                            double nx = std::clamp(
                                (cursor.x - centerX) /
                                    (std::max)(1.0, halfWidth),
                                -1.0, 1.0);
                            double ny = std::clamp(
                                (cursor.y - centerY) /
                                    (std::max)(1.0, halfHeight),
                                -1.0, 1.0);
                            // Only the light follows the pointer. Four DIPs is
                            // enough to add depth without making the logo move.
                            targetX = nx * 4.0;
                            targetY = ny * 4.0;
                        }
                    }
                }
            }
            catch (winrt::hresult_error const&)
            {
                hovered = false;
            }
        }

        AnimateEmptyStateGlow(info, hovered);

        // The native cursor poll already runs every 50 ms for the transport.
        // Smooth toward the target here instead of creating another timer.
        double currentX = info->emptyStateGlowTranslate.X();
        double currentY = info->emptyStateGlowTranslate.Y();
        double nextX = currentX + (targetX - currentX) * 0.34;
        double nextY = currentY + (targetY - currentY) * 0.34;
        if (std::abs(nextX) < 0.02 && !hovered) nextX = 0.0;
        if (std::abs(nextY) < 0.02 && !hovered) nextY = 0.0;
        info->emptyStateGlowTranslate.X(nextX);
        info->emptyStateGlowTranslate.Y(nextY);
    }

    void ApplyClientLayout(HWND window, WindowInfo* info, int width, int height)
    {
        if (!info) return;

        // The transport island must be a real hidden HWND while settings are
        // open. A transparent XAML root still owns a composition surface and
        // leaves a grey strip over the video.
        bool showControls = !g_deferredStartupMediaReveal &&
            !g_fullscreenLayoutTransition &&
            !g_pipReturnLayoutTransition &&
            !g_pipEntryLayoutTransition &&
            !IsSidePanelOpen() &&
            g_transportHostVisible && info->xamlSource;
        int transportLayoutHeight =
            min(CurrentControlsHeightPx(window), height);
        int controlsHeight = showControls ? transportLayoutHeight : 0;

        // libmpv owns and paints this D3D11 child. Invalidating it through
        // MoveWindow(..., TRUE) adds an unnecessary WM_PAINT for every mouse
        // movement while the swap chain is already reacting to WM_SIZE.
        // The transition cover above preserves the last presented frame while
        // the swap chain adapts to the settings panel width.
        // Settings is a true overlay. The MPV child keeps its exact geometry,
        // so opening the panel never rebuilds or rescales the D3D11 swap chain.
        MoveWindow(g_videoWindow, 0, 0, width, height, FALSE);

        ApplyTransportLayoutOnly(
            window, info, width, height,
            showControls, transportLayoutHeight);

        if (info->bufferingSource)
        {
            UINT dpi = GetDpiForWindow(window);
            double scale = static_cast<double>(dpi) / 96.0;
            double shortestDip = static_cast<double>((std::min)(width, height)) / scale;
            // The WinUI Lottie artwork uses only part of the control bounds.
            // Preserve the compact PiP treatment. Windowed and fullscreen
            // sizes remain DPI-aware, with restrained caps for large surfaces.
            double ringDip = g_pictureInPicture
                ? std::clamp(shortestDip * 0.13, 56.0, 150.0)
                : (g_fullscreen
                    ? std::clamp(shortestDip * 0.21, 155.0, 225.0)
                    : std::clamp(shortestDip * 0.155, 62.0, 190.0));
            info->bufferingRing.Width(ringDip);
            info->bufferingRing.Height(ringDip);
            int indicatorSize = static_cast<int>(std::lround(
                (ringDip + 24.0) * scale));
            indicatorSize = min(indicatorSize, min(width, height));
            info->bufferingSource.SiteBridge().MoveAndResize({
                max(0, (width - indicatorSize) / 2),
                max(0, (height - indicatorSize) / 2),
                indicatorSize, indicatorSize });
            HWND bufferingWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
                info->bufferingSource.SiteBridge().WindowId());
            bool showBuffering = !g_deferredStartupMediaReveal &&
                !g_fullscreenLayoutTransition &&
                info->bufferingVisible && !IsSidePanelOpen() &&
                indicatorSize > 0;
            bool bufferingWindowVisible =
                IsWindowVisible(bufferingWindow) != FALSE;
            if (bufferingWindowVisible != showBuffering)
            {
                ShowWindow(bufferingWindow,
                    showBuffering ? SW_SHOWNA : SW_HIDE);
            }
            if (showBuffering)
            {
                SetWindowPos(bufferingWindow, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }

        if (info->emptyStateSource)
        {
            UINT dpi = GetDpiForWindow(window);
            double scale = static_cast<double>(dpi) / 96.0;

            // Keep the branding optically centered in the video area that
            // exists above the compact idle transport. Reserving that height
            // even after the transport auto-hides prevents a visible vertical
            // jump in the empty-state artwork.
            int reservedTransportHeight = g_currentMediaPath.empty()
                ? min(CurrentControlsHeightPx(window), height)
                : controlsHeight;
            int availableHeight = max(0, height - reservedTransportHeight);

            int desiredWidth = static_cast<int>(std::lround(520.0 * scale));
            int desiredHeight = static_cast<int>(std::lround(312.0 * scale));
            int emptyWidth = min(width, desiredWidth);
            int emptyHeight = min(availableHeight, desiredHeight);

            int emptyX = max(0, (width - emptyWidth) / 2);
            int emptyY = max(0, (availableHeight - emptyHeight) / 2);

            info->emptyStateSource.SiteBridge().MoveAndResize(
                { emptyX, emptyY, emptyWidth, emptyHeight });

            HWND emptyWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
                info->emptyStateSource.SiteBridge().WindowId());

            // Unlike the transport and buffering overlays, the idle/empty-state
            // island is safe to keep alive while the top-level HWND changes to or
            // from fullscreen. Hiding it here exposed the bare Win32 parent for an
            // intermediate frame on rapid idle-screen fullscreen entry, allowing
            // Windows to briefly present legacy non-client chrome. Keep the idle
            // island visible and let each WM_SIZE simply recenter it.
            bool showEmptyState =
                !g_deferredStartupMediaReveal &&
                g_currentMediaPath.empty() &&
                !IsSidePanelOpen() &&
                !g_pictureInPicture &&
                emptyWidth > 0 &&
                emptyHeight > 0;

            bool emptyWindowVisible =
                IsWindowVisible(emptyWindow) != FALSE;
            if (emptyWindowVisible != showEmptyState)
            {
                if (!showEmptyState)
                {
                    // Reset the hidden island once, at the visibility transition.
                    // This guarantees the next idle screen always starts clean,
                    // while UpdateEmptyStateGlow can do an immediate no-work return
                    // for every timer tick during playback.
                    info->emptyStateGlowHovered = false;
                    info->emptyStateGlowTranslate.X(0.0);
                    info->emptyStateGlowTranslate.Y(0.0);

                    auto glowVisual =
                        winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                        GetElementVisual(info->emptyStateGlowLayer);
                    glowVisual.StopAnimation(L"Opacity");
                    glowVisual.StopAnimation(L"Scale");
                    glowVisual.Opacity(0.0f);
                    glowVisual.Scale(
                        winrt::Windows::Foundation::Numerics::float3{
                            0.88f, 0.88f, 1.0f });

                    auto iconVisual =
                        winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
                        GetElementVisual(info->emptyStateIcon);
                    iconVisual.StopAnimation(L"Scale");
                    iconVisual.Scale(
                        winrt::Windows::Foundation::Numerics::float3{
                            1.0f, 1.0f, 1.0f });
                }

                ShowWindow(
                    emptyWindow,
                    showEmptyState ? SW_SHOWNA : SW_HIDE);
            }

            info->emptyStateVisible = showEmptyState;

            if (showEmptyState)
            {
                SetWindowPos(
                    emptyWindow,
                    HWND_TOP,
                    0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE |
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }

        if (info->settingsSource && g_settingsOpen)
        {
            int panelWidth = min(DipToPx(window, SettingsPanelWidth), width);

            info->settingsSource.SiteBridge().MoveAndResize(
                { max(0, width - panelWidth), 0, panelWidth, height });
        }

        if (info->mediaInfoSource && g_mediaInfoOpen)
        {
            int panelWidth = min(DipToPx(window, MediaInfoPanelWidth), width);

            info->mediaInfoSource.SiteBridge().MoveAndResize(
                { 0, 0, panelWidth, height });
        }

        if (info->playlistSource && g_playlistOpen)
        {
            int panelWidth = min(DipToPx(window, PlaylistPanelWidth), width);

            info->playlistSource.SiteBridge().MoveAndResize(
                { 0, 0, panelWidth, height });
        }

        if (g_pipBottomResizeWindow)
        {
            constexpr int bottomGripHeight = 9;
            MoveWindow(g_pipBottomResizeWindow, 0,
                max(0, height - bottomGripHeight), width,
                min(bottomGripHeight, height), FALSE);
            // Native owner-window hit-testing now owns every resize edge/corner.
            // Keep the legacy 9 px helper hidden so it cannot leave a visible strip.
            bool showBottomGrip = false;
            ShowWindow(g_pipBottomResizeWindow,
                showBottomGrip ? SW_SHOWNA : SW_HIDE);
            if (showBottomGrip)
            {
                SetWindowPos(g_pipBottomResizeWindow, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }

        if (g_borderlessCaptionWindow)
        {
            UINT dpi = GetDpiForWindow(window);
            int captionButtonWidth = MulDiv(46, static_cast<int>(dpi), 96);
            int captionHeight = MulDiv(32, static_cast<int>(dpi), 96);
            int captionWidth = captionButtonWidth * 3;
            // Keep the caption strip rectangular. GDI regions are not
            // antialiased and made the outer corners visibly jagged.
            SetWindowRgn(g_borderlessCaptionWindow, nullptr, TRUE);
            MoveWindow(g_borderlessCaptionWindow,
                max(0, width - captionWidth), 0,
                min(captionWidth, width), min(captionHeight, height), FALSE);
            // True borderless presentation: no permanent caption strip.
            // B restores the normal window; Alt+F4 remains the system close path.
            bool showCaptionControls = false;
            ShowWindow(g_borderlessCaptionWindow,
                showCaptionControls ? SW_SHOWNA : SW_HIDE);
            if (showCaptionControls)
            {
                SetWindowPos(g_borderlessCaptionWindow, HWND_TOP,
                    0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                    SWP_SHOWWINDOW);
            }
        }
    }

    void RefreshCurrentClientLayout()
    {
        if (!g_mainWindow)
        {
            return;
        }

        auto* info = reinterpret_cast<WindowInfo*>(
            GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
        if (!info)
        {
            return;
        }

        RECT client{};
        GetClientRect(g_mainWindow, &client);
        ApplyClientLayout(
            g_mainWindow,
            info,
            client.right - client.left,
            client.bottom - client.top);
    }

    std::string ConfiguredD3D11SyncInterval()
    {
        auto native = g_mpvSettingsManager.Overrides().find("d3d11-sync-interval");
        if (native != g_mpvSettingsManager.Overrides().end()) return native->second;

        for (auto option = g_mpvSettingsManager.ImportedOptions().rbegin();
            option != g_mpvSettingsManager.ImportedOptions().rend(); ++option)
        {
            if (_wcsicmp(option->name.c_str(), L"d3d11-sync-interval") == 0)
            {
                return winrt::to_string(option->value);
            }
        }
        return "1";
    }

    enum PipResizeEdge
    {
        PipResizeNone = 0,
        PipResizeLeft = 1,
        PipResizeRight = 2,
        PipResizeTop = 4,
        PipResizeBottom = 8
    };

    int PipResizeEdgesAt(POINT screenPoint)
    {
        if (!g_pictureInPicture || !g_mainWindow) return PipResizeNone;
        RECT bounds{};
        if (!GetWindowRect(g_mainWindow, &bounds)) return PipResizeNone;
        constexpr int grip = 10;
        int edges = PipResizeNone;
        if (screenPoint.x < bounds.left + grip) edges |= PipResizeLeft;
        else if (screenPoint.x >= bounds.right - grip) edges |= PipResizeRight;
        if (screenPoint.y < bounds.top + grip) edges |= PipResizeTop;
        else if (screenPoint.y >= bounds.bottom - grip) edges |= PipResizeBottom;
        return edges;
    }

    HCURSOR PipResizeCursor(int edges)
    {
        if (edges == (PipResizeLeft | PipResizeTop) ||
            edges == (PipResizeRight | PipResizeBottom))
            return LoadCursorW(nullptr, IDC_SIZENWSE);
        if (edges == (PipResizeRight | PipResizeTop) ||
            edges == (PipResizeLeft | PipResizeBottom))
            return LoadCursorW(nullptr, IDC_SIZENESW);
        if (edges & (PipResizeLeft | PipResizeRight))
            return LoadCursorW(nullptr, IDC_SIZEWE);
        if (edges & (PipResizeTop | PipResizeBottom))
            return LoadCursorW(nullptr, IDC_SIZENS);
        return LoadCursorW(nullptr, IDC_ARROW);
    }

    LRESULT PipHitTest(int edges)
    {
        if ((edges & PipResizeLeft) && (edges & PipResizeTop)) return HTTOPLEFT;
        if ((edges & PipResizeRight) && (edges & PipResizeTop)) return HTTOPRIGHT;
        if ((edges & PipResizeLeft) && (edges & PipResizeBottom)) return HTBOTTOMLEFT;
        if ((edges & PipResizeRight) && (edges & PipResizeBottom)) return HTBOTTOMRIGHT;
        if (edges & PipResizeLeft) return HTLEFT;
        if (edges & PipResizeRight) return HTRIGHT;
        if (edges & PipResizeTop) return HTTOP;
        if (edges & PipResizeBottom) return HTBOTTOM;
        return HTCLIENT;
    }

    int BorderlessResizeEdgesAt(POINT screenPoint)
    {
        if (!g_borderless || !g_mainWindow || IsZoomed(g_mainWindow))
            return PipResizeNone;
        RECT bounds{};
        if (!GetWindowRect(g_mainWindow, &bounds)) return PipResizeNone;
        UINT dpi = GetDpiForWindow(g_mainWindow);
        int grip = (std::max)(6, MulDiv(8, static_cast<int>(dpi), 96));
        int edges = PipResizeNone;
        if (screenPoint.x < bounds.left + grip) edges |= PipResizeLeft;
        else if (screenPoint.x >= bounds.right - grip) edges |= PipResizeRight;
        if (screenPoint.y < bounds.top + grip) edges |= PipResizeTop;
        else if (screenPoint.y >= bounds.bottom - grip) edges |= PipResizeBottom;
        return edges;
    }

    bool CanStartVideoWindowDrag(HWND source)
    {
        if (!g_mainWindow || g_fullscreen || g_pictureInPicture)
            return false;

        // Borderless mode already supports the pending-drag path from both
        // video and transport children. In a regular bordered window, extend
        // it only to the actual video child so buttons/sliders remain normal.
        return g_borderless || source == g_videoWindow;
    }

    void ArmBorderlessDrag(HWND source)
    {
        if (!CanStartVideoWindowDrag(source)) return;
        g_borderlessDragSource = source;
        GetCursorPos(&g_borderlessDragStart);
    }

    void CancelBorderlessDrag(HWND source = nullptr)
    {
        if (!source || source == g_borderlessDragSource)
            g_borderlessDragSource = nullptr;
    }

    bool StartBorderlessDragIfNeeded(HWND source, WPARAM mouseState)
    {
        if (!CanStartVideoWindowDrag(source) ||
            g_borderlessDragSource != source ||
            !(mouseState & MK_LBUTTON) || !g_mainWindow)
        {
            if (!(mouseState & MK_LBUTTON)) CancelBorderlessDrag(source);
            return false;
        }
        POINT cursor{};
        if (!GetCursorPos(&cursor)) return false;
        int thresholdX = (std::max)(2, GetSystemMetrics(SM_CXDRAG) / 2);
        int thresholdY = (std::max)(2, GetSystemMetrics(SM_CYDRAG) / 2);
        if (abs(cursor.x - g_borderlessDragStart.x) < thresholdX &&
            abs(cursor.y - g_borderlessDragStart.y) < thresholdY)
        {
            return false;
        }

        g_borderlessDragSource = nullptr;
        ReleaseCapture();
        SendMessageW(g_mainWindow, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return true;
    }

    bool BeginPipResize(HWND captureWindow)
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        int edges = PipResizeEdgesAt(cursor);
        if (edges == PipResizeNone) return false;
        g_pipResizeEdges = edges;
        g_pipResizeStart = cursor;
        GetWindowRect(g_mainWindow, &g_pipResizeWindowStart);
        g_pipResizeCaptureWindow = captureWindow;
        SetCapture(captureWindow);
        return true;
    }

    bool UpdatePipResize()
    {
        if (!g_pipResizeEdges || !g_mainWindow) return false;
        POINT cursor{};
        GetCursorPos(&cursor);
        int dx = cursor.x - g_pipResizeStart.x;
        int dy = cursor.y - g_pipResizeStart.y;
        RECT target = g_pipResizeWindowStart;
        if (g_pipResizeEdges & PipResizeLeft) target.left += dx;
        if (g_pipResizeEdges & PipResizeRight) target.right += dx;
        if (g_pipResizeEdges & PipResizeTop) target.top += dy;
        if (g_pipResizeEdges & PipResizeBottom) target.bottom += dy;
        constexpr int minimumWidth = 320;
        constexpr int minimumHeight = 220;
        if (target.right - target.left < minimumWidth)
        {
            if (g_pipResizeEdges & PipResizeLeft)
                target.left = target.right - minimumWidth;
            else target.right = target.left + minimumWidth;
        }
        if (target.bottom - target.top < minimumHeight)
        {
            if (g_pipResizeEdges & PipResizeTop)
                target.top = target.bottom - minimumHeight;
            else target.bottom = target.top + minimumHeight;
        }
        SetWindowPos(g_mainWindow, nullptr, target.left, target.top,
            target.right - target.left, target.bottom - target.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }

    void EndPipResize()
    {
        if (!g_pipResizeEdges) return;
        g_pipResizeEdges = PipResizeNone;
        g_pipResizeCaptureWindow = nullptr;
        if (GetCapture()) ReleaseCapture();
    }
}

LRESULT CALLBACK PipResizeGripProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_SETCURSOR:
        if (g_pictureInPicture)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            SetCursor(PipResizeCursor(PipResizeEdgesAt(cursor)));
            return TRUE;
        }
        if (g_borderless)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            SetCursor(PipResizeCursor(BorderlessResizeEdgesAt(cursor)));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        if (g_pictureInPicture && BeginPipResize(window)) return 0;
        if (g_borderless && g_mainWindow)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            int edges = BorderlessResizeEdgesAt(cursor);
            if (edges == PipResizeNone) edges = PipResizeBottom;
            ReleaseCapture();
            SendMessageW(g_mainWindow, WM_NCLBUTTONDOWN,
                PipHitTest(edges), 0);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (UpdatePipResize()) return 0;
        break;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        EndPipResize();
        return 0;
    case WM_NCHITTEST:
        // This invisible child exists specifically to own the bottom strip;
        // never let hit-testing fall through to the XAML transport island.
        return HTCLIENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK BorderlessCaptionProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto buttonAt = [window](LPARAM point) -> int
        {
            RECT client{};
            GetClientRect(window, &client);
            int x = static_cast<short>(LOWORD(point));
            int y = static_cast<short>(HIWORD(point));
            if (x < 0 || y < 0 || x >= client.right || y >= client.bottom)
                return -1;
            return (std::min)(2, static_cast<int>(
                x * 3 / (std::max)(1L, client.right)));
        };

    switch (message)
    {
    case WM_NCHITTEST:
    {
        POINT cursor{
            static_cast<short>(LOWORD(lParam)),
            static_cast<short>(HIWORD(lParam)) };
        // Keep the real top/right sizing strips available through this child.
        if (!g_borderless ||
            BorderlessResizeEdgesAt(cursor) != PipResizeNone)
        {
            return HTTRANSPARENT;
        }
        return HTCLIENT;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    case WM_MOUSEMOVE:
    {
        int hotButton = buttonAt(lParam);
        if (hotButton != g_captionHotButton)
        {
            g_captionHotButton = hotButton;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_captionHotButton = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        g_captionPressedButton = buttonAt(lParam);
        SetCapture(window);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        int releasedButton = buttonAt(lParam);
        int pressedButton = g_captionPressedButton;
        g_captionPressedButton = -1;
        if (GetCapture() == window) ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        if (releasedButton == pressedButton && g_borderless)
        {
            if (releasedButton == 0)
                ShowWindow(g_mainWindow, SW_MINIMIZE);
            else if (releasedButton == 1)
            {
                ShowWindow(g_mainWindow,
                    IsZoomed(g_mainWindow) ? SW_RESTORE : SW_MAXIMIZE);
                InvalidateRect(g_borderlessCaptionWindow, nullptr, FALSE);
            }
            else if (releasedButton == 2)
                PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        g_captionPressedButton = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        // Use a flat Windows 11-style caption strip. A rectangular child
        // avoids the aliased edges produced by GDI window regions.
        COLORREF surface = g_lightTheme
            ? RGB(249, 249, 249) : RGB(31, 31, 31);
        HBRUSH surfaceBrush = CreateSolidBrush(surface);
        FillRect(dc, &client, surfaceBrush);
        DeleteObject(surfaceBrush);

        UINT dpi = GetDpiForWindow(window);
        int fontHeight = -MulDiv(10, static_cast<int>(dpi), 96);
        HFONT iconFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe Fluent Icons");
        HGDIOBJ previousFont = SelectObject(dc, iconFont);
        int previousBkMode = SetBkMode(dc, TRANSPARENT);
        COLORREF previousTextColor = GetTextColor(dc);

        int buttonWidth = client.right / 3;
        for (int button = 0; button < 3; ++button)
        {
            RECT bounds{ button * buttonWidth, 0,
                button == 2 ? client.right : (button + 1) * buttonWidth,
                client.bottom };
            bool hot = g_captionHotButton == button;
            bool pressed = g_captionPressedButton == button;
            if (hot || pressed)
            {
                COLORREF fill = button == 2
                    ? (pressed ? RGB(153, 27, 18) : RGB(196, 43, 28))
                    : (g_lightTheme
                        ? (pressed ? RGB(205, 205, 205) : RGB(229, 229, 229))
                        : (pressed ? RGB(74, 76, 81) : RGB(57, 60, 65)));
                HBRUSH hoverBrush = CreateSolidBrush(fill);
                FillRect(dc, &bounds, hoverBrush);
                DeleteObject(hoverBrush);
            }

            bool closeHighlighted = button == 2 && (hot || pressed);
            SetTextColor(dc, closeHighlighted || !g_lightTheme
                ? RGB(250, 250, 250) : RGB(28, 28, 28));
            wchar_t glyph = button == 0 ? L'\xE921'
                : button == 1
                ? (IsZoomed(g_mainWindow) ? L'\xE923' : L'\xE922')
                : L'\xE8BB';
            DrawTextW(dc, &glyph, 1, &bounds,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SetTextColor(dc, previousTextColor);
        SetBkMode(dc, previousBkMode);
        SelectObject(dc, previousFont);
        DeleteObject(iconFont);
        EndPaint(window, &paint);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

ATOM RegisterMainWindowClass(HINSTANCE instance);
BOOL CreateMainWindow(HINSTANCE instance, int showCommand);
LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK VideoWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK TransportHostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
bool RestartEnginePreservingPlayback();

std::wstring MpvKeyName(MSG const& message)
{
    std::wstring key;
    bool special = true;
    switch (message.wParam)
    {
    case VK_SPACE: key = L"SPACE"; break;
    case VK_RETURN: key = L"ENTER"; break;
    case VK_ESCAPE: key = L"ESC"; break;
    case VK_BACK: key = L"BS"; break;
    case VK_TAB: key = L"TAB"; break;
    case VK_DELETE: key = L"DEL"; break;
    case VK_INSERT: key = L"INS"; break;
    case VK_HOME: key = L"HOME"; break;
    case VK_END: key = L"END"; break;
    case VK_PRIOR: key = L"PGUP"; break;
    case VK_NEXT: key = L"PGDWN"; break;
    case VK_LEFT: key = L"LEFT"; break;
    case VK_RIGHT: key = L"RIGHT"; break;
    case VK_UP: key = L"UP"; break;
    case VK_DOWN: key = L"DOWN"; break;
    case VK_NUMPAD0: case VK_NUMPAD1: case VK_NUMPAD2: case VK_NUMPAD3:
    case VK_NUMPAD4: case VK_NUMPAD5: case VK_NUMPAD6: case VK_NUMPAD7:
    case VK_NUMPAD8: case VK_NUMPAD9:
        key = L"KP" + std::to_wstring(message.wParam - VK_NUMPAD0); break;
    case VK_F1: case VK_F2: case VK_F3: case VK_F4: case VK_F5: case VK_F6:
    case VK_F7: case VK_F8: case VK_F9: case VK_F10: case VK_F11: case VK_F12:
    case VK_F13: case VK_F14: case VK_F15: case VK_F16: case VK_F17: case VK_F18:
    case VK_F19: case VK_F20: case VK_F21: case VK_F22: case VK_F23: case VK_F24:
        key = L"F" + std::to_wstring(message.wParam - VK_F1 + 1); break;
    default:
        special = false;
        break;
    }

    if (!special)
    {
        BYTE state[256]{};
        GetKeyboardState(state);
        // Ctrl/Alt produce control characters; mpv wants the printable key
        // with modifiers written as prefixes.
        state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;
        state[VK_MENU] = state[VK_LMENU] = state[VK_RMENU] = 0;
        wchar_t text[8]{};
        int count = ToUnicodeEx(
            static_cast<UINT>(message.wParam),
            static_cast<UINT>((message.lParam >> 16) & 0xff),
            state, text, ARRAYSIZE(text), 0, GetKeyboardLayout(0));
        if (count <= 0) return {};
        key.assign(text, text + count);
    }

    std::wstring modifiers;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers += L"Ctrl+";
    if (GetKeyState(VK_MENU) & 0x8000) modifiers += L"Alt+";
    if (special && (GetKeyState(VK_SHIFT) & 0x8000)) modifiers += L"Shift+";
    return modifiers + key;
}

void StopPlaybackAndClearUi()
{
    // Persist already-resolved web metadata before mpv unloads the item. This
    // changes only the Recentes display label; the stored URL remains intact.
    CaptureCurrentRecentTitle();

    // An explicit Stop means the user is done with this item. Do not leave an
    // older continuation point that would unexpectedly resume next time.
    if (!g_currentMediaPath.empty())
    {
        RemoveResumePoint(g_currentMediaPath);
    }

    // mpv's "stop" command unloads the current file and clears its playlist
    // while idle=yes keeps the embedded engine alive for the next Open.
    if (g_mpv.handle && g_mpv.command)
    {
        const char* stopCommand[] = { "stop", nullptr };
        g_mpv.command(g_mpv.handle, stopCommand);

        // Defensive cleanup for the same runtime restore point that can be
        // created by RestartEnginePreservingPlayback().
        if (g_mpv.setProperty)
        {
            g_mpv.setProperty(g_mpv.handle, "start", "none");
        }
    }

    g_currentMediaPath.clear();
    g_currentMediaIsDisc = false;
    g_currentDiscIsBluray = false;
    g_shufflePlayback = false;

    if (g_mainWindow)
    {
        auto* info = reinterpret_cast<WindowInfo*>(
            GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));

        if (info && info->page)
        {
            winrt::get_self<
                winrt::HCPlayer::implementation::MainPage>(
                    info->page)->ClearPlayerState();
        }
    }
    RefreshCurrentClientLayout();

    // ProgressTimerTick stops polling once MainPage becomes not-ready, so clear
    // these native surfaces immediately rather than waiting for another tick.
    PlayerUpdateTaskbarProgress();
    PlayerUpdateWebBufferingIndicator();
}

bool HandlePlayerKeyMessage(MSG const& message)
{
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN)
    {
        return false;
    }

    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    // Side panels normally suppress all global player shortcuts so focused
    // XAML controls keep ownership of their keyboard input.  The two exact
    // panel chords are the deliberate exception: process them before that
    // guard so the same shortcut that opened a panel can close it again.
    // Context menus retain the old behavior and block both chords.
    if (!g_contextMenuOpen)
    {
        if (message.wParam == VK_OEM_COMMA && ctrl && !alt && !shift)
        {
            bool const repeated = (message.lParam & (1LL << 30)) != 0;
            if (!repeated)
            {
                if (g_settingsOpen) PlayerCloseSettings();
                else PlayerShowSettings();
            }
            return true;
        }

        if (message.wParam == 'I' && ctrl && !alt && !shift)
        {
            bool const repeated = (message.lParam & (1LL << 30)) != 0;
            if (!repeated)
            {
                if (g_mediaInfoOpen) PlayerCloseMediaInfo();
                else if (!g_currentMediaPath.empty()) PlayerShowMediaInfo();
            }
            return true;
        }


        // Ctrl+Shift+P owns the visual playback queue. Keep Ctrl+P reserved
        // for Picture-in-Picture and process this chord before the side-panel
        // guard so the same shortcut can close the queue again.
        if (message.wParam == 'P' && ctrl && !alt && shift)
        {
            bool const repeated = (message.lParam & (1LL << 30)) != 0;
            if (!repeated)
            {
                if (g_playlistOpen) PlayerClosePlaylist();
                else PlayerShowPlaylist();
            }
            return true;
        }
    }

    if (IsSidePanelOpen() || g_contextMenuOpen)
    {
        return false;
    }

    if (message.wParam == VK_ESCAPE && g_fullscreen)
    {
        // Keyboard fullscreen exit is immersive too: keep the transport hidden
        // until the user physically moves the pointer back into its hot zone.
        g_suppressFullscreenEntryTransportReveal = true;
        PlayerToggleFullscreen();
        return true;
    }
    if (message.wParam == VK_RETURN && (!ctrl || alt))
    {
        // Enter owns an immersive transition in both directions. The toolbar
        // button does not set this one-shot flag and therefore keeps its normal
        // transport reveal behavior.
        g_suppressFullscreenEntryTransportReveal = true;
        PlayerToggleFullscreen();
        return true;
    }
    if (message.wParam == 'O' && ctrl && !alt)
    {
        if (shift) PlayerShowOpenFolderDialog();
        else PlayerShowOpenDialog();
        return true;
    }
    if (message.wParam == 'V' && ctrl && !alt)
    {
        PlayerOpenClipboardMedia();
        return true;
    }

    // HC Player owns the exact Ctrl+P chord as the keyboard equivalent of the
    // existing PiP toolbar button. Ignore key-repeat so holding the chord
    // cannot bounce repeatedly between normal and Picture-in-Picture modes.
    if (message.wParam == 'P' && ctrl && !alt && !shift)
    {
        bool const repeated = (message.lParam & (1LL << 30)) != 0;
        if (!repeated && !g_currentMediaPath.empty())
        {
            PlayerTogglePictureInPicture();
        }
        return true;
    }

    // The bundled default-input.conf deliberately leaves i/I unused, so this
    // can become the HC Player statistics shortcut without stealing an
    // existing playback command. Both keyboard cases map to the same VK_I.
    if (message.wParam == 'I' && !ctrl && !alt)
    {
        PlayerToggleStats();
        return true;
    }

    if (message.wParam == 'S' && ctrl && !alt && !shift)
    {
        StopPlaybackAndClearUi();
        return true;
    }
    if (message.wParam == 'T' && ctrl)
    {
        PlayerToggleAlwaysOnTop();
        return true;
    }
    if (message.wParam == 'B' && !ctrl && !alt)
    {
        // Win32 reports both b and B as VK_B. Preserve the Shift distinction
        // explicitly instead of letting the unshifted host shortcut swallow
        // mpv's uppercase B binding for HDR target peak.
        if (shift) PlayerSendMpvKey(L"B");
        else PlayerToggleBorderless();
        return true;
    }
    if (message.wParam == VK_OEM_3 && !ctrl && !alt && !shift)
    {
        PlayerExecuteMpvCommand(L"script-binding console/enable");
        return true;
    }
    if (!ctrl && !alt && message.wParam >= VK_F1 && message.wParam <= VK_F12)
    {
        auto profiles = PlayerGetImportedProfileNames();
        size_t profileIndex = static_cast<size_t>(message.wParam - VK_F1);
        if (profileIndex < profiles.size())
        {
            PlayerApplyImportedProfile(profiles[profileIndex]);
            return true;
        }
    }

    std::wstring key = MpvKeyName(message);
    if (key.empty()) return false;
    PlayerSendMpvKey(key);

    if (message.wParam == 'Q' && !ctrl && !alt)
    {
        PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
    }
    return true;
}

LRESULT CALLBACK VideoWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_NCHITTEST:
        if (g_pictureInPicture || g_borderless)
        {
            POINT cursor{
                static_cast<short>(LOWORD(lParam)),
                static_cast<short>(HIWORD(lParam))
            };

            int const resizeEdges = g_pictureInPicture
                ? PipResizeEdgesAt(cursor)
                : BorderlessResizeEdgesAt(cursor);

            // Let the owner window handle native resize edges/corners instead
            // of allowing the embedded video child to block them.
            if (resizeEdges != PipResizeNone)
            {
                return HTTRANSPARENT;
            }
        }
        break;

    case WM_SETCURSOR:
        if (g_pictureInPicture)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            int edges = PipResizeEdgesAt(cursor);
            if (edges != PipResizeNone)
            {
                SetCursor(PipResizeCursor(edges));
                return TRUE;
            }
        }
        else if (g_borderless)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            int edges = BorderlessResizeEdgesAt(cursor);
            if (edges != PipResizeNone)
            {
                SetCursor(PipResizeCursor(edges));
                return TRUE;
            }
        }
        break;
    case WM_MOUSEMOVE:
    {
        UpdateVideoClickCandidateForMovement(wParam);
        if (UpdatePipResize()) return 0;
        if (StartBorderlessDragIfNeeded(window, wParam)) return 0;
        POINT physicalCursor{};
        if (!GetCursorPos(&physicalCursor)) break;
        if (g_hasLastVideoMouseScreenPoint &&
            physicalCursor.x == g_lastVideoMouseScreenPoint.x &&
            physicalCursor.y == g_lastVideoMouseScreenPoint.y)
        {
            // Hiding/showing the XAML island changes the child layout and can
            // synthesize WM_MOUSEMOVE even though the mouse never moved. Do
            // not treat that compositor reflow as fresh user activity.
            break;
        }
        g_lastVideoMouseScreenPoint = physicalCursor;
        g_hasLastVideoMouseScreenPoint = true;
        if (g_mainWindow)
        {
            POINT point{
                static_cast<short>(LOWORD(lParam)),
                static_cast<short>(HIWORD(lParam)) };
            ClientToScreen(window, &point);
            ScreenToClient(g_mainWindow, &point);
            RECT client{};
            GetClientRect(g_mainWindow, &client);
            auto* info = reinterpret_cast<WindowInfo*>(
                GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
            if (info && info->page)
            {
                auto* page = winrt::get_self<
                    winrt::HCPlayer::implementation::MainPage>(info->page);
                if (!IsSidePanelOpen())
                {
                    page->TransportVideoPointerMoved(
                        point.y >= client.bottom - CurrentControlsHeightPx(g_mainWindow));
                }
            }
        }
        break;
    }
    case WM_ERASEBKGND:
    {
        // libmpv normally covers the complete child with its swap chain. A
        // synchronous black erase prevents the class/background colour from
        // leaking through between swap-chain resizes.
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    case WM_LBUTTONDOWN:
        SetFocus(g_mainWindow);
        if (g_pictureInPicture && g_mainWindow)
        {
            // PiP preserves its current direct-drag interaction.
            g_videoClickCandidate = false;
            if (BeginPipResize(window)) return 0;
            ReleaseCapture();
            SendMessageW(g_mainWindow, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        if (g_borderless && g_mainWindow)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            int edges = BorderlessResizeEdgesAt(cursor);
            if (edges != PipResizeNone)
            {
                g_videoClickCandidate = false;
                ReleaseCapture();
                SendMessageW(g_mainWindow, WM_NCLBUTTONDOWN,
                    PipHitTest(edges), 0);
                return 0;
            }

            ArmVideoClickCandidate();

            // A short threshold preserves ordinary clicks and double-click
            // fullscreen while allowing a drag to begin anywhere on video.
            ArmBorderlessDrag(window);
            return 0;
        }

        ArmVideoClickCandidate();

        if (g_mainWindow && !g_fullscreen)
        {
            // Keep the native title bar fully functional, but also allow the
            // user to grab the regular bordered window directly from the video.
            // The same drag threshold preserves normal clicks/double-clicks.
            ArmBorderlessDrag(window);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
    {
        CancelBorderlessDrag(window);
        EndPipResize();

        if (g_suppressNextVideoClickUp)
        {
            g_suppressNextVideoClickUp = false;
            g_videoClickCandidate = false;
            break;
        }

        bool const scheduleSingleClick =
            g_videoClickCandidate &&
            UiToggleEnabled("ui-click-video-play-pause", false) &&
            !IsSidePanelOpen() &&
            g_mainWindow;

        g_videoClickCandidate = false;

        if (scheduleSingleClick)
        {
            KillTimer(g_mainWindow, VideoSingleClickTimer);
            // Keep single-click feeling immediate while still leaving a
            // short window for an ordinary double-click to cancel Play/Pause.
            constexpr UINT VideoSingleClickDelayMs = 170;
            SetTimer(
                g_mainWindow,
                VideoSingleClickTimer,
                VideoSingleClickDelayMs,
                nullptr);
        }
        break;
    }
    case WM_CAPTURECHANGED:
        g_videoClickCandidate = false;
        CancelBorderlessDrag(window);
        EndPipResize();
        break;
    case WM_LBUTTONDBLCLK:
        g_videoClickCandidate = false;
        g_suppressNextVideoClickUp = true;
        if (g_mainWindow)
            KillTimer(g_mainWindow, VideoSingleClickTimer);
        CancelBorderlessDrag(window);
        SetFocus(g_mainWindow);
        // Double-click is immersive in both directions. Suppress the automatic
        // transport reveal on fullscreen entry and on restore to windowed mode.
        g_suppressFullscreenEntryTransportReveal = true;
        PlayerToggleFullscreen();
        return 0;
    case WM_CONTEXTMENU:
    {
        if (g_pictureInPicture) return 0;
        POINT point{
            static_cast<short>(LOWORD(lParam)),
            static_cast<short>(HIWORD(lParam)) };
        if (point.x == -1 && point.y == -1)
        {
            RECT client{};
            GetClientRect(window, &client);
            point = { client.left + 24, client.top + 24 };
            ClientToScreen(window, &point);
        }
        POINT clientPoint = point;
        ScreenToClient(g_mainWindow, &clientPoint);
        PostMessageW(g_mainWindow, ShowContextMenuMessage, 0,
            MAKELPARAM(clientPoint.x, clientPoint.y));
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK SettingsHostWindowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
    {
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH panelBrush = CreateSolidBrush(g_lightTheme
            ? RGB(245, 245, 245) : RGB(32, 35, 40));
        FillRect(reinterpret_cast<HDC>(wParam), &client, panelBrush);
        DeleteObject(panelBrush);
        return 1;
    }
    case WM_NCHITTEST:
        return HTCLIENT;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK TransportHostProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCACTIVATE:
        return TRUE;
    case WM_NCHITTEST:
        if (g_pictureInPicture || g_borderless)
        {
            POINT cursor{
                static_cast<short>(LOWORD(lParam)),
                static_cast<short>(HIWORD(lParam)) };
            int const edges = g_pictureInPicture
                ? PipResizeEdgesAt(cursor)
                : BorderlessResizeEdgesAt(cursor);
            if (edges != PipResizeNone) return HTTRANSPARENT;
        }
        return HTCLIENT;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

#if 0 // Playback indicator removed until its visual design is revisited.
LRESULT CALLBACK PlaybackIndicatorWindowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_TIMER:
        if (wParam == PlaybackIndicatorTimer)
        {
            constexpr double totalMilliseconds = 410.0;
            double elapsed = static_cast<double>(GetTickCount64() -
                g_playbackIndicatorStarted);
            if (elapsed >= totalMilliseconds || !g_mainWindow)
            {
                KillTimer(window, PlaybackIndicatorTimer);
                ShowWindow(window, SW_HIDE);
                return 0;
            }

            // A quick ease-out arrival, a readable hold, then a soft exit.
            double opacity{};
            double scale{};
            if (elapsed < 75.0)
            {
                double progress = elapsed / 75.0;
                double eased = 1.0 - std::pow(1.0 - progress, 3.0);
                opacity = 0.76 + 0.24 * eased;
                scale = 0.90 + 0.10 * eased;
            }
            else if (elapsed < 255.0)
            {
                opacity = 1.0;
                scale = 1.0;
            }
            else
            {
                double progress = (elapsed - 255.0) / 155.0;
                double eased = progress * progress;
                opacity = 1.0 - eased;
                scale = 1.0 + 0.035 * eased;
            }

            constexpr int finalSize = 74;
            int size = static_cast<int>(std::round(finalSize * scale));
            RECT client{};
            GetClientRect(g_mainWindow, &client);
            POINT origin{};
            ClientToScreen(g_mainWindow, &origin);
            int x = origin.x + (client.right - client.left - size) / 2;
            int y = origin.y + (client.bottom - client.top - size) / 2;
            SetWindowPos(window, HWND_TOP, x, y, size, size,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            HRGN region = CreateRoundRectRgn(0, 0, size + 1, size + 1,
                size / 2, size / 2);
            SetWindowRgn(window, region, TRUE);
            SetLayeredWindowAttributes(window, 0,
                static_cast<BYTE>(std::clamp(opacity, 0.0, 1.0) * 255.0),
                LWA_ALPHA);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        COLORREF iconColor = g_lightTheme
            ? RGB(24, 26, 30) : RGB(250, 250, 250);
        HBRUSH iconBrush = CreateSolidBrush(iconColor);
        HGDIOBJ oldBrush = SelectObject(dc, iconBrush);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));

        int width = client.right - client.left;
        int height = client.bottom - client.top;
        int centerX = width / 2;
        int centerY = height / 2;
        if (g_playbackIndicatorPlaying)
        {
            POINT triangle[] = {
                { centerX - 9, centerY - 15 },
                { centerX - 9, centerY + 15 },
                { centerX + 16, centerY }
            };
            Polygon(dc, triangle, ARRAYSIZE(triangle));
        }
        else
        {
            RoundRect(dc, centerX - 13, centerY - 15,
                centerX - 4, centerY + 15, 5, 5);
            RoundRect(dc, centerX + 4, centerY - 15,
                centerX + 13, centerY + 15, 5, 5);
        }
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(iconBrush);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_NCDESTROY:
        KillTimer(window, PlaybackIndicatorTimer);
        if (g_playbackIndicatorWindow == window)
            g_playbackIndicatorWindow = nullptr;
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowPlaybackIndicator(bool playing)
{
    if (!g_mainWindow || IsSidePanelOpen()) return;
    // A fresh tiny popup guarantees a clean transparent composition surface
    // for every state. Repainting a backdrop HWND with GDI can otherwise
    // leave the previous play/pause glyph underneath the new one.
    if (g_playbackIndicatorWindow)
    {
        DestroyWindow(g_playbackIndicatorWindow);
    }
    {
        g_playbackIndicatorWindow = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            PlaybackIndicatorClassName, nullptr, WS_POPUP,
            0, 0, 82, 82, g_mainWindow, nullptr, g_instance, nullptr);
        if (!g_playbackIndicatorWindow) return;

        BOOL dark = g_lightTheme ? FALSE : TRUE;
        DwmSetWindowAttribute(g_playbackIndicatorWindow,
            DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        // Use the native transient backdrop directly. Creating and destroying
        // a WinRT compositor for every click can race CoreMessaging and abort
        // the application while play/pause is being processed.
        DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TRANSIENTWINDOW;
        DwmSetWindowAttribute(g_playbackIndicatorWindow,
            DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
        DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(g_playbackIndicatorWindow,
            DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        MARGINS margins{ -1 };
        DwmExtendFrameIntoClientArea(g_playbackIndicatorWindow, &margins);
    }

    g_playbackIndicatorPlaying = playing;
    g_playbackIndicatorStarted = GetTickCount64();
    SetLayeredWindowAttributes(g_playbackIndicatorWindow, 0, 0, LWA_ALPHA);
    ShowWindow(g_playbackIndicatorWindow, SW_SHOWNOACTIVATE);
    SetTimer(g_playbackIndicatorWindow, PlaybackIndicatorTimer, 16, nullptr);
}
#endif

// The empty-state XAML island is deliberately non-interactive, but its native
// DesktopWindowXamlSource HWND still occupies the branding rectangle. Forward
// the few native interactions that belong to the player so the center of the
// idle screen behaves exactly like the surrounding video surface.
LRESULT CALLBACK EmptyStatePanelSubclassProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(referenceData);

    if (message == WM_CONTEXTMENU)
    {
        if (!g_mainWindow || g_pictureInPicture) return 0;

        POINT point{
            static_cast<short>(LOWORD(lParam)),
            static_cast<short>(HIWORD(lParam)) };

        // Keyboard context-menu requests do not carry a screen coordinate.
        // Anchor them near the upper-left of whichever island child received
        // the message, matching the native video-window behavior.
        if (point.x == -1 && point.y == -1)
        {
            RECT client{};
            GetClientRect(window, &client);
            point = { client.left + 24, client.top + 24 };
            ClientToScreen(window, &point);
        }

        POINT clientPoint = point;
        ScreenToClient(g_mainWindow, &clientPoint);
        PostMessageW(
            g_mainWindow,
            ShowContextMenuMessage,
            0,
            MAKELPARAM(clientPoint.x, clientPoint.y));
        return 0;
    }
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(
            window, EmptyStatePanelSubclassProc, subclassId);
    }

    return DefSubclassProc(window, message, wParam, lParam);
}

BOOL CALLBACK InstallEmptyStateChildSubclass(HWND child, LPARAM)
{
    // Register descendants too: DesktopWindowXamlSource may expose an internal
    // child HWND as the OLE hit-test target depending on the Windows App SDK
    // build. Using the existing MediaDropTarget keeps behavior identical.
    RegisterMediaDropTarget(child);
    SetWindowSubclass(child, EmptyStatePanelSubclassProc, 3, 0);
    return TRUE;
}

void InstallEmptyStateSubclasses(HWND root)
{
    if (!root) return;
    SetWindowSubclass(root, EmptyStatePanelSubclassProc, 3, 0);
    EnumChildWindows(root, InstallEmptyStateChildSubclass, 0);
}

LRESULT CALLBACK SettingsPanelSubclassProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData)
{
    if (message == WM_ERASEBKGND)
    {
        // DesktopWindowXamlSource can expose its native child HWND for one
        // compositor frame while SettingsRoot is translated. Keep the light
        // path exactly as before and give dark mode the exact SettingsSurface
        // color instead of the default black host.
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH surface = CreateSolidBrush(
            g_lightTheme
                ? RGB(245, 245, 245)
                : RGB(32, 35, 40));
        FillRect(reinterpret_cast<HDC>(wParam), &client, surface);
        DeleteObject(surface);
        return 1;
    }
    if (message == WM_MOUSEWHEEL)
    {
        auto* page = reinterpret_cast<winrt::HCPlayer::implementation::SettingsPage*>(referenceData);
        if (page)
        {
            page->ScrollBy(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
    }
    else if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(window, SettingsPanelSubclassProc, subclassId);
    }

    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK MediaInfoPanelSubclassProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData)
{
    if (message == WM_ERASEBKGND)
    {
        // The translated XAML content can expose its child HWND for a frame.
        // Paint that native host with the exact MediaInfo surface instead of
        // letting the dark-mode default black show through.
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH surface = CreateSolidBrush(
            g_lightTheme
                ? RGB(245, 245, 245)
                : RGB(32, 35, 40));
        FillRect(reinterpret_cast<HDC>(wParam), &client, surface);
        DeleteObject(surface);
        return 1;
    }

    auto* page = reinterpret_cast<
        winrt::HCPlayer::implementation::MediaInfoPage*>(
            referenceData);

    if (message == WM_MOUSEWHEEL)
    {
        if (page)
        {
            page->ScrollBy(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
    }
    else if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
    {
        if (page)
        {
            page->RequestClose();
            return 0;
        }
    }
    else if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(
            window,
            MediaInfoPanelSubclassProc,
            subclassId);
    }

    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK PlaylistPanelSubclassProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData)
{
    if (message == WM_ERASEBKGND)
    {
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH surface = CreateSolidBrush(
            g_lightTheme
                ? RGB(245, 245, 245)
                : RGB(32, 35, 40));
        FillRect(reinterpret_cast<HDC>(wParam), &client, surface);
        DeleteObject(surface);
        return 1;
    }

    auto* page = reinterpret_cast<
        winrt::HCPlayer::implementation::PlaylistPage*>(
            referenceData);

    if (message == WM_MOUSEWHEEL)
    {
        if (page)
        {
            page->ScrollBy(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
    }
    else if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
    {
        if (page)
        {
            page->RequestClose();
            return 0;
        }
    }
    else if (message == WM_NCDESTROY)
    {
        if (window == g_playlistDropWindow)
        {
            RevokeDragDrop(window);
            g_playlistDropWindow = nullptr;
        }
        RemoveWindowSubclass(
            window,
            PlaylistPanelSubclassProc,
            subclassId);
    }

    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK TransportPanelSubclassProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData)
{
    UNREFERENCED_PARAMETER(subclassId);
    auto* page = reinterpret_cast<
        winrt::HCPlayer::implementation::MainPage*>(referenceData);
    if (message == WM_SETFOCUS && g_pictureInPicture)
    {
        // Keep mouse interaction working, but never let the XAML island HWND
        // retain focus and draw its host-wide focus visual in PiP.
        if (g_mainWindow) SetFocus(g_mainWindow);
        return 0;
    }
    if (message == WM_CONTEXTMENU && g_pictureInPicture)
    {
        return 0;
    }
    if (message == WM_SETCURSOR && g_pictureInPicture)
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        int edges = PipResizeEdgesAt(cursor);
        if (edges != PipResizeNone)
        {
            SetCursor(PipResizeCursor(edges));
            return TRUE;
        }
    }
    else if (message == WM_SETCURSOR && g_borderless)
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        int edges = BorderlessResizeEdgesAt(cursor);
        if (edges != PipResizeNone)
        {
            SetCursor(PipResizeCursor(edges));
            return TRUE;
        }
    }
    else if (message == WM_LBUTTONDOWN && g_pictureInPicture &&
        BeginPipResize(window))
    {
        return 0;
    }
    else if (message == WM_LBUTTONDOWN && g_borderless)
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        int edges = BorderlessResizeEdgesAt(cursor);
        if (edges != PipResizeNone)
        {
            ReleaseCapture();
            SendMessageW(g_mainWindow, WM_NCLBUTTONDOWN,
                PipHitTest(edges), 0);
            return 0;
        }
        // Do not consume the down event: buttons and sliders still receive a
        // normal click. Movement beyond the drag threshold takes over instead.
        ArmBorderlessDrag(window);
    }
    else if (message == WM_MOUSEMOVE && UpdatePipResize())
    {
        return 0;
    }
    else if (message == WM_MOUSEMOVE &&
        StartBorderlessDragIfNeeded(window, wParam))
    {
        return 0;
    }
    else if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED)
    {
        // XAML may transfer capture to a pressed button/slider. Keep the
        // pending window drag alive while the physical button is still down.
        if (message == WM_LBUTTONUP || !(GetKeyState(VK_LBUTTON) & 0x8000))
            CancelBorderlessDrag(window);
        EndPipResize();
        if (g_pictureInPicture && g_mainWindow && message == WM_LBUTTONUP)
        {
            // WinUI may assign focus after the button's routed Click handler.
            // Release it on the next owner-window turn so the island-wide
            // focus outline cannot remain after the transport fades away.
            PostMessageW(g_mainWindow, ReleaseTransportFocusMessage, 0, 0);
        }
    }
    else if (message == WM_POINTERUP && g_pictureInPicture && g_mainWindow)
    {
        PostMessageW(g_mainWindow, ReleaseTransportFocusMessage, 0, 0);
    }
    else if (message == WM_MOUSEMOVE)
    {
        if (g_pictureInPicture || g_borderless)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            int resizeEdges = g_pictureInPicture
                ? PipResizeEdgesAt(cursor)
                : BorderlessResizeEdgesAt(cursor);
            if (resizeEdges != PipResizeNone)
            {
                // The outer strip belongs to resizing, not to the XAML hover
                // reveal. This is especially important along the bottom.
                return 0;
            }
        }
        TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
        TrackMouseEvent(&tracking);
        if (page) page->TransportHostPointerEntered();
    }
    else if (message == WM_MOUSELEAVE)
    {
        if (page) page->TransportHostPointerExited();
    }
    else if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(window, TransportPanelSubclassProc, subclassId);
    }
    else if (message == WM_NCHITTEST)
    {
        if (g_pictureInPicture || g_borderless)
        {
            POINT cursor{
                static_cast<short>(LOWORD(lParam)),
                static_cast<short>(HIWORD(lParam))
            };

            int const resizeEdges = g_pictureInPicture
                ? PipResizeEdgesAt(cursor)
                : BorderlessResizeEdgesAt(cursor);

            // Let the owner window own its resize edges even when the XAML
            // transport is covering the bottom of the client area.
            if (resizeEdges != PipResizeNone)
            {
                return HTTRANSPARENT;
            }
        }

        // Everywhere else the transport stays interactive normally.
        return HTCLIENT;
    }
    if (message == WM_ERASEBKGND)
    {
        if (g_pictureInPicture)
        {
            // The PiP transport is a rounded, partially transparent XAML
            // island. Painting the rectangular host background after a click
            // exposes that paint around the rounded panel as a grey frame.
            // The compositor already supplies every visible PiP pixel, so mark
            // the erase as handled without filling the host.
            return 1;
        }
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(g_lightTheme
            ? RGB(247, 247, 247) : RGB(32, 35, 40));
        FillRect(reinterpret_cast<HDC>(wParam), &client, background);
        DeleteObject(background);
        return 1;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

BOOL CALLBACK InstallTransportChildSubclass(HWND child, LPARAM referenceData)
{
    SetWindowSubclass(child, TransportPanelSubclassProc, 2,
        static_cast<DWORD_PTR>(referenceData));
    return TRUE;
}

void InstallTransportSubclasses(HWND root, DWORD_PTR referenceData)
{
    if (!root) return;
    SetWindowSubclass(root, TransportPanelSubclassProc, 2, referenceData);
    EnumChildWindows(root, InstallTransportChildSubclass,
        static_cast<LPARAM>(referenceData));
}

LRESULT CALLBACK FullscreenTransitionShieldProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
    {
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        FillRect(dc, &paint.rcPaint,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        EndPaint(window, &paint);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK InitialMediaRevealShieldProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_NCHITTEST:
        // Presentation-only cover: never consume pointer input while the
        // player is becoming ready underneath it.
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
    {
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        EndPaint(window, &paint);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK MediaFullscreenTransitionShieldProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
        // WM_PAINT always covers the complete monitor-sized bridge.
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        auto* snapshot = reinterpret_cast<FullscreenTransitionSnapshot*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (snapshot && snapshot->bitmap &&
            snapshot->width > 0 && snapshot->height > 0)
        {
            HDC memoryDc = CreateCompatibleDC(dc);
            if (memoryDc)
            {
                HGDIOBJ previous = SelectObject(memoryDc, snapshot->bitmap);
                int const targetWidth = client.right - client.left;
                int const targetHeight = client.bottom - client.top;
                int const sourceX = snapshot->sourceWidth > 0
                    ? snapshot->sourceX : 0;
                int const sourceY = snapshot->sourceHeight > 0
                    ? snapshot->sourceY : 0;
                int const sourceWidth = snapshot->sourceWidth > 0
                    ? snapshot->sourceWidth : snapshot->width;
                int const sourceHeight = snapshot->sourceHeight > 0
                    ? snapshot->sourceHeight : snapshot->height;

                // The stable 34.20.8.16 behavior remains the fallback. For the
                // tightly-scoped 16:9 -> 16:9 case, sourceX/Y/Width/Height crop
                // only the old window's keepaspect margins from the frozen
                // bridge. The live mpv surface is never cropped or stretched by
                // HC Player.
                // 34.20.8.20: this window is only a frozen visual bridge.
                // Use GDI's high-quality stretch mode so thin diagonal lines do
                // not become visibly jagged while the snapshot is enlarged to
                // fullscreen. This never changes the live mpv render surface.
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                StretchBlt(
                    dc, 0, 0, targetWidth, targetHeight,
                    memoryDc, sourceX, sourceY, sourceWidth, sourceHeight,
                    SRCCOPY);

                SelectObject(memoryDc, previous);
                DeleteDC(memoryDc);
            }
        }

        EndPaint(window, &paint);
        return 0;
    }
    case WM_NCDESTROY:
    {
        auto* snapshot = reinterpret_cast<FullscreenTransitionSnapshot*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (snapshot)
        {
            if (snapshot->bitmap) DeleteObject(snapshot->bitmap);
            delete snapshot;
        }
        break;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

FullscreenTransitionSnapshot* CaptureFullscreenTransitionVideoFrame()
{
    if (!g_videoWindow || !IsWindowVisible(g_videoWindow)) return nullptr;

    RECT sourceRect{};
    if (!GetWindowRect(g_videoWindow, &sourceRect)) return nullptr;
    int const width = sourceRect.right - sourceRect.left;
    int const height = sourceRect.bottom - sourceRect.top;
    if (width <= 0 || height <= 0) return nullptr;

    // Capture exactly what DWM is showing before the transition begins. This is
    // intentionally screen-side rather than PrintWindow(): libmpv's D3D child
    // is a hardware-presented surface and PrintWindow may return black.
    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return nullptr;

    HDC memoryDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = memoryDc
        ? CreateCompatibleBitmap(screenDc, width, height)
        : nullptr;
    bool captured{};

    if (memoryDc && bitmap)
    {
        HGDIOBJ previous = SelectObject(memoryDc, bitmap);
        captured = BitBlt(
            memoryDc, 0, 0, width, height,
            screenDc, sourceRect.left, sourceRect.top,
            SRCCOPY | CAPTUREBLT) != FALSE;
        SelectObject(memoryDc, previous);
    }

    if (memoryDc) DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);

    if (!captured)
    {
        if (bitmap) DeleteObject(bitmap);
        return nullptr;
    }

    auto* snapshot = new (std::nothrow) FullscreenTransitionSnapshot{};
    if (!snapshot)
    {
        DeleteObject(bitmap);
        return nullptr;
    }

    snapshot->bitmap = bitmap;
    snapshot->width = width;
    snapshot->height = height;
    snapshot->sourceWidth = width;
    snapshot->sourceHeight = height;
    return snapshot;
}

HWND ShowMediaFullscreenTransitionShield(
    RECT const& bounds, bool enteringFullscreen = false)
{
    // Fail-open design: if the temporary bridge cannot be created, do nothing
    // and run the frozen 34.20.8.13 fullscreen path exactly as before.
    FullscreenTransitionSnapshot* snapshot =
        CaptureFullscreenTransitionVideoFrame();
    if (!snapshot) return nullptr;

    if (enteringFullscreen && g_mpv.handle && g_mpv.getProperty)
    {
        int64_t videoWidth{};
        int64_t videoHeight{};
        bool const haveVideoAspect =
            g_mpv.getProperty(
                g_mpv.handle, "video-out-params/dw",
                MpvFormatInt64, &videoWidth) >= 0 &&
            g_mpv.getProperty(
                g_mpv.handle, "video-out-params/dh",
                MpvFormatInt64, &videoHeight) >= 0 &&
            videoWidth > 0 && videoHeight > 0;

        int const monitorWidth = bounds.right - bounds.left;
        int const monitorHeight = bounds.bottom - bounds.top;
        if (haveVideoAspect && monitorWidth > 0 && monitorHeight > 0)
        {
            double constexpr aspect169 = 16.0 / 9.0;
            double const videoAspect =
                static_cast<double>(videoWidth) /
                static_cast<double>(videoHeight);
            double const monitorAspect =
                static_cast<double>(monitorWidth) /
                static_cast<double>(monitorHeight);

            // Deliberately narrow scope: this experiment does not run for
            // 2.40:1, 4:3, portrait, 16:10 or any other aspect. A ~0.35%
            // tolerance only absorbs rounding/coded-size differences around
            // true 16:9 media and 16:9 displays.
            bool const matching169 =
                std::abs(videoAspect - aspect169) <= 0.006 &&
                std::abs(monitorAspect - aspect169) <= 0.006 &&
                std::abs(videoAspect - monitorAspect) <= 0.006;

            if (matching169)
            {
                snapshot->settleMatchingFullscreenVideo = true;
                snapshot->targetWidth = monitorWidth;
                snapshot->targetHeight = monitorHeight;

                // The screen-side capture contains the whole windowed video
                // child, including any keepaspect bars that belong only to the
                // old window shape. Crop the frozen bridge to the destination
                // 16:9 aspect. This affects only the temporary bitmap.
                double const sourceAspect =
                    static_cast<double>(snapshot->width) /
                    static_cast<double>(snapshot->height);
                if (sourceAspect > monitorAspect)
                {
                    int const croppedWidth = (std::max)(1, (std::min)(
                        snapshot->width, static_cast<int>(std::lround(
                            static_cast<double>(snapshot->height) *
                            monitorAspect))));
                    snapshot->sourceX =
                        (snapshot->width - croppedWidth) / 2;
                    snapshot->sourceWidth = croppedWidth;
                }
                else if (sourceAspect < monitorAspect)
                {
                    int const croppedHeight = (std::max)(1, (std::min)(
                        snapshot->height, static_cast<int>(std::lround(
                            static_cast<double>(snapshot->width) /
                            monitorAspect))));
                    snapshot->sourceY =
                        (snapshot->height - croppedHeight) / 2;
                    snapshot->sourceHeight = croppedHeight;
                }
            }
        }
    }

    HWND shield = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        MediaFullscreenTransitionShieldClassName,
        L"",
        WS_POPUP,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        nullptr,
        nullptr,
        g_instance,
        nullptr);

    if (!shield)
    {
        if (snapshot->bitmap) DeleteObject(snapshot->bitmap);
        delete snapshot;
        return nullptr;
    }

    SetWindowLongPtrW(
        shield, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(snapshot));
    SetWindowPos(
        shield, HWND_TOPMOST,
        bounds.left, bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(shield);

    // Commit the frozen bridge first. Only then cloak the real player from DWM
    // presentation; its HWND, WinUI islands, video child and libmpv keep running
    // normally underneath the bridge.
    DwmFlush();
    BOOL cloak = TRUE;
    HRESULT const result = DwmSetWindowAttribute(
        g_mainWindow, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (FAILED(result))
    {
        DestroyWindow(shield);
        return nullptr;
    }
    DwmFlush();
    return shield;
}

void DestroyPendingFullscreenVideoSettleShield()
{
    if (g_mainWindow)
    {
        KillTimer(g_mainWindow, FullscreenVideoSettleTimer);
    }
    HWND shield = std::exchange(
        g_pendingFullscreenVideoSettleShield, nullptr);
    g_fullscreenVideoSettleStartedTick = 0;
    if (shield && IsWindow(shield))
    {
        DestroyWindow(shield);
        DwmFlush();
    }
}

bool FullscreenVideoViewportSettled(HWND shield)
{
    if (!shield || !g_mpv.handle || !g_mpv.getProperty) return true;

    auto* snapshot = reinterpret_cast<FullscreenTransitionSnapshot*>(
        GetWindowLongPtrW(shield, GWLP_USERDATA));
    if (!snapshot || !snapshot->settleMatchingFullscreenVideo) return true;

    int64_t width{};
    int64_t height{};
    int64_t marginTop{};
    int64_t marginBottom{};
    int64_t marginLeft{};
    int64_t marginRight{};

    // mpv documents osd-dimensions/* as the current VO size and the exact
    // OSD-to-video margins. Read-only polling gives us a renderer-owned handoff
    // signal without changing any mpv option, property or rendering state.
    bool const haveGeometry =
        g_mpv.getProperty(g_mpv.handle,
            "osd-dimensions/w", MpvFormatInt64, &width) >= 0 &&
        g_mpv.getProperty(g_mpv.handle,
            "osd-dimensions/h", MpvFormatInt64, &height) >= 0 &&
        g_mpv.getProperty(g_mpv.handle,
            "osd-dimensions/mt", MpvFormatInt64, &marginTop) >= 0 &&
        g_mpv.getProperty(g_mpv.handle,
            "osd-dimensions/mb", MpvFormatInt64, &marginBottom) >= 0 &&
        g_mpv.getProperty(g_mpv.handle,
            "osd-dimensions/ml", MpvFormatInt64, &marginLeft) >= 0 &&
        g_mpv.getProperty(g_mpv.handle,
            "osd-dimensions/mr", MpvFormatInt64, &marginRight) >= 0;

    if (!haveGeometry) return false;

    auto const nearTarget = [](int64_t value, int target)
        {
            int64_t delta = value - static_cast<int64_t>(target);
            if (delta < 0) delta = -delta;
            return delta <= 2;
        };
    bool const sizeReady =
        nearTarget(width, snapshot->targetWidth) &&
        nearTarget(height, snapshot->targetHeight);
    bool const marginsReady =
        marginTop <= 2 && marginBottom <= 2 &&
        marginLeft <= 2 && marginRight <= 2;
    return sizeReady && marginsReady;
}

void PollFullscreenVideoSettle()
{
    HWND shield = g_pendingFullscreenVideoSettleShield;
    if (!shield)
    {
        if (g_mainWindow) KillTimer(
            g_mainWindow, FullscreenVideoSettleTimer);
        return;
    }

    // Never let a presentation shield survive a state change. 120 ms is only
    // a fail-open ceiling for unavailable/stale VO properties; the normal path
    // removes the shield as soon as mpv reports the final 16:9 viewport.
    bool const timedOut = g_fullscreenVideoSettleStartedTick != 0 &&
        GetTickCount64() - g_fullscreenVideoSettleStartedTick >= 120;
    if (!g_fullscreen || FullscreenVideoViewportSettled(shield) || timedOut)
    {
        DwmFlush();
        DestroyPendingFullscreenVideoSettleShield();
    }
}

void FinishMediaFullscreenTransitionShield(HWND shield)
{
    if (!shield) return;

    // The established fullscreen style/frame/corner sequence has already
    // completed at this point. We only synchronize presentation, then reveal the
    // final HWND. No WS_* flags, SWP_FRAMECHANGED or DWM chrome attributes are
    // touched here, specifically to avoid reopening the old frame-border bugs.
    UpdateWindow(g_mainWindow);
    DwmFlush();

    BOOL cloak = FALSE;
    DwmSetWindowAttribute(
        g_mainWindow, DWMWA_CLOAK, &cloak, sizeof(cloak));
    DwmFlush();

    auto* snapshot = reinterpret_cast<FullscreenTransitionSnapshot*>(
        GetWindowLongPtrW(shield, GWLP_USERDATA));
    bool const deferForMatching169 =
        g_fullscreen && snapshot && snapshot->settleMatchingFullscreenVideo;
    if (deferForMatching169)
    {
        DestroyPendingFullscreenVideoSettleShield();
        g_pendingFullscreenVideoSettleShield = shield;
        g_fullscreenVideoSettleStartedTick = GetTickCount64();
        if (SetTimer(
            g_mainWindow, FullscreenVideoSettleTimer, 8, nullptr) != 0)
        {
            return;
        }

        // Fail open if Windows cannot allocate the one-shot poll timer. The
        // real player is already uncloaked; never leave a topmost bridge stuck.
        g_pendingFullscreenVideoSettleShield = nullptr;
        g_fullscreenVideoSettleStartedTick = 0;
    }

    DestroyWindow(shield);
    DwmFlush();
}

HWND ShowIdleFullscreenTransitionShield(RECT const& bounds)
{
    // The root HWND necessarily has to rebuild its non-client frame when
    // changing between the normal overlapped style and fullscreen popup.
    // On the idle screen there is no continuously presenting video surface to
    // visually cover a compositor frame caught in the middle of that rebuild.
    // Put a tiny, non-activating black popup above the player for exactly
    // that commit. The idle canvas is already black, so this never exposes a
    // foreign/native frame while leaving media fullscreen completely untouched.
    // Keep the shield independent from the player HWND. DWMWA_CLOAK is
    // inherited by owned windows, so making this popup an ownerless toolwindow
    // guarantees that it stays visible while the player itself is cloaked.
    HWND shield = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        FullscreenTransitionShieldClassName,
        L"",
        WS_POPUP,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        nullptr,
        nullptr,
        g_instance,
        nullptr);

    if (!shield) return nullptr;

    SetWindowPos(
        shield,
        HWND_TOPMOST,
        bounds.left,
        bounds.top,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(shield);

    // First commit the black cover. Then cloak the real top-level HWND before
    // any frame/style mutation. The HWND stays alive and keeps composing, but
    // DWM is not allowed to present its old redirected 1180x760 representation
    // while Win32 rebuilds the non-client frame underneath the cover.
    DwmFlush();
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(
        g_mainWindow, DWMWA_CLOAK, &cloak, sizeof(cloak));
    DwmFlush();
    return shield;
}

void FinishIdleFullscreenTransitionShield(HWND shield)
{
    if (!shield) return;

    // Finish layout/paint while the real HWND is still cloaked. Uncloak only
    // after DWM has consumed the final geometry, then keep the independent
    // black shield for one more compositor synchronization so the first visible
    // player frame is already the settled fullscreen/windowed result.
    UpdateWindow(g_mainWindow);
    DwmFlush();

    BOOL cloak = FALSE;
    DwmSetWindowAttribute(
        g_mainWindow, DWMWA_CLOAK, &cloak, sizeof(cloak));
    DwmFlush();

    DestroyWindow(shield);
}

void ApplyWindows11Visual(HWND window)
{
    BOOL dark = PlayerIsLightTheme() ? FALSE : TRUE;
    DwmSetWindowAttribute(
        window,
        DWMWA_USE_IMMERSIVE_DARK_MODE,
        &dark,
        sizeof(dark));

    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW;
    DwmSetWindowAttribute(
        window,
        DWMWA_SYSTEMBACKDROP_TYPE,
        &backdrop,
        sizeof(backdrop));

    // Theme changes call this function even while the main window is already
    // fullscreen. Never reintroduce Windows 11 rounded window chrome there.
    DWM_WINDOW_CORNER_PREFERENCE corners =
        g_fullscreen
            ? DWMWCP_DONOTROUND
            : DWMWCP_ROUND;

    DwmSetWindowAttribute(
        window,
        DWMWA_WINDOW_CORNER_PREFERENCE,
        &corners,
        sizeof(corners));

    if (window == g_mainWindow && g_fullscreen)
    {
        // DWMWA_COLOR_NONE: fullscreen must never expose a DWM frame/border
        // when immersive light/dark mode is reapplied.
        constexpr DWORD noBorderColor = 0xFFFFFFFE;
        DwmSetWindowAttribute(
            window,
            DWMWA_BORDER_COLOR,
            &noBorderColor,
            sizeof(noBorderColor));
    }
}

namespace
{
    std::string ConfiguredMpvValue(char const* name, char const* fallback)
    {
        auto value = g_mpvSettingsManager.Overrides().find(name);
        return value == g_mpvSettingsManager.Overrides().end() ? fallback : value->second;
    }

    bool ParseAutofitComponent(
        std::wstring text, int available, int& pixels)
    {
        text = Trim(std::move(text));
        if (text.empty()) return false;
        try
        {
            bool percent = text.back() == L'%';
            if (percent) text.pop_back();
            double value = std::stod(text);
            if (!std::isfinite(value) || value <= 0.0) return false;
            pixels = percent
                ? static_cast<int>(std::round(available * value / 100.0))
                : static_cast<int>(std::round(value));
            return pixels > 0;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ResolveAutofitBox(
        std::string const& value, RECT const& work, int& width, int& height)
    {
        std::wstring text = winrt::to_hstring(value).c_str();
        auto separator = text.find_first_of(L"xX");
        if (separator == std::wstring::npos) return false;
        int availableWidth = work.right - work.left;
        int availableHeight = work.bottom - work.top;
        return ParseAutofitComponent(
            text.substr(0, separator), availableWidth, width) &&
            ParseAutofitComponent(
                text.substr(separator + 1), availableHeight, height);
    }

    bool ResizeMainWindowForVideo(int64_t videoWidth, int64_t videoHeight)
    {
        if (!g_mainWindow || videoWidth <= 0 || videoHeight <= 0 ||
            g_fullscreen || g_pictureInPicture || IsZoomed(g_mainWindow))
        {
            return false;
        }

        MONITORINFO monitor{ sizeof(monitor) };
        if (!GetMonitorInfoW(
            MonitorFromWindow(g_mainWindow, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            return false;
        }

        DWORD style = static_cast<DWORD>(
            GetWindowLongPtrW(g_mainWindow, GWL_STYLE));
        DWORD exStyle = static_cast<DWORD>(
            GetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE));
        UINT dpi = GetDpiForWindow(g_mainWindow);
        if (!dpi) dpi = USER_DEFAULT_SCREEN_DPI;

        // Work in client pixels so the client itself follows the video's
        // display aspect ratio. Account for the current non-client frame before
        // deciding how much of the monitor is really available.
        RECT sample{ 0, 0, 100, 100 };
        AdjustWindowRectExForDpi(&sample, style, FALSE, exStyle, dpi);
        int nonClientWidth = (sample.right - sample.left) - 100;
        int nonClientHeight = (sample.bottom - sample.top) - 100;

        int workWidth = monitor.rcWork.right - monitor.rcWork.left;
        int workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
        int maxWidth = (std::max)(1, workWidth - nonClientWidth);
        int maxHeight = (std::max)(1, workHeight - nonClientHeight);

        int configuredWidth{};
        int configuredHeight{};
        if (ResolveAutofitBox(ConfiguredMpvValue("autofit", "1216x714"),
            monitor.rcWork, configuredWidth, configuredHeight))
        {
            maxWidth = (std::min)(maxWidth, configuredWidth);
            maxHeight = (std::min)(maxHeight, configuredHeight);
        }
        if (ResolveAutofitBox(ConfiguredMpvValue(
            "autofit-larger", "81%x81%"), monitor.rcWork,
            configuredWidth, configuredHeight))
        {
            maxWidth = (std::min)(maxWidth, configuredWidth);
            maxHeight = (std::min)(maxHeight, configuredHeight);
        }

        double maxScale = (std::min)(
            static_cast<double>(maxWidth) / static_cast<double>(videoWidth),
            static_cast<double>(maxHeight) / static_cast<double>(videoHeight));
        if (!std::isfinite(maxScale) || maxScale <= 0.0)
        {
            return false;
        }

        // Preserve the player's existing minimum tracking size without ever
        // breaking the video's aspect ratio. Small videos may be enlarged only
        // as much as necessary; normal/large videos are never enlarged beyond
        // their natural display size.
        int minOuterWidth = MulDiv(
            NormalMinimumWidth, dpi, USER_DEFAULT_SCREEN_DPI);
        int minOuterHeight = MulDiv(
            NormalMinimumHeight, dpi, USER_DEFAULT_SCREEN_DPI);
        int minClientWidth = (std::max)(
            1, minOuterWidth - nonClientWidth);
        int minClientHeight = (std::max)(
            1, minOuterHeight - nonClientHeight);

        double desiredScale = (std::max)({
            1.0,
            static_cast<double>(minClientWidth) /
                static_cast<double>(videoWidth),
            static_cast<double>(minClientHeight) /
                static_cast<double>(videoHeight) });
        double scale = (std::min)(desiredScale, maxScale);

        int clientWidth = (std::max)(1,
            static_cast<int>(std::round(videoWidth * scale)));
        int clientHeight = (std::max)(1,
            static_cast<int>(std::round(videoHeight * scale)));

        RECT windowRect{ 0, 0, clientWidth, clientHeight };
        AdjustWindowRectExForDpi(
            &windowRect, style, FALSE, exStyle, dpi);
        int outerWidth = windowRect.right - windowRect.left;
        int outerHeight = windowRect.bottom - windowRect.top;

        RECT current{};
        GetWindowRect(g_mainWindow, &current);
        int x = current.left +
            ((current.right - current.left) - outerWidth) / 2;
        int y = current.top +
            ((current.bottom - current.top) - outerHeight) / 2;

        int workLeft = static_cast<int>(monitor.rcWork.left);
        int workTop = static_cast<int>(monitor.rcWork.top);
        int workRight = static_cast<int>(monitor.rcWork.right);
        int workBottom = static_cast<int>(monitor.rcWork.bottom);
        x = (std::max)(workLeft,
            (std::min)(x, workRight - outerWidth));
        y = (std::max)(workTop,
            (std::min)(y, workBottom - outerHeight));

        SetWindowPos(g_mainWindow, nullptr, x, y, outerWidth, outerHeight,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }

    bool ReadCurrentVideoDisplaySize(int64_t& videoWidth, int64_t& videoHeight)
    {
        videoWidth = 0;
        videoHeight = 0;
        return g_mpv.handle && g_mpv.getProperty &&
            g_mpv.getProperty(g_mpv.handle,
                "video-out-params/dw", MpvFormatInt64, &videoWidth) >= 0 &&
            g_mpv.getProperty(g_mpv.handle,
                "video-out-params/dh", MpvFormatInt64, &videoHeight) >= 0 &&
            videoWidth > 0 && videoHeight > 0;
    }

    void ResetDynamicWindowFitTracking()
    {
        g_dynamicFitObservedWidth = 0;
        g_dynamicFitObservedHeight = 0;
        g_dynamicFitAppliedWidth = 0;
        g_dynamicFitAppliedHeight = 0;
        g_dynamicFitStableSamples = 0;
    }

    bool DynamicWindowFitEnabled()
    {
        return ConfiguredNativeToggle("ui-window-follow-video", false);
    }

    bool RememberWindowSizeEnabled()
    {
        return ConfiguredNativeToggle("ui-window-remember-size", false);
    }

    bool ParseRememberedWindowSize(
        std::string const& value, int& logicalWidth, int& logicalHeight)
    {
        logicalWidth = 0;
        logicalHeight = 0;
        std::wstring text = winrt::to_hstring(value).c_str();
        auto separator = text.find_first_of(L"xX");
        if (separator == std::wstring::npos) return false;

        try
        {
            auto widthText = Trim(text.substr(0, separator));
            auto heightText = Trim(text.substr(separator + 1));
            if (widthText.empty() || heightText.empty()) return false;

            long width = std::stol(widthText);
            long height = std::stol(heightText);
            if (width <= 0 || height <= 0 ||
                width > 32767 || height > 32767)
            {
                return false;
            }

            logicalWidth = static_cast<int>(width);
            logicalHeight = static_cast<int>(height);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool CaptureRememberedWindowSize(bool saveImmediately)
    {
        if (!g_mainWindow || !RememberWindowSizeEnabled() ||
            g_fullscreen || g_pictureInPicture ||
            IsIconic(g_mainWindow) || IsZoomed(g_mainWindow))
        {
            return false;
        }

        RECT rect{};
        if (!GetWindowRect(g_mainWindow, &rect))
        {
            return false;
        }

        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0)
        {
            return false;
        }

        UINT dpi = GetDpiForWindow(g_mainWindow);
        if (!dpi) dpi = USER_DEFAULT_SCREEN_DPI;

        // Persist logical (96-DPI) outer-window dimensions. Restoring from
        // logical units keeps the remembered size visually consistent when the
        // player later opens on a monitor with different Windows scaling.
        int logicalWidth = MulDiv(
            width, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi));
        int logicalHeight = MulDiv(
            height, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi));
        if (logicalWidth <= 0 || logicalHeight <= 0)
        {
            return false;
        }

        std::string value =
            std::to_string(logicalWidth) + "x" +
            std::to_string(logicalHeight);

        auto& overrides = g_mpvSettingsManager.Overrides();
        auto found = overrides.find("ui-window-last-size");
        if (found != overrides.end() && found->second == value)
        {
            return true;
        }

        overrides["ui-window-last-size"] = value;
        g_mpvSettingsManager.MarkDirty();

        if (saveImmediately)
        {
            if (g_mainWindow)
                KillTimer(g_mainWindow, NativeSettingsSaveTimer);
            return g_mpvSettingsManager.SaveNativeOptions();
        }

        ScheduleNativeOptionsSave();
        return true;
    }

    bool ApplyRememberedWindowSize()
    {
        if (!g_mainWindow || !RememberWindowSizeEnabled())
        {
            return false;
        }

        auto found =
            g_mpvSettingsManager.Overrides().find("ui-window-last-size");
        if (found == g_mpvSettingsManager.Overrides().end())
        {
            return false;
        }

        int logicalWidth{};
        int logicalHeight{};
        if (!ParseRememberedWindowSize(
            found->second, logicalWidth, logicalHeight))
        {
            return false;
        }

        UINT dpi = GetDpiForWindow(g_mainWindow);
        if (!dpi) dpi = USER_DEFAULT_SCREEN_DPI;

        int width = MulDiv(
            logicalWidth, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        int height = MulDiv(
            logicalHeight, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);

        int minWidth = MulDiv(
            NormalMinimumWidth, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        int minHeight = MulDiv(
            NormalMinimumHeight, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        width = (std::max)(width, minWidth);
        height = (std::max)(height, minHeight);

        MONITORINFO monitor{ sizeof(monitor) };
        if (!GetMonitorInfoW(
            MonitorFromWindow(g_mainWindow, MONITOR_DEFAULTTONEAREST),
            &monitor))
        {
            return false;
        }

        int workWidth = monitor.rcWork.right - monitor.rcWork.left;
        int workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
        width = (std::min)(width, workWidth);
        height = (std::min)(height, workHeight);

        RECT current{};
        if (!GetWindowRect(g_mainWindow, &current))
        {
            return false;
        }

        int x = current.left +
            ((current.right - current.left) - width) / 2;
        int y = current.top +
            ((current.bottom - current.top) - height) / 2;

        x = (std::max)(
            static_cast<int>(monitor.rcWork.left),
            (std::min)(
                x, static_cast<int>(monitor.rcWork.right) - width));
        y = (std::max)(
            static_cast<int>(monitor.rcWork.top),
            (std::min)(
                y, static_cast<int>(monitor.rcWork.bottom) - height));

        return SetWindowPos(
            g_mainWindow, nullptr, x, y, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
    }

    bool CenterMainWindowOnPrimaryWorkArea()
    {
        if (!g_mainWindow)
        {
            return false;
        }

        POINT primaryOrigin{ 0, 0 };
        HMONITOR monitor = MonitorFromPoint(
            primaryOrigin, MONITOR_DEFAULTTOPRIMARY);
        if (!monitor)
        {
            return false;
        }

        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        if (!GetMonitorInfoW(monitor, &monitorInfo))
        {
            return false;
        }

        RECT windowRect{};
        if (!GetWindowRect(g_mainWindow, &windowRect))
        {
            return false;
        }

        int const width = windowRect.right - windowRect.left;
        int const height = windowRect.bottom - windowRect.top;
        if (width <= 0 || height <= 0)
        {
            return false;
        }

        RECT const& work = monitorInfo.rcWork;
        int const workWidth = work.right - work.left;
        int const workHeight = work.bottom - work.top;

        // Startup placement is deterministic: center the final normal-window
        // size in the primary monitor's usable work area. This runs while the
        // HWND is still hidden, so the user never sees an intermediate move.
        int x = work.left + (workWidth - width) / 2;
        int y = work.top + (workHeight - height) / 2;

        // On an unusually small work area, keep the title bar/top-left reachable
        // rather than allowing a centered oversized window to begin off-screen.
        x = (std::max)(static_cast<int>(work.left), x);
        y = (std::max)(static_cast<int>(work.top), y);

        return SetWindowPos(
            g_mainWindow, nullptr, x, y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
    }

    bool TryApplyConfiguredAutofit()
    {
        if (!g_mainWindow || !g_mpv.handle || !g_mpv.getProperty ||
            g_fullscreen || g_pictureInPicture || IsZoomed(g_mainWindow))
        {
            return true;
        }

        // Remembered size is the user's explicit normal-window preference.
        // Dynamic follow-video deliberately has higher priority and may resize
        // again as soon as media dimensions are known.
        if (RememberWindowSizeEnabled() && !DynamicWindowFitEnabled())
        {
            return true;
        }

        int64_t videoWidth{};
        int64_t videoHeight{};
        if (!ReadCurrentVideoDisplaySize(videoWidth, videoHeight))
        {
            return false;
        }

        if (!ResizeMainWindowForVideo(videoWidth, videoHeight))
        {
            return true;
        }

        if (DynamicWindowFitEnabled())
        {
            g_dynamicFitAppliedWidth = videoWidth;
            g_dynamicFitAppliedHeight = videoHeight;
            g_dynamicFitObservedWidth = videoWidth;
            g_dynamicFitObservedHeight = videoHeight;
            g_dynamicFitStableSamples = 2;
        }
        return true;
    }

    void UpdateDynamicWindowFitMonitoring(bool applyNow)
    {
        if (!g_mainWindow) return;

        KillTimer(g_mainWindow, DynamicWindowFitTimer);
        ResetDynamicWindowFitTracking();

        if (!DynamicWindowFitEnabled())
        {
            return;
        }

        SetTimer(g_mainWindow, DynamicWindowFitTimer, 250, nullptr);

        if (applyNow && !IsSidePanelOpen() && !g_contextMenuOpen)
        {
            int64_t videoWidth{};
            int64_t videoHeight{};
            if (ReadCurrentVideoDisplaySize(videoWidth, videoHeight) &&
                ResizeMainWindowForVideo(videoWidth, videoHeight))
            {
                g_dynamicFitAppliedWidth = videoWidth;
                g_dynamicFitAppliedHeight = videoHeight;
                g_dynamicFitObservedWidth = videoWidth;
                g_dynamicFitObservedHeight = videoHeight;
                g_dynamicFitStableSamples = 2;
            }
        }
    }

    void PollDynamicWindowFit()
    {
        if (!DynamicWindowFitEnabled())
        {
            if (g_mainWindow)
                KillTimer(g_mainWindow, DynamicWindowFitTimer);
            ResetDynamicWindowFitTracking();
            return;
        }

        if (!g_mainWindow || g_fullscreen || g_pictureInPicture ||
            IsZoomed(g_mainWindow) || IsSidePanelOpen() || g_contextMenuOpen)
        {
            return;
        }

        int64_t videoWidth{};
        int64_t videoHeight{};
        if (!ReadCurrentVideoDisplaySize(videoWidth, videoHeight))
        {
            return;
        }

        if (videoWidth == g_dynamicFitAppliedWidth &&
            videoHeight == g_dynamicFitAppliedHeight)
        {
            return;
        }

        // Require two equal samples (about 500 ms) before resizing. Adaptive
        // streams can briefly expose transitional dimensions; this avoids a
        // visible window jump for values that exist for only one poll.
        if (videoWidth != g_dynamicFitObservedWidth ||
            videoHeight != g_dynamicFitObservedHeight)
        {
            g_dynamicFitObservedWidth = videoWidth;
            g_dynamicFitObservedHeight = videoHeight;
            g_dynamicFitStableSamples = 1;
            return;
        }

        if (++g_dynamicFitStableSamples < 2)
        {
            return;
        }

        if (ResizeMainWindowForVideo(videoWidth, videoHeight))
        {
            g_dynamicFitAppliedWidth = videoWidth;
            g_dynamicFitAppliedHeight = videoHeight;
        }
    }

    void ScheduleConfiguredAutofit()
    {
        if (!g_mainWindow) return;
        g_autofitAttemptsRemaining = 40;
        SetTimer(g_mainWindow, AutofitWindowTimer, 100, nullptr);
        UpdateDynamicWindowFitMonitoring(false);
    }

    bool StartupAudioOnlyPresentationReady()
    {
        if (!g_mpv.handle || !g_mpv.getProperty) return false;

        int idleActive{ 1 };
        if (g_mpv.getProperty(
            g_mpv.handle, "idle-active", MpvFormatFlag, &idleActive) < 0 ||
            idleActive != 0)
        {
            return false;
        }

        // A video item must wait for video-out-params so the normal autofit can
        // establish its final client aspect before the temporary black cover is
        // removed. Audio has no video geometry to wait for and can be revealed
        // as soon as its selected track exists. These are read-only properties.
        int64_t videoId{ -1 };
        if (g_mpv.getProperty(
            g_mpv.handle, "current-tracks/video/id",
            MpvFormatInt64, &videoId) >= 0 && videoId >= 0)
        {
            return false;
        }

        int64_t audioId{ -1 };
        return g_mpv.getProperty(
            g_mpv.handle, "current-tracks/audio/id",
            MpvFormatInt64, &audioId) >= 0 && audioId >= 0;
    }

    void ResizeInitialMediaRevealShield()
    {
        if (!g_initialMediaRevealShield || !g_mainWindow ||
            !IsWindow(g_initialMediaRevealShield))
        {
            return;
        }

        RECT client{};
        if (!GetClientRect(g_mainWindow, &client)) return;
        int const width = (std::max)(1, static_cast<int>(client.right - client.left));
        int const height = (std::max)(1, static_cast<int>(client.bottom - client.top));
        SetWindowPos(
            g_initialMediaRevealShield, HWND_TOP,
            0, 0, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void DestroyInitialMediaRevealShield()
    {
        HWND shield = std::exchange(g_initialMediaRevealShield, nullptr);
        if (shield && IsWindow(shield))
        {
            DestroyWindow(shield);
        }
    }

    void FinishDeferredStartupMediaReveal()
    {
        if (!g_deferredStartupMediaReveal || !g_mainWindow) return;

        KillTimer(g_mainWindow, InitialMediaRevealTimer);
        g_deferredStartupMediaReveal = false;
        g_deferredStartupRevealStartedTick = 0;

        // Restore the final overlay visibility while the black child is still
        // the visible presentation. Any island that becomes visible here is
        // immediately placed back underneath the shield before DWM commits.
        if (auto* info = reinterpret_cast<WindowInfo*>(
            GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA)))
        {
            RECT client{};
            if (GetClientRect(g_mainWindow, &client))
            {
                ApplyClientLayout(g_mainWindow, info,
                    client.right - client.left, client.bottom - client.top);
                ResizeInitialMediaRevealShield();
            }
        }

        // Commit the final client geometry and overlays together. Removing only
        // the shield then reveals the already-running, fully laid-out player
        // without ever hiding/re-showing the top-level HWND.
        UpdateWindow(g_mainWindow);
        DwmFlush();
        DestroyInitialMediaRevealShield();
        DwmFlush();
    }

    void BeginDeferredStartupMediaReveal(int showCommand)
    {
        if (!g_mainWindow) return;

        RECT client{};
        GetClientRect(g_mainWindow, &client);
        int const width = (std::max)(1, static_cast<int>(client.right - client.left));
        int const height = (std::max)(1, static_cast<int>(client.bottom - client.top));

        g_initialMediaRevealShield = CreateWindowExW(
            WS_EX_NOACTIVATE,
            InitialMediaRevealShieldClassName,
            L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, width, height,
            g_mainWindow,
            nullptr,
            g_instance,
            nullptr);

        // Fail open: if Windows cannot create the tiny presentation shield,
        // preserve the established fast startup rather than delaying the app.
        if (!g_initialMediaRevealShield)
        {
            ShowWindow(g_mainWindow, showCommand);
            UpdateWindow(g_mainWindow);
            return;
        }

        g_deferredStartupMediaReveal = true;
        g_deferredStartupRevealStartedTick = GetTickCount64();

        // DesktopWindowXamlSource can reinsert its child composition surface at
        // the top of the sibling stack when the hidden owner is first shown.
        // Hide every startup overlay through the normal layout path before that
        // first presentation, then reassert the black shield as the top child.
        if (auto* info = reinterpret_cast<WindowInfo*>(
            GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA)))
        {
            RECT currentClient{};
            if (GetClientRect(g_mainWindow, &currentClient))
            {
                ApplyClientLayout(g_mainWindow, info,
                    currentClient.right - currentClient.left,
                    currentClient.bottom - currentClient.top);
            }
        }
        ResizeInitialMediaRevealShield();

        // This is the key .36 change: the app window becomes visible now. Only
        // the client presentation stays black while mpv prepares video-out-params.
        ShowWindow(g_mainWindow, showCommand);
        UpdateWindow(g_mainWindow);
        if (SetTimer(g_mainWindow, InitialMediaRevealTimer, 50, nullptr) == 0)
        {
            FinishDeferredStartupMediaReveal();
        }
    }

    void PollDeferredStartupMediaReveal()
    {
        if (!g_deferredStartupMediaReveal || !g_mainWindow)
        {
            if (g_mainWindow) KillTimer(g_mainWindow, InitialMediaRevealTimer);
            return;
        }

        // Same read-only geometry/autofit proof from .35, but the top-level
        // HWND is already visible. The black child only masks transitional
        // keepaspect bars until the final client geometry has been applied.
        bool const geometryReady = TryApplyConfiguredAutofit();
        bool const audioReady = !geometryReady &&
            StartupAudioOnlyPresentationReady();
        bool const timedOut = g_deferredStartupRevealStartedTick != 0 &&
            GetTickCount64() - g_deferredStartupRevealStartedTick >=
                InitialMediaRevealTimeoutMs;

        if (geometryReady || audioReady || timedOut)
        {
            FinishDeferredStartupMediaReveal();
        }
    }

}

bool PlayerLoadOpticalDisc(
    std::wstring const& path, bool bluray, int64_t title = -1,
    bool addToRecent = true)
{
    CaptureCurrentRecentTitle();
    g_shufflePlayback = false;

    if (!g_mpv.Start(g_videoWindow)) return false;

    // An explicitly opened disc is also a fresh playback session.
    // Do not inherit a restore point left by an earlier engine rebuild.
    g_mpv.setProperty(g_mpv.handle, "start", "none");

    std::string utf8 = winrt::to_string(path);
    g_mpv.setProperty(g_mpv.handle,
        bluray ? "bluray-device" : "dvd-device", utf8.c_str());
    std::string target = bluray ? "bd://" : "dvd://";
    if (title >= 0) target += std::to_string(title);
    const char* load[] = { "loadfile", target.c_str(), "replace", nullptr };
    if (g_mpv.command(g_mpv.handle, load) < 0) return false;

    g_currentMediaPath = path;
    g_currentMediaIsDisc = true;
    g_currentDiscIsBluray = bluray;
    RefreshCurrentClientLayout();
    if (addToRecent) AddRecentFile(path);
    ScheduleConfiguredAutofit();

    if (g_mainWindow)
    {
        auto* info = reinterpret_cast<WindowInfo*>(
            GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
        if (info && info->page)
        {
            winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
                info->page)->RestorePlayerState(path);
        }
    }
    if (ConfiguredNativeToggle("fullscreen", false) && !g_fullscreen)
        PlayerToggleFullscreen();
    return true;
}

bool PlayerLoadFile(std::wstring const& path)
{
    // An explicit Open/Recent replaces the current media. Preserve its resume
    // point while mpv still exposes time-pos/duration, before loadfile unloads it.
    SaveCurrentResumePointOnExit();

    // Capture the outgoing page title before replace unloads its metadata.
    CaptureCurrentRecentTitle();

    // A new explicit load replaces the previous playlist. playlist-shuffle is
    // a playlist operation, not a persistent mode, so the UI returns to the
    // deterministic unshuffled state for the newly created playlist.
    g_shufflePlayback = false;

    std::filesystem::path discRoot;
    bool bluray{};
    if (TryGetOpticalDiscFolder(path, discRoot, bluray))
    {
        return PlayerLoadOpticalDisc(discRoot.wstring(), bluray);
    }

    if (!g_mpv.Start(g_videoWindow))
    {
        return false;
    }

    g_mpv.setProperty(g_mpv.handle, "loop-file", g_loopPlayback ? "inf" : "no");
    g_mpv.setProperty(g_mpv.handle, "osd-msg1", "");

    // RestartEnginePreservingPlayback() temporarily uses mpv's runtime
    // "start" option to restore the current position after rebuilding the
    // engine. Runtime options can persist across later files, so an explicit
    // user open must clear that restore point. "none" is mpv's documented
    // libmpv value for resetting a previously set --start option.
    g_mpv.setProperty(g_mpv.handle, "start", "none");

    // Normal single-file opens use mpv's native --autocreate-playlist=filter.
    // Explicit multi-file operations already build an exact playlist below, so
    // suppress native directory expansion only for that one load command.
    bool restoreNativeAutocreate = false;
    if (g_suppressAutoload && g_mpv.setProperty)
    {
        restoreNativeAutocreate =
            g_mpv.setProperty(
                g_mpv.handle,
                "autocreate-playlist",
                "no") >= 0;
    }

    std::string utf8 = winrt::to_string(MpvLoadTarget(path));
    std::vector<std::pair<std::string, std::string>> localOptions;

    if (IsLikelyHlsSource(path))
    {
        localOptions = HlsFileLocalOptions();
    }

    double resumePosition{};
    if (TryGetResumePosition(path, resumePosition))
    {
        // Keep the native per-file start option: it is the ideal zero-frame
        // resume path when mpv preserves file-local options. Also arm a one-shot
        // exact-seek fallback for older mpv builds where autocreate-playlist
        // drops that option while expanding the directory playlist.
        ArmPendingResumeSeek(path, resumePosition);
        std::ostringstream position;
        position << std::setprecision(12) << resumePosition;
        localOptions.emplace_back("start", position.str());
    }
    else
    {
        ClearPendingResumeSeek();
    }

    // mpv restores these values automatically when this media item ends, so
    // HLS resilience cannot leak into a later local file or playlist entry.
    bool const loaded =
        LoadFileWithLocalOptions(utf8, "replace", localOptions) >= 0;

    if (!loaded) ClearPendingResumeSeek();

    if (restoreNativeAutocreate)
    {
        g_mpv.setProperty(
            g_mpv.handle,
            "autocreate-playlist",
            "filter");
    }

    if (loaded)
    {
        g_currentMediaPath = path;
        g_currentMediaIsDisc = false;
        g_currentDiscIsBluray = false;
        RefreshCurrentClientLayout();
        AddRecentFile(path);
        ScheduleConfiguredAutofit();
        if (ConfiguredNativeToggle("fullscreen", false) && !g_fullscreen)
            PlayerToggleFullscreen();
    }
    return loaded;
}

void PlayerTogglePause()
{
    if (!g_mpv.handle)
    {
        return;
    }
    const char* args[] = { "cycle", "pause", nullptr };
    g_mpv.command(g_mpv.handle, args);
}

void PlayerReplay()
{
    if (!g_mpv.handle) return;
    const char* seek[] = { "seek", "0", "absolute", nullptr };
    g_mpv.command(g_mpv.handle, seek);
    g_mpv.setProperty(g_mpv.handle, "pause", "no");
}

bool PlayerGetPlaybackState(bool& paused, bool& eofReached)
{
    paused = false;
    eofReached = false;
    if (!g_mpv.handle || !g_mpv.getProperty) return false;

    int pauseFlag{};
    int eofFlag{};
    bool available = g_mpv.getProperty(
        g_mpv.handle, "pause", MpvFormatFlag, &pauseFlag) >= 0;
    g_mpv.getProperty(
        g_mpv.handle, "eof-reached", MpvFormatFlag, &eofFlag);
    paused = pauseFlag != 0;
    eofReached = eofFlag != 0;
    return available;
}

void PlayerSetLooping(bool enabled)
{
    g_loopPlayback = enabled;
    if (g_mpv.handle)
    {
        g_mpv.setProperty(g_mpv.handle, "loop-file", enabled ? "inf" : "no");
        std::wstring message = PlayerUiString(
            enabled ? L"OsdLoopEnabled" : L"OsdLoopDisabled",
            enabled ? L"Loop do arquivo: Ativado" : L"Loop do arquivo: Desativado");
        PlayerExecuteMpvCommand(L"show-text \"" + message + L"\"");
    }
}

bool PlayerGetLooping()
{
    // The L binding is intentionally still owned by mpv/default-input.conf.
    // Read the live property instead of trusting only HC Player's cached state
    // so keyboard/imported bindings and the toolbar always converge visually.
    if (g_mpv.handle && g_mpv.getProperty && g_mpv.freeNodeContents)
    {
        MpvNode node{};
        if (g_mpv.getProperty(
                g_mpv.handle, "loop-file", MpvFormatNode, &node) >= 0)
        {
            bool enabled = g_loopPlayback;

            if (node.format == MpvFormatString && node.value.string)
            {
                std::string const value{ node.value.string };
                enabled = !value.empty() && value != "no" && value != "0";
            }
            else if (node.format == MpvFormatInt64)
            {
                enabled = node.value.integer > 0;
            }
            else if (node.format == MpvFormatFlag)
            {
                enabled = node.value.flag != 0;
            }

            g_mpv.freeNodeContents(&node);
            g_loopPlayback = enabled;
        }
    }

    return g_loopPlayback;
}

bool PlayerSetPlaylistShuffle(bool enabled)
{
    if (!g_mpv.handle || !g_mpv.command)
    {
        return false;
    }

    const char* command[] = {
        enabled ? "playlist-shuffle" : "playlist-unshuffle",
        nullptr
    };

    if (g_mpv.command(g_mpv.handle, command) < 0)
    {
        return false;
    }

    g_shufflePlayback = enabled;
    return true;
}

bool PlayerGetPlaylistShuffle()
{
    return g_shufflePlayback;
}

bool PlayerGetChapterHoverCardEnabled()
{
    auto value = g_mpvSettingsManager.Overrides().find("ui-chapter-tooltip");
    return value == g_mpvSettingsManager.Overrides().end() || value->second != "no";
}

void PlayerToggleStats()
{
    if (!g_mpv.handle) return;

    // Use one native stats.lua surface for every media type. With the idle
    // video output kept alive by MpvHost::Start, the same overlay used by
    // videos also renders for audio-only files and other MPV media.
    const char* args[] = {
        "script-binding", "stats/display-stats-toggle", nullptr };
    g_mpv.command(g_mpv.handle, args);
}

void PlayerSeek(double percent)
{
    if (!g_mpv.handle)
    {
        return;
    }
    std::string value = std::to_string(percent);
    const char* args[] = { "seek", value.c_str(), "absolute-percent", nullptr };
    g_mpv.command(g_mpv.handle, args);
}

void PlayerSeekRelative(double seconds)
{
    if (!g_mpv.handle) return;
    std::string value = std::to_string(seconds);
    const char* args[] = { "seek", value.c_str(), "relative", nullptr };
    g_mpv.command(g_mpv.handle, args);
}

void PlayerSeekAbsolute(double seconds)
{
    if (!g_mpv.handle) return;
    std::string value = std::to_string((std::max)(0.0, seconds));
    const char* args[] = { "seek", value.c_str(), "absolute", nullptr };
    g_mpv.command(g_mpv.handle, args);
}

void PlayerSeekAbsoluteExact(double seconds)
{
    if (!g_mpv.handle) return;
    std::string value = std::to_string((std::max)(0.0, seconds));
    const char* args[] = { "seek", value.c_str(), "absolute+exact", nullptr };
    g_mpv.command(g_mpv.handle, args);
}

void PlayerChangePlaylistItem(int delta)
{
    if (!g_mpv.handle || delta == 0) return;

    // Weak playlist navigation: at the first/last entry, previous/next is a
    // complete no-op and never forces playback to terminate.
    int64_t playlistCount{};
    if (!g_mpv.getProperty ||
        g_mpv.getProperty(
            g_mpv.handle,
            "playlist-count",
            MpvFormatInt64,
            &playlistCount) < 0 ||
        playlistCount <= 0)
    {
        return;
    }

    int64_t playlistPosition{ -1 };

    // Prefer the item that is actually playing. During normal playback this
    // is the most precise index; fall back to playlist-pos outside that state.
    if (g_mpv.getProperty(
        g_mpv.handle,
        "playlist-playing-pos",
        MpvFormatInt64,
        &playlistPosition) < 0 ||
        playlistPosition < 0)
    {
        if (g_mpv.getProperty(
            g_mpv.handle,
            "playlist-pos",
            MpvFormatInt64,
            &playlistPosition) < 0)
        {
            return;
        }
    }

    if (playlistPosition < 0) return;

    if (delta < 0 && playlistPosition == 0) return;
    if (delta > 0 && playlistPosition >= playlistCount - 1) return;

    PlayerExecuteMpvCommand(delta > 0
        ? L"no-osd playlist-next"
        : L"no-osd playlist-prev");
}

void PlayerChangeChapter(int delta)
{
    if (!g_mpv.handle || delta == 0) return;

    if (PlayerGetMediaChapters().empty())
    {
        PlayerChangePlaylistItem(delta);
        return;
    }

    std::wstring message = PlayerUiString(
        delta > 0 ? L"OsdNextChapter" : L"OsdPreviousChapter",
        delta > 0 ? L"Próximo capítulo" : L"Capítulo anterior");
    PlayerExecuteMpvCommand(
        (delta > 0 ? L"no-osd add chapter 1; show-text \""
                   : L"no-osd add chapter -1; show-text \"") +
        message + L"\"");
}

bool PlayerAdvanceContinuousPlayback()
{
    if (!g_mpv.handle ||
        !g_mpv.getProperty ||
        !g_mpv.freeNodeContents ||
        !g_mpv.commandString)
    {
        return false;
    }

    auto readStringNode = [](std::string const& property, std::wstring& value)
        {
            value.clear();

            MpvNode node{};
            if (g_mpv.getProperty(
                g_mpv.handle,
                property.c_str(),
                MpvFormatNode,
                &node) < 0)
            {
                return false;
            }

            if (node.format == MpvFormatString && node.value.string)
                value = winrt::to_hstring(node.value.string).c_str();

            g_mpv.freeNodeContents(&node);
            return !value.empty();
        };

    // Current item must be an actual local file.
    std::wstring currentPath;
    if (!readStringNode("path", currentPath))
        currentPath = g_currentMediaPath;

    std::error_code currentError;
    if (currentPath.empty() ||
        !std::filesystem::is_regular_file(currentPath, currentError))
    {
        return false;
    }

    int64_t playlistCount{};
    if (g_mpv.getProperty(
        g_mpv.handle,
        "playlist-count",
        MpvFormatInt64,
        &playlistCount) < 0 ||
        playlistCount <= 1)
    {
        return false;
    }

    int64_t playlistPosition{ -1 };
    if (g_mpv.getProperty(
        g_mpv.handle,
        "playlist-playing-pos",
        MpvFormatInt64,
        &playlistPosition) < 0 ||
        playlistPosition < 0)
    {
        if (g_mpv.getProperty(
            g_mpv.handle,
            "playlist-pos",
            MpvFormatInt64,
            &playlistPosition) < 0)
        {
            return false;
        }
    }

    // Same bounded policy as the existing Previous/Next logic.
    if (playlistPosition < 0 ||
        playlistPosition >= playlistCount - 1)
    {
        return false;
    }

    std::wstring nextPath;
    std::string const nextProperty =
        "playlist/" +
        std::to_string(playlistPosition + 1) +
        "/filename";

    if (!readStringNode(nextProperty, nextPath))
        return false;

    std::filesystem::path nextFile{ nextPath };
    if (nextFile.is_relative())
    {
        nextFile =
            std::filesystem::path{ currentPath }.parent_path() /
            nextFile;
    }

    std::error_code nextError;
    if (!std::filesystem::is_regular_file(nextFile, nextError))
    {
        // Mixed local/web playlists never auto-cross into a web source.
        return false;
    }

    // A preserved engine restart may leave a runtime --start value behind.
    // Clear it so every automatic next file begins at 00:00.
    if (g_mpv.setProperty)
        g_mpv.setProperty(g_mpv.handle, "start", "none");

    return g_mpv.commandString(
        g_mpv.handle,
        "no-osd playlist-next") >= 0;
}

void PlayerSetVolume(double volume)
{
    std::string value = std::to_string(static_cast<int>(std::round(volume)));
    g_mpvSettingsManager.Overrides()["volume"] = value;
    ScheduleNativeOptionsSave();
    if (g_mpv.handle)
    {
        g_mpv.setProperty(g_mpv.handle, "volume", value.c_str());
    }
}

bool PlayerAdjustVolumeFromWheel(int wheelDelta)
{
    if (!g_mpv.handle || !g_mpv.commandString || wheelDelta == 0) return false;
    double change = static_cast<double>(wheelDelta) * 2.0 / WHEEL_DELTA;
    if (std::abs(change) < 0.05) return false;
    std::ostringstream command;
    command << "no-osd add volume " << change
        << "; show-text \"Volume: ${volume}%\"";
    if (g_mpv.commandString(g_mpv.handle, command.str().c_str()) < 0) return false;

    double volume{};
    if (g_mpv.getProperty &&
        g_mpv.getProperty(g_mpv.handle, "volume", MpvFormatDouble, &volume) >= 0)
    {
        g_mpvSettingsManager.Overrides()["volume"] = std::to_string(
            static_cast<int>(std::round(volume)));
        ScheduleNativeOptionsSave();
    }
    if (g_mainWindow)
    {
        auto* info = reinterpret_cast<WindowInfo*>(
            GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
        if (info && info->page)
        {
            winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
                info->page)->ShowVolumeFeedback();
        }
    }
    return true;
}

double PlayerGetVolume()
{
    double volume{ 100.0 };
    if (g_mpv.handle && g_mpv.getProperty)
    {
        g_mpv.getProperty(g_mpv.handle, "volume", MpvFormatDouble, &volume);
    }
    return (std::max)(0.0, volume);
}

bool PlayerGetPlaybackTimes(double& positionSeconds, double& durationSeconds)
{
    positionSeconds = 0.0;
    durationSeconds = 0.0;
    if (!g_mpv.handle || !g_mpv.getProperty) return false;

    if (g_mpv.getProperty(
        g_mpv.handle, "time-pos", MpvFormatDouble, &positionSeconds) < 0)
    {
        return false;
    }
    // Streams may not expose a duration. The current time is still valid and
    // useful, so report success while leaving duration at zero.
    g_mpv.getProperty(
        g_mpv.handle, "duration", MpvFormatDouble, &durationSeconds);

    // Compatibility fallback is dormant unless a stored point was accepted for
    // the file being opened. It verifies mpv's real current path before seeking,
    // so the asynchronous loadfile transition cannot touch the outgoing media.
    TryApplyPendingResumeSeek(positionSeconds);

    positionSeconds = (std::max)(0.0, positionSeconds);
    durationSeconds = (std::max)(0.0, durationSeconds);
    return true;
}

bool PlayerGetCurrentLocalMediaPath(std::wstring& path)
{
    path.clear();

    // Prefer mpv's current playlist item so Previous/Next/Shuffle thumbnails
    // always follow the file that is actually playing. Fall back to the host's
    // last opened path if mpv has not exposed the property yet.
    if (g_mpv.handle && g_mpv.getProperty && g_mpv.freeNodeContents)
    {
        MpvNode pathNode{};
        if (g_mpv.getProperty(
            g_mpv.handle,
            "path",
            MpvFormatNode,
            &pathNode) >= 0)
        {
            if (pathNode.format == MpvFormatString && pathNode.value.string)
            {
                path = winrt::to_hstring(pathNode.value.string).c_str();
            }

            g_mpv.freeNodeContents(&pathNode);
        }
    }

    if (path.empty())
    {
        path = g_currentMediaPath;
    }

    if (path.empty())
    {
        return false;
    }

    std::error_code error;
    std::filesystem::path const filePath{ path };
    if (!std::filesystem::is_regular_file(filePath, error))
    {
        path.clear();
        return false;
    }

    path = filePath.wstring();
    return true;
}

bool PlayerIsCurrentMediaImage()
{
    if (!g_mpv.handle || !g_mpv.getProperty)
        return false;

    int imageFlag{};
    if (g_mpv.getProperty(
        g_mpv.handle,
        "current-tracks/video/image",
        MpvFormatFlag,
        &imageFlag) < 0 ||
        imageFlag == 0)
    {
        return false;
    }

    // MPV also marks embedded/external album art as a one-frame image.
    // Album art belongs to an audio item, so it must keep the normal
    // playback timeline/time controls instead of entering HC Player's
    // still-image transport mode.
    int albumArtFlag{};
    if (g_mpv.getProperty(
        g_mpv.handle,
        "current-tracks/video/albumart",
        MpvFormatFlag,
        &albumArtFlag) >= 0 &&
        albumArtFlag != 0)
    {
        return false;
    }

    return true;
}

bool PlayerIsCurrentMediaAudio()
{
    if (!g_mpv.handle || !g_mpv.getProperty)
        return false;

    // Audio media must have a selected audio track. A selected video track is
    // allowed only when MPV explicitly identifies it as album art. This keeps
    // ordinary videos with audio tracks out of the audio-only path.
    int64_t audioTrackId{};
    if (g_mpv.getProperty(
        g_mpv.handle,
        "current-tracks/audio/id",
        MpvFormatInt64,
        &audioTrackId) < 0 ||
        audioTrackId < 0)
    {
        return false;
    }

    int64_t videoTrackId{};
    if (g_mpv.getProperty(
        g_mpv.handle,
        "current-tracks/video/id",
        MpvFormatInt64,
        &videoTrackId) >= 0 &&
        videoTrackId >= 0)
    {
        int albumArtFlag{};
        if (g_mpv.getProperty(
            g_mpv.handle,
            "current-tracks/video/albumart",
            MpvFormatFlag,
            &albumArtFlag) < 0 ||
            albumArtFlag == 0)
        {
            return false;
        }
    }

    return true;
}

void PlayerApplyAudioCoverScalingPolicy()
{
    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.setProperty)
        return;

    // Audio-only media may expose its cover as the selected video track.
    // MPV correctly marks that track as album art. Keep ordinary video and
    // real image files completely outside this policy.
    if (!PlayerIsCurrentMediaAudio())
        return;

    int64_t videoTrackId{};
    if (g_mpv.getProperty(
        g_mpv.handle,
        "current-tracks/video/id",
        MpvFormatInt64,
        &videoTrackId) < 0 ||
        videoTrackId < 0)
    {
        return;
    }

    int albumArtFlag{};
    if (g_mpv.getProperty(
        g_mpv.handle,
        "current-tracks/video/albumart",
        MpvFormatFlag,
        &albumArtFlag) < 0 ||
        albumArtFlag == 0)
    {
        return;
    }

    // Album art should fill as much of the playback surface as possible while
    // remaining completely visible and preserving its aspect ratio. These are
    // FILE-LOCAL properties: MPV restores the previous values automatically at
    // end-of-file, so HC Player's existing photo and real-video presentation is
    // not changed by this audio-only policy.
    auto setLocal = [](char const* option, char const* value)
        {
            std::string property = "file-local-options/";
            property += option;
            g_mpv.setProperty(
                g_mpv.handle,
                property.c_str(),
                value);
        };

    setLocal("video-unscaled", "no");
    setLocal("video-zoom", "0");
    setLocal("video-pan-x", "0");
    setLocal("video-pan-y", "0");
    setLocal("video-scale-x", "1");
    setLocal("video-scale-y", "1");
    setLocal("panscan", "0");
    setLocal("video-aspect-override", "no");
    setLocal("keepaspect", "yes");
}

void PlayerUpdateTaskbarProgress()
{
    if (!g_mainWindow) return;

    if (!g_taskbarList)
    {
        winrt::com_ptr<ITaskbarList3> taskbar;
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(taskbar.put()))) ||
            FAILED(taskbar->HrInit()))
        {
            return;
        }
        g_taskbarList = std::move(taskbar);
    }

    bool paused{ true };
    bool eofReached{};
    bool playbackAvailable = !g_currentMediaPath.empty() &&
        PlayerGetPlaybackState(paused, eofReached);
    UpdateTaskbarThumbnailButtons(
        playbackAvailable, !playbackAvailable || paused || eofReached);

    if (!ConfiguredNativeToggle("taskbar-progress", true) ||
        g_currentMediaPath.empty())
    {
        g_taskbarList->SetProgressState(g_mainWindow, TBPF_NOPROGRESS);
        return;
    }

    double position{};
    double duration{};
    if (!PlayerGetPlaybackTimes(position, duration))
    {
        g_taskbarList->SetProgressState(g_mainWindow, TBPF_NOPROGRESS);
        return;
    }

    if (duration <= 0.0)
    {
        g_taskbarList->SetProgressState(g_mainWindow,
            paused ? TBPF_PAUSED : TBPF_INDETERMINATE);
        return;
    }

    constexpr ULONGLONG scale = 10000;
    ULONGLONG completed = eofReached ? scale : static_cast<ULONGLONG>(
        std::clamp(position / duration, 0.0, 1.0) * scale);
    g_taskbarList->SetProgressState(g_mainWindow,
        paused ? TBPF_PAUSED : TBPF_NORMAL);
    g_taskbarList->SetProgressValue(g_mainWindow, completed, scale);
}

bool PlayerGetWebCacheEnd(double& cacheEndSeconds)
{
    cacheEndSeconds = 0.0;
    if (!g_mpv.handle || !g_mpv.getProperty) return false;

    int viaNetwork{};
    if (g_mpv.getProperty(g_mpv.handle, "demuxer-via-network",
        MpvFormatFlag, &viaNetwork) < 0 || !viaNetwork)
    {
        return false;
    }
    if (g_mpv.getProperty(g_mpv.handle, "demuxer-cache-time",
        MpvFormatDouble, &cacheEndSeconds) < 0 ||
        !std::isfinite(cacheEndSeconds))
    {
        cacheEndSeconds = 0.0;
        return false;
    }
    cacheEndSeconds = (std::max)(0.0, cacheEndSeconds);
    return true;
}

void PlayerUpdateWebBufferingIndicator()
{
    if (!g_mainWindow) return;
    auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
    if (!info || !info->bufferingSource || !info->bufferingRing) return;

    bool visible{};
    if (g_mpv.handle && g_mpv.getProperty)
    {
        int viaNetwork{};
        int pausedForCache{};
        int idleActive{ 1 };
        int seeking{};
        int64_t videoId{ -1 };
        bool network = g_mpv.getProperty(g_mpv.handle,
            "demuxer-via-network", MpvFormatFlag, &viaNetwork) >= 0 &&
            viaNetwork != 0;
        bool buffering = g_mpv.getProperty(g_mpv.handle,
            "paused-for-cache", MpvFormatFlag, &pausedForCache) >= 0 &&
            pausedForCache != 0;
        bool hasVideo = g_mpv.getProperty(g_mpv.handle,
            "current-tracks/video/id", MpvFormatInt64, &videoId) >= 0 &&
            videoId >= 0;
        g_mpv.getProperty(g_mpv.handle,
            "idle-active", MpvFormatFlag, &idleActive);
        bool seekInProgress = g_mpv.getProperty(g_mpv.handle,
            "seeking", MpvFormatFlag, &seeking) >= 0 && seeking != 0;
        bool webVideoRequest = IsWebUrl(g_currentMediaPath) ||
            g_currentMediaPath.starts_with(L"ytdl://");
        // While yt-dlp resolves a page there is no demuxer or video track yet,
        // but MPV is already busy. Include that opening phase. On a load error
        // MPV returns to idle and the indicator disappears automatically.
        bool opening = !hasVideo && idleActive == 0;
        visible = webVideoRequest && (opening || seekInProgress ||
            (network && buffering && hasVideo));
    }

    if (info->bufferingVisible == visible) return;
    info->bufferingVisible = visible;
    info->bufferingRing.IsActive(visible);
    RECT client{};
    GetClientRect(g_mainWindow, &client);
    ApplyClientLayout(g_mainWindow, info,
        client.right - client.left, client.bottom - client.top);
}

bool PlayerGetMediaInfoReport(
    std::wstring& report,
    std::wstring& error)
{
    report.clear();
    error.clear();

    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
    {
        error = L"No media is currently open.";
        return false;
    }

    // Ask mpv for the CURRENT playlist item's path rather than trusting the
    // path originally opened by the user. This keeps MediaInfo correct after
    // Previous, Next, playlist navigation and Shuffle.
    std::wstring currentPath;

    MpvNode pathNode{};
    if (g_mpv.getProperty(
        g_mpv.handle,
        "path",
        MpvFormatNode,
        &pathNode) >= 0)
    {
        if (pathNode.format == MpvFormatString && pathNode.value.string)
        {
            currentPath =
                winrt::to_hstring(pathNode.value.string).c_str();
        }

        g_mpv.freeNodeContents(&pathNode);
    }

    if (currentPath.empty())
    {
        currentPath = g_currentMediaPath;
    }

    if (g_currentMediaIsDisc)
    {
        error = L"Detailed MediaInfo information is not available for optical discs.";
        return false;
    }

    if (currentPath.empty())
    {
        error = L"No media file is available for analysis.";
        return false;
    }

    // MediaInfo's direct file API is used only for real local files.
    // Protocol sources (dvd://, bd://, http(s)://, ytdl://, etc.) remain
    // outside this path and can receive a dedicated implementation later.
    std::error_code fileError;
    std::filesystem::path const filePath{ currentPath };

    if (!std::filesystem::is_regular_file(filePath, fileError))
    {
        error =
            L"As informações detalhadas do MediaInfo estão disponíveis apenas para arquivos locais.";
        return false;
    }

    return MediaInfoBridge::AnalyzeFile(
        filePath.wstring(),
        report,
        error);
}

bool PlayerGetMediaInfoAnalysis(
    MediaInfoBridge::Analysis& analysis,
    std::wstring& error)
{
    analysis = {};
    error.clear();

    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
    {
        error = L"No media is currently open.";
        return false;
    }

    // Query mpv for the CURRENT playlist item, so Previous / Next / Shuffle
    // always analyze the media that is actually playing.
    std::wstring currentPath;

    MpvNode pathNode{};
    if (g_mpv.getProperty(
        g_mpv.handle,
        "path",
        MpvFormatNode,
        &pathNode) >= 0)
    {
        if (pathNode.format == MpvFormatString && pathNode.value.string)
        {
            currentPath =
                winrt::to_hstring(pathNode.value.string).c_str();
        }

        g_mpv.freeNodeContents(&pathNode);
    }

    if (currentPath.empty())
    {
        currentPath = g_currentMediaPath;
    }

    if (g_currentMediaIsDisc)
    {
        error = L"Detailed MediaInfo information is not available for optical discs.";
        return false;
    }

    if (currentPath.empty())
    {
        error = L"No media file is available for analysis.";
        return false;
    }

    std::error_code fileError;
    std::filesystem::path const filePath{ currentPath };

    if (!std::filesystem::is_regular_file(filePath, fileError))
    {
        error =
            L"As informações detalhadas do MediaInfo estão disponíveis apenas para arquivos locais.";
        return false;
    }

    return MediaInfoBridge::AnalyzeFileStructured(
        filePath.wstring(),
        analysis,
        error);
}

bool PlayerGetCurrentAudioArtist(std::wstring& artist)
{
    artist.clear();

    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
    {
        return false;
    }

    // A real audio item must have a selected audio track. If a selected
    // video track also exists, accept it only when MPV marks it as album art.
    // This keeps ordinary videos (even videos with an Artist tag) on the
    // existing chapter-title path.
    int64_t audioTrackId{};
    if (g_mpv.getProperty(
        g_mpv.handle,
        "current-tracks/audio/id",
        MpvFormatInt64,
        &audioTrackId) < 0)
    {
        return false;
    }

    int64_t videoTrackId{};
    if (g_mpv.getProperty(
        g_mpv.handle,
        "current-tracks/video/id",
        MpvFormatInt64,
        &videoTrackId) >= 0)
    {
        int albumArtFlag{};
        if (g_mpv.getProperty(
            g_mpv.handle,
            "current-tracks/video/albumart",
            MpvFormatFlag,
            &albumArtFlag) < 0 ||
            albumArtFlag == 0)
        {
            return false;
        }
    }

    // We deliberately do not synthesize an "unknown artist" label. An audio
    // file without an Artist tag simply leaves the existing second line hidden.
    constexpr char const* properties[] = {
        "filtered-metadata/by-key/artist",
        "metadata/by-key/artist"
    };

    for (auto const* property : properties)
    {
        MpvNode node{};
        if (g_mpv.getProperty(g_mpv.handle, property, MpvFormatNode, &node) < 0)
        {
            continue;
        }

        if (node.format == MpvFormatString && node.value.string)
        {
            artist = Trim(winrt::to_hstring(node.value.string).c_str());
        }

        g_mpv.freeNodeContents(&node);
        if (!artist.empty()) break;
    }

    // true means the current item is audio-only (possibly with cover art).
    // artist may intentionally be empty when the file has no Artist metadata.
    return true;
}

std::wstring PlayerGetMediaTitle()
{
    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
    {
        return {};
    }

    // yt-dlp places the actual page title in MPV's metadata map. Prefer it
    // over media-title because media-title can temporarily fall back to the
    // URL while the online source is being resolved.
    constexpr char const* properties[] = {
        "filtered-metadata/by-key/title",
        "metadata/by-key/title",
        "media-title"
    };
    for (auto const* property : properties)
    {
        MpvNode node{};
        if (g_mpv.getProperty(g_mpv.handle, property, MpvFormatNode, &node) < 0)
        {
            continue;
        }
        std::wstring title;
        if (node.format == MpvFormatString && node.value.string)
        {
            title = Trim(winrt::to_hstring(node.value.string).c_str());
        }
        g_mpv.freeNodeContents(&node);
        if (!title.empty()) return title;
    }
    return {};
}

std::vector<MediaChapterOption> PlayerGetMediaChapters()
{
    std::vector<MediaChapterOption> chapters;
    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
    {
        return chapters;
    }

    MpvNode root{};
    if (g_mpv.getProperty(g_mpv.handle, "chapter-list", MpvFormatNode, &root) < 0)
    {
        return chapters;
    }
    if (root.format != MpvFormatNodeArray || !root.value.list)
    {
        g_mpv.freeNodeContents(&root);
        return chapters;
    }

    for (int index = 0; index < root.value.list->count; ++index)
    {
        auto const& entry = root.value.list->values[index];
        if (entry.format != MpvFormatNodeMap || !entry.value.list) continue;

        MediaChapterOption chapter{};
        bool hasTime{};
        for (int field = 0; field < entry.value.list->count; ++field)
        {
            char const* key = entry.value.list->keys
                ? entry.value.list->keys[field] : nullptr;
            if (!key) continue;
            auto const& value = entry.value.list->values[field];
            if (strcmp(key, "time") == 0)
            {
                if (value.format == MpvFormatDouble)
                {
                    chapter.time = value.value.number;
                    hasTime = true;
                }
                else if (value.format == MpvFormatInt64)
                {
                    chapter.time = static_cast<double>(value.value.integer);
                    hasTime = true;
                }
            }
            else if (strcmp(key, "title") == 0 &&
                value.format == MpvFormatString && value.value.string)
            {
                chapter.title = winrt::to_hstring(value.value.string).c_str();
            }
        }
        if (hasTime) chapters.push_back(std::move(chapter));
    }
    g_mpv.freeNodeContents(&root);
    std::sort(chapters.begin(), chapters.end(), [](auto const& left, auto const& right)
        {
            return left.time < right.time;
        });
    return chapters;
}

std::vector<MediaEditionOption> PlayerGetMediaEditions()
{
    std::vector<MediaEditionOption> result;
    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
        return result;

    auto readList = [&](char const* listProperty, char const* selectedProperty,
        bool discTitles)
        {
            int64_t selectedId{ -1 };

            // Since mpv 0.31, "edition" reflects the user-set option/property
            // (often still "auto"), while "current-edition" reports the
            // edition/title actually selected at runtime. This matters for
            // optical media exposed through edition-list, where otherwise no
            // item may be marked as currently selected.
            int selectedResult = -1;
            if (!discTitles && strcmp(listProperty, "edition-list") == 0)
            {
                selectedResult = g_mpv.getProperty(
                    g_mpv.handle, "current-edition",
                    MpvFormatInt64, &selectedId);
            }

            // Preserve the existing property as a compatibility fallback.
            if (selectedResult < 0)
            {
                g_mpv.getProperty(g_mpv.handle, selectedProperty,
                    MpvFormatInt64, &selectedId);
            }

            MpvNode root{};
            if (g_mpv.getProperty(g_mpv.handle, listProperty,
                MpvFormatNode, &root) < 0)
            {
                return;
            }
            if (root.format != MpvFormatNodeArray || !root.value.list)
            {
                g_mpv.freeNodeContents(&root);
                return;
            }

            for (int index = 0; index < root.value.list->count; ++index)
            {
                auto const& entry = root.value.list->values[index];
                if (entry.format != MpvFormatNodeMap || !entry.value.list) continue;

                MediaEditionOption option{};
                option.id = index;
                option.discTitle = discTitles;
                for (int field = 0; field < entry.value.list->count; ++field)
                {
                    char const* key = entry.value.list->keys
                        ? entry.value.list->keys[field] : nullptr;
                    if (!key) continue;
                    auto const& value = entry.value.list->values[field];
                    if ((strcmp(key, "id") == 0 || strcmp(key, "title-id") == 0) &&
                        value.format == MpvFormatInt64)
                    {
                        option.id = value.value.integer;
                    }
                    else if (strcmp(key, "title") == 0 &&
                        value.format == MpvFormatString && value.value.string)
                    {
                        option.title = winrt::to_hstring(value.value.string).c_str();

                        // mpv labels optical-disc entries as "title: N".
                        // Localize only that presentation string; native IDs,
                        // duration and ordinary container edition names stay intact.
                        if (discTitles && option.title.starts_with(L"title:"))
                        {
                            option.title.replace(
                                0,
                                6,
                                PlayerUiString(
                                    L"DiscTitlePrefix", L"Título:"));
                        }
                    }
                    else if ((strcmp(key, "duration") == 0 ||
                        strcmp(key, "length") == 0))
                    {
                        if (value.format == MpvFormatDouble)
                            option.duration = value.value.number;
                        else if (value.format == MpvFormatInt64)
                            option.duration = static_cast<double>(value.value.integer);
                    }
                    else if (strcmp(key, "default") == 0 &&
                        value.format == MpvFormatFlag)
                    {
                        option.defaultEdition = value.value.flag != 0;
                    }
                }
                option.selected = option.id == selectedId;
                result.push_back(std::move(option));
            }
            g_mpv.freeNodeContents(&root);
        };

    // Optical-disc titles are the useful "editions" for DVD/Blu-ray. For
    // ordinary containers, fall back to mpv's native edition list (MKV, etc.).
    readList("disc-title-list", "disc-title", true);
    if (result.empty()) readList("edition-list", "edition", false);
    return result;
}

bool PlayerSelectMediaEdition(bool discTitle, int64_t id)
{
    if (!g_mpv.handle || id < 0) return false;
    if (discTitle && g_currentMediaIsDisc && !g_currentMediaPath.empty())
    {
        bool selected = PlayerLoadOpticalDisc(
            g_currentMediaPath, g_currentDiscIsBluray, id, false);
        if (selected)
        {
            PlayerExecuteMpvCommand(
                L"show-text \"" +
                PlayerUiString(L"OsdTitlePrefix", L"Título: ") +
                std::to_wstring(id + 1) + L"\"");
        }
        return selected;
    }

    std::string value = std::to_string(id);
    bool selected = g_mpv.setProperty(g_mpv.handle,
        discTitle ? "disc-title" : "edition", value.c_str()) >= 0;
    if (selected)
    {
        PlayerExecuteMpvCommand(
            L"show-text \"" +
            PlayerUiString(
                discTitle ? L"OsdTitlePrefix" : L"OsdEditionPrefix",
                discTitle ? L"Título: " : L"Edição: ") +
            std::to_wstring(id + 1) + L"\"");
    }
    return selected;
}

std::vector<MediaPlaylistItem> PlayerGetPlaylistItems()
{
    std::vector<MediaPlaylistItem> result;
    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
        return result;

    int64_t currentPosition{ -1 };
    g_mpv.getProperty(g_mpv.handle, "playlist-pos",
        MpvFormatInt64, &currentPosition);
    MpvNode root{};
    if (g_mpv.getProperty(g_mpv.handle, "playlist", MpvFormatNode, &root) < 0)
        return result;
    if (root.format != MpvFormatNodeArray || !root.value.list)
    {
        g_mpv.freeNodeContents(&root);
        return result;
    }

    for (int index = 0; index < root.value.list->count; ++index)
    {
        auto const& entry = root.value.list->values[index];
        if (entry.format != MpvFormatNodeMap || !entry.value.list) continue;
        MediaPlaylistItem item{};
        item.index = index;
        item.current = index == currentPosition;
        for (int field = 0; field < entry.value.list->count; ++field)
        {
            char const* key = entry.value.list->keys
                ? entry.value.list->keys[field] : nullptr;
            if (!key) continue;
            auto const& value = entry.value.list->values[field];
            if (strcmp(key, "filename") == 0 &&
                value.format == MpvFormatString && value.value.string)
            {
                item.filename = winrt::to_hstring(value.value.string).c_str();
            }
            else if (strcmp(key, "title") == 0 &&
                value.format == MpvFormatString && value.value.string)
            {
                item.title = winrt::to_hstring(value.value.string).c_str();
            }
            else if ((strcmp(key, "current") == 0 ||
                strcmp(key, "playing") == 0) &&
                value.format == MpvFormatFlag && value.value.flag)
            {
                item.current = true;
            }
        }

        if (!item.filename.empty())
        {
            try
            {
                std::filesystem::path path{ item.filename };
                auto stem = path.stem().wstring();
                if (!stem.empty() && item.filename.find(L"://") == std::wstring::npos)
                    item.title = std::move(stem);
                else if (item.title.empty())
                    item.title = std::move(stem);
                item.format = path.extension().wstring();
                if (!item.format.empty() && item.format.front() == L'.')
                    item.format.erase(item.format.begin());
                std::transform(item.format.begin(), item.format.end(),
                    item.format.begin(), towupper);
            }
            catch (...) {}
        }
        if (item.title.empty()) item.title = item.filename;
        if (item.title.empty()) item.title = L"Item " + std::to_wstring(index + 1);
        result.push_back(std::move(item));
    }
    g_mpv.freeNodeContents(&root);
    return result;
}

bool PlayerPlayPlaylistItem(int64_t index)
{
    if (!g_mpv.handle || index < 0) return false;
    std::string value = std::to_string(index);
    const char* command[] = { "playlist-play-index", value.c_str(), nullptr };
    if (g_mpv.command(g_mpv.handle, command) >= 0) return true;
    return g_mpv.setProperty(g_mpv.handle, "playlist-pos", value.c_str()) >= 0;
}

bool PlayerRemovePlaylistItem(int64_t index)
{
    // Keep playlist mutation inside mpv: the HC Player queue is only a visual
    // controller over mpv's native playlist and never maintains a second copy.
    if (!g_mpv.handle || !g_mpv.command || index < 0) return false;

    std::string value = std::to_string(index);
    const char* command[] = { "playlist-remove", value.c_str(), nullptr };
    return g_mpv.command(g_mpv.handle, command) >= 0;
}

bool PlayerClearPlaylistExceptCurrent()
{
    // mpv owns the queue clear semantics: playlist-clear removes every playlist
    // entry except the file currently being played. Refuse the operation if no
    // current entry exists, so this bridge always honors its "except current"
    // contract even while the player is transitioning or idle.
    if (!g_mpv.handle || !g_mpv.command) return false;

    auto const playlist = PlayerGetPlaylistItems();
    if (playlist.size() <= 1) return true;
    if (std::none_of(playlist.begin(), playlist.end(),
        [](MediaPlaylistItem const& item) { return item.current; }))
    {
        return false;
    }

    const char* command[] = { "playlist-clear", nullptr };
    return g_mpv.command(g_mpv.handle, command) >= 0;
}

bool PlayerMovePlaylistItem(int64_t fromIndex, int64_t finalIndex)
{
    // Reordering stays entirely inside mpv's native playlist. The public bridge
    // accepts the final visual index so PlaylistPage never has to depend on
    // playlist-move's unusual target-entry semantics.
    if (!g_mpv.handle || !g_mpv.command || fromIndex < 0 || finalIndex < 0)
        return false;

    auto const playlist = PlayerGetPlaylistItems();
    int64_t const count = static_cast<int64_t>(playlist.size());
    if (fromIndex >= count || finalIndex >= count) return false;
    if (fromIndex == finalIndex) return true;

    auto moveEntry = [](int64_t source, int64_t target)
    {
        std::string sourceValue = std::to_string(source);
        std::string targetValue = std::to_string(target);
        const char* command[] = {
            "playlist-move", sourceValue.c_str(), targetValue.c_str(), nullptr };
        return g_mpv.command(g_mpv.handle, command) >= 0;
    };

    if (fromIndex > finalIndex)
    {
        // Moving upward is direct: when source is after target, mpv places the
        // source at the exact target index.
        return moveEntry(fromIndex, finalIndex);
    }

    if (finalIndex < count - 1)
    {
        // When source is before target, mpv's second argument names the target
        // entry rather than the source's final index. Target one entry beyond
        // the desired final position to compensate for the source removal.
        return moveEntry(fromIndex, finalIndex + 1);
    }

    // Moving downward all the way to the last slot has no target entry after
    // that slot. Shift each following entry one place before the dragged item;
    // this produces the same final order without using an out-of-range index.
    for (int64_t position = fromIndex; position < finalIndex; ++position)
    {
        if (!moveEntry(position + 1, position)) return false;
    }
    return true;
}

bool PlayerAddPlaylistFiles(std::vector<std::wstring> const& droppedFiles)
{
    // Central append path shared by the native file picker and Explorer drops.
    // Keep mpv as the single source of truth: an existing current item is never
    // replaced, restarted, paused or seeked when files are appended.
    std::vector<std::wstring> paths;
    paths.reserve(droppedFiles.size());
    for (auto const& value : droppedFiles)
    {
        if (value.empty()) continue;
        std::filesystem::path path{ value };
        if (IsPlayableFolderFile(path)) paths.push_back(value);
    }
    if (paths.empty()) return false;

    // If the queue is genuinely empty, reuse HC Player's established multi-file
    // open path so the first file becomes current and all remaining files are
    // appended exactly once.
    if (!g_mpv.handle || PlayerGetPlaylistItems().empty())
    {
        return PlayerOpenDroppedMedia(paths);
    }

    bool appendedAny = false;
    for (auto const& path : paths)
    {
        if (!g_mpv.handle || !g_mpv.command) break;
        std::string target = winrt::to_string(MpvLoadTarget(path));
        const char* append[] = { "loadfile", target.c_str(), "append", nullptr };
        if (g_mpv.command(g_mpv.handle, append) >= 0)
        {
            appendedAny = true;
            AddRecentFile(path);
        }
    }
    return appendedAny;
}

bool PlayerAddPlaylistFilesFromDialog()
{
    // Use the same native Windows picker style and media groups as the normal
    // Open command, but allow multiple selection and append to mpv's existing
    // playlist. The currently playing item is never replaced by this path.
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.put()))))
    {
        return false;
    }

    FILEOPENDIALOGOPTIONS options{};
    if (FAILED(dialog->GetOptions(&options))) return false;
    options |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
        FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT;
    if (FAILED(dialog->SetOptions(options))) return false;

    auto mediaFilesLabel = PlayerUiString(
        L"MainPageDynMediaFiles", L"Arquivos de mídia");
    auto videoFilesLabel = PlayerUiString(
        L"MainPageDynVideoFiles", L"Arquivos de vídeo");
    auto audioFilesLabel = PlayerUiString(
        L"MainPageDynAudioFiles", L"Arquivos de áudio");
    auto playlistsLabel = PlayerUiString(
        L"PlaylistAddPlaylistsFilter", L"Playlists");
    auto imageFilesLabel = PlayerUiString(
        L"MainPageDynImageFiles", L"Arquivos de imagem");
    auto allFilesLabel = PlayerUiString(
        L"MainPageDynAllFiles", L"Todos os arquivos");

    // Disc images are intentionally omitted here: opening ISO media in HC Player
    // uses a dedicated DVD/Blu-ray path and is not a normal mpv playlist entry.
    COMDLG_FILTERSPEC filters[] = {
        { mediaFilesLabel.c_str(), L"*.mkv;*.mk3d;*.mp4;*.m4v;*.mov;*.webm;*.avi;*.wmv;*.asf;*.flv;*.ts;*.mts;*.m2ts;*.mpg;*.mpeg;*.vob;*.ogv;*.rm;*.rmvb;*.3gp;*.3g2;*.divx;*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.oga;*.opus;*.wav;*.wma;*.aiff;*.aif;*.ape;*.mka;*.ac3;*.eac3;*.dts;*.dtshd;*.spx;*.tak;*.tta;*.wv;*.mid;*.midi;*.mod;*.xm;*.s3m;*.it;*.m3u;*.m3u8;*.pls;*.cue;*.avif;*.bmp;*.gif;*.jpeg;*.jpg;*.jxl;*.png;*.svg;*.tga;*.tif;*.tiff;*.webp" },
        { videoFilesLabel.c_str(), L"*.mkv;*.mk3d;*.mp4;*.m4v;*.mov;*.webm;*.avi;*.wmv;*.asf;*.flv;*.ts;*.mts;*.m2ts;*.mpg;*.mpeg;*.vob;*.ogv;*.rm;*.rmvb;*.3gp;*.3g2;*.divx" },
        { audioFilesLabel.c_str(), L"*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.oga;*.opus;*.wav;*.wma;*.aiff;*.aif;*.ape;*.mka;*.ac3;*.eac3;*.dts;*.dtshd;*.spx;*.tak;*.tta;*.wv;*.mid;*.midi;*.mod;*.xm;*.s3m;*.it" },
        { playlistsLabel.c_str(), L"*.m3u;*.m3u8;*.pls;*.cue" },
        { imageFilesLabel.c_str(), L"*.avif;*.bmp;*.gif;*.jpeg;*.jpg;*.jxl;*.png;*.svg;*.tga;*.tif;*.tiff;*.webp" },
        { allFilesLabel.c_str(), L"*.*" }
    };
    if (FAILED(dialog->SetFileTypes(ARRAYSIZE(filters), filters))) return false;
    dialog->SetFileTypeIndex(1);

    auto dialogTitle = PlayerUiString(
        L"PlaylistAddDialogTitle", L"Adicionar arquivos à fila");
    dialog->SetTitle(dialogTitle.c_str());

    if (FAILED(dialog->Show(g_mainWindow))) return false;

    winrt::com_ptr<IShellItemArray> results;
    if (FAILED(dialog->GetResults(results.put()))) return false;

    DWORD count{};
    if (FAILED(results->GetCount(&count)) || count == 0) return false;

    std::vector<std::wstring> paths;
    paths.reserve(count);
    for (DWORD index = 0; index < count; ++index)
    {
        winrt::com_ptr<IShellItem> item;
        if (FAILED(results->GetItemAt(index, item.put()))) continue;

        PWSTR rawPath{};
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))) continue;
        std::wstring path{ rawPath ? rawPath : L"" };
        CoTaskMemFree(rawPath);
        if (!path.empty() && IsPlayableFolderFile(std::filesystem::path{ path }))
        {
            paths.push_back(std::move(path));
        }
    }
    return PlayerAddPlaylistFiles(paths);
}

bool PlayerAddPlaylistFolderFromDialog()
{
    // Pick one filesystem folder, enumerate only its direct children, and pass
    // the resulting media files through the same append path used by the file
    // picker and Explorer drop. Subfolders are intentionally not traversed.
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.put()))))
    {
        return false;
    }

    FILEOPENDIALOGOPTIONS options{};
    if (FAILED(dialog->GetOptions(&options))) return false;
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_PICKFOLDERS;
    if (FAILED(dialog->SetOptions(options))) return false;

    auto dialogTitle = PlayerUiString(
        L"PlaylistAddFolderDialogTitle", L"Adicionar pasta à fila");
    dialog->SetTitle(dialogTitle.c_str());

    if (FAILED(dialog->Show(g_mainWindow))) return false;

    winrt::com_ptr<IShellItem> result;
    if (FAILED(dialog->GetResult(result.put()))) return false;

    PWSTR rawPath{};
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)))
        return false;

    std::filesystem::path folder{ rawPath ? rawPath : L"" };
    CoTaskMemFree(rawPath);

    std::error_code error;
    if (folder.empty() || !std::filesystem::is_directory(folder, error))
        return false;

    std::vector<std::wstring> paths;
    std::filesystem::directory_iterator iterator{
        folder,
        std::filesystem::directory_options::skip_permission_denied,
        error };
    std::filesystem::directory_iterator end{};

    while (!error && iterator != end)
    {
        std::error_code fileError;
        if (iterator->is_regular_file(fileError) && !fileError)
        {
            auto const path = iterator->path();
            if (IsPlayableFolderFile(path)) paths.push_back(path.wstring());
        }

        iterator.increment(error);
    }

    if (paths.empty()) return false;

    // Match Explorer's familiar logical ordering: Episode 2 sorts before
    // Episode 10. shlwapi is already used and linked by HC Player.
    std::stable_sort(paths.begin(), paths.end(),
        [](std::wstring const& left, std::wstring const& right)
        {
            std::filesystem::path const leftPath{ left };
            std::filesystem::path const rightPath{ right };
            int const logical = StrCmpLogicalW(
                leftPath.filename().c_str(), rightPath.filename().c_str());
            if (logical != 0) return logical < 0;
            return _wcsicmp(left.c_str(), right.c_str()) < 0;
        });

    return PlayerAddPlaylistFiles(paths);
}

double PlayerGetPlaybackSpeed()
{
    if (!g_mpv.handle || !g_mpv.getProperty) return 1.0;
    double speed = 1.0;
    if (g_mpv.getProperty(g_mpv.handle, "speed", MpvFormatDouble, &speed) < 0)
    {
        return 1.0;
    }
    return speed;
}

void PlayerSetPlaybackSpeed(double speed)
{
    if (!g_mpv.handle || !g_mpv.setProperty) return;
    speed = (std::max)(0.05, (std::min)(100.0, speed));
    std::string value = std::to_string(speed);
    g_mpv.setProperty(g_mpv.handle, "speed", value.c_str());
    std::wostringstream label;
    label << std::fixed << std::setprecision(2) << speed;
    std::wstring display = label.str();
    while (!display.empty() && display.back() == L'0') display.pop_back();
    if (!display.empty() && display.back() == L'.') display.pop_back();
    std::wstring decimalSeparator =
        PlayerUiString(L"OsdDecimalSeparator", L",");
    if (!decimalSeparator.empty() && decimalSeparator.front() != L'.')
    {
        std::replace(
            display.begin(), display.end(), L'.', decimalSeparator.front());
    }
    PlayerExecuteMpvCommand(
        L"show-text \"" +
        PlayerUiString(L"OsdSpeedPrefix", L"Velocidade: ") +
        display + L"×\"");
}

PlayerEngineVersionInfo PlayerGetEngineVersionInfo()
{
    PlayerEngineVersionInfo info{};

    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
    {
        return info;
    }

    auto readStringProperty = [](char const* property) -> std::wstring
        {
            MpvNode node{};
            if (g_mpv.getProperty(
                g_mpv.handle,
                property,
                MpvFormatNode,
                &node) < 0)
            {
                return {};
            }

            std::wstring value;
            if (node.format == MpvFormatString && node.value.string)
            {
                value = winrt::to_hstring(node.value.string).c_str();
            }
            g_mpv.freeNodeContents(&node);
            return value;
        };

    // These are read-only mpv runtime properties. Keeping them in the backend
    // lets the About page report the exact playback engine that is actually
    // loaded by HC Player rather than a hard-coded build description.
    info.mpv = readStringProperty("mpv-version");
    info.ffmpeg = readStringProperty("ffmpeg-version");
    info.libplacebo = readStringProperty("libplacebo-version");
    info.libass = readStringProperty("libass-version");
    return info;
}

bool PlayerEngineReady()
{
    return g_mpv.handle != nullptr;
}

bool PlayerIsMediaPresentationReady()
{
    if (g_currentMediaPath.empty() ||
        !g_mpv.handle || !g_mpv.getProperty)
    {
        return false;
    }

    // A successful loadfile command only means mpv accepted the request.
    // Keep transport input gated while mpv is still idle/opening. This is a
    // read-only query and does not participate in playback or rendering.
    int idleActive{ 1 };
    if (g_mpv.getProperty(
        g_mpv.handle, "idle-active", MpvFormatFlag, &idleActive) < 0 ||
        idleActive != 0)
    {
        return false;
    }

    int64_t videoId{ -1 };
    bool const hasVideo =
        g_mpv.getProperty(
            g_mpv.handle,
            "current-tracks/video/id",
            MpvFormatInt64,
            &videoId) >= 0 &&
        videoId >= 0;

    if (hasVideo)
    {
        // video-out-params appears only after the current video output has
        // been configured. This is deliberately later than ShowWindow(),
        // which can precede the first presented video frame by a noticeable
        // amount during a cold open.
        int64_t videoWidth{};
        int64_t videoHeight{};
        return ReadCurrentVideoDisplaySize(videoWidth, videoHeight);
    }

    int64_t audioId{ -1 };
    return g_mpv.getProperty(
        g_mpv.handle,
        "current-tracks/audio/id",
        MpvFormatInt64,
        &audioId) >= 0 &&
        audioId >= 0;
}

bool PlayerIsConsoleOpen()
{
    if (!g_mpv.handle || !g_mpv.getProperty)
    {
        return false;
    }

    int open{};
    return g_mpv.getProperty(
        g_mpv.handle,
        "user-data/mpv/console/open",
        MpvFormatFlag,
        &open) >= 0 && open != 0;
}

double PlayerGetPositionPercent()
{
    if (!g_mpv.handle || !g_mpv.getProperty)
    {
        return -1.0;
    }

    double position{};
    if (g_mpv.getProperty(g_mpv.handle, "percent-pos", MpvFormatDouble, &position) < 0)
    {
        return -1.0;
    }

    return max(0.0, min(100.0, position));
}

void PlayerShowSettings()
{
    if (g_pictureInPicture)
    {
        // The 520 px settings pane is intentionally a normal-window feature.
        PlayerTogglePictureInPicture();
    }
    if (g_mainWindow)
    {
        PostMessageW(g_mainWindow, ShowSettingsMessage, 0, 0);
    }
}

void PlayerCloseSettings()
{
    if (g_mainWindow)
    {
        PostMessageW(g_mainWindow, CloseSettingsMessage, 0, 0);
    }
}

void PlayerShowMediaInfo()
{
    if (g_pictureInPicture)
    {
        // Keep the 520 px information pane as a normal-window feature,
        // matching Settings.
        PlayerTogglePictureInPicture();
    }

    if (g_mainWindow)
    {
        PostMessageW(g_mainWindow, ShowMediaInfoMessage, 0, 0);
    }
}

void PlayerCloseMediaInfo()
{
    if (g_mainWindow)
    {
        PostMessageW(g_mainWindow, CloseMediaInfoMessage, 0, 0);
    }
}


void PlayerShowPlaylist()
{
    if (g_pictureInPicture)
    {
        // Keep the full-height queue panel as a normal-window feature,
        // matching MediaInfo and Settings.
        PlayerTogglePictureInPicture();
    }

    if (g_mainWindow)
    {
        PostMessageW(g_mainWindow, ShowPlaylistMessage, 0, 0);
    }
}

void PlayerClosePlaylist()
{
    if (g_mainWindow)
    {
        PostMessageW(g_mainWindow, ClosePlaylistMessage, 0, 0);
    }
}

void* PlayerGetMainWindowHandle()
{
    return g_mainWindow;
}

void PlayerShowOpenDialog()
{
    if (g_mainWindow)
    {
        PostMessageW(g_mainWindow, CloseContextMenuMessage, 0, 0);
        PostMessageW(g_mainWindow, OpenFileMessage, 0, 0);
    }
}

void PlayerShowOpenFolderDialog()
{
    if (g_mainWindow)
    {
        PostMessageW(g_mainWindow, CloseContextMenuMessage, 0, 0);
        PostMessageW(g_mainWindow, OpenFolderMessage, 0, 0);
    }
}

void PlayerShowOpenDiscImageDialog(bool bluray)
{
    if (g_mainWindow)
    {
        PostMessageW(g_mainWindow, CloseContextMenuMessage, 0, 0);
        PostMessageW(g_mainWindow, OpenDiscImageMessage, bluray ? 1 : 0, 0);
    }
}

void PlayerShowAddExternalAudioDialog()
{
    if (g_mainWindow && g_mpv.handle && !g_currentMediaPath.empty())
    {
        PostMessageW(g_mainWindow, CloseContextMenuMessage, 0, 0);
        PostMessageW(g_mainWindow, AddExternalAudioMessage, 0, 0);
    }
}

void PlayerShowAddExternalSubtitleDialog()
{
    if (g_mainWindow && g_mpv.handle && !g_currentMediaPath.empty())
    {
        PostMessageW(g_mainWindow, CloseContextMenuMessage, 0, 0);
        PostMessageW(g_mainWindow, AddExternalSubtitleMessage, 0, 0);
    }
}

bool PlayerOpenClipboardMedia()
{
    if (!OpenClipboard(g_mainWindow)) return false;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data)
    {
        CloseClipboard();
        return false;
    }

    wchar_t const* text = static_cast<wchar_t const*>(GlobalLock(data));
    std::wstring value = text ? text : L"";
    if (text) GlobalUnlock(data);
    CloseClipboard();

    value = Trim(value);
    value.erase(std::remove(value.begin(), value.end(), L'\r'), value.end());
    value.erase(std::remove(value.begin(), value.end(), L'\n'), value.end());
    if (value.empty()) return false;

    bool recognized = value.starts_with(L"http://") ||
        value.starts_with(L"https://") || value.starts_with(L"rtsp://") ||
        value.starts_with(L"rtmp://") || value.starts_with(L"rtmps://") ||
        value.starts_with(L"ftp://") || value.starts_with(L"magnet:") ||
        (value.size() > 2 && iswalpha(value[0]) && value[1] == L':') ||
        value.starts_with(L"\\\\");
    if (!recognized) return false;
    return PlayerOpenRecentFile(value);
}

bool PlayerOpenDroppedMedia(std::vector<std::wstring> const& droppedItems)
{
    std::vector<std::wstring> items;
    for (auto value : droppedItems)
    {
        value = Trim(std::move(value));
        bool url = value.starts_with(L"http://") ||
            value.starts_with(L"https://") || value.starts_with(L"rtsp://") ||
            value.starts_with(L"rtmp://") || value.starts_with(L"rtmps://") ||
            value.starts_with(L"ftp://") || value.starts_with(L"magnet:");
        std::error_code error;
        bool file = !url && std::filesystem::is_regular_file(value, error);
        if (url || file) items.push_back(std::move(value));
    }
    if (items.empty()) return false;

    g_suppressAutoload = items.size() > 1;
    bool opened = PlayerOpenRecentFile(items.front());
    g_suppressAutoload = false;
    if (!opened) return false;

    for (size_t index = 1; index < items.size(); ++index)
    {
        if (!g_mpv.handle || !g_mpv.command) break;
        std::string path = winrt::to_string(MpvLoadTarget(items[index]));
        const char* append[] = { "loadfile", path.c_str(), "append", nullptr };
        if (g_mpv.command(g_mpv.handle, append) >= 0)
            AddRecentFile(items[index]);
    }
    return true;
}

void PlayerCloseContextMenu()
{
    if (g_mainWindow) PostMessageW(g_mainWindow, CloseContextMenuMessage, 0, 0);
}

void PlayerQuitApp()
{
    if (g_mainWindow) PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
}

bool PlayerOpenRecentFile(std::wstring const& path)
{
    if (!g_mainWindow || path.empty()) return false;
    auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
    if (info && info->page)
    {
        return winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
            info->page)->OpenPath(path);
    }
    return PlayerLoadFile(path);
}

std::vector<RecentMediaItem> PlayerGetRecentFiles()
{
    // Opening Recentes is a safe metadata checkpoint: if yt-dlp has already
    // resolved the current page, persist its human-readable title now.
    CaptureCurrentRecentTitle();
    auto const recentItems = g_recentMediaManager.GetItems();
    std::vector<RecentMediaItem> result;
    for (auto const& item : recentItems)
    {
        bool protocol = item.path.find(L"://") != std::wstring::npos ||
            item.path.starts_with(L"magnet:");
        std::filesystem::path path{ item.path };
        std::wstring title = protocol
            ? (item.title.empty() ? item.path : item.title)
            : path.filename().wstring();
        if (title.empty()) title = item.path;
        constexpr size_t maximumRecentTitleLength = 42;
        if (title.size() > maximumRecentTitleLength)
        {
            title = title.substr(0, maximumRecentTitleLength - 1) + L"\u2026";
        }

        std::time_t playedAt = static_cast<std::time_t>(item.playedAt);
        std::tm local{};
        localtime_s(&local, &playedAt);
        wchar_t date[32]{};
        wcsftime(date, ARRAYSIZE(date), L"%d/%m %H:%M", &local);
        std::wstring extension = protocol ? L"URL" : path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towupper);
        if (!extension.empty() && extension.front() == L'.') extension.erase(extension.begin());
        std::wstring hint = extension.empty() ? date : extension + L"  \u2022  " + date;
        result.push_back({ item.path, std::move(title), std::move(hint) });
    }
    return result;
}

void PlayerClearRecentFiles()
{
    g_recentMediaManager.Clear();
}

void PlayerSendMpvKey(std::wstring const& key)
{
    if (key.empty()) return;

    // "Parar" in the context menu is represented by the same Ctrl+S chord as
    // the global keyboard shortcut. Do not forward that chord as a raw mpv
    // keypress: route both entry points through HC Player's full stop/reset
    // path so the current media is unloaded and the no-media home screen is
    // restored consistently.
    if (key == L"Ctrl+s" || key == L"Ctrl+S")
    {
        StopPlaybackAndClearUi();
        return;
    }

    if (!g_mpv.handle) return;

    // HC Player owns the unmodified Y shortcut for the two subtitle colors
    // exposed by the bundled input preset. Handle it here instead of asking
    // mpv to execute the generic cycle-values binding so keyboard and the
    // context-menu "Alternar cor" action share the same state-aware OSD.
    if (key == L"y" && g_mpv.getProperty &&
        g_mpv.freeNodeContents && g_mpv.setProperty)
    {
        constexpr char IceWhite[] = "#FFF0F0F0";
        constexpr char Gold[] = "#FFFFD700";

        bool currentlyGold = false;
        MpvNode node{};
        if (g_mpv.getProperty(
            g_mpv.handle,
            "sub-color",
            MpvFormatNode,
            &node) >= 0)
        {
            if (node.format == MpvFormatString && node.value.string)
            {
                std::wstring current =
                    winrt::to_hstring(node.value.string).c_str();
                std::transform(
                    current.begin(), current.end(), current.begin(), towupper);

                // Accept both the configured 8-digit form and a possible
                // normalized six-digit representation returned by mpv.
                currentlyGold =
                    current == L"#FFFFD700" || current == L"#FFD700";
            }
            g_mpv.freeNodeContents(&node);
        }

        char const* nextColor = currentlyGold ? IceWhite : Gold;
        std::wstring nextName = currentlyGold
            ? PlayerUiString(L"OsdSubtitleColorIceWhite", L"Branco Gelo")
            : PlayerUiString(L"OsdSubtitleColorGold", L"Amarelo Ouro");

        if (g_mpv.setProperty(g_mpv.handle, "sub-color", nextColor) >= 0)
        {
            PlayerExecuteMpvCommand(
                L"show-text \"" +
                PlayerUiString(L"OsdSubtitleColorPrefix", L"Cor da legenda: ") +
                nextName + L"\" 1500");
        }
        return;
    }

    std::string utf8 = winrt::to_string(key);
    const char* args[] = { "keypress", utf8.c_str(), nullptr };
    g_mpv.command(g_mpv.handle, args);
}

void PlayerExecuteMpvCommand(std::wstring const& command)
{
    if (command.empty()) return;
    // Commands such as the native console are useful before a file is open.
    // Start MPV lazily here so an idle player has no extra startup cost.
    if (!g_mpv.handle && (!g_videoWindow || !g_mpv.Start(g_videoWindow))) return;
    if (!g_mpv.commandString) return;
    std::string utf8 = winrt::to_string(command);
    g_mpv.commandString(g_mpv.handle, utf8.c_str());
}

void PlayerCaptureScreenshot(bool withSubtitles)
{
    std::wstring message = PlayerUiString(
        withSubtitles ? L"OsdScreenshotWithSubtitlesSaved"
                      : L"OsdScreenshotVideoOnlySaved",
        withSubtitles ? L"Captura de tela salva"
                      : L"Captura salva (sem legendas)");
    PlayerExecuteMpvCommand(
        (withSubtitles
            ? L"no-osd async screenshot subtitles; show-text \""
            : L"no-osd async screenshot video; show-text \"") +
        message + L"\"");
}

void PlayerOpenScreenshotDirectory()
{
    std::wstring configured = winrt::to_hstring(ConfiguredNativeValue(
        "screenshot-directory", "~/Pictures/Capturas do HC Player")).c_str();
    if (configured.starts_with(L"~/") || configured.starts_with(L"~\\"))
    {
        wchar_t profile[32768]{};
        DWORD length = GetEnvironmentVariableW(
            L"USERPROFILE", profile, ARRAYSIZE(profile));
        if (length > 0 && length < ARRAYSIZE(profile))
        {
            configured = (std::filesystem::path{ profile } /
                configured.substr(2)).wstring();
        }
    }

    std::filesystem::path folder{ configured };
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (!error)
    {
        ShellExecuteW(g_mainWindow, L"open", folder.c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
    }
}

bool PlayerDeactivateImportedProfile()
{
    // MPV profiles are a collection of assignments and have no inverse
    // command. Rebuilding the engine is the only reliable way to remove every
    // option a profile may have changed; playback state is restored afterward.
    std::wstring previous = g_mpvSettingsManager.ActiveImportedProfile();
    g_mpvSettingsManager.ClearActiveImportedProfile();
    if (RestartEnginePreservingPlayback())
    {
        PlayerExecuteMpvCommand(
            L"show-text \"" +
            PlayerUiString(L"OsdProfileDisabled", L"Perfil desativado") +
            L"\"");
        return true;
    }
    g_mpvSettingsManager.SetActiveImportedProfile(std::move(previous));
    return false;
}

bool PlayerApplyImportedProfile(std::wstring const& name)
{
    if (!g_mpv.handle || name.empty()) return false;
    auto profiles = PlayerGetImportedProfileNames();
    if (std::find(profiles.begin(), profiles.end(), name) == profiles.end())
    {
        return false;
    }
    std::string utf8 = winrt::to_string(name);
    const char* command[] = { "apply-profile", utf8.c_str(), nullptr };
    if (g_mpv.command(g_mpv.handle, command) < 0) return false;
    g_mpvSettingsManager.SetActiveImportedProfile(name);
    PlayerExecuteMpvCommand(
        L"show-text \"" +
        PlayerUiString(L"OsdProfilePrefix", L"Perfil: ") +
        name + L"\"");
    return true;
}

std::wstring PlayerGetActiveImportedProfile()
{
    return g_mpvSettingsManager.ActiveImportedProfile();
}

void PlayerSetTransportVisible(bool visible)
{
    if (!g_mpv.handle || !g_mpv.setProperty) return;
    // This MPV property exists specifically for temporary OSC/UI avoidance and
    // does not overwrite the user's persistent sub-margin-y preference.
    int subtitleGap = CurrentControlsHeightPx(g_mainWindow) +
        DipToPx(g_mainWindow, 12);
    std::string offset = visible ? std::to_string(subtitleGap) : "0";
    g_mpv.setProperty(g_mpv.handle, "sub-margin-y-offset", offset.c_str());
}

void PlayerSetTransportHostVisible(bool visible)
{
    if (!g_mainWindow) return;
    SendMessageW(g_mainWindow, TransportVisibilityMessage, visible ? TRUE : FALSE, 0);
}

void PlayerRefreshTransportLayout()
{
    if (!g_mainWindow) return;
    auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
    if (!info) return;
    RECT client{};
    if (GetClientRect(g_mainWindow, &client))
    {
        ApplyClientLayout(g_mainWindow, info,
            client.right - client.left, client.bottom - client.top);
    }
}

void PlayerSetTransportMinimal(bool minimal)
{
    if (!g_mainWindow || g_transportMinimal == minimal) return;
    g_transportMinimal = minimal;

    auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
    if (!info) return;

    // Hide the currently attached island immediately. Rehosting is deferred
    // one dispatcher turn so it never happens inside MainPage::Loaded or a
    // Settings visual-state transaction.
    if (info->xamlSource)
    {
        HWND island = winrt::Microsoft::UI::GetWindowFromWindowId(
            info->xamlSource.SiteBridge().WindowId());
        if (island) ShowWindow(island, SW_HIDE);
    }
    if (info->transportHostWindow)
        ShowWindow(info->transportHostWindow, SW_HIDE);

    ScheduleTransportRehost(g_mainWindow);
    PlayerSetTransportVisible(g_transportHostVisible && !IsSidePanelOpen());
}

void PlayerSetTransportFlyoutOpen(bool open)
{
    g_transportFlyoutOpen = open;

    // A XAML MenuFlyout is an active player interaction even when the physical
    // mouse has not moved. Keep the native cursor-autohide state synchronized
    // with that UI state so the pointer cannot disappear over an open flyout.
    g_lastCursorActivityTick = GetTickCount64();
    SetApplicationCursorHidden(false);
}

void PlayerSetTransportCompact(bool compact)
{
    if (!g_mainWindow || g_transportCompact == compact) return;
    g_transportCompact = compact;
    auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
    if (!info) return;

    RECT client{};
    GetClientRect(g_mainWindow, &client);
    ApplyClientLayout(g_mainWindow, info,
        client.right - client.left, client.bottom - client.top);
    PlayerSetTransportVisible(g_transportHostVisible && !IsSidePanelOpen());
}

void PlayerSetTransportBarCompactLayout(bool compact)
{
    if (!g_mainWindow || g_transportBarCompactLayout == compact) return;
    g_transportBarCompactLayout = compact;

    auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
    if (!info) return;

    RECT client{};
    GetClientRect(g_mainWindow, &client);
    ApplyClientLayout(g_mainWindow, info,
        client.right - client.left, client.bottom - client.top);
    PlayerSetTransportVisible(g_transportHostVisible && !IsSidePanelOpen());
}

void PlayerSetTransportImageMode(bool imageMode)
{
    if (!g_mainWindow || g_transportImageMode == imageMode) return;

    g_transportImageMode = imageMode;

    auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
    if (!info) return;

    RECT client{};
    GetClientRect(g_mainWindow, &client);
    ApplyClientLayout(g_mainWindow, info,
        client.right - client.left, client.bottom - client.top);

    // Subtitle avoidance follows the actual transport height as well.
    PlayerSetTransportVisible(g_transportHostVisible && !IsSidePanelOpen());
}

void PlayerReleaseTransportFocus()
{
    if (g_pictureInPicture && g_mainWindow)
    {
        SetFocus(g_mainWindow);
    }
}

bool PlayerIsCursorInTransportHotZone()
{
    if (!g_mainWindow || IsSidePanelOpen()) return false;
    POINT screenCursor{};
    if (!GetCursorPos(&screenCursor)) return false;

    // When the transport was hidden, remember the exact physical cursor point.
    // Fullscreen/window restoration can move the hot zone underneath a stationary
    // cursor; that is layout motion, not user intent. Require an actual mouse move
    // before the dispatcher fallback is allowed to reveal the transport again.
    if (!g_transportHostVisible && g_hasTransportHiddenCursor &&
        screenCursor.x == g_transportHiddenCursor.x &&
        screenCursor.y == g_transportHiddenCursor.y)
    {
        return false;
    }

    POINT cursor = screenCursor;
    RECT client{};
    if (!ScreenToClient(g_mainWindow, &cursor) ||
        !GetClientRect(g_mainWindow, &client) || !PtInRect(&client, cursor))
    {
        return false;
    }
    if ((g_pictureInPicture &&
        PipResizeEdgesAt(screenCursor) != PipResizeNone) ||
        (g_borderless &&
            BorderlessResizeEdgesAt(screenCursor) != PipResizeNone))
    {
        // The sizing strip belongs to the native window. Revealing the XAML
        // transport here steals the bottom edge before it can be grabbed.
        return false;
    }
    return cursor.y >= client.bottom - CurrentControlsHeightPx(g_mainWindow);
}

bool PlayerIsLightTheme()
{
    EnsureThemeLoaded();
    return g_lightTheme;
}

void PlayerSetLightTheme(bool light)
{
    EnsureThemeLoaded();
    if (g_lightTheme == light) return;
    g_lightTheme = light;
    SaveTheme();

    auto theme = light
        ? winrt::Microsoft::UI::Xaml::ElementTheme::Light
        : winrt::Microsoft::UI::Xaml::ElementTheme::Dark;
    if (g_mainWindow)
    {
        auto* info = reinterpret_cast<WindowInfo*>(
            GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
        if (info)
        {
            if (info->page)
            {
                info->page.RequestedTheme(theme);
                winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
                    info->page)->RefreshThemeVisuals();
            }
            if (info->settingsPage) info->settingsPage.RequestedTheme(theme);
            if (info->mediaInfoPage) info->mediaInfoPage.RequestedTheme(theme);
            if (info->contextPage) info->contextPage.RequestedTheme(theme);
            if (info->bufferingRing) info->bufferingRing.RequestedTheme(theme);
            if (info->transportHostedInPopup && info->transportMicaConfiguration)
            {
                UpdateMinimalTransportMica(
                    info,
                    GetForegroundWindow() == g_mainWindow ||
                    GetActiveWindow() == g_mainWindow);
            }
        }
        ApplyWindows11Visual(g_mainWindow);
        RedrawWindow(g_mainWindow, nullptr, nullptr,
            RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
}

std::vector<MediaTrackOption> PlayerGetMediaTracks()
{
    std::vector<MediaTrackOption> tracks;
    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
    {
        return tracks;
    }

    MpvNode root{};
    if (g_mpv.getProperty(g_mpv.handle, "track-list", MpvFormatNode, &root) < 0)
    {
        return tracks;
    }
    if (root.format != MpvFormatNodeArray || !root.value.list)
    {
        g_mpv.freeNodeContents(&root);
        return tracks;
    }

    for (int index = 0; index < root.value.list->count; ++index)
    {
        auto const& entry = root.value.list->values[index];
        if (entry.format != MpvFormatNodeMap || !entry.value.list) continue;

        MediaTrackOption track{};
        bool hasId = false;
        for (int field = 0; field < entry.value.list->count; ++field)
        {
            char const* key = entry.value.list->keys
                ? entry.value.list->keys[field] : nullptr;
            if (!key) continue;
            auto const& value = entry.value.list->values[field];

            if (strcmp(key, "id") == 0 && value.format == MpvFormatInt64)
            {
                track.id = value.value.integer;
                hasId = true;
            }
            else if (strcmp(key, "type") == 0 && value.format == MpvFormatString && value.value.string)
            {
                track.type = winrt::to_hstring(value.value.string).c_str();
            }
            else if (strcmp(key, "title") == 0 && value.format == MpvFormatString && value.value.string)
            {
                track.title = winrt::to_hstring(value.value.string).c_str();
            }
            else if (strcmp(key, "lang") == 0 && value.format == MpvFormatString && value.value.string)
            {
                track.language = winrt::to_hstring(value.value.string).c_str();
            }
            else if (strcmp(key, "codec") == 0 && value.format == MpvFormatString && value.value.string)
            {
                track.codec = winrt::to_hstring(value.value.string).c_str();
            }
            else if (strcmp(key, "selected") == 0 && value.format == MpvFormatFlag)
            {
                track.selected = value.value.flag != 0;
            }
            else if (strcmp(key, "external") == 0 && value.format == MpvFormatFlag)
            {
                track.external = value.value.flag != 0;
            }
            else if (strcmp(key, "forced") == 0 && value.format == MpvFormatFlag)
            {
                track.forced = value.value.flag != 0;
            }
            else if (strcmp(key, "default") == 0 && value.format == MpvFormatFlag)
            {
                track.defaultTrack = value.value.flag != 0;
            }
            else if (strcmp(key, "main-selection") == 0 && value.format == MpvFormatInt64)
            {
                track.mainSelection = static_cast<int>(value.value.integer);
            }
        }

        if (hasId && (track.type == L"video" || track.type == L"audio" || track.type == L"sub"))
        {
            tracks.push_back(std::move(track));
        }
    }

    g_mpv.freeNodeContents(&root);
    return tracks;
}

MediaBadgeInfo PlayerGetMediaBadgeInfo()
{
    MediaBadgeInfo info{};

    if (!g_currentMediaPath.empty())
    {
        // Optical-disc badges are contextual source labels only. They are driven
        // by the same explicit disc-session state used by playback, so a loose
        // .m2ts/.vob file can never be mislabeled as Blu-ray/DVD. This changes
        // presentation only; no mpv option, demuxer, decoder or disc routing is
        // modified here.
        if (g_currentMediaIsDisc)
            info.source = g_currentDiscIsBluray
                ? MediaSourceBadge::BluRay
                : MediaSourceBadge::DVD;
        // URL source badges describe the URL the user actually opened. Keep
        // YouTube ahead of HLS so a normal yt-dlp page can never be mislabeled
        // because of an incidental .m3u8 token in its URL. Direct HLS manifests
        // use the same detector as playback routing, without changing playback.
        else if (IsYouTubeUrl(g_currentMediaPath))
            info.source = MediaSourceBadge::YouTube;
        else if (IsLikelyHlsSource(g_currentMediaPath))
            info.source = MediaSourceBadge::HLS;
        else
            info.source = MediaSourceBadge::None;
        info.sourceReady = true;
    }

    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
    {
        return info;
    }

    auto getStringProperty =
        [](char const* property, std::wstring& result) -> bool
        {
            MpvNode node{};

            if (g_mpv.getProperty(
                g_mpv.handle,
                property,
                MpvFormatNode,
                &node) < 0)
            {
                return false;
            }

            bool available =
                node.format == MpvFormatString &&
                node.value.string != nullptr;

            if (available)
            {
                result = winrt::to_hstring(node.value.string).c_str();
            }

            g_mpv.freeNodeContents(&node);
            return available;
        };

    auto getInt64Property =
        [](char const* property, int64_t& result) -> bool
        {
            return g_mpv.getProperty(
                g_mpv.handle,
                property,
                MpvFormatInt64,
                &result) >= 0;
        };

    auto getDoubleProperty =
        [](char const* property, double& result) -> bool
        {
            return g_mpv.getProperty(
                g_mpv.handle,
                property,
                MpvFormatDouble,
                &result) >= 0;
        };

    auto lower =
        [](std::wstring value)
        {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                towlower);

            return value;
        };

    // ------------------------------------------------------------
    // VIDEO
    // ------------------------------------------------------------

    if (getInt64Property(
        "current-tracks/video/id",
        info.videoTrackId))
    {
        int64_t dolbyVisionProfile{};

        if (getInt64Property(
            "current-tracks/video/dolby-vision-profile",
            dolbyVisionProfile) &&
            dolbyVisionProfile >= 0)
        {
            info.video = MediaVideoBadge::DolbyVision;
            info.videoReady = true;
        }
        else
        {
            std::wstring videoDecoder;

            if (getStringProperty(
                "current-tracks/video/decoder",
                videoDecoder))
            {
                double sceneMaxR{};
                double sceneMaxG{};
                double sceneMaxB{};

                bool hasSceneMaxR =
                    getDoubleProperty(
                        "video-dec-params/scene-max-r",
                        sceneMaxR) &&
                    sceneMaxR > 0.0;

                bool hasSceneMaxG =
                    getDoubleProperty(
                        "video-dec-params/scene-max-g",
                        sceneMaxG) &&
                    sceneMaxG > 0.0;

                bool hasSceneMaxB =
                    getDoubleProperty(
                        "video-dec-params/scene-max-b",
                        sceneMaxB) &&
                    sceneMaxB > 0.0;

                if (hasSceneMaxR ||
                    hasSceneMaxG ||
                    hasSceneMaxB)
                {
                    info.video = MediaVideoBadge::HDR10Plus;
                }

                // Dolby Vision and HDR10+ above keep their existing priority.
                // Only when neither matched do we classify standard HDR from
                // the transfer characteristic reported by mpv. PQ covers
                // HDR10/static PQ sources; HLG covers BT.2100 HLG.
                if (info.video == MediaVideoBadge::None)
                {
                    std::wstring transfer;
                    bool hasTransfer =
                        getStringProperty("video-params/gamma", transfer) ||
                        getStringProperty("video-dec-params/gamma", transfer);

                    if (hasTransfer)
                    {
                        transfer = lower(std::move(transfer));
                        if (transfer == L"pq" ||
                            transfer == L"hlg" ||
                            transfer == L"st2084" ||
                            transfer == L"smpte2084" ||
                            transfer == L"arib-std-b67" ||
                            transfer == L"arib-b67" ||
                            transfer == L"bt.2100-pq" ||
                            transfer == L"bt.2100-hlg")
                        {
                            info.video = MediaVideoBadge::HDR;
                        }
                    }
                }

                info.videoReady = true;
            }
        }
    }

    // ------------------------------------------------------------
    // AUDIO
    // ------------------------------------------------------------

    if (getInt64Property(
        "current-tracks/audio/id",
        info.audioTrackId))
    {
        std::wstring audioDecoder;

        if (getStringProperty(
            "current-tracks/audio/decoder",
            audioDecoder))
        {
            std::wstring codec;
            std::wstring profile;

            bool hasCodec =
                getStringProperty(
                    "current-tracks/audio/codec",
                    codec);

            getStringProperty(
                "current-tracks/audio/codec-profile",
                profile);

            codec = lower(std::move(codec));
            profile = lower(std::move(profile));

            if (profile.find(L"atmos") != std::wstring::npos)
            {
                info.audio = MediaAudioBadge::DolbyAtmos;
            }
            else if (profile.find(L"dts:x") != std::wstring::npos ||
                profile.find(L"dts-x") != std::wstring::npos)
            {
                info.audio = MediaAudioBadge::DTSX;
            }
            else if (hasCodec)
            {
                if (codec == L"ac3" ||
                    codec == L"eac3")
                {
                    info.audio = MediaAudioBadge::DolbyAudio;
                }
                else if (codec == L"dts" ||
                    codec == L"dca" ||
                    codec.starts_with(L"dts"))
                {
                    info.audio = MediaAudioBadge::DTS;
                }
            }

            info.audioReady = hasCodec;
        }
    }

    // YouTube live classification. mpv's builtin ytdl hook maps yt-dlp's
    // explicit JSON field `is_live` to the global metadata tag
    // `ytdl_is_live`. Use that authoritative extractor signal instead of
    // inferring live state from duration/cache behavior. Presentation only.
    if (info.source == MediaSourceBadge::YouTube &&
        (info.videoReady || info.audioReady))
    {
        std::wstring ytdlIsLive;
        if (getStringProperty(
                "metadata/by-key/ytdl_is_live",
                ytdlIsLive))
        {
            ytdlIsLive = lower(std::move(ytdlIsLive));
            if (ytdlIsLive == L"true" ||
                ytdlIsLive == L"yes" ||
                ytdlIsLive == L"1")
            {
                info.source = MediaSourceBadge::YouTubeLive;
            }
        }
    }

    // Direct HLS live classification. Our diagnostic pass showed the important
    // distinction reported by mpv itself: the tested VOD is fully seekable
    // (partially-seekable=no), while the live HLS is seekable only through the
    // active demuxer cache (partially-seekable=yes). Restrict this signal to the
    // original direct-HLS source and wait for selected A/V media to be ready so
    // normal startup cannot create a transient LIVE badge. Presentation only:
    // no mpv option, network/cache policy, seeking or demuxer state is changed.
    if (info.source == MediaSourceBadge::HLS &&
        (info.videoReady || info.audioReady))
    {
        int partiallySeekable{};
        bool const hasPartiallySeekable =
            g_mpv.getProperty(
                g_mpv.handle,
                "partially-seekable",
                MpvFormatFlag,
                &partiallySeekable) >= 0;

        if (hasPartiallySeekable && partiallySeekable != 0)
        {
            info.source = MediaSourceBadge::HLSLive;
        }
    }

    return info;
}

int PlayerGetCustomBadgeVariantMask(std::wstring const& badgeName)
{
    if (!IsSupportedCustomBadgeName(badgeName))
    {
        return 0;
    }

    auto directory = CustomMediaBadgesDirectory();
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
    {
        return 0;
    }

    auto hasStem = [&](std::wstring const& stem)
    {
        for (auto const* extension : { L".png", L".svg" })
        {
            error.clear();
            if (std::filesystem::is_regular_file(
                directory / (stem + extension), error))
            {
                return true;
            }
        }
        return false;
    };

    // 1 = dark, 2 = light.
    int mask{};
    if (hasStem(badgeName + L".Dark"))  mask |= 1;
    if (hasStem(badgeName + L".Light")) mask |= 2;
    return mask;
}

int PlayerGetCustomBadgeFileCount()
{
    auto directory = CustomMediaBadgesDirectory();
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
    {
        return 0;
    }

    int count{};
    for (auto const& entry : std::filesystem::directory_iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error))
    {
        if (entry.is_regular_file() && IsCustomBadgeImage(entry.path()))
        {
            ++count;
        }
    }
    return count;
}

bool PlayerImportCustomBadgeSet(
    std::wstring const& sourceFolder,
    int& importedCount,
    std::wstring& error)
{
    importedCount = 0;
    error.clear();

    std::filesystem::path source{ sourceFolder };
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(source, filesystemError))
    {
        error = L"A pasta selecionada não está disponível.";
        return false;
    }

    auto destination = CustomMediaBadgesDirectory();
    auto staging = destination;
    staging += L".importing";

    std::filesystem::remove_all(staging, filesystemError);
    filesystemError.clear();
    std::filesystem::create_directories(staging, filesystemError);
    if (filesystemError)
    {
        error = L"Não foi possível preparar a pasta das badges personalizadas.";
        return false;
    }

    constexpr std::uintmax_t MaxBadgeFileSize = 8ull * 1024ull * 1024ull;

    for (auto const& entry : std::filesystem::recursive_directory_iterator(
        source,
        std::filesystem::directory_options::skip_permission_denied,
        filesystemError))
    {
        if (filesystemError)
        {
            filesystemError.clear();
            continue;
        }
        if (!entry.is_regular_file() || !IsCustomBadgeImage(entry.path()))
        {
            continue;
        }

        auto size = entry.file_size(filesystemError);
        if (filesystemError || size > MaxBadgeFileSize)
        {
            filesystemError.clear();
            continue;
        }

        std::wstring const sourceStem =
            Lowercase(entry.path().stem().wstring());
        std::wstring extension = Lowercase(entry.path().extension().wstring());

        // Compatibility with HC Player's original pre-license DTS asset.
        // That approved set intentionally had one neutral DTS.png used in both
        // light and dark themes. Keep V7's strict themed runtime untouched by
        // expanding that one legacy filename into the two canonical slots only
        // while importing. Explicit DTS.Dark/DTS.Light files still win because
        // this compatibility copy never overwrites an existing themed slot.
        if (sourceStem == L"dts")
        {
            for (auto const* legacyStem : { L"DTS.Dark", L"DTS.Light" })
            {
                auto target = staging / (std::wstring{ legacyStem } + extension);
                filesystemError.clear();
                bool const copied = std::filesystem::copy_file(
                    entry.path(),
                    target,
                    std::filesystem::copy_options::skip_existing,
                    filesystemError);
                if (!filesystemError && copied)
                {
                    ++importedCount;
                }
                filesystemError.clear();
            }
            continue;
        }

        // DVD/Blu-ray artwork is intentionally not distributed by HC Player.
        // A neutral user-provided logo can be imported once and used in both
        // themes. The common original-style names dvd-logo and blu-ray-disc
        // are accepted as aliases, while explicit .Dark/.Light files still
        // remain available through the normal canonical import path below.
        std::wstring opticalImportStem;
        if (sourceStem == L"dvd" || sourceStem == L"dvd-logo" ||
            sourceStem == L"dvd_logo" || sourceStem == L"dvdlogo")
        {
            opticalImportStem = L"DVD";
        }
        else if (sourceStem == L"bluray" || sourceStem == L"blu-ray" ||
            sourceStem == L"blu-ray-disc" || sourceStem == L"blu_ray_disc" ||
            sourceStem == L"bluraydisc")
        {
            opticalImportStem = L"BluRay";
        }

        if (!opticalImportStem.empty())
        {
            for (auto const* suffix : { L".Dark", L".Light" })
            {
                auto target = staging /
                    (opticalImportStem + std::wstring{ suffix } + extension);
                filesystemError.clear();
                bool const copied = std::filesystem::copy_file(
                    entry.path(),
                    target,
                    std::filesystem::copy_options::skip_existing,
                    filesystemError);
                if (!filesystemError && copied)
                {
                    ++importedCount;
                }
                filesystemError.clear();
            }
            continue;
        }

        std::wstring canonicalStem =
            CanonicalCustomBadgeStem(entry.path().stem().wstring());
        if (canonicalStem.empty())
        {
            continue;
        }

        auto target = staging / (canonicalStem + extension);

        std::filesystem::copy_file(
            entry.path(),
            target,
            std::filesystem::copy_options::overwrite_existing,
            filesystemError);
        if (filesystemError)
        {
            filesystemError.clear();
            continue;
        }
        ++importedCount;
    }

    if (importedCount == 0)
    {
        std::filesystem::remove_all(staging, filesystemError);
        error = L"Nenhuma badge compatível foi encontrada na pasta selecionada.";
        return false;
    }

    std::filesystem::remove_all(destination, filesystemError);
    if (filesystemError)
    {
        std::filesystem::remove_all(staging, filesystemError);
        error = L"Não foi possível substituir o conjunto personalizado atual.";
        return false;
    }

    std::filesystem::rename(staging, destination, filesystemError);
    if (filesystemError)
    {
        std::filesystem::remove_all(staging, filesystemError);
        error = L"Não foi possível concluir a importação das badges.";
        return false;
    }

    RefreshCustomBadgeVisuals();
    return true;
}

bool PlayerResetCustomBadgeSet(std::wstring& error)
{
    error.clear();
    std::error_code filesystemError;
    std::filesystem::remove_all(
        CustomMediaBadgesDirectory(),
        filesystemError);
    if (filesystemError)
    {
        error = L"Não foi possível remover o conjunto personalizado.";
        return false;
    }

    RefreshCustomBadgeVisuals();
    return true;
}

bool PlayerImportCustomBadgeFile(
    std::wstring const& sourceFile,
    std::wstring& importedBadgeName,
    std::wstring& importedVariant,
    std::wstring& error)
{
    importedBadgeName.clear();
    importedVariant.clear();
    error.clear();

    std::filesystem::path source{ sourceFile };
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(source, filesystemError) ||
        !IsCustomBadgeImage(source))
    {
        error = L"Selecione um arquivo PNG ou SVG válido.";
        return false;
    }

    constexpr std::uintmax_t MaxBadgeFileSize =
        8ull * 1024ull * 1024ull;

    auto size = std::filesystem::file_size(source, filesystemError);
    if (filesystemError || size > MaxBadgeFileSize)
    {
        error = L"O arquivo da badge ultrapassa o limite de 8 MB.";
        return false;
    }

    // HC Player's original DTS badge was deliberately theme-neutral and was
    // shipped as DTS.png. Importing that exact legacy filename restores the
    // old behavior by writing the same artwork into both V7 theme slots.
    // No neutral runtime fallback is introduced.
    if (Lowercase(source.stem().wstring()) == L"dts")
    {
        auto destination = CustomMediaBadgesDirectory();
        std::filesystem::create_directories(destination, filesystemError);
        if (filesystemError)
        {
            error = L"Não foi possível preparar a pasta das badges personalizadas.";
            return false;
        }

        std::wstring const extension =
            Lowercase(source.extension().wstring());
        std::vector<std::filesystem::path> temporaries;
        temporaries.reserve(2);

        // Stage both copies first so a copy failure cannot leave only one theme
        // updated.
        for (auto const* legacyStem : { L"DTS.Dark", L"DTS.Light" })
        {
            auto temporary = destination /
                (std::wstring{ legacyStem } + extension + L".importing");
            std::filesystem::remove(temporary, filesystemError);
            filesystemError.clear();
            std::filesystem::copy_file(
                source,
                temporary,
                std::filesystem::copy_options::overwrite_existing,
                filesystemError);
            if (filesystemError)
            {
                for (auto const& staged : temporaries)
                {
                    std::error_code cleanupError;
                    std::filesystem::remove(staged, cleanupError);
                }
                error = L"Não foi possível copiar a imagem selecionada.";
                return false;
            }
            temporaries.push_back(std::move(temporary));
        }

        for (size_t i = 0; i < 2; ++i)
        {
            std::wstring const legacyStem =
                i == 0 ? L"DTS.Dark" : L"DTS.Light";

            for (auto const* oldExtension : { L".png", L".svg" })
            {
                std::filesystem::remove(
                    destination / (legacyStem + oldExtension),
                    filesystemError);
                filesystemError.clear();
            }

            auto target = destination / (legacyStem + extension);
            std::filesystem::rename(
                temporaries[i],
                target,
                filesystemError);
            if (filesystemError)
            {
                for (size_t j = i; j < temporaries.size(); ++j)
                {
                    std::error_code cleanupError;
                    std::filesystem::remove(temporaries[j], cleanupError);
                }
                error = L"Não foi possível concluir a importação da badge.";
                return false;
            }
        }

        importedBadgeName = L"DTS";
        importedVariant = L"Both";
        RefreshCustomBadgeVisuals();
        return true;
    }

    // Optical-source logos are optional user assets and are not shipped with
    // HC Player. Accept a neutral DVD/Blu-ray logo as a convenient Both-theme
    // import, including the common dvd-logo / blu-ray-disc filenames.
    std::wstring const sourceStemLower =
        Lowercase(source.stem().wstring());
    std::wstring opticalImportStem;
    if (sourceStemLower == L"dvd" || sourceStemLower == L"dvd-logo" ||
        sourceStemLower == L"dvd_logo" || sourceStemLower == L"dvdlogo")
    {
        opticalImportStem = L"DVD";
    }
    else if (sourceStemLower == L"bluray" || sourceStemLower == L"blu-ray" ||
        sourceStemLower == L"blu-ray-disc" ||
        sourceStemLower == L"blu_ray_disc" ||
        sourceStemLower == L"bluraydisc")
    {
        opticalImportStem = L"BluRay";
    }

    if (!opticalImportStem.empty())
    {
        auto destination = CustomMediaBadgesDirectory();
        std::filesystem::create_directories(destination, filesystemError);
        if (filesystemError)
        {
            error = L"Não foi possível preparar a pasta das badges personalizadas.";
            return false;
        }

        std::wstring const extension =
            Lowercase(source.extension().wstring());
        std::vector<std::filesystem::path> temporaries;
        temporaries.reserve(2);

        for (auto const* suffix : { L".Dark", L".Light" })
        {
            std::wstring const canonical =
                opticalImportStem + std::wstring{ suffix };
            auto temporary = destination /
                (canonical + extension + L".importing");
            std::filesystem::remove(temporary, filesystemError);
            filesystemError.clear();
            std::filesystem::copy_file(
                source,
                temporary,
                std::filesystem::copy_options::overwrite_existing,
                filesystemError);
            if (filesystemError)
            {
                for (auto const& staged : temporaries)
                {
                    std::error_code cleanupError;
                    std::filesystem::remove(staged, cleanupError);
                }
                error = L"Não foi possível copiar a imagem selecionada.";
                return false;
            }
            temporaries.push_back(std::move(temporary));
        }

        for (size_t i = 0; i < 2; ++i)
        {
            std::wstring const canonical = opticalImportStem +
                (i == 0 ? L".Dark" : L".Light");

            for (auto const* oldExtension : { L".png", L".svg" })
            {
                std::filesystem::remove(
                    destination / (canonical + oldExtension),
                    filesystemError);
                filesystemError.clear();
            }

            auto target = destination / (canonical + extension);
            std::filesystem::rename(
                temporaries[i],
                target,
                filesystemError);
            if (filesystemError)
            {
                for (size_t j = i; j < temporaries.size(); ++j)
                {
                    std::error_code cleanupError;
                    std::filesystem::remove(temporaries[j], cleanupError);
                }
                error = L"Não foi possível concluir a importação da badge.";
                return false;
            }
        }

        importedBadgeName = opticalImportStem;
        importedVariant = L"Both";
        RefreshCustomBadgeVisuals();
        return true;
    }

    std::wstring canonicalStem =
        CanonicalCustomBadgeStem(source.stem().wstring());
    if (canonicalStem.empty())
    {
        error =
            L"O nome do arquivo não identifica uma badge e um tema compatíveis.";
        return false;
    }

    auto separator = canonicalStem.rfind(L'.');
    if (separator == std::wstring::npos)
    {
        error = L"O nome da badge não contém uma variante de tema válida.";
        return false;
    }

    importedBadgeName = canonicalStem.substr(0, separator);
    importedVariant = canonicalStem.substr(separator + 1);

    if (!IsSupportedCustomBadgeName(importedBadgeName) ||
        (importedVariant != L"Dark" && importedVariant != L"Light"))
    {
        importedBadgeName.clear();
        importedVariant.clear();
        error = L"O nome da badge não é compatível.";
        return false;
    }

    auto destination = CustomMediaBadgesDirectory();
    std::filesystem::create_directories(destination, filesystemError);
    if (filesystemError)
    {
        error = L"Não foi possível preparar a pasta das badges personalizadas.";
        return false;
    }

    std::wstring extension = Lowercase(source.extension().wstring());
    auto target = destination / (canonicalStem + extension);
    auto temporary =
        destination / (canonicalStem + extension + L".importing");

    std::filesystem::remove(temporary, filesystemError);
    filesystemError.clear();

    std::filesystem::copy_file(
        source,
        temporary,
        std::filesystem::copy_options::overwrite_existing,
        filesystemError);
    if (filesystemError)
    {
        error = L"Não foi possível copiar a imagem selecionada.";
        return false;
    }

    // Exactly one canonical slot/theme can own this import. Remove a
    // previous PNG/SVG for that same slot/theme, but never touch another
    // badge or the opposite theme.
    for (auto const* oldExtension : { L".png", L".svg" })
    {
        std::filesystem::remove(
            destination / (canonicalStem + oldExtension),
            filesystemError);
        filesystemError.clear();
    }

    std::filesystem::rename(
        temporary,
        target,
        filesystemError);
    if (filesystemError)
    {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        error = L"Não foi possível concluir a importação da badge.";
        return false;
    }

    RefreshCustomBadgeVisuals();
    return true;
}

bool PlayerRemoveCustomBadgeFile(
    std::wstring const& badgeName,
    std::wstring const& variant,
    int& removedCount,
    std::wstring& error)
{
    removedCount = 0;
    error.clear();

    if (!IsSupportedCustomBadgeName(badgeName) ||
        !IsSupportedCustomBadgeVariant(variant))
    {
        error = L"A badge ou a variante selecionada não é compatível.";
        return false;
    }

    auto destination = CustomMediaBadgesDirectory();
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(destination, filesystemError))
    {
        return true;
    }

    std::vector<std::wstring> stems;
    if (variant == L"both")
    {
        stems = {
            badgeName + L".Dark",
            badgeName + L".Light",
            badgeName
        };
    }
    else
    {
        stems = {
            badgeName + (variant == L"light" ? L".Light" : L".Dark")
        };
    }

    for (auto const& stem : stems)
    {
        for (auto const* extension : { L".png", L".svg" })
        {
            auto candidate = destination / (stem + extension);
            bool const existed =
                std::filesystem::is_regular_file(candidate, filesystemError);
            filesystemError.clear();
            if (!existed)
            {
                continue;
            }

            bool const removed =
                std::filesystem::remove(candidate, filesystemError);
            if (filesystemError)
            {
                error = L"Não foi possível remover a badge personalizada.";
                return false;
            }
            if (removed)
            {
                ++removedCount;
            }
        }
    }

    RefreshCustomBadgeVisuals();
    return true;
}

std::wstring PlayerGetCustomBadgePath(
    std::wstring const& badgeName,
    bool lightTheme)
{
    auto path = FindCustomBadgeFile(badgeName, lightTheme);
    return path.empty() ? std::wstring{} : path.wstring();
}

bool PlayerSelectMediaTrack(std::wstring const& property, std::wstring const& value)
{
    if (!g_mpv.handle || !g_mpv.setProperty || property.empty() || value.empty())
    {
        return false;
    }

    std::string propertyUtf8 = winrt::to_string(property);
    std::string valueUtf8 = winrt::to_string(value);
    bool selected = g_mpv.setProperty(
        g_mpv.handle, propertyUtf8.c_str(), valueUtf8.c_str()) >= 0;
    if (selected)
    {
        std::wstring label = property == L"aid"
            ? PlayerUiString(L"OsdTrackAudio", L"Faixa de áudio")
            : property == L"vid"
                ? PlayerUiString(L"OsdTrackVideo", L"Faixa de vídeo")
                : property == L"secondary-sid"
                    ? PlayerUiString(
                        L"OsdTrackSecondarySubtitle", L"Legenda secundária")
                    : PlayerUiString(L"OsdTrackSubtitle", L"Legenda");
        std::wstring selection = value == L"no"
            ? PlayerUiString(L"OsdTrackDisabled", L"Desativada")
            : value;
        PlayerExecuteMpvCommand(
            L"show-text \"" + label + L": " + selection + L"\"");
    }
    return selected;
}

void PlayerToggleAlwaysOnTop()
{
    bool enabled = false;
    if (auto value = g_mpvSettingsManager.Overrides().find("ui-ontop");
        value != g_mpvSettingsManager.Overrides().end())
    {
        enabled = value->second == "yes";
    }
    PlayerSetAlwaysOnTop(!enabled);
}

void PlayerSetAlwaysOnTop(bool enabled)
{
    g_mpvSettingsManager.Overrides()["ui-ontop"] = enabled ? "yes" : "no";
    ScheduleNativeOptionsSave();
    if (g_mainWindow)
    {
        SetWindowPos(g_mainWindow, enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        // SettingsPage is intentionally persistent between openings. Keep its
        // Toggle synchronized with the same live state changed by Ctrl+T,
        // context-menu "Sempre visível", or the Toggle itself.
        auto* info = reinterpret_cast<WindowInfo*>(
            GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
        if (info && info->settingsPage)
        {
            auto* settingsImplementation =
                winrt::get_self<
                    winrt::HCPlayer::implementation::SettingsPage>(
                        info->settingsPage);
            settingsImplementation->SyncAlwaysOnTopState(enabled);
        }
    }
    std::wstring message = PlayerUiString(
        enabled ? L"OsdAlwaysOnTopEnabled" : L"OsdAlwaysOnTopDisabled",
        enabled ? L"Janela sempre visível: Ativada"
                : L"Janela sempre visível: Desativada");
    PlayerExecuteMpvCommand(L"show-text \"" + message + L"\"");
}

void PlayerSetHardwareDecoding(bool enabled)
{
    PlayerSetMpvOption(L"hwdec", enabled ? L"auto-safe" : L"no");
}

void PlayerSetSubtitleSize(double size)
{
    PlayerSetMpvOption(L"sub-font-size", std::to_wstring(static_cast<int>(size)));
}

struct EnginePlaybackSnapshot
{
    bool engineWasRunning{};
    bool hasPosition{};
    double position{};
    double volume{ 100.0 };
    int paused{};
    std::wstring media;
    bool mediaIsDisc{};
    bool discIsBluray{};
    bool loopPlayback{};
};

EnginePlaybackSnapshot CaptureEnginePlaybackSnapshot()
{
    EnginePlaybackSnapshot snapshot;
    snapshot.engineWasRunning = g_mpv.handle != nullptr;
    snapshot.media = g_currentMediaPath;
    snapshot.mediaIsDisc = g_currentMediaIsDisc;
    snapshot.discIsBluray = g_currentDiscIsBluray;
    snapshot.loopPlayback = g_loopPlayback;

    if (!snapshot.engineWasRunning)
    {
        return snapshot;
    }

    snapshot.hasPosition = g_mpv.getProperty(
        g_mpv.handle, "time-pos", MpvFormatDouble, &snapshot.position) >= 0;
    g_mpv.getProperty(
        g_mpv.handle, "volume", MpvFormatDouble, &snapshot.volume);
    g_mpv.getProperty(
        g_mpv.handle, "pause", MpvFormatFlag, &snapshot.paused);
    return snapshot;
}

bool RestartEngineFromSnapshot(EnginePlaybackSnapshot const& snapshot)
{
    if (!snapshot.engineWasRunning) return true;

    // Always stop first. This is also safe after a failed Start(), where mpv's
    // handle may already be null, and is what lets the transaction retry using
    // the previous known-good overrides.
    g_mpv.Stop();
    if (!g_mpv.Start(g_videoWindow)) return false;
    g_mpv.setProperty(
        g_mpv.handle, "loop-file", snapshot.loopPlayback ? "inf" : "no");

    // A profile is runtime state, not merely a saved preference. Preserve it
    // across engine restarts caused by settings that require rebuilding MPV.
    if (!g_mpvSettingsManager.ActiveImportedProfile().empty())
    {
        std::string activeProfile = winrt::to_string(
            g_mpvSettingsManager.ActiveImportedProfile());
        const char* profileCommand[] = {
            "apply-profile", activeProfile.c_str(), nullptr };
        if (g_mpv.command(g_mpv.handle, profileCommand) < 0) return false;
    }
    if (snapshot.media.empty()) return true;

    if (snapshot.hasPosition)
    {
        std::string start = std::to_string(snapshot.position);
        g_mpv.setProperty(g_mpv.handle, "start", start.c_str());
    }
    g_mpv.setProperty(
        g_mpv.handle, "pause", snapshot.paused ? "yes" : "no");

    std::string path;
    if (snapshot.mediaIsDisc)
    {
        std::string device = winrt::to_string(snapshot.media);
        g_mpv.setProperty(g_mpv.handle,
            snapshot.discIsBluray ? "bluray-device" : "dvd-device",
            device.c_str());
        path = snapshot.discIsBluray ? "bd://" : "dvd://";
    }
    else
    {
        path = winrt::to_string(MpvLoadTarget(snapshot.media));
    }

    std::vector<std::pair<std::string, std::string>> restartLocalOptions;
    if (!snapshot.mediaIsDisc && IsLikelyHlsSource(snapshot.media))
    {
        restartLocalOptions = HlsFileLocalOptions();
    }
    if (LoadFileWithLocalOptions(path, "replace", restartLocalOptions) < 0)
        return false;

    std::string restoredVolume = std::to_string(snapshot.volume);
    g_mpv.setProperty(g_mpv.handle, "volume", restoredVolume.c_str());
    ScheduleConfiguredAutofit();
    return true;
}

bool RestartEnginePreservingPlayback()
{
    if (!g_mpv.handle) return true;
    return RestartEngineFromSnapshot(CaptureEnginePlaybackSnapshot());
}

bool PlayerSetMpvOption(std::wstring const& name, std::wstring const& value)
{
    if (!ValidateMpvOption(name, value, false))
    {
        return false;
    }

    std::string utf8Name = winrt::to_string(name);
    std::string utf8Value = winrt::to_string(value);
    g_mpvSettingsManager.Overrides()[utf8Name] = utf8Value;
    ScheduleNativeOptionsSave();
    if (IsHostManagedOption(name))
    {
        UpdateCursorAutohide();
        PlayerUpdateTaskbarProgress();
        return true;
    }
    if (g_mpv.handle)
    {
        hc::settings::OptionApplyMode mode = g_mpvSettingsManager.ApplyModeForOption(name);
        if (mode == hc::settings::OptionApplyMode::NextFile)
        {
            return true;
        }
        if (mode == hc::settings::OptionApplyMode::RestartEngine)
        {
            return RestartEnginePreservingPlayback();
        }
        return g_mpv.setProperty(g_mpv.handle, utf8Name.c_str(), utf8Value.c_str()) >= 0;
    }
    return true;
}

bool PlayerApplyMpvOptions(
    std::vector<std::pair<std::wstring, std::wstring>> const& options,
    std::wstring& error)
{
    error.clear();
    if (options.empty()) return true;

    auto effectiveOptions = options;
    auto findOption = [&effectiveOptions](std::wstring const& name)
        -> std::vector<std::pair<std::wstring, std::wstring>>::iterator
        {
            return std::find_if(effectiveOptions.begin(), effectiveOptions.end(),
                [&name](auto const& option) { return option.first == name; });
        };
    auto setEffectiveOption = [&effectiveOptions, &findOption](
        std::wstring const& name, std::wstring const& value)
        {
            auto found = findOption(name);
            if (found == effectiveOptions.end())
                effectiveOptions.emplace_back(name, value);
            else
                found->second = value;
        };

    auto interpolation = findOption(L"interpolation");
    auto videoSync = findOption(L"video-sync");
    if (interpolation != effectiveOptions.end() && interpolation->second == L"yes")
    {
        setEffectiveOption(L"video-sync", L"display-resample");
    }
    else if (videoSync != effectiveOptions.end() &&
        !videoSync->second.starts_with(L"display"))
    {
        setEffectiveOption(L"interpolation", L"no");
    }

    auto sigmoidUpscaling = findOption(L"sigmoid-upscaling");
    auto linearUpscaling = findOption(L"linear-upscaling");
    if (sigmoidUpscaling != effectiveOptions.end() &&
        sigmoidUpscaling->second == L"yes")
    {
        setEffectiveOption(L"linear-upscaling", L"no");
    }
    else if (linearUpscaling != effectiveOptions.end() &&
        linearUpscaling->second == L"yes")
    {
        setEffectiveOption(L"sigmoid-upscaling", L"no");
    }

    // 34.17: validate the advanced video choices as one coherent pipeline.
    // libmpv validates each option value independently, but a valid gpu-api,
    // gpu-context or hwdec value can still be incompatible with its partners.
    // Run this only when a related option is part of this Save, so unrelated
    // settings are never blocked by an old/imported expert configuration.
    auto const videoPipelineChanged = std::any_of(
        options.begin(), options.end(), [](auto const& option)
        {
            static const std::set<std::wstring> guardedOptions = {
                L"vo", L"gpu-api", L"gpu-context", L"hwdec",
                L"tone-mapping", L"target-colorspace-hint"
            };
            return guardedOptions.contains(option.first);
        });

    if (videoPipelineChanged)
    {
        auto effectiveVideoValue = [&](std::wstring const& name,
            std::wstring const& fallback)
            {
                if (auto pending = findOption(name);
                    pending != effectiveOptions.end())
                {
                    return pending->second;
                }
                auto saved = g_mpvSettingsManager.Overrides().find(
                    winrt::to_string(name));
                return saved != g_mpvSettingsManager.Overrides().end()
                    ? std::wstring{ winrt::to_hstring(saved->second).c_str() }
                    : fallback;
            };

        auto const vo = effectiveVideoValue(L"vo", L"gpu-next");
        auto const gpuApi = effectiveVideoValue(L"gpu-api", L"d3d11");
        auto const gpuContext = effectiveVideoValue(L"gpu-context", L"d3d11");
        auto const hwdec = effectiveVideoValue(L"hwdec", L"d3d11va");
        auto const toneMapping = effectiveVideoValue(L"tone-mapping", L"auto");
        auto const colorspaceHint = effectiveVideoValue(
            L"target-colorspace-hint", L"auto");

        // A context set to auto is intentionally left to mpv's own probing.
        // When both sides are explicit, enforce the Windows backend mapping
        // documented by mpv: D3D11<->d3d11, Vulkan<->winvk and
        // OpenGL<->win/angle.
        if (gpuContext != L"auto")
        {
            if (gpuApi == L"d3d11" && gpuContext != L"d3d11")
            {
                error = L"A API gráfica D3D11 requer o contexto gráfico d3d11.";
                return false;
            }
            if (gpuApi == L"vulkan" && gpuContext != L"winvk")
            {
                error = L"A API gráfica Vulkan requer o contexto gráfico winvk no Windows.";
                return false;
            }
            if (gpuApi == L"opengl" &&
                gpuContext != L"win" && gpuContext != L"angle")
            {
                error = L"A API gráfica OpenGL requer o contexto gráfico win ou angle no Windows.";
                return false;
            }
        }

        // d3d11va is the direct zero-copy path. mpv requires gpu/gpu-next
        // together with a D3D11-compatible presentation path. Copy variants do
        // not share this context restriction and are deliberately not blocked.
        if (hwdec == L"d3d11va")
        {
            if (vo != L"gpu" && vo != L"gpu-next")
            {
                error = L"d3d11va direto requer a saída de vídeo gpu ou gpu-next.";
                return false;
            }
            if (gpuApi == L"vulkan")
            {
                error = L"d3d11va direto não é compatível com a API gráfica Vulkan.";
                return false;
            }
            if (gpuApi == L"opengl" && gpuContext != L"angle")
            {
                error = L"d3d11va direto com OpenGL requer o contexto gráfico angle.";
                return false;
            }
            if (gpuApi != L"opengl" && gpuContext != L"auto" &&
                gpuContext != L"d3d11" && gpuContext != L"angle")
            {
                error = L"d3d11va direto requer o contexto gráfico d3d11 ou angle.";
                return false;
            }
        }

        // These selected modes are explicitly documented as gpu-next-only.
        // Keep common modes (auto, bt.2390, mobius, etc.) available on the
        // legacy gpu renderer instead of over-restricting expert users.
        if (vo == L"gpu" &&
            (toneMapping == L"spline" || toneMapping == L"bt.2446a" ||
                toneMapping == L"st2094-40"))
        {
            error = L"O tone mapping selecionado requer a saída de vídeo gpu-next.";
            return false;
        }
        if (colorspaceHint == L"yes")
        {
            if (vo != L"gpu-next")
            {
                error = L"target-colorspace-hint=yes requer a saída de vídeo gpu-next.";
                return false;
            }
            if (gpuApi == L"opengl" || gpuContext == L"win" ||
                gpuContext == L"angle")
            {
                error = L"target-colorspace-hint=yes requer contexto gráfico d3d11 ou winvk no Windows.";
                return false;
            }
        }
    }

    // volume and volume-max are individually valid mpv options, but the UI
    // should never persist a startup volume above the user's amplification
    // ceiling. Resolve the untouched partner from the saved state so changing
    // either box is validated as one coherent pair.
    if (findOption(L"volume") != effectiveOptions.end() ||
        findOption(L"volume-max") != effectiveOptions.end())
    {
        auto effectiveValue = [&](std::wstring const& name,
            std::wstring const& fallback)
            {
                if (auto pending = findOption(name);
                    pending != effectiveOptions.end())
                {
                    return pending->second;
                }
                auto saved = g_mpvSettingsManager.Overrides().find(
                    winrt::to_string(name));
                return saved != g_mpvSettingsManager.Overrides().end()
                    ? std::wstring{ winrt::to_hstring(saved->second).c_str() }
                    : fallback;
            };

        double volume{};
        double volumeMax{};
        if (!ParseSettingDouble(effectiveValue(L"volume", L"100"), volume) ||
            volume < 0.0 || volume > 1000.0)
        {
            error = L"Volume inicial deve estar entre 0 e 1000.";
            return false;
        }
        if (!ParseSettingDouble(
            effectiveValue(L"volume-max", L"100"), volumeMax) ||
            volumeMax < 100.0 || volumeMax > 1000.0)
        {
            error = L"Limite de volume deve estar entre 100 e 1000.";
            return false;
        }
        if (volume > volumeMax)
        {
            error = L"Volume inicial não pode ser maior que o limite de volume.";
            return false;
        }
    }

    if (auto ytdlRaw = findOption(L"ytdl-raw-options");
        ytdlRaw != effectiveOptions.end() && !Trim(ytdlRaw->second).empty())
    {
        if (!ValidateYtdlpRawOptions(ytdlRaw->second, error))
            return false;
    }

    for (auto const& [name, value] : effectiveOptions)
    {
        // ui-* values belong to this Win32/WinUI shell rather than libmpv.
        // Persist them alongside the MPV choices, but never ask MPV to
        // validate a property that intentionally does not exist in MPV.
        if (name.starts_with(L"ui-")) continue;
        if ((name == L"autofit" || name == L"autofit-larger") &&
            Trim(value).empty())
        {
            continue;
        }
        if (!ValidateMpvOption(name, value, false))
        {
            error = L"O mecanismo de reprodução rejeitou " + name + L" = " + value;
            return false;
        }
    }

    // 34.18: stage restart-sensitive settings as a transaction. Keep the
    // previous in-memory overrides and playback state untouched on disk until
    // the new engine has proved that it can initialize with the candidate
    // configuration.
    auto const previousOverrides = g_mpvSettingsManager.Overrides();
    auto const previousPlayback = CaptureEnginePlaybackSnapshot();

    auto applyUiSideEffect = [](std::wstring const& name,
        std::wstring const& value)
        {
            if (name == L"ui-ontop")
            {
                if (g_mainWindow)
                {
                    SetWindowPos(g_mainWindow,
                        value == L"yes" ? HWND_TOPMOST : HWND_NOTOPMOST,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                return;
            }
            if (name == L"ui-resume-playback")
            {
                if (value != L"yes") ClearResumePoints();
                return;
            }
            if (name == L"ui-instance-mode")
            {
                ApplySingleInstanceModeRuntime(value);
                return;
            }
            if (name == L"ui-window-follow-video")
            {
                UpdateDynamicWindowFitMonitoring(value == L"yes");
                return;
            }
            if (name == L"ui-window-remember-size" && value == L"yes")
            {
                // This helper performs its own tiny persistence write. Run it
                // only after the main transaction has committed successfully.
                CaptureRememberedWindowSize(true);
            }
        };

    bool restartEngine{};
    bool updateAutofit{};
    for (auto const& [name, value] : effectiveOptions)
    {
        std::string utf8Name = winrt::to_string(name);
        bool const clearAutofit =
            (name == L"autofit" || name == L"autofit-larger") &&
            Trim(value).empty();
        std::string utf8Value = clearAutofit
            ? std::string{}
            : winrt::to_string(value);

        // Keep an explicit empty override for the two window-fit fields.
        // Absence means "use HC Player's built-in default"; an empty value
        // means "the user intentionally disabled this limit".
        g_mpvSettingsManager.Overrides()[utf8Name] = utf8Value;

        if (name == L"ui-ytdl-cookie-browser")
        {
            // The ytdl hook consumes raw options during engine setup.
            restartEngine = true;
            continue;
        }
        if (name.starts_with(L"ui-"))
        {
            // Shell-side effects are staged until persistence succeeds. This
            // prevents a mixed Save (for example graphics + window behavior)
            // from leaving Win32/WinUI state changed when the engine transaction
            // has to roll back.
            continue;
        }
        if (IsHostManagedOption(name)) continue;

        if (clearAutofit)
        {
            updateAutofit = true;
            continue;
        }

        auto mode = g_mpvSettingsManager.ApplyModeForOption(name);
        updateAutofit |= name == L"autofit" || name == L"autofit-larger";
        restartEngine |= mode == hc::settings::OptionApplyMode::RestartEngine;
        if (g_mpv.handle && mode == hc::settings::OptionApplyMode::Immediate)
        {
            if (g_mpv.setProperty(
                g_mpv.handle, utf8Name.c_str(), utf8Value.c_str()) < 0)
            {
                restartEngine = true;
            }
        }
    }

    if (g_mainWindow) KillTimer(g_mainWindow, NativeSettingsSaveTimer);

    if (restartEngine)
    {
        // Immediate properties may have changed runtime state (for example
        // volume) before a restart became necessary. Preserve that desired
        // post-Save state for the candidate engine, while previousPlayback
        // remains the untouched rollback point.
        auto const desiredPlayback = CaptureEnginePlaybackSnapshot();

        if (!RestartEngineFromSnapshot(desiredPlayback))
        {
            // The candidate overrides never reached disk. Put the known-good
            // map back in memory and rebuild mpv from the pre-Save playback
            // snapshot, even if the failed Start() already destroyed its handle.
            g_mpvSettingsManager.Overrides() = previousOverrides;
            bool const engineRestored =
                RestartEngineFromSnapshot(previousPlayback);

            error = engineRestored
                ? L"A nova configuração de vídeo não pôde iniciar o mecanismo de reprodução. A configuração anterior foi restaurada."
                : L"Não foi possível concluir a alteração e a restauração automática do mecanismo também falhou. Reinicie o HC Player antes de continuar.";
            return false;
        }
    }

    // Persist only after a restart-sensitive candidate has initialized. This
    // closes the old window where an unusable backend could be written to disk
    // before mpv had proved it could start.
    g_mpvSettingsManager.MarkDirty();
    if (!g_mpvSettingsManager.SaveNativeOptions())
    {
        if (!restartEngine)
        {
            error = L"Não foi possível salvar as configurações no disco.";
            return false;
        }

        // The candidate engine is alive, but the persistence commit failed.
        // Restore both the previous map on disk (best effort) and the previous
        // engine/playback state so runtime and next launch stay aligned.
        g_mpvSettingsManager.Overrides() = previousOverrides;
        g_mpvSettingsManager.MarkDirty();
        bool const diskRestored = g_mpvSettingsManager.SaveNativeOptions();
        bool const engineRestored =
            RestartEngineFromSnapshot(previousPlayback);

        error = diskRestored && engineRestored
            ? L"Não foi possível salvar as novas configurações no disco. A configuração anterior foi restaurada."
            : L"Não foi possível concluir a alteração e a restauração automática também não foi concluída com segurança. Reinicie o HC Player antes de continuar.";
        return false;
    }

    // Commit shell-side effects only after the settings transaction has
    // succeeded. ui-window-remember-size may perform its own follow-up write
    // for ui-window-last-size, just as it did before this transaction layer.
    for (auto const& [name, value] : effectiveOptions)
    {
        if (name.starts_with(L"ui-"))
            applyUiSideEffect(name, value);
    }
    if (updateAutofit && !g_currentMediaPath.empty())
    {
        ScheduleConfiguredAutofit();
    }
    UpdateCursorAutohide();
    PlayerUpdateTaskbarProgress();
    if (g_mainWindow)
    {
        auto* info = reinterpret_cast<WindowInfo*>(
            GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
        if (info && info->page)
        {
            winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
                info->page)->RefreshInterfacePreferences();
        }
    }
    return true;
}

bool PlayerTryGetMpvRuntimeOption(
    std::wstring const& name, std::wstring& value)
{
    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
    {
        return false;
    }

    MpvNode node{};
    std::string property = winrt::to_string(name);
    if (g_mpv.getProperty(
        g_mpv.handle, property.c_str(), MpvFormatNode, &node) < 0)
    {
        return false;
    }

    bool converted = true;
    if (node.format == MpvFormatString && node.value.string)
    {
        value = winrt::to_hstring(node.value.string).c_str();
    }
    else if (node.format == MpvFormatFlag)
    {
        value = node.value.flag ? L"yes" : L"no";
    }
    else if (node.format == MpvFormatInt64)
    {
        value = std::to_wstring(node.value.integer);
    }
    else
    {
        converted = false;
    }

    g_mpv.freeNodeContents(&node);
    return converted;
}

bool PlayerTryGetSavedMpvOption(std::wstring const& name, std::wstring& value)
{
    auto found = g_mpvSettingsManager.Overrides().find(winrt::to_string(name));
    if (found != g_mpvSettingsManager.Overrides().end())
    {
        value = winrt::to_hstring(found->second).c_str();
        return true;
    }
    std::string requested = winrt::to_string(name);
    for (auto const& [defaultName, defaultValue] : BaseMpvOptions)
    {
        if (requested == defaultName)
        {
            value = winrt::to_hstring(
                LocalizedBaseMpvOptionValue(defaultName, defaultValue)).c_str();
            return true;
        }
    }
    return false;
}

std::vector<std::wstring> PlayerGetMpvOptionChoices(std::wstring const& name)
{
    std::vector<std::wstring> choices;
    if (!g_mpv.handle || !g_mpv.getProperty || !g_mpv.freeNodeContents)
    {
        return choices;
    }

    std::string property = "option-info/" + winrt::to_string(name) + "/choices";
    MpvNode node{};
    if (g_mpv.getProperty(g_mpv.handle, property.c_str(), MpvFormatNode, &node) >= 0)
    {
        if (node.format == MpvFormatNodeArray && node.value.list)
        {
            for (int index = 0; index < node.value.list->count; ++index)
            {
                auto const& item = node.value.list->values[index];
                if (item.format == 1 && item.value.string)
                {
                    choices.push_back(winrt::to_hstring(item.value.string).c_str());
                }
            }
        }
        g_mpv.freeNodeContents(&node);
    }

    // Scalers are implemented by a custom option type in some MPV builds and
    // may not expose /choices. In that case validate the complete known
    // libplacebo/MPV scaler catalog against this exact libmpv build.
    if (choices.empty() && (name == L"scale" || name == L"dscale" || name == L"cscale"))
    {
        static constexpr wchar_t const* candidates[] = {
            L"bilinear", L"bicubic_fast", L"oversample", L"linear",
            L"spline16", L"spline36", L"spline64", L"sinc", L"lanczos",
            L"ginseng", L"jinc", L"ewa_lanczos", L"ewa_hanning",
            L"ewa_ginseng", L"ewa_lanczossharp", L"ewa_lanczos4sharpest",
            L"ewa_lanczossoft", L"haasnsoft", L"bicubic", L"bcspline",
            L"catmull_rom", L"mitchell", L"robidoux", L"robidouxsharp",
            L"ewa_robidoux", L"ewa_robidouxsharp", L"box", L"nearest",
            L"triangle", L"gaussian", L"bartlett", L"cosine", L"hanning",
            L"hamming", L"quadric", L"welch", L"kaiser", L"blackman",
            L"sphinx", L"hermite"
        };

        mpv_handle* validator = g_mpv.create();
        if (validator)
        {
            std::string utf8Name = winrt::to_string(name);
            for (auto const* candidate : candidates)
            {
                std::string utf8Value = winrt::to_string(candidate);
                if (g_mpv.setOption(validator, utf8Name.c_str(), utf8Value.c_str()) >= 0)
                {
                    choices.emplace_back(candidate);
                }
            }
            g_mpv.destroy(validator);
        }
    }

    // Audio outputs are an object-settings list and frequently do not expose
    // option-info choices. Validate the relevant official MPV drivers against
    // the exact libmpv bundled with this application.
    if (choices.empty() && name == L"ao")
    {
        static constexpr wchar_t const* candidates[] = {
            L"wasapi", L"sdl", L"openal", L"null", L"pcm"
        };

        mpv_handle* validator = g_mpv.create();
        if (validator)
        {
            for (auto const* candidate : candidates)
            {
                std::string utf8Value = winrt::to_string(candidate);
                if (g_mpv.setOption(validator, "ao", utf8Value.c_str()) >= 0)
                {
                    choices.emplace_back(candidate);
                }
            }
            g_mpv.destroy(validator);
        }
    }
    return choices;
}

std::vector<AudioDeviceOption> PlayerGetAudioDevices()
{
    std::vector<AudioDeviceOption> devices{
        { L"auto", PlayerUiString(
            L"AudioDeviceSystemDefault", L"Padr\u00e3o do sistema") }
    };
    if (!g_mpv.LoadFunctions()) return devices;

    mpv_handle* queryHandle = g_mpv.handle;
    bool temporary{};
    if (!queryHandle)
    {
        queryHandle = g_mpv.create();
        if (!queryHandle) return devices;
        temporary = true;
        g_mpv.setOption(queryHandle, "terminal", "no");
        g_mpv.setOption(queryHandle, "config", "no");
        g_mpv.setOption(queryHandle, "ao", "wasapi");
        if (g_mpv.initialize(queryHandle) < 0)
        {
            g_mpv.destroy(queryHandle);
            return devices;
        }
    }

    MpvNode root{};
    if (g_mpv.getProperty(queryHandle, "audio-device-list", MpvFormatNode, &root) >= 0 &&
        root.format == MpvFormatNodeArray && root.value.list)
    {
        for (int index = 0; index < root.value.list->count; ++index)
        {
            auto const& entry = root.value.list->values[index];
            if (entry.format != MpvFormatNodeMap || !entry.value.list) continue;
            std::wstring name;
            std::wstring description;
            for (int field = 0; field < entry.value.list->count; ++field)
            {
                auto const& value = entry.value.list->values[field];
                char const* key = entry.value.list->keys[field];
                if (!key || value.format != MpvFormatString || !value.value.string) continue;
                if (strcmp(key, "name") == 0) name = winrt::to_hstring(value.value.string).c_str();
                if (strcmp(key, "description") == 0) description = winrt::to_hstring(value.value.string).c_str();
            }
            if (!name.empty() && name != L"auto")
            {
                if (description.starts_with(L"Default ("))
                {
                    description.replace(
                        0,
                        7,
                        PlayerUiString(
                            L"AudioDeviceDefaultPrefix", L"Padr\u00e3o"));
                }
                else if (description == L"Default")
                {
                    description = PlayerUiString(
                        L"AudioDeviceDefaultPrefix", L"Padr\u00e3o");
                }
                devices.push_back({ name, description.empty() ? name : description });
            }
        }
        g_mpv.freeNodeContents(&root);
    }

    if (temporary) g_mpv.destroy(queryHandle);
    return devices;
}

std::vector<std::wstring> PlayerGetImportedProfileNames()
{
    return g_mpvSettingsManager.GetImportedProfileNames();
}

std::vector<PlayerShaderInfo> PlayerGetShaders()
{
    return g_shaderManager.GetShaders();
}

PlayerAnime4KStatus PlayerGetAnime4KStatus()
{
    return g_shaderManager.GetAnime4KStatus();
}

bool PlayerSetAnime4KMode(
    std::wstring const& profile,
    std::wstring const& mode,
    std::wstring& error)
{
    return g_shaderManager.SetAnime4KMode(
        profile, mode, ManagedShaderRuntime(), error);
}

bool PlayerDisableAnime4K(std::wstring& error)
{
    return g_shaderManager.DisableAnime4K(
        ManagedShaderRuntime(), error);
}

bool PlayerImportShader(
    std::wstring const& sourcePath,
    std::wstring& error)
{
    return g_shaderManager.ImportShader(
        sourcePath, ManagedShaderRuntime(), error);
}

bool PlayerSetShaderEnabled(
    std::wstring const& shaderPath,
    bool enabled,
    std::wstring& error)
{
    return g_shaderManager.SetShaderEnabled(
        shaderPath, enabled, ManagedShaderRuntime(), error);
}

bool PlayerMoveShader(
    std::wstring const& shaderPath,
    int direction,
    std::wstring& error)
{
    return g_shaderManager.MoveShader(
        shaderPath, direction, ManagedShaderRuntime(), error);
}

bool PlayerRemoveShader(
    std::wstring const& shaderPath,
    std::wstring& error)
{
    return g_shaderManager.RemoveShader(
        shaderPath, ManagedShaderRuntime(), error);
}

bool PlayerRemoveAllShaders(std::wstring& error)
{
    return g_shaderManager.RemoveAllShaders(
        ManagedShaderRuntime(), error);
}

YtdlpStatus PlayerGetYtdlpStatus()
{
    auto toolStatus = g_externalToolsManager.GetStatus();
    YtdlpStatus status{};
    status.available = toolStatus.ytdlpAvailable;
    status.imported = toolStatus.ytdlpImported;
    status.jsRuntimeAvailable = toolStatus.denoAvailable;
    status.jsRuntimeImported = toolStatus.denoImported;
    status.jsRuntimeInvalid = toolStatus.denoInvalid;
    status.path = std::move(toolStatus.ytdlpPath);
    status.jsRuntimePath = std::move(toolStatus.denoPath);
    status.message = std::move(toolStatus.ytdlpMessage);
    return status;
}

bool PlayerImportYtdlpBinary(std::wstring const& sourcePath, std::wstring& error)
{
    return g_externalToolsManager.ImportYtdlpBinary(
        sourcePath,
        [] { return RestartEnginePreservingPlayback(); },
        error);
}

bool PlayerResetImportedYtdlp(std::wstring& error)
{
    return g_externalToolsManager.ResetImportedYtdlp(
        [] { return RestartEnginePreservingPlayback(); },
        error);
}

bool PlayerImportDenoBinary(std::wstring const& sourcePath, std::wstring& error)
{
    return g_externalToolsManager.ImportDenoBinary(
        sourcePath,
        [] { return RestartEnginePreservingPlayback(); },
        error);
}

bool PlayerResetImportedDeno(std::wstring& error)
{
    return g_externalToolsManager.ResetImportedDeno(
        [] { return RestartEnginePreservingPlayback(); },
        error);
}

ImportedMpvConfig PlayerGetImportedConfig()
{
    return g_mpvSettingsManager.GetImportedConfig();
}

ImportedMpvConfig PlayerImportMpvConfig(std::wstring const& path)
{
    ImportedMpvConfig result{};

    if (!g_mpv.LoadFunctions())
    {
        result.message = L"O mecanismo de reprodu\u00e7\u00e3o n\u00e3o est\u00e1 dispon\u00edvel para validar o arquivo.";
        return result;
    }

    // Preserve the previous error ordering: verify the selected file before
    // creating a temporary libmpv validator. The manager reopens it for the
    // actual parse so all config-file persistence stays outside this host file.
    std::ifstream input(std::filesystem::path{ path }, std::ios::binary);
    if (!input)
    {
        result.message = L"N\u00e3o foi poss\u00edvel ler o arquivo selecionado.";
        return result;
    }
    input.close();

    mpv_handle* validator = g_mpv.create();
    if (!validator)
    {
        result.message = L"O mecanismo de reprodu\u00e7\u00e3o n\u00e3o conseguiu iniciar a valida\u00e7\u00e3o.";
        return result;
    }

    result = g_mpvSettingsManager.ImportMpvConfig(
        path,
        [validator](std::wstring const& name,
            std::wstring const& value,
            bool profile)
        {
            if (profile &&
                (name == L"profile-desc" || name == L"profile-restore"))
            {
                return true;
            }
            std::string utf8Name = winrt::to_string(name);
            std::string utf8Value = winrt::to_string(value);
            return g_mpv.setOption(
                validator, utf8Name.c_str(), utf8Value.c_str()) >= 0;
        });

    g_mpv.destroy(validator);

    if (!result.success)
    {
        return result;
    }

    if (g_mpv.handle)
    {
        if (RestartEnginePreservingPlayback())
            result.message += L". Aplicada preservando a reprodu\u00e7\u00e3o atual.";
        else
            result.message += L". Salva; ser\u00e1 aplicada na pr\u00f3xima inicializa\u00e7\u00e3o do motor.";
    }
    else
    {
        result.message += L". Ser\u00e1 aplicada ao iniciar o player.";
    }
    return result;
}

bool PlayerResetImportedConfig()
{
    if (!g_mpvSettingsManager.ResetImportedConfig())
    {
        return false;
    }
    return RestartEnginePreservingPlayback();
}

bool PlayerResetAllSettingsToDefaults(std::wstring& error)
{
    error.clear();
    if (g_mainWindow) KillTimer(g_mainWindow, NativeSettingsSaveTimer);

    // Do not delete imported shaders: reset only their enabled state. Passing
    // an empty runtime keeps the current playback untouched; the clean shader
    // state is applied together with all other defaults on the next launch.
    auto const shaderSnapshot = g_shaderManager.GetShaders();
    if (!g_shaderManager.DisableAllPreservingFiles({}, error))
    {
        if (error.empty())
            error = L"Não foi possível redefinir o estado dos shaders.";
        return false;
    }

    if (!g_mpvSettingsManager.ResetToDefaults())
    {
        // The shader registry was written first so a failure there can abort
        // without touching core settings. If the later settings write fails,
        // restore the exact enabled flags best-effort to avoid a partial reset.
        for (auto const& shader : shaderSnapshot)
        {
            if (!shader.enabled) continue;
            std::wstring ignored;
            g_shaderManager.SetShaderEnabled(
                shader.path, true, {}, ignored);
        }
        error = L"Não foi possível redefinir as configurações salvas.";
        return false;
    }

    // Resume points are state derived from a setting whose factory default is
    // disabled. Remove those tiny records so re-enabling the feature later can
    // never resurrect a pre-reset position. Recent-media history and the chosen
    // light/dark appearance are personal shell state and are deliberately kept.
    ClearResumePoints();

    return true;
}

bool PlayerUpdateImportedOption(
    std::wstring const& section,
    std::wstring const& name,
    std::wstring const& value,
    bool profile)
{
    if (!g_mpvSettingsManager.UpdateImportedOption(
        section,
        name,
        value,
        profile,
        [&section](std::wstring const& optionName,
            std::wstring const& optionValue,
            bool optionProfile)
        {
            return ValidateImportedMpvOptionEdit(
                section,
                optionName,
                optionValue,
                optionProfile);
        }))
    {
        return false;
    }

    if (g_mpv.handle && !profile)
    {
        std::string utf8Name = winrt::to_string(name);
        std::string utf8Value = winrt::to_string(value);
        g_mpv.setProperty(
            g_mpv.handle, utf8Name.c_str(), utf8Value.c_str());
        return true;
    }
    if (g_mpv.handle && profile)
    {
        return RestartEnginePreservingPlayback();
    }
    return true;
}

void PlayerToggleFullscreen()
{
    if (!g_mainWindow)
    {
        g_suppressFullscreenEntryTransportReveal = false;
        return;
    }

    // If the user toggles again before the read-only 16:9 settle probe has
    // completed, remove only its temporary shield. The real player is already
    // uncloaked and fully owns its current state.
    DestroyPendingFullscreenVideoSettleShield();

    bool const suppressTransitionTransport =
        g_suppressFullscreenEntryTransportReveal;
    g_suppressFullscreenEntryTransportReveal = false;

    SendMessageW(g_mainWindow, CloseSettingsMessage, 0, 0);
    SendMessageW(g_mainWindow, CloseContextMenuMessage, 0, 0);

    auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));

    // PiP -> fullscreen used to call PlayerTogglePictureInPicture(), which
    // visibly restored the normal window (SWP_SHOWWINDOW) before fullscreen
    // was applied. Keep that intermediate window completely out of the DWM
    // presentation path: use the pre-PiP restore snapshot directly instead.
    bool const transitionFromPip = g_pictureInPicture && !g_fullscreen;

    auto applyCurrentClientLayout = [&]()
        {
            if (!info) return;

            RECT client{};
            if (!GetClientRect(g_mainWindow, &client)) return;

            ApplyClientLayout(
                g_mainWindow,
                info,
                client.right - client.left,
                client.bottom - client.top);
        };

    auto beginLayoutTransition = [&]()
        {
            g_fullscreenLayoutTransition = true;

            // Hide the XAML transport at the OLD geometry before changing the
            // top-level HWND. Any WM_SIZE generated by the style/placement swap
            // keeps it hidden, so DWM never gets a chance to present a mixed
            // old-width/new-width composition frame.
            applyCurrentClientLayout();
        };

    auto endLayoutTransition = [&]()
        {
            // A rapid second fullscreen toggle can arrive while compositor work
            // from the previous resize is still settling. For silent transitions,
            // reassert both the XAML state and the native host state immediately
            // before committing the final client layout. This makes the final
            // frame deterministic: either the complete transport is visible, or
            // no transport height is reserved at all.
            if (suppressTransitionTransport && info && info->page)
            {
                winrt::get_self<
                    winrt::HCPlayer::implementation::MainPage>(
                        info->page)->PrepareSilentFullscreenEntry();

                POINT cursor{};
                if (GetCursorPos(&cursor))
                {
                    g_lastVideoMouseScreenPoint = cursor;
                    g_hasLastVideoMouseScreenPoint = true;
                }
            }

            // Re-enable overlays only after GetClientRect() reports the final
            // client size. ApplyClientLayout moves/resizes the video and WinUI
            // islands first, then reveals only hosts whose logical state is visible.
            g_fullscreenLayoutTransition = false;
            applyCurrentClientLayout();

            // The Minimal transport lives in an owned top-level popup. The
            // owner's fullscreen frame/z-order transaction can finish one DWM
            // turn after the synchronous WM_SIZE/layout work above. Reassert
            // the final Minimal popup geometry and z-order on the next UI turn;
            // the classic child-island path is deliberately untouched.
            if (g_transportMinimal)
            {
                auto dispatcher =
                    winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
                if (dispatcher)
                {
                    HWND const targetWindow = g_mainWindow;
                    dispatcher.TryEnqueue([targetWindow]()
                        {
                            if (!IsWindow(targetWindow)) return;
                            auto* currentInfo = reinterpret_cast<WindowInfo*>(
                                GetWindowLongPtrW(targetWindow, GWLP_USERDATA));
                            if (!currentInfo || !g_transportMinimal) return;
                            RECT client{};
                            if (!GetClientRect(targetWindow, &client)) return;
                            ApplyClientLayout(
                                targetWindow,
                                currentInfo,
                                client.right - client.left,
                                client.bottom - client.top);
                        });
                }
            }
        };

    if (!g_fullscreen)
    {
        MONITORINFO monitor{ sizeof(monitor) };
        bool haveWindowedRestoreState{};

        if (transitionFromPip)
        {
            // PiP already owns a snapshot of the real window that existed before
            // it was entered. Promote that snapshot to fullscreen's restore state
            // instead of briefly applying it to the HWND. Fullscreen itself uses
            // the monitor where the PiP is currently being viewed.
            g_previousPlacement = g_pipPreviousPlacement;
            g_previousStyle = g_pipPreviousStyle;
            g_previousExStyle = g_pipPreviousExStyle;
            haveWindowedRestoreState =
                GetMonitorInfoW(MonitorFromWindow(
                    g_mainWindow, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
        }
        else
        {
            haveWindowedRestoreState =
                GetWindowPlacement(g_mainWindow, &g_previousPlacement) &&
                GetMonitorInfoW(MonitorFromWindow(
                    g_mainWindow, MONITOR_DEFAULTTONEAREST), &monitor);
            if (haveWindowedRestoreState)
            {
                g_previousStyle = GetWindowLongPtrW(g_mainWindow, GWL_STYLE);
                g_previousExStyle = GetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE);
            }
        }

        if (haveWindowedRestoreState)
        {
            bool layoutTransitionStarted{};

            if (transitionFromPip)
            {
                // Hide every WinUI island while it is still at PiP geometry.
                // SetPictureInPictureMode(false) can now rebuild the normal
                // transport controls without any intermediate window becoming
                // visible; the final fullscreen layout is committed below.
                beginLayoutTransition();
                layoutTransitionStarted = true;
                g_pictureInPicture = false;

                if (info && info->page)
                {
                    winrt::get_self<
                        winrt::HCPlayer::implementation::MainPage>(
                            info->page)->SetPictureInPictureMode(false);
                }
            }

            g_fullscreen = true;
            if (info && info->page)
            {
                auto* page = winrt::get_self<
                    winrt::HCPlayer::implementation::MainPage>(
                        info->page);

                if (suppressTransitionTransport)
                {
                    // Silent fullscreen must never pass through a visible
                    // transport state. Calling SetSettingsOverlayOpen(false)
                    // here would briefly reveal the native XAML host and then
                    // hide its content again, which can leave a blank host strip
                    // if fullscreen is toggled repeatedly before composition
                    // catches up. Keep both layers hidden from the outset.
                    page->PrepareSilentFullscreenEntry();

                    // Resize/layout can synthesize WM_MOUSEMOVE at the same
                    // physical cursor position. Mark that position as already
                    // seen so it cannot immediately reopen the hidden transport.
                    POINT cursor{};
                    if (GetCursorPos(&cursor))
                    {
                        g_lastVideoMouseScreenPoint = cursor;
                        g_hasLastVideoMouseScreenPoint = true;
                    }
                }
                else
                {
                    // Toolbar/normal fullscreen keeps the established reveal.
                    page->SetSettingsOverlayOpen(false);
                }
            }
            SetThreadExecutionState(
                ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

            if (!layoutTransitionStarted)
            {
                beginLayoutTransition();
            }

            HWND idleTransitionShield{};
            HWND mediaTransitionShield{};
            if (g_currentMediaPath.empty() && !transitionFromPip)
            {
                idleTransitionShield =
                    ShowIdleFullscreenTransitionShield(monitor.rcMonitor);
            }
            else if (suppressTransitionTransport && !transitionFromPip)
            {
                // Scope this experiment to the immersive Enter/double-click
                // media entry path only. PiP -> fullscreen and any normal
                // toolbar caller stay byte-for-byte on their established path.
                mediaTransitionShield =
                    ShowMediaFullscreenTransitionShield(
                        monitor.rcMonitor, true);
            }

            // Prime DWM with the final fullscreen chrome state before Win32
            // rebuilds the non-client frame. On a rapid idle-screen transition,
            // the empty-state island is already hidden at this point; without
            // this ordering Windows can briefly present the legacy window frame
            // while SWP_FRAMECHANGED is committing the popup style.
            ApplyWindows11Visual(g_mainWindow);

            SetWindowLongPtrW(g_mainWindow, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
            SetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE,
                g_previousExStyle & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME));

            DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
            DwmSetWindowAttribute(g_mainWindow, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
            HWND fullscreenZOrder = HWND_TOP;
            if (transitionFromPip)
            {
                // PiP is always topmost/toolwindow. Restore the original window's
                // topmost policy while jumping directly to fullscreen, otherwise
                // the temporary PiP z-order can leak into the fullscreen state.
                fullscreenZOrder = (g_previousExStyle & WS_EX_TOPMOST)
                    ? HWND_TOPMOST
                    : HWND_NOTOPMOST;
            }

            SetWindowPos(g_mainWindow, fullscreenZOrder,
                monitor.rcMonitor.left, monitor.rcMonitor.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left,
                monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

            endLayoutTransition();
            FinishMediaFullscreenTransitionShield(mediaTransitionShield);
            FinishIdleFullscreenTransitionShield(idleTransitionShield);
        }
    }
    else
    {
        beginLayoutTransition();

        HWND idleTransitionShield{};
        HWND mediaTransitionShield{};
        MONITORINFO currentMonitor{ sizeof(currentMonitor) };
        bool const haveCurrentMonitor = GetMonitorInfoW(
            MonitorFromWindow(g_mainWindow, MONITOR_DEFAULTTONEAREST),
            &currentMonitor) != FALSE;

        if (g_currentMediaPath.empty())
        {
            if (haveCurrentMonitor)
            {
                idleTransitionShield =
                    ShowIdleFullscreenTransitionShield(currentMonitor.rcMonitor);
            }
        }
        else if (suppressTransitionTransport && haveCurrentMonitor)
        {
            // Enter -> windowed uses the same presentation-only bridge that is
            // already proven on fullscreen entry. Capture the final fullscreen
            // frame and cloak only the real top-level HWND while its existing
            // style/placement/DWM restore path runs underneath. This deliberately
            // does not change any fullscreen chrome, libmpv, video-child or
            // SWP_FRAMECHANGED logic; it only prevents DWM from presenting the
            // half-restored intermediate frame.
            mediaTransitionShield =
                ShowMediaFullscreenTransitionShield(currentMonitor.rcMonitor);
        }

        g_fullscreen = false;
        if (info && info->page)
        {
            auto* page = winrt::get_self<
                winrt::HCPlayer::implementation::MainPage>(
                    info->page);

            if (suppressTransitionTransport)
            {
                // Exit is silent too. Do not call SetSettingsOverlayOpen(false)
                // first: that path intentionally reveals the transport and creates
                // a show -> hide race between the native island and its XAML root.
                page->PrepareSilentFullscreenEntry();

                // Restoring the window can synthesize mouse movement at the same
                // screen coordinate. Seed both movement guards with the current
                // physical cursor so only genuine user motion can reveal controls.
                POINT cursor{};
                if (GetCursorPos(&cursor))
                {
                    g_lastVideoMouseScreenPoint = cursor;
                    g_hasLastVideoMouseScreenPoint = true;
                }
            }
            else
            {
                page->SetSettingsOverlayOpen(false);
            }
        }
        SetThreadExecutionState(ES_CONTINUOUS);
        SetWindowLongPtrW(g_mainWindow, GWL_STYLE, g_previousStyle);
        SetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE, g_previousExStyle);
        SetWindowPlacement(g_mainWindow, &g_previousPlacement);
        SetWindowPos(g_mainWindow, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ApplyWindows11Visual(g_mainWindow);

        // A theme change performed while fullscreen deliberately sets
        // DWMWA_BORDER_COLOR to COLOR_NONE. Restore the correct normal-window
        // border here when leaving fullscreen; otherwise the light theme can
        // retain that fullscreen DWM state and expose an odd frame.
        constexpr DWORD noBorderColor = 0xFFFFFFFE;      // DWMWA_COLOR_NONE
        constexpr DWORD defaultBorderColor = 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT
        DWORD borderColor = g_borderless
            ? noBorderColor
            : defaultBorderColor;
        DwmSetWindowAttribute(
            g_mainWindow,
            DWMWA_BORDER_COLOR,
            &borderColor,
            sizeof(borderColor));

        endLayoutTransition();
        FinishMediaFullscreenTransitionShield(mediaTransitionShield);
        FinishIdleFullscreenTransitionShield(idleTransitionShield);
    }
    g_lastCursorActivityTick = GetTickCount64();
    SetApplicationCursorHidden(false);
    UpdateCursorAutohide();
}

void PlayerToggleBorderless()
{
    if (!g_mainWindow) return;

    SendMessageW(g_mainWindow, CloseSettingsMessage, 0, 0);
    SendMessageW(g_mainWindow, CloseContextMenuMessage, 0, 0);

    // Fullscreen and PiP already own a temporary borderless style. Changing
    // the normal-window chrome underneath either mode would corrupt the style
    // snapshot used when returning to the regular window.
    if (g_fullscreen || g_pictureInPicture)
    {
        if (g_mpv.handle)
        {
            std::string localizedMessage = winrt::to_string(
                PlayerUiString(
                    L"OsdBorderlessWindowOnly",
                    L"Sem bordas está disponível no modo janela"));
            const char* message[] = {
                "show-text", localizedMessage.c_str(), nullptr
            };
            g_mpv.command(g_mpv.handle, message);
        }
        return;
    }

    if (!g_borderless)
    {
        g_borderedStyle = GetWindowLongPtrW(g_mainWindow, GWL_STYLE);
        g_borderedExStyle = GetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE);
        g_borderless = true;

        LONG_PTR style = (g_borderedStyle &
            ~(WS_CAPTION | WS_BORDER | WS_DLGFRAME | WS_SYSMENU |
                WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) |
            WS_POPUP | WS_VISIBLE | WS_THICKFRAME |
            WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        LONG_PTR exStyle = g_borderedExStyle &
            ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
                WS_EX_DLGMODALFRAME);
        SetWindowLongPtrW(g_mainWindow, GWL_STYLE, style);
        SetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE, exStyle);
    }
    else
    {
        CancelBorderlessDrag();
        g_borderless = false;
        SetWindowLongPtrW(g_mainWindow, GWL_STYLE, g_borderedStyle);
        SetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE, g_borderedExStyle);
    }

    SetWindowPos(g_mainWindow, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
        SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
    if (info)
    {
        RECT client{};
        GetClientRect(g_mainWindow, &client);
        ApplyClientLayout(g_mainWindow, info,
            client.right - client.left, client.bottom - client.top);
    }
    ApplyWindows11Visual(g_mainWindow);

    constexpr DWORD noBorderColor = 0xFFFFFFFE; // DWMWA_COLOR_NONE
    constexpr DWORD defaultBorderColor = 0xFFFFFFFF;
    DWORD borderColor = g_borderless ? noBorderColor : defaultBorderColor;
    DwmSetWindowAttribute(g_mainWindow, DWMWA_BORDER_COLOR,
        &borderColor, sizeof(borderColor));

    if (g_mpv.handle)
    {
        std::wstring localized = PlayerUiString(
            g_borderless ? L"OsdBorderlessEnabled" : L"OsdBorderlessDisabled",
            g_borderless ? L"Janela sem bordas: Ativada"
                         : L"Janela sem bordas: Desativada");
        std::string localizedMessage = winrt::to_string(localized);
        const char* message[] = {
            "show-text", localizedMessage.c_str(), nullptr
        };
        g_mpv.command(g_mpv.handle, message);
    }
}

void PlayerTogglePictureInPicture()
{
    if (!g_mainWindow) return;

    SendMessageW(g_mainWindow, CloseSettingsMessage, 0, 0);
    SendMessageW(g_mainWindow, CloseContextMenuMessage, 0, 0);

    auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));

    bool const transitionFromFullscreen =
        g_fullscreen && !g_pictureInPicture;

    if (transitionFromFullscreen)
    {
        // Keep the established fullscreen -> window -> PiP state logic exactly
        // as-is, but make those intermediate top-level frames unpresentable.
        // This is visual isolation only; playback and the D3D11 child are not
        // paused, recreated or reconfigured.
        g_pipEntryLayoutTransition = true;
        if (info)
        {
            RECT currentClient{};
            if (GetClientRect(g_mainWindow, &currentClient))
            {
                ApplyClientLayout(
                    g_mainWindow,
                    info,
                    currentClient.right - currentClient.left,
                    currentClient.bottom - currentClient.top);
            }
        }
        SetPipEntryWindowCloaked(g_mainWindow, true);
    }

    if (g_fullscreen) PlayerToggleFullscreen();

    if (!g_pictureInPicture)
    {
        bool const waitForTransportRehost =
            transitionFromFullscreen &&
            (g_transportMinimal ||
                (info && info->transportHostedInPopup));

        MONITORINFO monitor{ sizeof(monitor) };
        if (!GetWindowPlacement(g_mainWindow, &g_pipPreviousPlacement) ||
            !GetMonitorInfoW(MonitorFromWindow(
                g_mainWindow, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            if (g_pipEntryLayoutTransition)
            {
                FinishPipEntryLayoutTransition(g_mainWindow, info);
            }
            return;
        }

        g_pipPreviousStyle = GetWindowLongPtrW(g_mainWindow, GWL_STYLE);
        g_pipPreviousExStyle = GetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE);
        g_pictureInPicture = true;

        if (info && info->page)
        {
            winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
                info->page)->SetPictureInPictureMode(true);
        }

        if (info && info->xamlSource && info->page)
        {
            HWND transportWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
                info->xamlSource.SiteBridge().WindowId());
            InstallTransportSubclasses(transportWindow,
                reinterpret_cast<DWORD_PTR>(
                    winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
                        info->page)));
        }

        // A pure borderless popup avoids the bright native sizing frame. The
        // video and transport children provide invisible eight-pixel resize
        // grips, so the PiP remains freely resizable without visual chrome.
        LONG_PTR style = WS_POPUP | WS_VISIBLE | WS_THICKFRAME |
            WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        LONG_PTR exStyle = (g_pipPreviousExStyle &
            ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
                WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW)) |
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
        SetWindowLongPtrW(g_mainWindow, GWL_STYLE, style);
        SetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE, exStyle);

        int const pipWidth = DipToPx(g_mainWindow, PictureInPictureWidth);
        int const pipHeight = DipToPx(g_mainWindow, PictureInPictureHeight);
        int const pipMargin = DipToPx(g_mainWindow, 20);
        int x = monitor.rcWork.right - pipWidth - pipMargin;
        int y = monitor.rcWork.bottom - pipHeight - pipMargin;
        SetWindowPos(g_mainWindow, HWND_TOPMOST, x, y,
            pipWidth, pipHeight,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        ApplyWindows11Visual(g_mainWindow);
        constexpr DWORD noBorderColor = 0xFFFFFFFE; // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(g_mainWindow, DWMWA_BORDER_COLOR,
            &noBorderColor, sizeof(noBorderColor));

        if (transitionFromFullscreen)
        {
            if (!waitForTransportRehost)
            {
                // Classic Bar is already fully built at final PiP geometry.
                // Minimal remains cloaked until its queued XAML rehost finishes.
                FinishPipEntryLayoutTransition(g_mainWindow, info);
            }
        }
        else
        {
            SetForegroundWindow(g_mainWindow);
        }
    }
    else
    {
        // Freeze the transport before restoring the top-level HWND.  Win32
        // delivers WM_SIZE/WM_MOVE synchronously during SetWindowPlacement /
        // SWP_FRAMECHANGED; without this guard DWM can briefly show the PiP
        // toolbar stretched inside the restored window.  This is especially
        // visible when Minimal mode must subsequently rehost into its popup.
        g_pipReturnLayoutTransition = true;
        if (info)
        {
            RECT currentClient{};
            if (GetClientRect(g_mainWindow, &currentClient))
            {
                ApplyClientLayout(g_mainWindow, info,
                    currentClient.right - currentClient.left,
                    currentClient.bottom - currentClient.top);
            }
        }

        g_pictureInPicture = false;
        SetWindowLongPtrW(g_mainWindow, GWL_STYLE, g_pipPreviousStyle);
        SetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE, g_pipPreviousExStyle);
        SetWindowPlacement(g_mainWindow, &g_pipPreviousPlacement);
        HWND zOrder = (g_pipPreviousExStyle & WS_EX_TOPMOST)
            ? HWND_TOPMOST : HWND_NOTOPMOST;
        SetWindowPos(g_mainWindow, zOrder, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER |
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        ApplyWindows11Visual(g_mainWindow);
        constexpr DWORD defaultBorderColor = 0xFFFFFFFF;
        DwmSetWindowAttribute(g_mainWindow, DWMWA_BORDER_COLOR,
            &defaultBorderColor, sizeof(defaultBorderColor));

        if (info && info->page)
        {
            winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
                info->page)->SetPictureInPictureMode(false);
        }

        // Classic Bar does not need a XAML rehost, so it can be revealed on the
        // final layout below.  Minimal keeps the guard until ScheduleTransportRehost
        // has attached the page to the rounded popup at the restored geometry.
        if (!g_transportMinimal)
        {
            g_pipReturnLayoutTransition = false;
        }
        SetForegroundWindow(g_mainWindow);
    }

    if (info)
    {
        RECT client{};
        GetClientRect(g_mainWindow, &client);
        ApplyClientLayout(g_mainWindow, info,
            client.right - client.left, client.bottom - client.top);
    }
}

namespace
{
    void WritePortableRegistrationStatus(std::wstring_view message) noexcept
    {
        if (message.empty()) return;

        bool attachedHere = false;
        bool wrote = false;
        if (AttachConsole(ATTACH_PARENT_PROCESS))
        {
            attachedHere = true;
        }
        else if (GetLastError() != ERROR_ACCESS_DENIED)
        {
            // No parent console is normal when a command is launched outside a
            // terminal. Fall back to a small message box below.
        }

        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output && output != INVALID_HANDLE_VALUE)
        {
            DWORD mode{};
            if (GetConsoleMode(output, &mode))
            {
                DWORD written{};
                std::wstring text(message);
                text.append(L"\r\n");
                wrote = WriteConsoleW(
                    output,
                    text.c_str(),
                    static_cast<DWORD>(text.size()),
                    &written,
                    nullptr) != FALSE;
            }
        }

        if (attachedHere) FreeConsole();

        if (!wrote)
        {
            MessageBoxW(
                nullptr,
                std::wstring(message).c_str(),
                L"HC Player Portable",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        }
    }

    bool TryHandlePortableRegistrationCommand(
        std::wstring const& fullCommandLine,
        int& exitCode) noexcept
    {
        exitCode = 0;
        if (fullCommandLine.empty()) return false;

        int argumentCount{};
        LPWSTR* arguments = CommandLineToArgvW(
            fullCommandLine.c_str(), &argumentCount);
        if (!arguments) return false;

        bool registerCommand = false;
        bool unregisterCommand = false;
        if (argumentCount >= 2)
        {
            registerCommand = _wcsicmp(arguments[1], L"--register") == 0;
            unregisterCommand = _wcsicmp(arguments[1], L"--unregister") == 0;
        }

        if (!registerCommand && !unregisterCommand)
        {
            LocalFree(arguments);
            return false;
        }

        if (argumentCount != 2 || (registerCommand && unregisterCommand))
        {
            LocalFree(arguments);
            WritePortableRegistrationStatus(
                L"Usage: HC Player.exe --register\n"
                L"       HC Player.exe --unregister");
            exitCode = 2;
            return true;
        }

        LocalFree(arguments);

        std::wstring status;
        bool const success = registerCommand
            ? hc::portable_registration::Register(status)
            : hc::portable_registration::Unregister(status);

        WritePortableRegistrationStatus(status);
        exitCode = success ? 0 : 1;
        return true;
    }
}

bool IsPlayerMessageWindow(HWND messageWindow)
{
    if (!g_mainWindow || !messageWindow) return false;
    if (messageWindow == g_mainWindow || IsChild(g_mainWindow, messageWindow))
        return true;
    auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA));
    return info && info->transportHostWindow &&
        (messageWindow == info->transportHostWindow ||
            IsChild(info->transportHostWindow, messageWindow));
}

int APIENTRY wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE previousInstance,
    _In_ LPWSTR commandLine,
    _In_ int showCommand)
{
    UNREFERENCED_PARAMETER(previousInstance);
    try
    {
        // The manifest declares PerMonitorV2. This early call is a defensive
        // fallback for unpackaged/debug launches where manifest merging is
        // accidentally bypassed. ERROR_ACCESS_DENIED simply means the manifest
        // already established the process DPI context, which is the normal case.
        SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        // Instance policy is a shell preference stored in native-options.dat,
        // so it can be resolved before WinRT/WinUI or libmpv is initialized.
        // This keeps a secondary launch extremely light.
        g_mpvSettingsManager.LoadNativeOptions();

        // Installed mode marks its per-user data namespace so the elevated
        // uninstaller can prove ownership before recursively removing it.
        // Portable mode owns .\Data beside the executable and has no uninstaller.
        if (!hc::storage::IsPortableMode())
        {
            EnsureUserDataOwnershipMarker();
        }

        // The installer invokes this one-shot command in the original desktop
        // user's context. It only seeds ui-language when that preference does
        // not already exist, then exits before WinUI, file associations or mpv
        // are initialized.
        int installerLanguageExitCode{};
        if (TryApplyInstallerInitialLanguage(
            GetCommandLineW(), installerLanguageExitCode))
        {
            return installerLanguageExitCode;
        }

        // Portable Shell/SMTC registration is intentionally explicit. These
        // commands exit before single-instance coordination, WinUI, file
        // associations or libmpv initialization. Normal Portable launches still
        // write nothing outside .\Data unless the user opted in via --register.
        int portableRegistrationExitCode{};
        if (TryHandlePortableRegistrationCommand(
            GetCommandLineW(), portableRegistrationExitCode))
        {
            return portableRegistrationExitCode;
        }

        LoadStringW(instance, IDS_APP_TITLE, g_title, MAX_LOADSTRING);
        LoadStringW(instance, IDC_HCPLAYER, g_windowClass, MAX_LOADSTRING);

        if (SingleInstanceModeEnabled() &&
            !BecomeSingleInstancePrimaryOrForward())
        {
            return 0;
        }

        // Keep file-type integration isolated from WinUI and playback. Installed
        // mode performs the established best-effort per-user registration. A
        // portable copy deliberately leaves the Registry and Windows defaults alone.
        if (!hc::storage::IsPortableMode())
        {
            hc::file_associations::EnsureRegistered();
        }

        winrt::init_apartment(winrt::apartment_type::single_threaded);
        struct OleLifetime
        {
            HRESULT result{ OleInitialize(nullptr) };
            ~OleLifetime() { if (SUCCEEDED(result)) OleUninitialize(); }
        } oleLifetime;
        auto dispatcher = winrt::DispatcherQueueController::CreateOnCurrentThread();

        // Localization must be resolved before the WinUI App object is created,
        // because XAML x:Uid resources are selected while the visual tree loads.
        // The setting remains an ordinary ui-* override and never reaches mpv.
        std::wstring languagePreference = L"system";
        if (auto language = g_mpvSettingsManager.Overrides().find("ui-language");
            language != g_mpvSettingsManager.Overrides().end())
        {
            languagePreference = winrt::to_hstring(language->second).c_str();
        }
        hc::localization::ApplyPrimaryLanguageOverride(languagePreference);

        auto app = winrt::make<winrt::HCPlayer::implementation::App>();
        g_shaderManager.Load();

        auto storedConfig = g_mpvSettingsManager.ImportedConfigStoragePath();
        if (std::filesystem::exists(storedConfig))
        {
            // Loading the persisted imported mpv.conf rebuilds the imported
            // option model, but it must not overwrite a subtitle font the user
            // selected later in HC Player's own settings panel. Preserve that
            // explicit UI choice so the documented precedence remains:
            // built-in defaults < imported config < user overrides.
            auto const savedSubtitleFont =
                g_mpvSettingsManager.Overrides().find("sub-font");
            bool const hadSavedSubtitleFont =
                savedSubtitleFont != g_mpvSettingsManager.Overrides().end();
            std::string const subtitleFont = hadSavedSubtitleFont
                ? savedSubtitleFont->second
                : std::string{};

            PlayerImportMpvConfig(storedConfig.wstring());

            if (hadSavedSubtitleFont)
            {
                auto& overrides = g_mpvSettingsManager.Overrides();
                auto current = overrides.find("sub-font");
                if (current == overrides.end() || current->second != subtitleFont)
                {
                    overrides["sub-font"] = subtitleFont;
                    g_mpvSettingsManager.MarkDirty();
                    g_mpvSettingsManager.SaveNativeOptions();
                }
            }
        }

        g_taskbarButtonCreatedMessage =
            RegisterWindowMessageW(L"TaskbarButtonCreated");
        RegisterMainWindowClass(instance);

        // A shell file launch used to expose the no-media transport for one
        // painted frame because CreateMainWindow showed the HWND before the
        // command-line path reached MainPage::OpenPath(). Cold shell media
        // launches now begin with the top-level HWND hidden only
        // until a black client shield can be installed. The .36 test then shows
        // the app immediately and keeps only the video presentation masked.
        bool const deferInitialMediaReveal =
            HasStartupMediaArgument(GetCommandLineW());

        if (!CreateMainWindow(
            instance, deferInitialMediaReveal ? SW_HIDE : showCommand))
        {
            return FALSE;
        }

        // Optional Windows 11 media-session integration. It only publishes
        // state/metadata and routes system Play/Pause back to the UI thread.
        hc::system_media_controls::Initialize(
            g_mainWindow, SystemMediaControlsCommandMessage);

        if (auto topmost = g_mpvSettingsManager.Overrides().find("ui-ontop");
            topmost != g_mpvSettingsManager.Overrides().end() && topmost->second == "yes")
        {
            SetWindowPos(g_mainWindow, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        if (g_singleInstanceMutex)
        {
            SetPropW(
                g_mainWindow,
                SingleInstancePrimaryPropertyName().c_str(),
                reinterpret_cast<HANDLE>(1));
        }

        if (deferInitialMediaReveal)
        {
            // Install the cover before the first visible frame, then show the
            // app immediately. HandleLaunchCommandLine starts mpv underneath it.
            BeginDeferredStartupMediaReveal(showCommand);
        }

        HandleLaunchCommandLine(GetCommandLineW(), false);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0))
        {
            if (message.message == WM_MOUSEMOVE &&
                IsPlayerMessageWindow(message.hwnd))
            {
                RegisterCursorActivity();
            }
            if (message.message == WM_MOUSEWHEEL && !IsSidePanelOpen() &&
                !g_contextMenuOpen && IsPlayerMessageWindow(message.hwnd))
            {
                if (PlayerAdjustVolumeFromWheel(
                    GET_WHEEL_DELTA_WPARAM(message.wParam)))
                {
                    continue;
                }
            }
            if (HandlePlayerKeyMessage(message))
            {
                continue;
            }
            if (::ContentPreTranslateMessage(&message))
            {
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        hc::system_media_controls::Shutdown();
        g_mpv.Stop();
        // Let C++/WinRT release the XAML application and dispatcher in their
        // natural reverse-construction order. Calling ShutdownQueue here while
        // the Windows App SDK is still draining work from a secondary XAML
        // island (settings/context menu) can crash in Microsoft.UI.Xaml.dll.
        return static_cast<int>(message.wParam);
    }
    catch (winrt::hresult_error const& error)
    {
        MessageBoxW(nullptr, error.message().c_str(), L"HC Player", MB_ICONERROR);
        return error.code().value;
    }
}

ATOM RegisterMainWindowClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    // Child surfaces cover the client area and lay themselves out on WM_SIZE.
    // Class-wide redraw flags would invalidate the complete window for every
    // intermediate resize position.
    windowClass.style = 0;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIcon(instance, MAKEINTRESOURCE(IDI_HCPLAYER));
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Child HWNDs are resized independently. Pure black here guarantees that
    // a compositor frame caught between those operations never exposes a
    // grey seam around the video surface.
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = g_windowClass;
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCE(IDI_HCPLAYER),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    ATOM mainWindowClass = RegisterClassExW(&windowClass);

    // The system STATIC class erases itself with COLOR_WINDOW (white), which
    // creates a bright flash before libmpv presents its first video frame.
    WNDCLASSEXW videoClass{};
    videoClass.cbSize = sizeof(videoClass);
    videoClass.style = CS_OWNDC | CS_DBLCLKS;
    videoClass.lpfnWndProc = VideoWindowProc;
    videoClass.hInstance = instance;
    videoClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    videoClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    videoClass.lpszClassName = VideoWindowClassName;
    RegisterClassExW(&videoClass);

    WNDCLASSEXW transportHostClass{};
    transportHostClass.cbSize = sizeof(transportHostClass);
    transportHostClass.lpfnWndProc = TransportHostProc;
    transportHostClass.hInstance = instance;
    transportHostClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    transportHostClass.hbrBackground = nullptr;
    transportHostClass.lpszClassName = TransportHostClassName;
    RegisterClassExW(&transportHostClass);

    WNDCLASSEXW gripClass{};
    gripClass.cbSize = sizeof(gripClass);
    gripClass.lpfnWndProc = PipResizeGripProc;
    gripClass.hInstance = instance;
    gripClass.hCursor = LoadCursor(nullptr, IDC_SIZENS);
    gripClass.hbrBackground = nullptr;
    gripClass.lpszClassName = PipResizeGripClassName;
    RegisterClassExW(&gripClass);

    WNDCLASSEXW captionClass{};
    captionClass.cbSize = sizeof(captionClass);
    captionClass.lpfnWndProc = BorderlessCaptionProc;
    captionClass.hInstance = instance;
    captionClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    captionClass.hbrBackground = nullptr;
    captionClass.lpszClassName = BorderlessCaptionClassName;
    RegisterClassExW(&captionClass);

    WNDCLASSEXW shieldClass{};
    shieldClass.cbSize = sizeof(shieldClass);
    shieldClass.lpfnWndProc = FullscreenTransitionShieldProc;
    shieldClass.hInstance = instance;
    shieldClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    shieldClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    shieldClass.lpszClassName = FullscreenTransitionShieldClassName;
    RegisterClassExW(&shieldClass);

    WNDCLASSEXW mediaShieldClass{};
    mediaShieldClass.cbSize = sizeof(mediaShieldClass);
    mediaShieldClass.lpfnWndProc = MediaFullscreenTransitionShieldProc;
    mediaShieldClass.hInstance = instance;
    mediaShieldClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    mediaShieldClass.hbrBackground =
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    mediaShieldClass.lpszClassName = MediaFullscreenTransitionShieldClassName;
    RegisterClassExW(&mediaShieldClass);

    WNDCLASSEXW initialMediaShieldClass{};
    initialMediaShieldClass.cbSize = sizeof(initialMediaShieldClass);
    initialMediaShieldClass.lpfnWndProc = InitialMediaRevealShieldProc;
    initialMediaShieldClass.hInstance = instance;
    initialMediaShieldClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    initialMediaShieldClass.hbrBackground =
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    initialMediaShieldClass.lpszClassName = InitialMediaRevealShieldClassName;
    RegisterClassExW(&initialMediaShieldClass);

    return mainWindowClass;
}

BOOL CreateMainWindow(HINSTANCE instance, int showCommand)
{
    g_instance = instance;

    UINT startupDpi = GetDpiForSystem();
    if (!startupDpi) startupDpi = USER_DEFAULT_SCREEN_DPI;
    int const initialWidth = MulDiv(
        1180, static_cast<int>(startupDpi), USER_DEFAULT_SCREEN_DPI);
    int const initialHeight = MulDiv(
        760, static_cast<int>(startupDpi), USER_DEFAULT_SCREEN_DPI);

    g_mainWindow = CreateWindowW(
        g_windowClass,
        L"HC Player",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        initialWidth,
        initialHeight,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!g_mainWindow)
    {
        return FALSE;
    }

    ApplyWindows11Visual(g_mainWindow);
    ApplyRememberedWindowSize();
    CenterMainWindowOnPrimaryWorkArea();

    if (auto* info = reinterpret_cast<WindowInfo*>(
        GetWindowLongPtrW(g_mainWindow, GWLP_USERDATA)))
    {
        RECT client{};
        if (GetClientRect(g_mainWindow, &client))
        {
            ApplyClientLayout(g_mainWindow, info,
                client.right - client.left, client.bottom - client.top);
        }
    }

    ShowWindow(g_mainWindow, showCommand);
    UpdateWindow(g_mainWindow);
    return TRUE;
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* info = reinterpret_cast<WindowInfo*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (g_taskbarButtonCreatedMessage &&
        message == g_taskbarButtonCreatedMessage)
    {
        // Explorer may recreate its taskbar at runtime. Recreate the COM
        // connection and thumbnail toolbar instead of leaving stale buttons.
        g_taskbarList = nullptr;
        g_taskbarButtonsAdded = false;
        PlayerUpdateTaskbarProgress();
        return 0;
    }

    switch (message)
    {
    case WM_ACTIVATE:
        if (info && info->transportHostedInPopup &&
            info->transportMicaConfiguration)
        {
            bool inputActive = LOWORD(wParam) != WA_INACTIVE;
            if (!inputActive)
            {
                // Minimal is an owned top-level popup with its own XAML island.
                // Moving activation from the main HWND to any HC Player window
                // (the pill, its island, or a WinUI flyout) is still an active
                // HC Player interaction and must not deactivate the Mica.
                HWND const activatingWindow = reinterpret_cast<HWND>(lParam);
                if (activatingWindow)
                {
                    DWORD activatingProcessId{};
                    GetWindowThreadProcessId(
                        activatingWindow, &activatingProcessId);
                    inputActive =
                        activatingProcessId == GetCurrentProcessId();
                }
            }
            UpdateMinimalTransportMica(info, inputActive);
        }
        break;

    case SystemMediaControlsCommandMessage:
    {
        auto const command = static_cast<hc::system_media_controls::Command>(wParam);

        if (command == hc::system_media_controls::Command::Previous)
        {
            PlayerChangeChapter(-1);
            PlayerUpdateTaskbarProgress();
            return 0;
        }
        if (command == hc::system_media_controls::Command::Next)
        {
            PlayerChangeChapter(1);
            PlayerUpdateTaskbarProgress();
            return 0;
        }

        bool paused{};
        bool eofReached{};
        if (!PlayerGetPlaybackState(paused, eofReached))
        {
            return 0;
        }

        if (command == hc::system_media_controls::Command::Play)
        {
            if (eofReached)
            {
                PlayerReplay();
            }
            else if (paused)
            {
                PlayerTogglePause();
            }
        }
        else if (command == hc::system_media_controls::Command::Pause &&
            !paused && !eofReached)
        {
            PlayerTogglePause();
        }
        return 0;
    }

    case WM_COPYDATA:
    {
        auto const* data = reinterpret_cast<COPYDATASTRUCT const*>(lParam);
        if (!data ||
            data->dwData != SingleInstanceCopyDataId ||
            !data->lpData ||
            data->cbData < sizeof(wchar_t) ||
            (data->cbData % sizeof(wchar_t)) != 0)
        {
            return FALSE;
        }

        size_t const characterCount =
            data->cbData / sizeof(wchar_t);
        auto const* text =
            static_cast<wchar_t const*>(data->lpData);
        if (characterCount == 0 ||
            text[characterCount - 1] != L'\0')
        {
            return FALSE;
        }

        HandleLaunchCommandLine(
            std::wstring{ text, characterCount - 1 }, true);
        return TRUE;
    }

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        // Windows can switch its taskbar/thumbnail surface between light and
        // dark while HC Player is running. Re-publish only the toolbar icons;
        // playback state and the player pipeline are untouched.
        PlayerUpdateTaskbarProgress();
        break;

    case WM_NCPAINT:
        if (g_borderless)
            return 0;
        break;

    case WM_NCACTIVATE:
        if (g_borderless)
            return TRUE;
        break;

    case WM_NCCALCSIZE:
        if ((g_pictureInPicture || g_borderless) && wParam)
        {
            // Retain WS_THICKFRAME's native sizing loop, but make its complete
            // non-client frame part of our borderless client surface.
            return 0;
        }
        break;

    case WM_NCHITTEST:
        if (g_pictureInPicture)
        {
            POINT cursor{
                static_cast<short>(LOWORD(lParam)),
                static_cast<short>(HIWORD(lParam)) };
            int edges = PipResizeEdgesAt(cursor);
            if (edges != PipResizeNone) return PipHitTest(edges);
        }
        else if (g_borderless)
        {
            POINT cursor{
                static_cast<short>(LOWORD(lParam)),
                static_cast<short>(HIWORD(lParam)) };
            int edges = BorderlessResizeEdgesAt(cursor);
            if (edges != PipResizeNone) return PipHitTest(edges);
            return HTCAPTION;
        }
        break;

    case WM_ERASEBKGND:
    {
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    case WM_CREATE:
    {
        info = new WindowInfo();
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(info));
        g_lastCursorActivityTick = GetTickCount64();

        g_videoWindow = CreateWindowExW(
            0,
            VideoWindowClassName,
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, 100, 100,
            window,
            nullptr,
            g_instance,
            nullptr);

        g_pipBottomResizeWindow = CreateWindowExW(
            WS_EX_NOACTIVATE,
            PipResizeGripClassName,
            nullptr,
            WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 1, 1,
            window,
            nullptr,
            g_instance,
            nullptr);

        g_borderlessCaptionWindow = CreateWindowExW(
            WS_EX_NOACTIVATE,
            BorderlessCaptionClassName,
            nullptr,
            WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 1, 1,
            window,
            nullptr,
            g_instance,
            nullptr);

        info->xamlSource = winrt::DesktopWindowXamlSource{};
        info->xamlSource.Initialize(
            winrt::Microsoft::UI::GetWindowIdFromWindow(window));

        info->page =
            winrt::make<winrt::HCPlayer::implementation::MainPage>();
        info->xamlSource.Content(info->page);

        // Preserve the exact stable/classic Mica path. Minimal mode rehosts
        // this same MainPage later and uses its own explicit popup controller.
        QueueClassicTransportMica(window);

        // A tiny independent island keeps the animated buffering indicator in
        // the center of the video without expanding the transport island over
        // the D3D11 surface. It is non-interactive and normally a hidden HWND.
        info->bufferingSource = winrt::DesktopWindowXamlSource{};
        info->bufferingSource.Initialize(
            winrt::Microsoft::UI::GetWindowIdFromWindow(window));
        winrt::Microsoft::UI::Xaml::Controls::Grid bufferingRoot;
        bufferingRoot.Background(
            winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
                winrt::Windows::UI::Color{ 0, 0, 0, 0 } });
        bufferingRoot.IsHitTestVisible(false);
        info->bufferingRing =
            winrt::Microsoft::UI::Xaml::Controls::ProgressRing{};
        info->bufferingRing.Width(48.0);
        info->bufferingRing.Height(48.0);
        info->bufferingRing.IsActive(false);
        info->bufferingRing.IsHitTestVisible(false);
        info->bufferingRing.RequestedTheme(g_lightTheme
            ? winrt::Microsoft::UI::Xaml::ElementTheme::Light
            : winrt::Microsoft::UI::Xaml::ElementTheme::Dark);
        bufferingRoot.Children().Append(info->bufferingRing);
        info->bufferingSource.Content(bufferingRoot);
        HWND bufferingWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
            info->bufferingSource.SiteBridge().WindowId());
        LONG_PTR bufferingExStyle = GetWindowLongPtrW(
            bufferingWindow, GWL_EXSTYLE);
        SetWindowLongPtrW(bufferingWindow, GWL_EXSTYLE,
            bufferingExStyle | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        ShowWindow(bufferingWindow, SW_HIDE);

        // Empty player state: a small independent XAML island keeps the
        // branding crisp at every DPI while leaving the native libmpv surface
        // completely untouched once media is loaded.
        info->emptyStateSource = winrt::DesktopWindowXamlSource{};
        info->emptyStateSource.Initialize(
            winrt::Microsoft::UI::GetWindowIdFromWindow(window));

        winrt::Microsoft::UI::Xaml::Controls::Grid emptyStateRoot;
        emptyStateRoot.Background(
            winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
                winrt::Windows::UI::Color{ 0, 0, 0, 0 } });
        emptyStateRoot.IsHitTestVisible(false);

        winrt::Microsoft::UI::Xaml::Controls::StackPanel emptyStateContent;
        emptyStateContent.HorizontalAlignment(
            winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
        emptyStateContent.VerticalAlignment(
            winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
        emptyStateContent.Spacing(0.0);
        emptyStateContent.IsHitTestVisible(false);

        // Keep the original 120-DIP logo footprint so the approved empty-state
        // geometry does not move. The glow is allowed to paint outside this
        // tiny Canvas and therefore never changes StackPanel measurement.
        winrt::Microsoft::UI::Xaml::Controls::Canvas emptyStateIconCanvas;
        emptyStateIconCanvas.Width(120.0);
        emptyStateIconCanvas.Height(120.0);
        emptyStateIconCanvas.HorizontalAlignment(
            winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
        emptyStateIconCanvas.Margin({ 0.0, 0.0, 0.0, 20.0 });
        emptyStateIconCanvas.IsHitTestVisible(false);

        winrt::Microsoft::UI::Xaml::Controls::Grid emptyStateGlowLayer;
        emptyStateGlowLayer.Width(190.0);
        emptyStateGlowLayer.Height(190.0);
        emptyStateGlowLayer.HorizontalAlignment(
            winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
        emptyStateGlowLayer.VerticalAlignment(
            winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
        emptyStateGlowLayer.Opacity(0.0);
        emptyStateGlowLayer.IsHitTestVisible(false);
        winrt::Microsoft::UI::Xaml::Controls::Canvas::SetLeft(
            emptyStateGlowLayer, -35.0);
        winrt::Microsoft::UI::Xaml::Controls::Canvas::SetTop(
            emptyStateGlowLayer, -35.0);

        // Build the glow from many tiny low-alpha discs instead of a few
        // larger rings. This keeps the same lightweight XAML-only approach,
        // but visually blends into a much smoother and more uniform bloom.
        auto appendGlowDisc = [&](double size, winrt::Windows::UI::Color color)
        {
            winrt::Microsoft::UI::Xaml::Shapes::Ellipse disc;
            disc.Width(size);
            disc.Height(size);
            disc.HorizontalAlignment(
                winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
            disc.VerticalAlignment(
                winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
            disc.Fill(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{ color });
            disc.IsHitTestVisible(false);
            emptyStateGlowLayer.Children().Append(disc);
        };

        constexpr int glowSteps = 24;
        constexpr double glowOuterSize = 190.0;
        constexpr double glowInnerSize = 122.0;
        for (int i = 0; i < glowSteps; ++i)
        {
            double t = static_cast<double>(i) /
                static_cast<double>(glowSteps - 1);
            double eased = std::pow(t, 1.75);
            double size = glowOuterSize -
                ((glowOuterSize - glowInnerSize) * t);
            std::uint8_t alpha = static_cast<std::uint8_t>(
                std::lround(3.0 + (eased * 16.0)));
            std::uint8_t green = static_cast<std::uint8_t>(
                std::lround(86.0 + (t * 140.0)));
            appendGlowDisc(
                size,
                winrt::Windows::UI::Color{ alpha, 0, green, 255 });
        }

        winrt::Microsoft::UI::Xaml::Media::TranslateTransform glowTranslate;
        emptyStateGlowLayer.RenderTransform(glowTranslate);

        winrt::Microsoft::UI::Xaml::Controls::Image emptyStateIcon;
        winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage brandingBitmap;
        brandingBitmap.UriSource(
            winrt::Windows::Foundation::Uri{
                L"ms-appx:///Assets/Branding/HCPlayer.Brand.png" });
        emptyStateIcon.Source(brandingBitmap);
        emptyStateIcon.Width(120.0);
        emptyStateIcon.Height(120.0);
        emptyStateIcon.Stretch(
            winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform);
        emptyStateIcon.IsHitTestVisible(false);

        emptyStateIconCanvas.Children().Append(emptyStateGlowLayer);
        emptyStateIconCanvas.Children().Append(emptyStateIcon);
        info->emptyStateGlowLayer = emptyStateGlowLayer;
        info->emptyStateGlowTranslate = glowTranslate;
        info->emptyStateIcon = emptyStateIcon;

        winrt::Windows::UI::Text::FontWeight semibold{};
        semibold.Weight = 600;

        winrt::Microsoft::UI::Xaml::Controls::TextBlock emptyStateTitle;
        emptyStateTitle.Text(PlayerUiString(
            L"NativeEmptyStateTitle", L"Pronto para reproduzir"));
        emptyStateTitle.FontFamily(
            winrt::Microsoft::UI::Xaml::Media::FontFamily{
                L"Segoe UI Variable Display" });
        emptyStateTitle.FontSize(34.0);
        emptyStateTitle.FontWeight(semibold);
        emptyStateTitle.TextAlignment(
            winrt::Microsoft::UI::Xaml::TextAlignment::Center);
        emptyStateTitle.HorizontalAlignment(
            winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
        emptyStateTitle.Foreground(
            winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
                winrt::Windows::UI::Color{ 255, 245, 245, 245 } });
        emptyStateTitle.Margin({ 0.0, 0.0, 0.0, 9.0 });

        winrt::Microsoft::UI::Xaml::Controls::TextBlock emptyStateSubtitle;
        emptyStateSubtitle.Text(PlayerUiString(
            L"NativeEmptyStateSubtitle",
            L"Arraste um arquivo ou cole uma URL para começar"));
        emptyStateSubtitle.FontFamily(
            winrt::Microsoft::UI::Xaml::Media::FontFamily{
                L"Segoe UI Variable Text" });
        emptyStateSubtitle.FontSize(16.0);
        emptyStateSubtitle.TextAlignment(
            winrt::Microsoft::UI::Xaml::TextAlignment::Center);
        emptyStateSubtitle.TextWrapping(
            winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
        emptyStateSubtitle.MaxWidth(520.0);
        emptyStateSubtitle.HorizontalAlignment(
            winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
        emptyStateSubtitle.Foreground(
            winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
                winrt::Windows::UI::Color{ 255, 158, 158, 158 } });

        emptyStateContent.Children().Append(emptyStateIconCanvas);
        emptyStateContent.Children().Append(emptyStateTitle);
        emptyStateContent.Children().Append(emptyStateSubtitle);
        emptyStateRoot.Children().Append(emptyStateContent);
        info->emptyStateRoot = emptyStateRoot;
        info->emptyStateSource.Content(emptyStateRoot);

        // Seed the compositor state once. Hover transitions animate from these
        // values and never participate in XAML layout.
        auto emptyGlowVisual =
            winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(info->emptyStateGlowLayer);
        emptyGlowVisual.CenterPoint(
            winrt::Windows::Foundation::Numerics::float3{ 95.0f, 95.0f, 0.0f });
        emptyGlowVisual.Scale(
            winrt::Windows::Foundation::Numerics::float3{ 0.88f, 0.88f, 1.0f });
        auto emptyIconVisual =
            winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::
            GetElementVisual(info->emptyStateIcon);
        emptyIconVisual.CenterPoint(
            winrt::Windows::Foundation::Numerics::float3{ 60.0f, 60.0f, 0.0f });
        emptyIconVisual.Scale(
            winrt::Windows::Foundation::Numerics::float3{ 1.0f, 1.0f, 1.0f });

        HWND emptyStateWindow =
            winrt::Microsoft::UI::GetWindowFromWindowId(
                info->emptyStateSource.SiteBridge().WindowId());
        LONG_PTR emptyStateExStyle = GetWindowLongPtrW(
            emptyStateWindow, GWL_EXSTYLE);
        SetWindowLongPtrW(
            emptyStateWindow,
            GWL_EXSTYLE,
            emptyStateExStyle |
            WS_EX_TRANSPARENT |
            WS_EX_NOACTIVATE);

        // WS_EX_TRANSPARENT keeps the branding island visually/passively
        // composed, but it does not make the native HWND disappear from OLE
        // drag/drop or context-menu routing. Register the same media drop target
        // already used by the main/video/transport HWNDs and forward right-click
        // through a tiny subclass. The XAML tree itself remains non-interactive.
        RegisterMediaDropTarget(emptyStateWindow);
        InstallEmptyStateSubclasses(emptyStateWindow);

        ShowWindow(emptyStateWindow, SW_HIDE);

        // WM_SIZE can arrive before the XAML island exists. Apply the initial
        // layout now so the transparent island does not cover the video area.
        RECT client{};
        GetClientRect(window, &client);
        int width = client.right - client.left;
        int height = client.bottom - client.top;
        int controlsHeight = min(CurrentControlsHeightPx(window), height);
        MoveWindow(g_videoWindow, 0, 0, width, height, FALSE);
        info->xamlSource.SiteBridge().MoveAndResize(
            { 0, max(0, height - controlsHeight), width, controlsHeight });
        ApplyClientLayout(window, info, width, height);
        HWND transportWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
            info->xamlSource.SiteBridge().WindowId());
        g_transportDropWindow = transportWindow;
        RegisterMediaDropTarget(window);
        RegisterMediaDropTarget(g_videoWindow);
        RegisterMediaDropTarget(transportWindow);
        InstallTransportSubclasses(transportWindow,
            reinterpret_cast<DWORD_PTR>(
                winrt::get_self<winrt::HCPlayer::implementation::MainPage>(info->page)));
        // This tiny native poll remains alive even when the WinUI island itself
        // is hidden, so entering the bottom hot zone always restores controls.
        SetTimer(window, TransportPointerTimer, 50, nullptr);

        // Construct the settings island before libmpv starts presenting. The
        // costly DirectComposition tree insertion therefore happens during
        // startup, not at the moment the user opens Settings over a video.
        info->settingsSource = winrt::DesktopWindowXamlSource{};
        info->settingsSource.Initialize(
            winrt::Microsoft::UI::GetWindowIdFromWindow(window));
        info->settingsPage =
            winrt::make<winrt::HCPlayer::implementation::SettingsPage>();
        info->settingsSource.Content(info->settingsPage);
        info->settingsSource.SiteBridge().MoveAndResize({ width + 1, 0, 1, 1 });
        HWND settingsWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
            info->settingsSource.SiteBridge().WindowId());
        auto* settingsImplementation =
            winrt::get_self<winrt::HCPlayer::implementation::SettingsPage>(
                info->settingsPage);
        SetWindowSubclass(settingsWindow, SettingsPanelSubclassProc, 1,
            reinterpret_cast<DWORD_PTR>(settingsImplementation));
        ShowWindow(settingsWindow, SW_HIDE);

        // Media Information mirrors Settings but lives on the LEFT. Pre-create
        // its XAML island during startup so the first click has no compositor
        // construction hitch. MediaInfo.dll itself is still loaded on demand.
        info->mediaInfoSource = winrt::DesktopWindowXamlSource{};
        info->mediaInfoSource.Initialize(
            winrt::Microsoft::UI::GetWindowIdFromWindow(window));
        info->mediaInfoPage =
            winrt::make<
                winrt::HCPlayer::implementation::MediaInfoPage>();
        info->mediaInfoSource.Content(info->mediaInfoPage);
        info->mediaInfoSource.SiteBridge().MoveAndResize({ -2, 0, 1, 1 });

        HWND mediaInfoWindow =
            winrt::Microsoft::UI::GetWindowFromWindowId(
                info->mediaInfoSource.SiteBridge().WindowId());

        auto* mediaInfoImplementation =
            winrt::get_self<
                winrt::HCPlayer::implementation::MediaInfoPage>(
                    info->mediaInfoPage);

        SetWindowSubclass(
            mediaInfoWindow,
            MediaInfoPanelSubclassProc,
            1,
            reinterpret_cast<DWORD_PTR>(mediaInfoImplementation));

        ShowWindow(mediaInfoWindow, SW_HIDE);
        return 0;
    }

    case WM_SIZE:
        if (info)
        {
            if (wParam == SIZE_MINIMIZED)
            {
                g_mainWindowWasMinimized = true;
                g_minimalCursorOutsideOwner = false;

                if (g_transportMinimal && info->page)
                {
                    // Reuse the already-proven immediate transport collapse:
                    // stop pending XAML hide/collapse work and make the logical
                    // transport state hidden before Windows starts minimizing
                    // the owner. No fullscreen state is changed by this call.
                    winrt::get_self<
                        winrt::HCPlayer::implementation::MainPage>(
                            info->page)->PrepareSilentFullscreenEntry();
                }

                if (info->transportHostWindow)
                    ShowWindow(info->transportHostWindow, SW_HIDE);
            }
            else if (g_mainWindowWasMinimized)
            {
                g_mainWindowWasMinimized = false;
                g_minimalCursorOutsideOwner = false;

                if (g_transportMinimal && !g_transportHostVisible)
                {
                    // Treat the cursor position at restore as a new baseline.
                    // Window/layout motion can synthesize WM_MOUSEMOVE before
                    // DWM has visibly presented the owner; only movement after
                    // this point is allowed to reveal the Minimal popup.
                    POINT cursor{};
                    if (GetCursorPos(&cursor))
                    {
                        g_transportHiddenCursor = cursor;
                        g_hasTransportHiddenCursor = true;
                        g_lastVideoMouseScreenPoint = cursor;
                        g_hasLastVideoMouseScreenPoint = true;
                    }
                }
            }
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            // Keep the child surfaces attached to the pointer. The D3D11
            // presenter is paced to the compositor during the sizing loop,
            // so an additional Win32 timer only adds visible stepping/jitter.
            ApplyClientLayout(window, info, width, height);
            ResizeInitialMediaRevealShield();
            // The context-menu island is deliberately only 1x1 at its anchor.
            // Expanding a transparent XAML island over the embedded D3D11 HWND
            // occludes the MPV swap chain and makes video appear black.
        }
        return 0;

    case WM_DPICHANGED:
    {
        // PerMonitorV2 keeps XAML content crisp automatically. The native
        // top-level/child HWND geometry still lives in physical pixels, so use
        // Windows' suggested outer rect and then recompute all child hosts.
        auto const* suggested = reinterpret_cast<RECT const*>(lParam);
        if (suggested && !g_fullscreen)
        {
            SetWindowPos(window, nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }

        if (info)
        {
            RECT client{};
            if (GetClientRect(window, &client))
            {
                ApplyClientLayout(window, info,
                    client.right - client.left,
                    client.bottom - client.top);
                ResizeInitialMediaRevealShield();
            }
        }

        ApplyWindows11Visual(window);
        return 0;
    }

    case WM_GETMINMAXINFO:
    {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        UINT dpi = GetDpiForWindow(window);
        if (!dpi) dpi = USER_DEFAULT_SCREEN_DPI;
        if (g_pictureInPicture)
        {
            limits->ptMinTrackSize = {
                MulDiv(320, dpi, USER_DEFAULT_SCREEN_DPI),
                MulDiv(220, dpi, USER_DEFAULT_SCREEN_DPI) };
        }
        else
        {
            limits->ptMinTrackSize = {
                MulDiv(NormalMinimumWidth, dpi, USER_DEFAULT_SCREEN_DPI),
                MulDiv(NormalMinimumHeight, dpi, USER_DEFAULT_SCREEN_DPI) };
        }
        return 0;
    }

    case WM_COMMAND:
        if (HIWORD(wParam) == THBN_CLICKED)
        {
            switch (LOWORD(wParam))
            {
            case TaskbarPreviousButtonId:
                PlayerChangeChapter(-1);
                PlayerUpdateTaskbarProgress();
                return 0;
            case TaskbarPlayPauseButtonId:
                PlayerTogglePause();
                PlayerUpdateTaskbarProgress();
                return 0;
            case TaskbarNextButtonId:
                PlayerChangeChapter(1);
                PlayerUpdateTaskbarProgress();
                return 0;
            }
        }
        break;

    case WM_MOVE:
        if (info && (IsSidePanelOpen() || info->transportHostedInPopup))
        {
            RECT client{};
            GetClientRect(window, &client);
            if (info->transportHostedInPopup && !IsSidePanelOpen())
            {
                int const width = client.right - client.left;
                int const height = client.bottom - client.top;
                int const transportHeight =
                    min(CurrentControlsHeightPx(window), height);
                bool const showTransport =
                    !g_fullscreenLayoutTransition &&
                    !g_pipReturnLayoutTransition &&
                    g_transportHostVisible &&
                    IsWindowVisible(window) && !IsIconic(window);
                ApplyTransportLayoutOnly(window, info, width, height,
                    showTransport, transportHeight);
            }
            else
            {
                ApplyClientLayout(window, info,
                    client.right - client.left, client.bottom - client.top);
            }
        }
        return 0;

    case TransportVisibilityMessage:
        if (info)
        {
            g_transportHostVisible = wParam != FALSE;
            if (g_transportHostVisible)
            {
                g_minimalCursorOutsideOwner = false;
                g_hasTransportHiddenCursor = false;
            }
            else
            {
                g_hasTransportHiddenCursor =
                    GetCursorPos(&g_transportHiddenCursor) != FALSE;
                if (g_hasTransportHiddenCursor)
                {
                    // Hiding/repositioning the XAML island can make Windows emit
                    // WM_MOUSEMOVE even though the physical pointer did not move.
                    // Seed the native movement filter from the same hide-time
                    // cursor snapshot so that layout motion cannot immediately
                    // reveal an otherwise hidden transport.
                    g_lastVideoMouseScreenPoint = g_transportHiddenCursor;
                    g_hasLastVideoMouseScreenPoint = true;
                }
            }
            RECT client{};
            GetClientRect(window, &client);
            ApplyClientLayout(window, info,
                client.right - client.left, client.bottom - client.top);
        }
        return 0;

    case ReleaseTransportFocusMessage:
        if (g_pictureInPicture)
        {
            SetFocus(window);
        }
        return 0;

    case WM_ENTERSIZEMOVE:
        if (info)
        {
            // MPC-HC updates its destination rectangle immediately and relies
            // on its presenter for pacing. Our embedded mpv swap chain uses an
            // unlocked interval by default, so temporarily pace presentation
            // to the DWM only during interactive resize. This does not change
            // the user's normal playback preference.
            if (g_mpv.handle && g_mpv.setProperty)
            {
                g_mpv.setProperty(g_mpv.handle,
                    "d3d11-sync-interval", "1");
            }
        }
        return 0;

    case WM_EXITSIZEMOVE:
        if (info)
        {
            if (g_mpv.handle && g_mpv.setProperty)
            {
                std::string configuredInterval = ConfiguredD3D11SyncInterval();
                g_mpv.setProperty(g_mpv.handle,
                    "d3d11-sync-interval", configuredInterval.c_str());
            }
            RECT client{};
            GetClientRect(window, &client);
            ApplyClientLayout(window, info,
                client.right - client.left, client.bottom - client.top);
        }

        // Interactive sizing is the strongest signal that this is the size the
        // user actually chose. Fullscreen, PiP and maximized states are filtered
        // inside CaptureRememberedWindowSize.
        CaptureRememberedWindowSize(true);
        return 0;

    case ShowSettingsMessage:
        // Only one side panel owns the overlay surface at a time.
        if (info && g_mediaInfoOpen)
        {
            SendMessageW(window, CloseMediaInfoMessage, 1, 0);
            return 0;
        }
        if (info && g_playlistOpen)
        {
            SendMessageW(window, ClosePlaylistMessage, 1, 0);
            return 0;
        }

        // Never tear down the context-menu island and build the settings
        // island in parallel. Finish one compositor transition first.
        if (info && g_contextMenuOpen)
        {
            PostMessageW(window, CloseContextMenuMessage, 1, 0);
            return 0;
        }
        if (info && !g_settingsOpen)
        {
            g_settingsOpen = true;
            RECT client{};
            GetClientRect(window, &client);
            int width = client.right - client.left;
            int height = client.bottom - client.top;

            // No snapshot cover is needed: Settings now slides above an
            // unchanged MPV surface instead of forcing a swap-chain resize.
            RemoveSettingsTransition(info);

            // The settings island is normally created during WM_CREATE, before
            // video presentation begins. Keep a defensive fallback for an
            // initialization failure, still using the main child tree.
            if (!info->settingsSource)
            {
                info->settingsSource = winrt::DesktopWindowXamlSource{};
                info->settingsSource.Initialize(
                    winrt::Microsoft::UI::GetWindowIdFromWindow(window));
                info->settingsPage = winrt::make<winrt::HCPlayer::implementation::SettingsPage>();
                info->settingsSource.Content(info->settingsPage);

                HWND settingsWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
                    info->settingsSource.SiteBridge().WindowId());
                auto* settingsImplementation =
                    winrt::get_self<winrt::HCPlayer::implementation::SettingsPage>(
                        info->settingsPage);
                SetWindowSubclass(settingsWindow, SettingsPanelSubclassProc, 1,
                    reinterpret_cast<DWORD_PTR>(settingsImplementation));
            }
            HWND settingsWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
                info->settingsSource.SiteBridge().WindowId());
            auto* settingsImplementation =
                winrt::get_self<winrt::HCPlayer::implementation::SettingsPage>(info->settingsPage);
            settingsImplementation->PrepareForOpen();
            if (info->page)
            {
                winrt::get_self<winrt::HCPlayer::implementation::MainPage>(
                    info->page)->SetSettingsOverlayOpen(true);
            }
            ApplyClientLayout(window, info, width, height);
            SetWindowPos(settingsWindow, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            // Queue the theme-correct backing color without synchronously
            // painting the complete XAML island on the playback/UI thread.
            RedrawWindow(settingsWindow, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE);

            settingsImplementation->BeginOpenAnimation();
        }
        return 0;

    case CloseSettingsMessage:
    {
        bool const showMediaInfoAfterClose = wParam == 1;
        bool const showPlaylistAfterClose = wParam == 2;

        if (info && info->settingsSource && g_settingsOpen)
        {
            HWND settingsWindow =
                winrt::Microsoft::UI::GetWindowFromWindowId(
                    info->settingsSource.SiteBridge().WindowId());

            g_settingsOpen = false;
            ShowWindow(settingsWindow, SW_HIDE);

            if (!showMediaInfoAfterClose && !showPlaylistAfterClose)
            {
                RECT client{};
                GetClientRect(window, &client);

                ApplyClientLayout(
                    window,
                    info,
                    client.right - client.left,
                    client.bottom - client.top);

                SetWindowPos(
                    g_videoWindow,
                    HWND_TOP,
                    0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE |
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);

                if (info->xamlSource)
                {
                    HWND controlsWindow =
                        winrt::Microsoft::UI::GetWindowFromWindowId(
                            info->xamlSource.SiteBridge().WindowId());

                    SetWindowPos(
                        controlsWindow,
                        HWND_TOP,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE |
                        SWP_NOACTIVATE | SWP_SHOWWINDOW);

                    winrt::get_self<
                        winrt::HCPlayer::implementation::MainPage>(
                            info->page)->SetSettingsOverlayOpen(false);
                }
            }
        }

        if (showMediaInfoAfterClose)
        {
            PostMessageW(window, ShowMediaInfoMessage, 0, 0);
        }
        else if (showPlaylistAfterClose)
        {
            PostMessageW(window, ShowPlaylistMessage, 0, 0);
        }

        return 0;
    }

    case ShowMediaInfoMessage:
        // If Settings owns the overlay, switch directly without briefly
        // restoring the transport bar between the two panels.
        if (info && g_settingsOpen)
        {
            SendMessageW(window, CloseSettingsMessage, 1, 0);
            return 0;
        }
        if (info && g_playlistOpen)
        {
            SendMessageW(window, ClosePlaylistMessage, 2, 0);
            return 0;
        }

        if (info && g_contextMenuOpen)
        {
            PostMessageW(window, CloseContextMenuMessage, 2, 0);
            return 0;
        }

        if (info && !g_mediaInfoOpen)
        {
            g_mediaInfoOpen = true;

            RECT client{};
            GetClientRect(window, &client);

            int const width = client.right - client.left;
            int const height = client.bottom - client.top;
            int const panelWidth =
                min(DipToPx(window, MediaInfoPanelWidth), width);

            // Defensive fallback: normally this island was pre-created in
            // WM_CREATE before libmpv started presenting.
            if (!info->mediaInfoSource)
            {
                info->mediaInfoSource =
                    winrt::DesktopWindowXamlSource{};

                info->mediaInfoSource.Initialize(
                    winrt::Microsoft::UI::GetWindowIdFromWindow(
                        window));

                info->mediaInfoPage =
                    winrt::make<
                        winrt::HCPlayer::implementation::
                            MediaInfoPage>();

                info->mediaInfoSource.Content(
                    info->mediaInfoPage);

                HWND mediaInfoWindow =
                    winrt::Microsoft::UI::GetWindowFromWindowId(
                        info->mediaInfoSource.SiteBridge().WindowId());

                auto* mediaInfoImplementation =
                    winrt::get_self<
                        winrt::HCPlayer::implementation::
                            MediaInfoPage>(
                                info->mediaInfoPage);

                SetWindowSubclass(
                    mediaInfoWindow,
                    MediaInfoPanelSubclassProc,
                    1,
                    reinterpret_cast<DWORD_PTR>(
                        mediaInfoImplementation));
            }

            HWND mediaInfoWindow =
                winrt::Microsoft::UI::GetWindowFromWindowId(
                    info->mediaInfoSource.SiteBridge().WindowId());

            auto* mediaInfoImplementation =
                winrt::get_self<
                    winrt::HCPlayer::implementation::
                        MediaInfoPage>(
                            info->mediaInfoPage);

            mediaInfoImplementation->PrepareForOpen();

            info->mediaInfoSource.SiteBridge().MoveAndResize(
                { 0, 0, panelWidth, height });

            if (info->page)
            {
                // This historical MainPage method represents "a full-height
                // side overlay is open"; MediaInfo deliberately reuses the
                // exact same transport suppression behavior as Settings.
                winrt::get_self<
                    winrt::HCPlayer::implementation::MainPage>(
                        info->page)->SetSettingsOverlayOpen(true);
            }

            ApplyClientLayout(
                window,
                info,
                width,
                height);

            SetWindowPos(
                mediaInfoWindow,
                HWND_TOP,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE |
                SWP_NOACTIVATE | SWP_SHOWWINDOW);

            RedrawWindow(
                mediaInfoWindow,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_ERASE |
                RDW_UPDATENOW);

            mediaInfoImplementation->BeginOpenAnimation();
        }

        return 0;

    case CloseMediaInfoMessage:
    {
        bool const showSettingsAfterClose = wParam == 1;
        bool const showPlaylistAfterClose = wParam == 2;

        if (info && info->mediaInfoSource && g_mediaInfoOpen)
        {
            HWND mediaInfoWindow =
                winrt::Microsoft::UI::GetWindowFromWindowId(
                    info->mediaInfoSource.SiteBridge().WindowId());

            g_mediaInfoOpen = false;
            ShowWindow(mediaInfoWindow, SW_HIDE);

            if (!showSettingsAfterClose && !showPlaylistAfterClose)
            {
                RECT client{};
                GetClientRect(window, &client);

                ApplyClientLayout(
                    window,
                    info,
                    client.right - client.left,
                    client.bottom - client.top);

                SetWindowPos(
                    g_videoWindow,
                    HWND_TOP,
                    0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE |
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);

                if (info->xamlSource)
                {
                    HWND controlsWindow =
                        winrt::Microsoft::UI::GetWindowFromWindowId(
                            info->xamlSource.SiteBridge().WindowId());

                    SetWindowPos(
                        controlsWindow,
                        HWND_TOP,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE |
                        SWP_NOACTIVATE | SWP_SHOWWINDOW);

                    winrt::get_self<
                        winrt::HCPlayer::implementation::MainPage>(
                            info->page)->SetSettingsOverlayOpen(false);
                }
            }
        }

        if (showSettingsAfterClose)
        {
            PostMessageW(window, ShowSettingsMessage, 0, 0);
        }
        else if (showPlaylistAfterClose)
        {
            PostMessageW(window, ShowPlaylistMessage, 0, 0);
        }

        return 0;
    }

    case ShowPlaylistMessage:
        // Playlist shares MediaInfo's LEFT overlay position. Switch directly
        // between panels without briefly restoring the transport in between.
        if (info && g_settingsOpen)
        {
            SendMessageW(window, CloseSettingsMessage, 2, 0);
            return 0;
        }
        if (info && g_mediaInfoOpen)
        {
            SendMessageW(window, CloseMediaInfoMessage, 2, 0);
            return 0;
        }
        if (info && g_contextMenuOpen)
        {
            PostMessageW(window, CloseContextMenuMessage, 3, 0);
            return 0;
        }

        if (info && !g_playlistOpen)
        {
            g_playlistOpen = true;

            RECT client{};
            GetClientRect(window, &client);
            int const width = client.right - client.left;
            int const height = client.bottom - client.top;
            int const panelWidth =
                min(DipToPx(window, PlaylistPanelWidth), width);

            // Unlike the much heavier Settings tree, this compact queue is
            // created lazily so adding the feature has zero startup cost.
            if (!info->playlistSource)
            {
                info->playlistSource = winrt::DesktopWindowXamlSource{};
                info->playlistSource.Initialize(
                    winrt::Microsoft::UI::GetWindowIdFromWindow(window));
                info->playlistPage =
                    winrt::make<
                        winrt::HCPlayer::implementation::PlaylistPage>();
                info->playlistSource.Content(info->playlistPage);

                HWND playlistWindow =
                    winrt::Microsoft::UI::GetWindowFromWindowId(
                        info->playlistSource.SiteBridge().WindowId());
                auto* playlistImplementation =
                    winrt::get_self<
                        winrt::HCPlayer::implementation::PlaylistPage>(
                            info->playlistPage);
                SetWindowSubclass(
                    playlistWindow,
                    PlaylistPanelSubclassProc,
                    1,
                    reinterpret_cast<DWORD_PTR>(playlistImplementation));
                RegisterPlaylistDropTarget(
                    playlistWindow, playlistImplementation);
            }

            HWND playlistWindow =
                winrt::Microsoft::UI::GetWindowFromWindowId(
                    info->playlistSource.SiteBridge().WindowId());
            auto* playlistImplementation =
                winrt::get_self<
                    winrt::HCPlayer::implementation::PlaylistPage>(
                        info->playlistPage);

            playlistImplementation->PrepareForOpen();
            info->playlistSource.SiteBridge().MoveAndResize(
                { 0, 0, panelWidth, height });

            if (info->page)
            {
                winrt::get_self<
                    winrt::HCPlayer::implementation::MainPage>(
                        info->page)->SetSettingsOverlayOpen(true);
            }

            ApplyClientLayout(window, info, width, height);
            SetWindowPos(
                playlistWindow,
                HWND_TOP,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE |
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            RedrawWindow(
                playlistWindow,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
            playlistImplementation->BeginOpenAnimation();
        }
        return 0;

    case ClosePlaylistMessage:
    {
        bool const showSettingsAfterClose = wParam == 1;
        bool const showMediaInfoAfterClose = wParam == 2;

        if (info && info->playlistSource && g_playlistOpen)
        {
            HWND playlistWindow =
                winrt::Microsoft::UI::GetWindowFromWindowId(
                    info->playlistSource.SiteBridge().WindowId());
            auto* playlistImplementation =
                winrt::get_self<
                    winrt::HCPlayer::implementation::PlaylistPage>(
                        info->playlistPage);

            playlistImplementation->PrepareForClose();
            g_playlistOpen = false;
            ShowWindow(playlistWindow, SW_HIDE);

            if (!showSettingsAfterClose && !showMediaInfoAfterClose)
            {
                RECT client{};
                GetClientRect(window, &client);
                ApplyClientLayout(
                    window, info,
                    client.right - client.left,
                    client.bottom - client.top);

                SetWindowPos(
                    g_videoWindow, HWND_TOP,
                    0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE |
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);

                if (info->xamlSource)
                {
                    HWND controlsWindow =
                        winrt::Microsoft::UI::GetWindowFromWindowId(
                            info->xamlSource.SiteBridge().WindowId());
                    SetWindowPos(
                        controlsWindow, HWND_TOP,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE |
                        SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    winrt::get_self<
                        winrt::HCPlayer::implementation::MainPage>(
                            info->page)->SetSettingsOverlayOpen(false);
                }
            }
        }

        if (showSettingsAfterClose)
        {
            PostMessageW(window, ShowSettingsMessage, 0, 0);
        }
        else if (showMediaInfoAfterClose)
        {
            PostMessageW(window, ShowMediaInfoMessage, 0, 0);
        }
        return 0;
    }

    case OpenFileMessage:
        if (info && info->page)
        {
            winrt::get_self<winrt::HCPlayer::implementation::MainPage>(info->page)
                ->OpenFromDialog();
        }
        return 0;

    case OpenFolderMessage:
    {
        std::wstring folder;
        auto dialogTitle = PlayerUiString(
            L"PickerOpenFolderDisc", L"Abrir pasta, DVD ou Blu-ray");
        if (!PickFileSystemPath(
            folder, true, false, dialogTitle.c_str()))
        {
            return 0;
        }

        std::filesystem::path discRoot;
        bool bluray{};
        if (TryGetOpticalDiscFolder(folder, discRoot, bluray))
        {
            PlayerLoadOpticalDisc(discRoot.wstring(), bluray);
            return 0;
        }

        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (auto const& entry : std::filesystem::directory_iterator(folder, error))
        {
            if (!error && entry.is_regular_file(error) &&
                IsPlayableFolderFile(entry.path()))
            {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end(),
            [](auto const& left, auto const& right)
            {
                return StrCmpLogicalW(
                    left.filename().c_str(), right.filename().c_str()) < 0;
            });
        if (files.empty()) return 0;

        g_suppressAutoload = true;
        bool opened = PlayerOpenRecentFile(files.front().wstring());
        g_suppressAutoload = false;
        if (!opened) return 0;
        for (size_t index = 1; index < files.size(); ++index)
        {
            std::string utf8 = winrt::to_string(files[index].wstring());
            const char* append[] = { "loadfile", utf8.c_str(), "append", nullptr };
            g_mpv.command(g_mpv.handle, append);
        }
    }
    return 0;

    case OpenDiscImageMessage:
    {
        bool bluray = wParam != 0;
        std::wstring image;
        auto dialogTitle = PlayerUiString(
            bluray ? L"PickerOpenBluRayIso" : L"PickerOpenDvdIso",
            bluray ? L"Abrir imagem Blu-ray ISO" : L"Abrir imagem DVD ISO");
        if (!PickFileSystemPath(image, false, true, dialogTitle.c_str()))
        {
            return 0;
        }
        PlayerLoadOpticalDisc(image, bluray);
    }
    return 0;

    case AddExternalAudioMessage:
    case AddExternalSubtitleMessage:
    {
        bool const audio = message == AddExternalAudioMessage;
        std::wstring path;
        if (!PickExternalTrackFile(path, audio)) return 0;
        AddExternalTrack(path, audio);
    }
    return 0;

    case ShowContextMenuMessage:
        if (g_pictureInPicture) return 0;
        try
        {
            if (info && !g_contextMenuOpen && !IsSidePanelOpen())
            {
                g_contextMenuOpen = true;
                if (!info->contextSource)
                {
                    info->contextSource = winrt::DesktopWindowXamlSource{};
                    info->contextSource.Initialize(winrt::Microsoft::UI::GetWindowIdFromWindow(window));
                    info->contextPage = winrt::make<winrt::HCPlayer::implementation::ContextMenuPage>();
                    info->contextSource.Content(info->contextPage);
                }

                RECT client{};
                GetClientRect(window, &client);
                int width = client.right - client.left;
                int height = client.bottom - client.top;
                int anchorX = max(0, min(width - 1,
                    static_cast<int>(static_cast<short>(LOWORD(lParam)))));
                int anchorY = max(0, min(height - 1,
                    static_cast<int>(static_cast<short>(HIWORD(lParam)))));
                info->contextSource.SiteBridge().MoveAndResize(
                    { anchorX, anchorY, 1, 1 });
                HWND contextWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
                    info->contextSource.SiteBridge().WindowId());
                SetWindowPos(contextWindow, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

                // ShowAt is invoked after the island has a XamlRoot; see the
                // deferred message below.
                PostMessageW(window, ShowContextMenuMessage, 1, lParam);
            }
            else if (info && info->contextPage && wParam == 1)
            {
                winrt::get_self<winrt::HCPlayer::implementation::ContextMenuPage>(
                    info->contextPage)->ShowAt(
                        0.0, 0.0);
            }
        }
        catch (winrt::hresult_error const& error)
        {
            std::wstring diagnostic = PlayerUiString(
                L"NativeContextMenuErrorPrefix",
                L"Erro no menu de contexto: ");
            diagnostic += error.message().c_str();
            SetWindowTextW(window, diagnostic.c_str());
            if (info && info->contextSource)
            {
                info->contextSource.Close();
                info->contextSource = nullptr;
                info->contextPage = nullptr;
            }
            g_contextMenuOpen = false;
        }
        return 0;

    case CloseContextMenuMessage:
    {
        bool const showSettingsAfterClose = wParam == 1;
        bool const showMediaInfoAfterClose = wParam == 2;
        bool const showPlaylistAfterClose = wParam == 3;
        if (info && info->contextSource)
        {
            HWND contextWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
                info->contextSource.SiteBridge().WindowId());
            ShowWindow(contextWindow, SW_HIDE);
            RECT client{};
            GetClientRect(window, &client);
            info->contextSource.SiteBridge().MoveAndResize(
                { client.right + 1, client.bottom + 1, 1, 1 });
            g_contextMenuOpen = false;
            SetFocus(window);
            RedrawWindow(window, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
        }
        if (showSettingsAfterClose)
        {
            PostMessageW(window, ShowSettingsMessage, 0, 0);
        }
        else if (showMediaInfoAfterClose)
        {
            PostMessageW(window, ShowMediaInfoMessage, 0, 0);
        }
        else if (showPlaylistAfterClose)
        {
            PostMessageW(window, ShowPlaylistMessage, 0, 0);
        }
        return 0;
    }

    case WM_DWMCOMPOSITIONCHANGED:
        ApplyWindows11Visual(window);
        return 0;

    case WM_TIMER:
        if (wParam == FullscreenVideoSettleTimer)
        {
            PollFullscreenVideoSettle();
            return 0;
        }
        if (wParam == VideoSingleClickTimer)
        {
            KillTimer(window, VideoSingleClickTimer);

            if (UiToggleEnabled("ui-click-video-play-pause", false) &&
                !IsSidePanelOpen())
            {
                bool paused{};
                bool eofReached{};
                if (PlayerGetPlaybackState(paused, eofReached) && eofReached)
                    PlayerReplay();
                else
                    PlayerTogglePause();
            }
            return 0;
        }
        if (wParam == TransportPointerTimer)
        {
            UpdateCursorAutohide();
            UpdateEmptyStateGlow(info);
            if (info && info->page && !IsSidePanelOpen())
            {
                POINT screenCursor{};
                RECT client{};
                if (!GetCursorPos(&screenCursor)) return 0;

                auto* page = winrt::get_self<
                    winrt::HCPlayer::implementation::MainPage>(info->page);

                if (g_transportMinimal && g_transportHostVisible &&
                    !g_transportFlyoutOpen && !g_contextMenuOpen)
                {
                    RECT ownerRect{};
                    bool const cursorInsideOwner =
                        GetWindowRect(window, &ownerRect) &&
                        PtInRect(&ownerRect, screenCursor);

                    if (!cursorInsideOwner)
                    {
                        // XAML and the island HWND normally deliver PointerExited
                        // / WM_MOUSELEAVE. This one-shot physical-cursor fallback
                        // closes the tiny race where ownership/rehosting causes
                        // both leave notifications to be missed. Do not restart
                        // the 180-ms hide timer every 50 ms.
                        if (!g_minimalCursorOutsideOwner)
                        {
                            g_minimalCursorOutsideOwner = true;
                            page->TransportHostPointerExited();
                        }
                    }
                    else
                    {
                        g_minimalCursorOutsideOwner = false;
                    }
                }
                else if (!g_transportHostVisible)
                {
                    g_minimalCursorOutsideOwner = false;
                }

                if (!g_transportHostVisible && g_hasTransportHiddenCursor &&
                    screenCursor.x == g_transportHiddenCursor.x &&
                    screenCursor.y == g_transportHiddenCursor.y)
                {
                    // Collapsing the XAML island can place the same stationary
                    // pointer over the native hot zone. Wait for genuine mouse
                    // movement instead of immediately reopening the transport.
                    return 0;
                }
                POINT cursor = screenCursor;
                ScreenToClient(window, &cursor);
                GetClientRect(window, &client);
                bool overResizeEdge =
                    (g_pictureInPicture &&
                        PipResizeEdgesAt(screenCursor) != PipResizeNone) ||
                    (g_borderless &&
                        BorderlessResizeEdgesAt(screenCursor) != PipResizeNone);
                if (!overResizeEdge &&
                    PtInRect(&client, cursor) &&
                    cursor.y >= client.bottom - CurrentControlsHeightPx(window))
                {
                    page->TransportHostPointerEntered();
                }
            }
            return 0;
        }
        if (wParam == SettingsTransitionTimer)
        {
            KillTimer(window, SettingsTransitionTimer);
            RemoveSettingsTransition(info);
            return 0;
        }
        if (wParam == NativeSettingsSaveTimer)
        {
            KillTimer(window, NativeSettingsSaveTimer);
            if (g_mpvSettingsManager.Dirty()) g_mpvSettingsManager.SaveNativeOptions();
            return 0;
        }
        if (wParam == DynamicWindowFitTimer)
        {
            PollDynamicWindowFit();
            return 0;
        }
        if (wParam == AutofitWindowTimer)
        {
            if (TryApplyConfiguredAutofit() || --g_autofitAttemptsRemaining <= 0)
            {
                KillTimer(window, AutofitWindowTimer);
                g_autofitAttemptsRemaining = 0;
            }
            return 0;
        }
        if (wParam == InitialMediaRevealTimer)
        {
            PollDeferredStartupMediaReveal();
            return 0;
        }
        break;

    case WM_CLOSE:
        // Save a resolved web title while mpv metadata is still alive. This is
        // a tiny synchronous metadata write, completely outside decoding.
        CaptureCurrentRecentTitle();

        // Resume state is a single tiny synchronous write performed only on a
        // normal application close. It never runs in the playback timer or the
        // decoding/render path.
        SaveCurrentResumePointOnExit();

        // Keep only a normal-window size. When dynamic follow-video is enabled,
        // do not overwrite the last manually chosen size with an automatic fit
        // performed for the current video's aspect ratio.
        if (!DynamicWindowFitEnabled())
        {
            CaptureRememberedWindowSize(true);
        }

        // Close transient islands through their normal paths before destroying
        // the Win32 owner. In particular, the settings island must finish its
        // teardown while its owner and dispatcher are still valid.
        if (info)
        {
            if (info->contextSource)
            {
                SendMessageW(window, CloseContextMenuMessage, 0, 0);
            }
            if (info->settingsSource)
            {
                SendMessageW(window, CloseSettingsMessage, 0, 0);
            }
            if (info->mediaInfoSource)
            {
                SendMessageW(window, CloseMediaInfoMessage, 0, 0);
            }
            if (info->playlistSource)
            {
                SendMessageW(window, ClosePlaylistMessage, 0, 0);
            }
        }
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        KillTimer(window, NativeSettingsSaveTimer);
        KillTimer(window, SettingsTransitionTimer);
        KillTimer(window, TransportPointerTimer);
        KillTimer(window, AutofitWindowTimer);
        KillTimer(window, DynamicWindowFitTimer);
        KillTimer(window, VideoSingleClickTimer);
        KillTimer(window, FullscreenVideoSettleTimer);
        KillTimer(window, InitialMediaRevealTimer);
        g_deferredStartupMediaReveal = false;
        g_deferredStartupRevealStartedTick = 0;
        DestroyInitialMediaRevealShield();
        DestroyPendingFullscreenVideoSettleShield();
        SetApplicationCursorHidden(false);
        if (g_taskbarList)
        {
            g_taskbarList->SetProgressState(window, TBPF_NOPROGRESS);
            g_taskbarList = nullptr;
        }
        DestroyTaskbarButtonIcons();
        if (g_mpvSettingsManager.Dirty()) g_mpvSettingsManager.SaveNativeOptions();
        ReleaseSingleInstanceGate();
        SetThreadExecutionState(ES_CONTINUOUS);
        g_mainWindow = nullptr;
        PostQuitMessage(0);
        return 0;

    case WM_NCDESTROY:
        if (info)
        {
            RevokeDragDrop(window);
            if (g_videoWindow) RevokeDragDrop(g_videoWindow);
            if (g_transportDropWindow) RevokeDragDrop(g_transportDropWindow);
            g_transportDropWindow = nullptr;
            if (g_playlistDropWindow) RevokeDragDrop(g_playlistDropWindow);
            g_playlistDropWindow = nullptr;
            RemoveSettingsTransition(info);
            if (info->settingsSource)
            {
                info->settingsSource.Close();
            }
            if (info->mediaInfoSource)
            {
                info->mediaInfoSource.Close();
            }
            if (info->playlistSource)
            {
                info->playlistSource.Close();
            }
            if (info->contextSource)
            {
                info->contextSource.Close();
            }
            if (info->emptyStateSource)
            {
                info->emptyStateSource.Close();
            }
            if (info->bufferingSource)
            {
                info->bufferingSource.Close();
            }
            CloseMinimalTransportMica(info);
            if (info->xamlSource)
            {
                info->xamlSource.Close();
            }
            if (info->transportHostWindow && IsWindow(info->transportHostWindow))
            {
                DestroyWindow(info->transportHostWindow);
                info->transportHostWindow = nullptr;
            }
            delete info;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        // Also terminate the loop if Windows tears down the HWND without the
        // normal close path. This prevents an invisible process from blocking
        // the next Visual Studio build/run.
        PostQuitMessage(0);
        return DefWindowProcW(window, message, wParam, lParam);

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}
