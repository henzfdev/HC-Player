#include "pch.h"
#include "ThumbnailController.h"
#include "PlayerBridge.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <utility>

ThumbnailController::~ThumbnailController()
{
    Shutdown();
}

void ThumbnailController::EnsureThreadLocked()
{
    if (m_worker.joinable())
    {
        return;
    }

    m_stopping = false;
    m_worker = std::thread(
        [this]()
        {
            // Thumbnail decoding is auxiliary work. Never let it compete at
            // normal priority with playback, input or the WinUI thread.
            SetThreadPriority(
                GetCurrentThread(),
                THREAD_PRIORITY_BELOW_NORMAL);

            WorkerLoop();
        });
}

void ThumbnailController::Request(
    double seconds,
    bool scrubbing,
    ResultCallback callback)
{
    if (!std::isfinite(seconds))
    {
        return;
    }

    seconds = (std::max)(0.0, seconds);

    std::scoped_lock lock{ m_mutex };

    if (m_stopping)
    {
        return;
    }

    EnsureThreadLocked();

    PendingRequest request;
    request.id = ++m_latestRequestId;
    request.seconds = seconds;
    request.scrubbing = scrubbing;
    request.changedAt = std::chrono::steady_clock::now();
    request.callback = std::move(callback);

    // Exactly one pending target. New mouse motion replaces stale work.
    m_pending = std::move(request);
    m_cv.notify_one();
}

void ThumbnailController::Cancel()
{
    std::scoped_lock lock{ m_mutex };

    ++m_latestRequestId;
    m_pending.reset();
    m_cv.notify_one();
}

void ThumbnailController::Shutdown()
{
    {
        std::scoped_lock lock{ m_mutex };

        if (!m_worker.joinable())
        {
            ThumbnailBridge::Shutdown();
            return;
        }

        m_stopping = true;
        ++m_latestRequestId;
        m_pending.reset();
        m_cv.notify_one();
    }

    m_worker.join();

    {
        std::scoped_lock lock{ m_mutex };
        m_stopping = false;
        m_cache.clear();
    }

    ThumbnailBridge::Shutdown();
}

bool ThumbnailController::TryGetCachedFrameLocked(
    std::wstring const& mediaPath,
    double seconds,
    ThumbnailBridge::Frame& frame)
{
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
    {
        if (it->mediaPath != mediaPath)
        {
            continue;
        }

        if (std::abs(it->seconds - seconds) >
            CacheTimeToleranceSeconds)
        {
            continue;
        }

        CacheEntry hit = std::move(*it);
        m_cache.erase(it);
        m_cache.push_front(std::move(hit));

        frame = m_cache.front().frame;
        return true;
    }

    return false;
}

void ThumbnailController::StoreCachedFrameLocked(
    std::wstring const& mediaPath,
    double seconds,
    ThumbnailBridge::Frame const& frame)
{
    if (frame.pixels.empty() ||
        frame.width <= 0 ||
        frame.height <= 0)
    {
        return;
    }

    for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
    {
        if (it->mediaPath == mediaPath &&
            std::abs(it->seconds - seconds) <=
                CacheTimeToleranceSeconds)
        {
            m_cache.erase(it);
            break;
        }
    }

    CacheEntry entry;
    entry.mediaPath = mediaPath;
    entry.seconds = seconds;
    entry.frame = frame;

    m_cache.push_front(std::move(entry));

    while (m_cache.size() > MaxCachedFrames)
    {
        m_cache.pop_back();
    }
}

