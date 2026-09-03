#pragma once

#include "ThumbnailBridge.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

class ThumbnailController
{
public:
    struct Result
    {
        std::uint64_t requestId{};
        double requestedSeconds{};
        double seconds{};
        bool exact{};

        ThumbnailBridge::Frame frame;
        std::wstring error;

        explicit operator bool() const noexcept
        {
            return !frame.pixels.empty() &&
                frame.width > 0 &&
                frame.height > 0;
        }
    };

    using ResultCallback = std::function<void(Result&&)>;

    ThumbnailController() = default;
    ~ThumbnailController();

    ThumbnailController(ThumbnailController const&) = delete;
    ThumbnailController& operator=(ThumbnailController const&) = delete;

    // One latest-target slot only. Requests are rate-limited inside the worker,
    // so PointerMoved may call this freely without turning every mouse event
    // into a decode. scrubbing=true uses an even lower background rate.
    void Request(
        double seconds,
        bool scrubbing,
        ResultCallback callback = {});

    void Cancel();
    void Shutdown();

private:
    struct PendingRequest
    {
        std::uint64_t id{};
        double seconds{};
        bool scrubbing{};
        std::chrono::steady_clock::time_point changedAt{};
        ResultCallback callback;
    };

    struct CacheEntry
    {
        std::wstring mediaPath;
        double seconds{};
        ThumbnailBridge::Frame frame;
    };

    void EnsureThreadLocked();
    void WorkerLoop();

    bool TryGetCachedFrameLocked(
        std::wstring const& mediaPath,
        double seconds,
        ThumbnailBridge::Frame& frame);

    void StoreCachedFrameLocked(
        std::wstring const& mediaPath,
        double seconds,
        ThumbnailBridge::Frame const& frame);

    // 38P: decoding is intentionally much slower than pointer/UI motion.
    // Normal hover: at most 12.5 FAST frames/s.
    // Button-held scrubbing: at most ~7 FAST frames/s.
    static constexpr auto FastRequestInterval =
        std::chrono::milliseconds(80);
    static constexpr auto ScrubFastRequestInterval =
        std::chrono::milliseconds(140);

    // EXACT exists only as the final refinement after the pointer has settled.
    static constexpr auto ExactSettleDelay =
        std::chrono::milliseconds(160);
    static constexpr auto ScrubExactSettleDelay =
        std::chrono::milliseconds(260);

    // 32 x 320x180 BGRA is only about 7 MiB and greatly reduces repeated
    // decoding when the pointer revisits the same timeline neighborhood.
    static constexpr std::size_t MaxCachedFrames = 32;
    static constexpr double CacheTimeToleranceSeconds = 0.050;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    std::optional<PendingRequest> m_pending;
    std::deque<CacheEntry> m_cache;
    std::uint64_t m_latestRequestId{};
    bool m_stopping{};
};
