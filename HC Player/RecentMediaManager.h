#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hc::recent
{
    struct Item
    {
        std::wstring path;
        std::int64_t playedAt{};
        // Optional presentation metadata for protocol sources. The path stays
        // the identity used to reopen the item.
        std::wstring title;
    };

    class Manager
    {
    public:
        void Add(std::wstring const& path);
        bool UpdateProtocolTitle(
            std::wstring const& currentPath,
            std::wstring title);
        std::vector<Item> GetItems();
        void Clear();

    private:
        void Load();
        void Save() const;

        bool m_loaded{};
        std::vector<Item> m_items;
    };
}
