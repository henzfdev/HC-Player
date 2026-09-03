#include "pch.h"
#include "RecentMediaManager.h"
#include "StoragePaths.h"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>
#include <winrt/base.h>

namespace
{
    constexpr std::size_t MaximumRecentItems = 10;

    std::filesystem::path RecentFilesStoragePath()
    {
        return hc::storage::UserDataRoot() / L"recent-files.dat";
    }

    bool IsProtocolPath(std::wstring const& path)
    {
        return path.find(L"://") != std::wstring::npos ||
            path.starts_with(L"magnet:");
    }

    bool EqualsInsensitive(
        std::wstring const& left,
        std::wstring const& right)
    {
        return _wcsicmp(left.c_str(), right.c_str()) == 0;
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

    std::wstring SanitizeTitle(std::wstring title)
    {
        title = Trim(std::move(title));
        for (auto& ch : title)
        {
            if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        }

        // Keep the persistence format strictly one physical line per item.
        std::wstring clean;
        clean.reserve(title.size());
        bool previousSpace{};
        for (wchar_t ch : title)
        {
            bool const space = iswspace(ch) != 0;
            if (space)
            {
                if (!previousSpace) clean.push_back(L' ');
            }
            else
            {
                clean.push_back(ch);
            }
            previousSpace = space;
        }
        return Trim(std::move(clean));
    }
}

namespace hc::recent
{
    void Manager::Save() const
    {
        auto storage = RecentFilesStoragePath();
        std::error_code error;
        std::filesystem::create_directories(storage.parent_path(), error);
        if (error) return;

        std::ofstream output(storage, std::ios::binary | std::ios::trunc);
        if (!output) return;

        for (auto const& item : m_items)
        {
            auto utf8Path = winrt::to_string(winrt::hstring{ item.path });
            auto utf8Title = winrt::to_string(winrt::hstring{ item.title });
            // Backslashes and quotes are escaped by std::quoted. The optional
            // title has already been sanitized to remain on this same line.
            output << item.playedAt << ' ' << std::quoted(utf8Path)
                << ' ' << std::quoted(utf8Title) << '\n';
        }
    }

    void Manager::Load()
    {
        if (m_loaded) return;
        m_loaded = true;
        m_items.clear();

        std::ifstream input(RecentFilesStoragePath(), std::ios::binary);
        std::string line;
        bool removedMissing{};
        while (std::getline(input, line))
        {
            if (line.empty()) continue;

            // Backward compatible with both HC Player formats:
            // old: <time> "<path>"
            // new: <time> "<path>" "<optional title>"
            std::istringstream row(line);
            std::int64_t playedAt{};
            std::string utf8Path;
            if (!(row >> playedAt >> std::quoted(utf8Path))) continue;

            std::string utf8Title;
            row >> std::ws;
            if (!row.eof())
            {
                if (!(row >> std::quoted(utf8Title))) utf8Title.clear();
            }

            Item item{
                std::wstring{ winrt::to_hstring(utf8Path) },
                playedAt,
                std::wstring{ winrt::to_hstring(utf8Title) } };

            std::error_code error;
            if (IsProtocolPath(item.path) ||
                std::filesystem::is_regular_file(item.path, error))
            {
                m_items.push_back(std::move(item));
                if (m_items.size() == MaximumRecentItems) break;
            }
            else
            {
                removedMissing = true;
            }
        }

        if (removedMissing) Save();
    }

    void Manager::Add(std::wstring const& path)
    {
        if (path.empty()) return;
        Load();

        bool const protocol = IsProtocolPath(path);
        std::wstring value = path;
        if (!protocol)
        {
            std::filesystem::path normalized{ path };
            std::error_code error;
            normalized = std::filesystem::weakly_canonical(normalized, error);
            if (!error) value = normalized.wstring();
        }

        // Reopening an item moves it to the top while retaining any title that
        // yt-dlp resolved during an earlier session.
        std::wstring retainedTitle;
        auto existing = std::find_if(
            m_items.begin(), m_items.end(),
            [&](Item const& item)
            {
                return EqualsInsensitive(item.path, value);
            });
        if (existing != m_items.end()) retainedTitle = existing->title;

        m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
            [&](Item const& item)
            {
                return EqualsInsensitive(item.path, value);
            }), m_items.end());

        auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        m_items.insert(m_items.begin(),
            { value, static_cast<std::int64_t>(now), std::move(retainedTitle) });
        if (m_items.size() > MaximumRecentItems)
            m_items.resize(MaximumRecentItems);
        Save();
    }

    bool Manager::UpdateProtocolTitle(
        std::wstring const& currentPath,
        std::wstring title)
    {
        if (currentPath.empty() || !IsProtocolPath(currentPath)) return false;

        title = SanitizeTitle(std::move(title));
        if (title.empty()) return false;

        // mpv may expose the URL itself while yt-dlp is still resolving. That
        // fallback is not useful metadata and must never replace a real title.
        if (EqualsInsensitive(title, currentPath) ||
            title.find(L"://") != std::wstring::npos ||
            title.starts_with(L"ytdl:"))
        {
            return false;
        }

        Load();
        auto item = std::find_if(
            m_items.begin(), m_items.end(),
            [&](Item const& recent)
            {
                return EqualsInsensitive(recent.path, currentPath);
            });
        if (item == m_items.end()) return false;
        if (item->title == title) return true;

        item->title = std::move(title);
        Save();
        return true;
    }

    std::vector<Item> Manager::GetItems()
    {
        Load();
        return m_items;
    }

    void Manager::Clear()
    {
        m_loaded = true;
        m_items.clear();
        std::error_code error;
        std::filesystem::remove(RecentFilesStoragePath(), error);
    }
}
