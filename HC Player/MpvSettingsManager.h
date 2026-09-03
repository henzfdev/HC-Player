#pragma once

#include "PlayerBridge.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hc::settings
{
    enum class OptionApplyMode
    {
        Immediate,
        NextFile,
        RestartEngine
    };

    using OptionMap = std::map<std::string, std::string>;
    using ValidateOption = std::function<bool(
        std::wstring const& name,
        std::wstring const& value,
        bool profile)>;

    class Manager
    {
    public:
        OptionMap& Overrides() noexcept { return m_overrides; }
        OptionMap const& Overrides() const noexcept { return m_overrides; }

        bool Dirty() const noexcept { return m_dirty; }
        void MarkDirty() noexcept { m_dirty = true; }

        bool SaveNativeOptions();
        void LoadNativeOptions();
        bool ResetToDefaults();

        std::filesystem::path ImportedConfigStoragePath() const;
        std::wstring const& ImportedConfigPath() const noexcept
        {
            return m_importedConfigPath;
        }
        std::vector<ImportedMpvOption> const& ImportedOptions() const noexcept
        {
            return m_importedOptions;
        }

        std::vector<std::wstring> GetImportedProfileNames() const;
        ImportedMpvConfig GetImportedConfig() const;
        ImportedMpvConfig ImportMpvConfig(
            std::wstring const& path,
            ValidateOption const& validate);
        bool ResetImportedConfig();
        bool UpdateImportedOption(
            std::wstring const& section,
            std::wstring const& name,
            std::wstring const& value,
            bool profile,
            ValidateOption const& validate);

        std::wstring const& ActiveImportedProfile() const noexcept
        {
            return m_activeImportedProfile;
        }
        void SetActiveImportedProfile(std::wstring value)
        {
            m_activeImportedProfile = std::move(value);
        }
        void ClearActiveImportedProfile() noexcept
        {
            m_activeImportedProfile.clear();
        }

        OptionApplyMode ApplyModeForOption(std::wstring const& name) const;

    private:
        std::filesystem::path NativeOptionsStoragePath() const;
        bool SaveImportedOptions(
            std::vector<ImportedMpvOption> const& options);

        OptionMap m_overrides;
        bool m_dirty{};
        std::wstring m_importedConfigPath;
        std::vector<ImportedMpvOption> m_importedOptions;
        std::wstring m_activeImportedProfile;
    };
}
