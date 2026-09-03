#pragma once

#include <windows.h>
#include <cstdint>
#include <string_view>

namespace hc::system_media_controls
{
    enum class Command : std::uintptr_t
    {
        Play = 1,
        Pause = 2,
        Previous = 3,
        Next = 4,
    };

    // Best-effort Win32/SMTC integration. Failure must never affect playback.
    bool Initialize(HWND window, unsigned int commandMessage) noexcept;
    void Shutdown() noexcept;

    // These methods only publish already-known player state to Windows.
    void UpdatePlaybackState(bool paused, bool eofReached) noexcept;
    void UpdateMetadata(
        std::wstring_view title,
        std::wstring_view artist,
        bool audioOnly) noexcept;
    void Clear() noexcept;
}