void ThumbnailController::WorkerLoop()
{
    using Clock = std::chrono::steady_clock;

    Clock::time_point lastFastStarted{};

    auto deliverResult =
        [this](PendingRequest const& request, Result&& result)
        {
            bool deliver = false;

            {
                std::scoped_lock lock{ m_mutex };
                deliver =
                    !m_stopping &&
                    request.id == m_latestRequestId;
            }

#ifdef _DEBUG
            if (deliver)
            {
                wchar_t message[360]{};

                if (result)
                {
                    swprintf_s(
                        message,
                        result.exact
                            ? L"[HC Thumbnail 38P] EXACT target %.3fs -> frame %.3fs -> %dx%d (%zu bytes)\n"
                            : L"[HC Thumbnail 38P] FAST  target %.3fs -> frame %.3fs -> %dx%d (%zu bytes)\n",
                        result.requestedSeconds,
                        result.seconds,
                        result.frame.width,
                        result.frame.height,
                        result.frame.pixels.size());
                }
                else
                {
                    swprintf_s(
                        message,
                        result.exact
                            ? L"[HC Thumbnail 38P] EXACT %.3fs -> sem frame (%s)\n"
                            : L"[HC Thumbnail 38P] FAST  %.3fs -> sem frame (%s)\n",
                        result.requestedSeconds,
                        result.error.c_str());
                }

                OutputDebugStringW(message);
            }
#endif

            if (deliver && request.callback)
            {
                request.callback(std::move(result));
            }
        };

    for (;;)
    {
        PendingRequest request;

        {
            std::unique_lock lock{ m_mutex };

            m_cv.wait(
                lock,
                [this]()
                {
                    return m_stopping || m_pending.has_value();
                });

            if (m_stopping)
            {
                return;
            }

            // Rate limit the decoder, not the UI. PointerMoved can update the
            // one pending slot hundreds of times; only the newest target at the
            // next decode window survives.
            for (;;)
            {
                if (m_stopping)
                {
                    return;
                }

                if (!m_pending)
                {
                    break;
                }

                auto const interval =
                    m_pending->scrubbing
                        ? ScrubFastRequestInterval
                        : FastRequestInterval;

                auto const now = Clock::now();
                auto const due =
                    lastFastStarted.time_since_epoch().count() == 0
                        ? now
                        : lastFastStarted + interval;

                if (now < due)
                {
                    // A notification means a newer target replaced m_pending.
                    // Wake, re-read that target, but keep the same rate limit.
                    m_cv.wait_until(lock, due);
                    continue;
                }

                request = std::move(*m_pending);
                m_pending.reset();
                lastFastStarted = Clock::now();
                break;
            }

            if (request.id == 0)
            {
                continue;
            }
        }

        std::wstring mediaPath;

        if (!PlayerGetCurrentLocalMediaPath(mediaPath))
        {
            Result unavailable;
            unavailable.requestId = request.id;
            unavailable.requestedSeconds = request.seconds;
            unavailable.seconds = request.seconds;
            unavailable.exact = false;
            unavailable.error =
                L"Thumbnail preview is currently limited to local media files.";
            deliverResult(request, std::move(unavailable));
            continue;
        }

        // Cache contains only exact frames, so a hit is already authoritative
        // and avoids both FAST and EXACT decoding.
        ThumbnailBridge::Frame cachedFrame;
        bool cacheHit = false;

        {
            std::scoped_lock lock{ m_mutex };
            cacheHit = TryGetCachedFrameLocked(
                mediaPath,
                request.seconds,
                cachedFrame);
        }

        if (cacheHit)
        {
            Result cached;
            cached.requestId = request.id;
            cached.requestedSeconds = request.seconds;
            cached.seconds = request.seconds;
            cached.exact = true;
            cached.frame = std::move(cachedFrame);

            if (std::isfinite(cached.frame.timestampSeconds) &&
                cached.frame.timestampSeconds >= 0.0)
            {
                cached.seconds = cached.frame.timestampSeconds;
            }

            deliverResult(request, std::move(cached));
            continue;
        }

        Result fast;
        fast.requestId = request.id;
        fast.requestedSeconds = request.seconds;
        fast.seconds = request.seconds;
        fast.exact = false;

        ThumbnailBridge::GetFrame(
            mediaPath,
            request.seconds,
            ThumbnailBridge::SeekMode::Fast,
            fast.frame,
            fast.error);

        if (fast &&
            std::isfinite(fast.frame.timestampSeconds) &&
            fast.frame.timestampSeconds >= 0.0)
        {
            fast.seconds = fast.frame.timestampSeconds;
        }

        deliverResult(request, std::move(fast));

        // EXACT is a single trailing refinement only. Any newer hover target
        // cancels it before it starts.
        {
            std::unique_lock lock{ m_mutex };

            if (m_stopping)
            {
                return;
            }

            if (request.id != m_latestRequestId)
            {
                continue;
            }

            auto const settleDelay =
                request.scrubbing
                    ? ScrubExactSettleDelay
                    : ExactSettleDelay;

            auto const exactDue =
                request.changedAt + settleDelay;

            bool const changed = m_cv.wait_until(
                lock,
                exactDue,
                [this, requestId = request.id]()
                {
                    return m_stopping ||
                        requestId != m_latestRequestId;
                });

            if (m_stopping)
            {
                return;
            }

            if (changed || request.id != m_latestRequestId)
            {
                continue;
            }
        }

        Result exact;
        exact.requestId = request.id;
        exact.requestedSeconds = request.seconds;
        exact.seconds = request.seconds;
        exact.exact = true;

        ThumbnailBridge::GetFrame(
            mediaPath,
            request.seconds,
            ThumbnailBridge::SeekMode::Exact,
            exact.frame,
            exact.error);

        if (exact &&
            std::isfinite(exact.frame.timestampSeconds) &&
            exact.frame.timestampSeconds >= 0.0)
        {
            exact.seconds = exact.frame.timestampSeconds;
        }

        if (exact)
        {
            std::scoped_lock lock{ m_mutex };

            if (!m_stopping &&
                request.id == m_latestRequestId)
            {
                StoreCachedFrameLocked(
                    mediaPath,
                    request.seconds,
                    exact.frame);
            }
        }

        deliverResult(request, std::move(exact));
    }
}
