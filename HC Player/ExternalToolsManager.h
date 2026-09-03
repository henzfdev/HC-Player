#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace hc::tools
{
    struct Status
    {
        bool ytdlpAvailable{};
        bool ytdlpImported{};
        bool denoAvailable{};
        bool denoImported{};
        bool denoInvalid{};
        std::wstring ytdlpPath;
        std::wstring denoPath;
        std::wstring ytdlpMessage;
    };

    class Manager
    {
    public:
        std::filesystem::path ResolveYtdlpPath(bool* imported = nullptr) const;
        std::filesystem::path ResolveDenoPath(bool* imported = nullptr) const;
        void AddExecutableDirectoryToPath(
            std::filesystem::path const& executable) const;

        Status GetStatus() const;

        bool ImportYtdlpBinary(
            std::wstring const& sourcePath,
            std::function<bool()> const& restartEngine,
            std::wstring& error) const;
        bool ResetImportedYtdlp(
            std::function<bool()> const& restartEngine,
            std::wstring& error) const;
        bool ImportDenoBinary(
            std::wstring const& sourcePath,
            std::function<bool()> const& restartEngine,
            std::wstring& error) const;
        bool ResetImportedDeno(
            std::function<bool()> const& restartEngine,
            std::wstring& error) const;

    private:
        std::filesystem::path FindDenoCandidate(bool* imported = nullptr) const;
        bool ProbeDenoExecutable(std::filesystem::path const& path) const;

        mutable std::filesystem::path m_cachedDenoPath;
        mutable std::filesystem::file_time_type m_cachedDenoWriteTime{};
        mutable bool m_cachedDenoResult{};
        mutable bool m_hasCachedDenoResult{};
    };
}
