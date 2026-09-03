#include "pch.h"
#include "ThumbnailBridge.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct mpv_handle;

    constexpr int MpvFormatNone = 0;
    constexpr int MpvFormatString = 1;
    constexpr int MpvFormatFlag = 3;
    constexpr int MpvFormatInt64 = 4;
    constexpr int MpvFormatDouble = 5;
    constexpr int MpvFormatNode = 6;
    constexpr int MpvFormatNodeArray = 7;
    constexpr int MpvFormatNodeMap = 8;
    constexpr int MpvFormatByteArray = 9;

    struct MpvNode;

    struct MpvNodeList
    {
        int num{};
        MpvNode* values{};
        char** keys{};
    };

    struct MpvByteArray
    {
        void* data{};
        size_t size{};
    };

    struct MpvNode
    {
        union
        {
            char* string;
            int flag;
            std::int64_t integer;
            double number;
            MpvNodeList* list;
            MpvByteArray* byteArray;
        } value{};

        int format{ MpvFormatNone };
    };

    using mpv_create_fn =
        mpv_handle* (*)();

    using mpv_initialize_fn =
        int (*)(mpv_handle*);

    using mpv_set_option_string_fn =
        int (*)(mpv_handle*, char const*, char const*);

    using mpv_set_property_string_fn =
        int (*)(mpv_handle*, char const*, char const*);

    using mpv_get_property_fn =
        int (*)(mpv_handle*, char const*, int, void*);

    using mpv_command_fn =
        int (*)(mpv_handle*, char const* const*);

    using mpv_command_ret_fn =
        int (*)(mpv_handle*, char const* const*, MpvNode*);

    using mpv_terminate_destroy_fn =
        void (*)(mpv_handle*);

    using mpv_free_node_contents_fn =
        void (*)(MpvNode*);

    std::wstring ExecutableDirectory()
    {
        std::wstring buffer(32768, L'\0');

        DWORD const length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));

        if (length == 0 || length >= buffer.size())
        {
            return {};
        }

        buffer.resize(length);

        size_t const separator =
            buffer.find_last_of(L"\\/");

        if (separator == std::wstring::npos)
        {
            return {};
        }

        buffer.resize(separator);
        return buffer;
    }

    std::string Utf8(std::wstring const& value)
    {
        if (value.empty())
        {
            return {};
        }

        int const required = WideCharToMultiByte(
            CP_UTF8,
            0,
            value.c_str(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (required <= 0)
        {
            return {};
        }

        std::string result(
            static_cast<size_t>(required),
            '\0');

        WideCharToMultiByte(
            CP_UTF8,
            0,
            value.c_str(),
            static_cast<int>(value.size()),
            result.data(),
            required,
            nullptr,
            nullptr);

        return result;
    }

    std::string SecondsArgument(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0.0)
        {
            seconds = 0.0;
        }

        char buffer[64]{};

        sprintf_s(
            buffer,
            "%.6f",
            seconds);

        return buffer;
    }

    class ThumbnailEngine
    {
    public:
        ~ThumbnailEngine()
        {
            ShutdownUnlocked();

            if (m_module)
            {
                FreeLibrary(m_module);
                m_module = nullptr;
            }
        }

        bool IsAvailable()
        {
            std::scoped_lock lock{ m_mutex };
            return LoadFunctionsUnlocked();
        }

        bool GetFrame(
            std::wstring const& filePath,
            double seconds,
            ThumbnailBridge::SeekMode mode,
            ThumbnailBridge::Frame& frame,
            std::wstring& error)
        {
            std::scoped_lock lock{ m_mutex };

            frame = {};
            error.clear();

            if (filePath.empty())
            {
                error = L"No local media file was provided.";
                return false;
            }

            std::error_code filesystemError;
            std::filesystem::path const path{ filePath };

            if (!std::filesystem::is_regular_file(
                path,
                filesystemError))
            {
                error =
                    L"Timeline thumbnails are currently available for local files only.";
                return false;
            }

            if (!EnsureWorkerUnlocked(error))
            {
                return false;
            }

            if (m_currentPath != path.wstring())
            {
                if (!LoadFileUnlocked(
                    path.wstring(),
                    error))
                {
                    return false;
                }
            }

            if (!SeekUnlocked(seconds, mode, error))
            {
                return false;
            }

            // A paused seek can finish a little before the decoded frame has
            // reached screenshot-raw. Retry briefly instead of ever touching
            // the main playback engine.
            int const captureAttempts =
                mode == ThumbnailBridge::SeekMode::Exact ? 30 : 14;

            for (int attempt = 0; attempt < captureAttempts; ++attempt)
            {
                if (CaptureUnlocked(frame))
                {
                    // screenshot-raw has now returned the decoded frame that
                    // the private mpv currently considers active. Preserve
                    // that mpv-reported timestamp, rather than assuming that
                    // the requested arbitrary second is itself a frame PTS.
                    double actualTimestamp = seconds;

                    double playbackTime{};
                    if (m_getProperty(
                        m_handle,
                        "playback-time",
                        MpvFormatDouble,
                        &playbackTime) >= 0 &&
                        std::isfinite(playbackTime) &&
                        playbackTime >= 0.0)
                    {
                        actualTimestamp = playbackTime;
                    }

                    frame.timestampSeconds =
                        actualTimestamp;

                    return true;
                }

                std::this_thread::sleep_for(
                    mode == ThumbnailBridge::SeekMode::Exact
                        ? std::chrono::milliseconds(12)
                        : std::chrono::milliseconds(6));
            }

            error =
                L"The thumbnail worker could not obtain a decoded frame.";
            return false;
        }

        void Shutdown()
        {
            std::scoped_lock lock{ m_mutex };
            ShutdownUnlocked();
        }

    private:
        bool LoadFunctionsUnlocked()
        {
            if (m_module)
            {
                return
                    m_create &&
                    m_initialize &&
                    m_setOption &&
                    m_setProperty &&
                    m_getProperty &&
                    m_command &&
                    m_commandRet &&
                    m_destroy &&
                    m_freeNodeContents;
            }

            std::wstring const directory =
                ExecutableDirectory();

            if (directory.empty())
            {
                return false;
            }

            std::wstring const primary =
                directory + L"\\libmpv-2.dll";

            std::wstring const fallback =
                directory + L"\\mpv-2.dll";

            m_module = LoadLibraryW(primary.c_str());

            if (!m_module)
            {
                m_module =
                    LoadLibraryW(fallback.c_str());
            }

            if (!m_module)
            {
                return false;
            }

            m_create =
                reinterpret_cast<mpv_create_fn>(
                    GetProcAddress(
                        m_module,
                        "mpv_create"));

            m_initialize =
                reinterpret_cast<mpv_initialize_fn>(
                    GetProcAddress(
                        m_module,
                        "mpv_initialize"));

            m_setOption =
                reinterpret_cast<mpv_set_option_string_fn>(
                    GetProcAddress(
                        m_module,
                        "mpv_set_option_string"));

            m_setProperty =
                reinterpret_cast<mpv_set_property_string_fn>(
                    GetProcAddress(
                        m_module,
                        "mpv_set_property_string"));

            m_getProperty =
                reinterpret_cast<mpv_get_property_fn>(
                    GetProcAddress(
                        m_module,
                        "mpv_get_property"));

            m_command =
                reinterpret_cast<mpv_command_fn>(
                    GetProcAddress(
                        m_module,
                        "mpv_command"));

            m_commandRet =
                reinterpret_cast<mpv_command_ret_fn>(
                    GetProcAddress(
                        m_module,
                        "mpv_command_ret"));

            m_destroy =
                reinterpret_cast<mpv_terminate_destroy_fn>(
                    GetProcAddress(
                        m_module,
                        "mpv_terminate_destroy"));

            m_freeNodeContents =
                reinterpret_cast<mpv_free_node_contents_fn>(
                    GetProcAddress(
                        m_module,
                        "mpv_free_node_contents"));

            bool const ready =
                m_create &&
                m_initialize &&
                m_setOption &&
                m_setProperty &&
                m_getProperty &&
                m_command &&
                m_commandRet &&
                m_destroy &&
                m_freeNodeContents;

            if (!ready)
            {
                FreeLibrary(m_module);
                m_module = nullptr;

                m_create = nullptr;
                m_initialize = nullptr;
                m_setOption = nullptr;
                m_setProperty = nullptr;
                m_getProperty = nullptr;
                m_command = nullptr;
                m_commandRet = nullptr;
                m_destroy = nullptr;
                m_freeNodeContents = nullptr;
            }

            return ready;
        }

        bool SetOptionUnlocked(
            char const* name,
            char const* value)
        {
            return
                m_handle &&
                m_setOption &&
                m_setOption(
                    m_handle,
                    name,
                    value) >= 0;
        }

        bool EnsureWorkerUnlocked(
            std::wstring& error)
        {
            if (m_handle)
            {
                return true;
            }

            if (!LoadFunctionsUnlocked())
            {
                error =
                    L"The bundled libmpv does not expose the thumbnail API.";
                return false;
            }

            m_handle = m_create();

            if (!m_handle)
            {
                error =
                    L"The thumbnail libmpv instance could not be created.";
                return false;
            }

            // IMPORTANT:
            // This is intentionally NOT HC Player's playback configuration.
            // It is a small, isolated decoder used only for hover previews.
            SetOptionUnlocked("config", "no");
            SetOptionUnlocked("terminal", "no");
            SetOptionUnlocked(
                "input-default-bindings",
                "no");
            SetOptionUnlocked(
                "input-vo-keyboard",
                "no");

            // This private instance is a thumbnail scanner, not a second
            // interactive player. Keep all Lua/user/builtin UI scripts out of
            // it: they add no value here and can create avoidable background
            // work (and noisy first-chance LuaJIT exceptions under Debug).
            SetOptionUnlocked("load-scripts", "no");
            SetOptionUnlocked("osc", "no");
            SetOptionUnlocked(
                "load-stats-overlay",
                "no");
            SetOptionUnlocked(
                "load-console",
                "no");
            SetOptionUnlocked(
                "load-commands",
                "no");
            SetOptionUnlocked(
                "load-auto-profiles",
                "no");
            SetOptionUnlocked(
                "load-select",
                "no");
            SetOptionUnlocked(
                "load-context-menu",
                "no");
            SetOptionUnlocked(
                "load-positioning",
                "no");
            SetOptionUnlocked("ytdl", "no");

            // mpv explicitly recommends disabling references for automatic
            // thumbnail scanners. Also avoid side-loading subtitles, audio,
            // cover art, playlists or other neighboring media.
            SetOptionUnlocked(
                "access-references",
                "no");
            SetOptionUnlocked(
                "autoload-files",
                "no");

            SetOptionUnlocked("audio", "no");
            SetOptionUnlocked("osd-level", "0");
            SetOptionUnlocked("pause", "yes");
            SetOptionUnlocked("keep-open", "yes");
            SetOptionUnlocked("idle", "yes");

            // Never contend with the GPU renderer used by HC Player.
            // screenshot-sw=yes asks mpv to perform screenshot RGB conversion
            // through its software path instead.
            SetOptionUnlocked("hwdec", "no");
            SetOptionUnlocked("vo", "null");
            SetOptionUnlocked(
                "screenshot-sw",
                "yes");

            // Deliberately cap background software decoding. If a difficult
            // codec makes a thumbnail arrive later, that is preferable to
            // stealing many CPU cores from the real playback instance.
            SetOptionUnlocked(
                "vd-lavc-threads",
                "2");

            // ThumbFast-style scanner tuning: these options affect only this
            // private thumbnail mpv. They trade decoder niceties for latency
            // while the main HC Player playback instance remains untouched.
            SetOptionUnlocked(
                "vd-lavc-skiploopfilter",
                "all");
            SetOptionUnlocked(
                "vd-lavc-fast",
                "yes");
            SetOptionUnlocked(
                "demuxer-max-bytes",
                "128KiB");
            SetOptionUnlocked(
                "sws-scaler",
                "fast-bilinear");

            // The visual card is designed around a ~320 x 180 preview. Scale
            // the private worker output before screenshot-raw so a 1080p frame
            // is not copied through our pipeline as an 8.3 MB BGRA buffer.
            // force_original_aspect_ratio preserves 4:3, 16:9, 21:9, portrait,
            // etc. instead of stretching them.
            SetOptionUnlocked(
                "vf",
                "lavfi=[scale=320:180:force_original_aspect_ratio=decrease:force_divisible_by=2]");
            SetOptionUnlocked(
                "sws-fast",
                "yes");

            // Local-file preview should not build a second playback cache.
            SetOptionUnlocked("cache", "no");
            SetOptionUnlocked(
                "demuxer-readahead-secs",
                "0");

            int const initialized =
                m_initialize(m_handle);

            if (initialized < 0)
            {
                m_destroy(m_handle);
                m_handle = nullptr;

                error =
                    L"The thumbnail libmpv instance could not be initialized.";
                return false;
            }

            return true;
        }

        bool LoadFileUnlocked(
            std::wstring const& filePath,
            std::wstring& error)
        {
            std::string const utf8Path =
                Utf8(filePath);

            if (utf8Path.empty())
            {
                error =
                    L"The media path could not be converted to UTF-8.";
                return false;
            }

            char const* command[] = {
                "loadfile",
                utf8Path.c_str(),
                "replace",
                nullptr
            };

            if (m_command(
                m_handle,
                command) < 0)
            {
                error =
                    L"The thumbnail worker could not load this media file.";
                return false;
            }

            // Wait only for basic video metadata. Internal mpv decoding
            // continues on its own threads; the UI thread will never call this
            // function directly when the feature is wired in.
            bool loaded = false;

            for (int attempt = 0; attempt < 200; ++attempt)
            {
                double duration{};

                if (m_getProperty(
                    m_handle,
                    "duration",
                    MpvFormatDouble,
                    &duration) >= 0 &&
                    std::isfinite(duration) &&
                    duration > 0.0)
                {
                    loaded = true;
                    break;
                }

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }

            if (!loaded)
            {
                // Some valid formats expose duration late. Do not reject them
                // yet; seeking/screenshot-raw below gets the final say.
            }

            m_setProperty(
                m_handle,
                "pause",
                "yes");

            m_currentPath = filePath;
            return true;
        }

        bool SeekUnlocked(
            double seconds,
            ThumbnailBridge::SeekMode mode,
            std::wstring& error)
        {
            std::string const target =
                SecondsArgument(seconds);

            char const* seekMode =
                mode == ThumbnailBridge::SeekMode::Exact
                    ? "absolute+exact"
                    : "absolute+keyframes";

            char const* seekCommand[] = {
                "seek",
                target.c_str(),
                seekMode,
                nullptr
            };

            if (m_command(
                m_handle,
                seekCommand) < 0)
            {
                error =
                    L"The thumbnail worker could not seek to this position.";
                return false;
            }

            // Keep the worker paused. A paused mpv still refreshes the decoded
            // frame after a seek, which is exactly what screenshot-raw needs.
            m_setProperty(
                m_handle,
                "pause",
                "yes");

            // Wait for seek completion, but cap the wait so a malformed file
            // can never stall the hover pipeline indefinitely.
            int const seekAttempts =
                mode == ThumbnailBridge::SeekMode::Exact ? 120 : 45;

            for (int attempt = 0; attempt < seekAttempts; ++attempt)
            {
                int seeking{};

                int const result =
                    m_getProperty(
                        m_handle,
                        "seeking",
                        MpvFormatFlag,
                        &seeking);

                if (result >= 0 && !seeking)
                {
                    return true;
                }

                std::this_thread::sleep_for(
                    mode == ThumbnailBridge::SeekMode::Exact
                        ? std::chrono::milliseconds(8)
                        : std::chrono::milliseconds(4));
            }

            // screenshot-raw may still succeed even when an exotic demuxer did
            // not expose the transient seeking flag cleanly.
            return true;
        }

        bool CaptureUnlocked(
            ThumbnailBridge::Frame& frame)
        {
            char const* screenshotCommand[] = {
                "screenshot-raw",
                "video",
                "bgra",
                nullptr
            };

            MpvNode result{};

            if (m_commandRet(
                m_handle,
                screenshotCommand,
                &result) < 0)
            {
                return false;
            }

            struct NodeGuard
            {
                mpv_free_node_contents_fn freeNode{};
                MpvNode* node{};

                ~NodeGuard()
                {
                    if (freeNode && node)
                    {
                        freeNode(node);
                    }
                }
            } guard{
                m_freeNodeContents,
                &result
            };

            if (result.format != MpvFormatNodeMap ||
                !result.value.list)
            {
                return false;
            }

            std::int64_t width{};
            std::int64_t height{};
            std::int64_t stride{};
            std::string format;
            MpvByteArray* bytes{};

            MpvNodeList const* const map =
                result.value.list;

            for (int index = 0;
                index < map->num;
                ++index)
            {
                if (!map->keys ||
                    !map->keys[index])
                {
                    continue;
                }

                std::string const key =
                    map->keys[index];

                MpvNode const& value =
                    map->values[index];

                if (key == "w" &&
                    value.format == MpvFormatInt64)
                {
                    width = value.value.integer;
                }
                else if (key == "h" &&
                    value.format == MpvFormatInt64)
                {
                    height = value.value.integer;
                }
                else if (key == "stride" &&
                    value.format == MpvFormatInt64)
                {
                    stride = value.value.integer;
                }
                else if (key == "format" &&
                    value.format == MpvFormatString &&
                    value.value.string)
                {
                    format = value.value.string;
                }
                else if (key == "data" &&
                    value.format == MpvFormatByteArray)
                {
                    bytes =
                        value.value.byteArray;
                }
            }

            if (width <= 0 ||
                height <= 0 ||
                stride == 0 ||
                format != "bgra" ||
                !bytes ||
                !bytes->data)
            {
                return false;
            }

            if (width >
                    static_cast<std::int64_t>(
                        INT_MAX) ||
                height >
                    static_cast<std::int64_t>(
                        INT_MAX))
            {
                return false;
            }

            size_t const rowBytes =
                static_cast<size_t>(width) * 4u;

            size_t const absoluteStride =
                static_cast<size_t>(
                    stride < 0
                    ? -stride
                    : stride);

            if (absoluteStride < rowBytes)
            {
                return false;
            }

            size_t const required =
                absoluteStride *
                    static_cast<size_t>(
                        height - 1) +
                rowBytes;

            if (bytes->size < required)
            {
                return false;
            }

            frame.width =
                static_cast<int>(width);

            frame.height =
                static_cast<int>(height);

            frame.stride =
                frame.width * 4;

            frame.pixels.resize(
                static_cast<size_t>(
                    frame.stride) *
                static_cast<size_t>(
                    frame.height));

            auto const* source =
                static_cast<std::uint8_t const*>(
                    bytes->data);

            for (int y = 0;
                y < frame.height;
                ++y)
            {
                std::ptrdiff_t const sourceOffset =
                    static_cast<std::ptrdiff_t>(y) *
                    static_cast<std::ptrdiff_t>(
                        stride);

                auto const* sourceRow =
                    source + sourceOffset;

                auto* destinationRow =
                    frame.pixels.data() +
                    static_cast<size_t>(y) *
                    static_cast<size_t>(
                        frame.stride);

                std::memcpy(
                    destinationRow,
                    sourceRow,
                    rowBytes);
            }

            return true;
        }

        void ShutdownUnlocked()
        {
            m_currentPath.clear();

            if (m_handle && m_destroy)
            {
                m_destroy(m_handle);
            }

            m_handle = nullptr;
        }

    private:
        std::mutex m_mutex;

        HMODULE m_module{};
        mpv_handle* m_handle{};

        mpv_create_fn m_create{};
        mpv_initialize_fn m_initialize{};
        mpv_set_option_string_fn m_setOption{};
        mpv_set_property_string_fn m_setProperty{};
        mpv_get_property_fn m_getProperty{};
        mpv_command_fn m_command{};
        mpv_command_ret_fn m_commandRet{};
        mpv_terminate_destroy_fn m_destroy{};
        mpv_free_node_contents_fn m_freeNodeContents{};

        std::wstring m_currentPath;
    };

    ThumbnailEngine g_thumbnailEngine;
}

namespace ThumbnailBridge
{
    bool IsAvailable()
    {
        return g_thumbnailEngine.IsAvailable();
    }

    bool GetFrame(
        std::wstring const& filePath,
        double seconds,
        SeekMode mode,
        Frame& frame,
        std::wstring& error)
    {
        return g_thumbnailEngine.GetFrame(
            filePath,
            seconds,
            mode,
            frame,
            error);
    }

    bool GetFrame(
        std::wstring const& filePath,
        double seconds,
        Frame& frame,
        std::wstring& error)
    {
        return GetFrame(
            filePath,
            seconds,
            SeekMode::Exact,
            frame,
            error);
    }

    void Shutdown()
    {
        g_thumbnailEngine.Shutdown();
    }
}
