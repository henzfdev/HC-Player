#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ThumbnailBridge
{
    enum class SeekMode
    {
        Fast,
        Exact
    };

    struct Frame
    {
        int width{};
        int height{};
        int stride{};

        // Actual playback timestamp reported by the private mpv after the
        // decoded frame used by screenshot-raw became current.
        double timestampSeconds{};

        // Normalized top-to-bottom BGRA8 pixels.
        // stride is always width * 4 in the returned Frame.
        std::vector<std::uint8_t> pixels;
    };

    // Only checks whether the bundled libmpv exposes everything required
    // by the independent thumbnail worker.
    bool IsAvailable();

    // Generates a frame using a SECOND libmpv instance. The HC Player's main
    // playback handle is never sought, paused or otherwise modified.
    //
    // The worker is lazy: it is created only on the first real request.
    // Successive requests for the same file reuse the private handle.
    bool GetFrame(
        std::wstring const& filePath,
        double seconds,
        SeekMode mode,
        Frame& frame,
        std::wstring& error);

    // Compatibility overload: callers that do not choose a mode keep the
    // previous exact-seek behavior.
    bool GetFrame(
        std::wstring const& filePath,
        double seconds,
        Frame& frame,
        std::wstring& error);

    // Releases only the private thumbnail worker.
    // It has no relationship with the main playback handle.
    void Shutdown();
}
