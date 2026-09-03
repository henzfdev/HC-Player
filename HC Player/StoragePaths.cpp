#include "pch.h"
#include "StoragePaths.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <vector>

namespace hc::storage
{
    std::filesystem::path ExecutableDirectory() noexcept
    {
        try
        {
            std::vector<wchar_t> buffer(512);
            for (;;)
            {
                DWORD const length = GetModuleFileNameW(
                    nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length == 0) return {};

                if (length < buffer.size())
                {
                    return std::filesystem::path{
                        std::wstring(buffer.data(), length) }.parent_path();
                }

                if (buffer.size() >= 32768) return {};
                buffer.resize((std::min)(buffer.size() * 2, std::size_t{ 32768 }));
            }
        }
        catch (...)
        {
            return {};
        }
    }

    bool IsPortableMode() noexcept
    {
        static bool const portable = []() noexcept
            {
                try
                {
                    auto const directory = ExecutableDirectory();
                    if (directory.empty()) return false;

                    std::error_code error;
                    auto const marker = directory / L"portable.flag";
                    bool const exists = std::filesystem::is_regular_file(marker, error);
                    return !error && exists;
                }
                catch (...)
                {
                    return false;
                }
            }();
        return portable;
    }

    std::filesystem::path UserDataRoot()
    {
        if (IsPortableMode())
        {
            auto const directory = ExecutableDirectory();
            if (!directory.empty())
            {
                return directory / L"Data";
            }

            // Fail closed with respect to portability. If the executable path
            // cannot be resolved after portable mode was selected, do not
            // silently spill private data into the user's LocalAppData.
            return std::filesystem::temp_directory_path() /
                L"HC Player Portable - unavailable path";
        }

        wchar_t localAppData[MAX_PATH]{};
        DWORD const length = GetEnvironmentVariableW(
            L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
        return length
            ? std::filesystem::path{ localAppData } / L"HC Player"
            : std::filesystem::temp_directory_path() / L"HC Player";
    }
}
