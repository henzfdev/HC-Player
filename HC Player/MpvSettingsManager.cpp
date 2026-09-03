#include "pch.h"
#include "MpvSettingsManager.h"
#include "StoragePaths.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>

namespace hc::settings
{
    namespace
    {
        std::filesystem::path LocalStorageRoot()
        {
            return hc::storage::UserDataRoot();
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

        std::wstring StripInlineComment(std::wstring const& line)
        {
            wchar_t quote{};
            std::wstring result;
            for (wchar_t character : line)
            {
                if (character == L'\'' || character == L'"')
                {
                    if (!quote)
                    {
                        quote = character;
                    }
                    else if (quote == character)
                    {
                        quote = 0;
                    }
                }
                if (character == L'#' && !quote)
                {
                    break;
                }
                result.push_back(character);
            }
            return Trim(result);
        }

        std::wstring DecodeConfigText(std::string const& bytes)
        {
            try
            {
                auto text = winrt::to_hstring(bytes);
                std::wstring result{ text.c_str() };
                if (!result.empty() && result.front() == 0xFEFF)
                    result.erase(result.begin());
                return result;
            }
            catch (...)
            {
                int count = MultiByteToWideChar(
                    CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
                std::wstring result(count, L'\0');
                MultiByteToWideChar(
                    CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()),
                    result.data(), count);
                return result;
            }
        }

        std::wstring SerializeConfigValue(std::wstring value)
        {
            if (value.find(L'#') == std::wstring::npos &&
                value.find(L'"') == std::wstring::npos)
            {
                return value;
            }

            std::wstring escaped;
            for (wchar_t character : value)
            {
                if (character == L'"' || character == L'\\')
                    escaped.push_back(L'\\');
                escaped.push_back(character);
            }
            return L"\"" + escaped + L"\"";
        }

        const std::set<std::wstring> PlayerOwnedImportedOptions = {
            L"taskbar-progress", L"fullscreen", L"force-seekable", L"keep-open",
            L"autofit", L"autofit-larger",
            L"ytdl-format", L"ytdl-raw-options", L"hwdec", L"vo",
            L"gpu-api", L"gpu-context", L"icc-profile-auto", L"dither-depth", L"dither",
            L"sub-scale-by-window", L"sub-use-margins", L"sub-scale",
            L"scale", L"dscale", L"cscale", L"linear-upscaling",
            L"sigmoid-upscaling", L"correct-downscaling",
            L"video-output-levels", L"deband", L"deband-iterations",
            L"deband-threshold", L"deband-range", L"deband-grain", L"hr-seek",
            L"alang", L"slang", L"target-colorspace-hint", L"tone-mapping",
            L"cursor-autohide-fs-only", L"cursor-autohide",
            L"osd-level", L"osd-duration", L"osd-status-msg", L"osd-msg3",
            L"osd-font", L"osd-font-size", L"osd-color",
            L"osd-border-color", L"osd-outline-color",
            L"osd-border-size", L"osd-outline-size", L"osd-blur",
            L"screenshot-format", L"screenshot-high-bit-depth",
            L"screenshot-png-compression", L"screenshot-directory",
            L"screenshot-dir", L"screenshot-template", L"blend-subtitles",
            L"video-sync", L"interpolation", L"tscale",
            L"demuxer-mkv-subtitle-preroll", L"sub-fix-timing", L"sub-auto",
            L"sub-font", L"sub-font-size", L"sub-color", L"sub-border-color",
            L"sub-outline-color", L"sub-border-size", L"sub-outline-size",
            L"sub-shadow-offset", L"sub-spacing", L"sub-blur", L"sub-gauss",
            L"volume", L"audio-file-auto", L"volume-max",
            L"audio-pitch-correction", L"audio-device", L"audio-exclusive",
            L"deinterlace"
        };

        const std::set<std::wstring> IgnoredImportedOptions = {
            L"title", L"osd-bar", L"osc", L"border", L"ao", L"sub-fonts-dir"
        };

        std::wstring CanonicalImportedOptionName(std::wstring name)
        {
            std::transform(name.begin(), name.end(), name.begin(), towlower);
            if (name == L"screenshot-dir") return L"screenshot-directory";
            if (name == L"osd-status-msg") return L"osd-msg3";
            if (name == L"osd-outline-color") return L"osd-border-color";
            if (name == L"osd-outline-size") return L"osd-border-size";
            if (name == L"sub-outline-color") return L"sub-border-color";
            if (name == L"sub-outline-size") return L"sub-border-size";
            return name;
        }

        bool IsPlayerOwnedImportedOption(std::wstring name)
        {
            std::transform(name.begin(), name.end(), name.begin(), towlower);
            return PlayerOwnedImportedOptions.contains(name);
        }

        bool IsIgnoredImportedOption(std::wstring name)
        {
            std::transform(name.begin(), name.end(), name.begin(), towlower);
            return IgnoredImportedOptions.contains(name);
        }
    }

    std::filesystem::path Manager::ImportedConfigStoragePath() const
    {
        return LocalStorageRoot() / L"imported-mpv.conf";
    }

    std::filesystem::path Manager::NativeOptionsStoragePath() const
    {
        return LocalStorageRoot() / L"native-options.dat";
    }

    bool Manager::SaveNativeOptions()
    {
        auto storage = NativeOptionsStoragePath();
        std::error_code error;
        std::filesystem::create_directories(storage.parent_path(), error);
        if (error) return false;

        std::ofstream output(storage, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        for (auto const& [name, value] : m_overrides)
            output << std::quoted(name) << ' ' << std::quoted(value) << '\n';

        m_dirty = !output.good();
        return !m_dirty;
    }

    void Manager::LoadNativeOptions()
    {
        std::ifstream input(NativeOptionsStoragePath(), std::ios::binary);
        std::string name;
        std::string value;
        while (input >> std::quoted(name) >> std::quoted(value))
            m_overrides[name] = value;

        for (auto const* ignored : { "title", "osd-bar", "osc", "border", "ao" })
        {
            if (m_overrides.erase(ignored) > 0)
                m_dirty = true;
        }
        if (auto screenshots = m_overrides.find("screenshot-directory");
            screenshots != m_overrides.end() &&
            (screenshots->second == "~/Pictures/mpv-screenshots" ||
                screenshots->second == "~/Pictures/HCPlayer-screenshots"))
        {
            screenshots->second = "~/Pictures/Capturas do HC Player";
            m_dirty = true;
        }
        // reset-on-next-file is no longer exposed as a raw text field in the
        // normal Settings UI. Remove any historical native-panel override once
        // so the HC Player built-in default (pause) becomes authoritative again.
        // A persisted imported mpv.conf is loaded later during startup and may
        // deliberately reapply its own reset-on-next-file value.
        if (!m_overrides.contains("ui-reset-on-next-file-hidden-v1"))
        {
            m_overrides.erase("reset-on-next-file");
            m_overrides["ui-reset-on-next-file-hidden-v1"] = "yes";
            m_dirty = true;
        }

        // The direct DXVA2 path is deliberately no longer exposed because mpv
        // documents it as unsafe on Windows. Preserve an existing user's
        // hardware-decoding intent by migrating only that exact legacy value
        // to the copy-back variant, which remains available in the UI.
        if (auto hwdec = m_overrides.find("hwdec");
            hwdec != m_overrides.end() && hwdec->second == "dxva2")
        {
            hwdec->second = "dxva2-copy";
            m_dirty = true;
        }
        // The app-managed subtitle-font directory is an HC Player private
        // path. Rebase it when a portable folder is moved to another location;
        // imported mpv.conf sub-fonts-dir values are deliberately ignored by
        // the importer, so this cannot rewrite an external imported directory.
        if (hc::storage::IsPortableMode())
        {
            if (auto fontDirectory = m_overrides.find("sub-fonts-dir");
                fontDirectory != m_overrides.end())
            {
                std::string const portableFonts = winrt::to_string(
                    (LocalStorageRoot() / L"fonts").wstring());
                if (fontDirectory->second != portableFonts)
                {
                    fontDirectory->second = portableFonts;
                    m_dirty = true;
                }
            }
        }

        // HC Player previously bundled Netflix Sans Medium as the default
        // subtitle font. It is no longer distributed. Migrate only the old
        // bundled-font choice; a user-imported font with the same family/full
        // name is stored under HC Player's private data root\fonts and must
        // remain authoritative (LocalAppData when installed, .\Data portable).
        bool usesImportedFontsDirectory = false;
        if (auto fontDirectory = m_overrides.find("sub-fonts-dir");
            fontDirectory != m_overrides.end())
        {
            auto configuredDirectory = std::filesystem::path{
                winrt::to_hstring(fontDirectory->second).c_str() }.lexically_normal();
            auto importedDirectory = (LocalStorageRoot() / L"fonts").lexically_normal();
            usesImportedFontsDirectory = _wcsicmp(
                configuredDirectory.c_str(), importedDirectory.c_str()) == 0;
        }
        if (auto subtitleFont = m_overrides.find("sub-font");
            subtitleFont != m_overrides.end() &&
            subtitleFont->second == "Netflix Sans Medium" &&
            !usesImportedFontsDirectory)
        {
            subtitleFont->second = "Segoe UI";
            m_overrides.erase("sub-fonts-dir");
            m_dirty = true;
        }
        // 39Z46B briefly migrated the historical window-fit defaults to empty.
        // Keep that cleanup migration one-shot only. HC Player now supplies its
        // current official defaults (1216x714 and 81%x81%) from BaseMpvOptions,
        // while any explicit user override remains authoritative.
        if (!m_overrides.contains("ui-window-fit-empty-defaults-v1"))
        {
            if (auto autofit = m_overrides.find("autofit");
                autofit != m_overrides.end() &&
                autofit->second == "1218x716")
            {
                m_overrides.erase(autofit);
            }

            if (auto autofitLarger = m_overrides.find("autofit-larger");
                autofitLarger != m_overrides.end() &&
                autofitLarger->second == "81%x81%")
            {
                m_overrides.erase(autofitLarger);
            }

            m_overrides["ui-window-fit-empty-defaults-v1"] = "yes";
            m_dirty = true;
        }

        // HC Player no longer imposes a fixed yt-dlp format chain. Remove only
        // exact historical built-in defaults so existing users migrate to mpv/
        // yt-dlp automatic selection. Any genuinely custom user or imported
        // ytdl-format value is deliberately preserved.
        if (auto ytdlFormat = m_overrides.find("ytdl-format");
            ytdlFormat != m_overrides.end())
        {
            static const std::set<std::string> formerBuiltInYtdlFormats = {
                "616+251/335+251/313+251/308+251/308+140/303+251/299+251/271+251/248+251/137+140/136+140/135+140/best",
                "335+251/308+251/303+251/299+251/271+251/248+251/137+140/136+140/135+140/best"
            };

            if (formerBuiltInYtdlFormats.contains(ytdlFormat->second))
            {
                m_overrides.erase(ytdlFormat);
                m_dirty = true;
            }
        }
        if (!m_overrides.contains("osd-msg3"))
        {
            if (auto legacy = m_overrides.find("osd-status-msg");
                legacy != m_overrides.end())
            {
                m_overrides["osd-msg3"] = legacy->second;
                m_overrides.erase(legacy);
                m_dirty = true;
            }
        }
        if (auto interpolation = m_overrides.find("interpolation");
            interpolation != m_overrides.end() && interpolation->second == "yes")
        {
            auto videoSync = m_overrides.find("video-sync");
            if (videoSync == m_overrides.end() ||
                !videoSync->second.starts_with("display"))
            {
                m_overrides["video-sync"] = "display-resample";
                m_dirty = true;
            }
        }
        if (auto sigmoid = m_overrides.find("sigmoid-upscaling");
            sigmoid != m_overrides.end() && sigmoid->second == "yes")
        {
            if (auto linear = m_overrides.find("linear-upscaling");
                linear != m_overrides.end() && linear->second == "yes")
            {
                linear->second = "no";
                m_dirty = true;
            }
        }
        if (!m_overrides.contains("ui-osd-font-default-v1"))
        {
            m_overrides["osd-font"] = "Verdana";
            m_overrides["ui-osd-font-default-v1"] = "yes";
            m_dirty = true;
            SaveNativeOptions();
        }
        if (m_dirty) SaveNativeOptions();
        m_dirty = false;
    }

    bool Manager::ResetToDefaults()
    {
        // A factory reset is represented by the absence of user overrides.
        // Keep user-owned assets in their separate folders; only the settings
        // registry and an imported mpv.conf belong to this reset operation.
        auto const previousOverrides = m_overrides;
        auto const previousImportedOptions = m_importedOptions;
        auto const previousActiveProfile = m_activeImportedProfile;
        bool const hadImportedConfig = !m_importedConfigPath.empty();

        if (!ResetImportedConfig())
        {
            return false;
        }

        m_overrides.clear();
        m_dirty = true;
        if (!SaveNativeOptions())
        {
            // Best-effort rollback: a disk error must not intentionally leave
            // half of the previous configuration removed.
            m_overrides = previousOverrides;
            m_dirty = true;
            SaveNativeOptions();
            if (hadImportedConfig && !previousImportedOptions.empty())
            {
                SaveImportedOptions(previousImportedOptions);
                m_activeImportedProfile = previousActiveProfile;
            }
            return false;
        }

        m_dirty = false;
        return true;
    }

    OptionApplyMode Manager::ApplyModeForOption(std::wstring const& name) const
    {
        static const std::set<std::wstring> nextFile = {
            L"alang", L"slang", L"audio-file-auto", L"sub-auto",
            L"demuxer-mkv-subtitle-preroll"
        };
        static const std::set<std::wstring> restartEngine = {
            L"ao", L"vo", L"gpu-api", L"gpu-context"
        };
        if (restartEngine.contains(name)) return OptionApplyMode::RestartEngine;
        if (nextFile.contains(name)) return OptionApplyMode::NextFile;
        return OptionApplyMode::Immediate;
    }

    bool Manager::SaveImportedOptions(
        std::vector<ImportedMpvOption> const& options)
    {
        auto storage = ImportedConfigStoragePath();
        std::error_code error;
        std::filesystem::create_directories(storage.parent_path(), error);
        if (error) return false;

        std::ofstream output(storage, std::ios::binary | std::ios::trunc);
        if (!output) return false;

        std::wstring lastSection;
        bool lastWasProfile{};
        bool firstSection = true;
        for (auto const& option : options)
        {
            if (firstSection || option.section != lastSection || option.profile != lastWasProfile)
            {
                if (!firstSection) output << "\n";
                if (option.profile)
                    output << "[" << winrt::to_string(option.section) << "]\n";
                else
                    output << "# ===== " << winrt::to_string(option.section) << " =====\n";
                lastSection = option.section;
                lastWasProfile = option.profile;
                firstSection = false;
            }

            output << winrt::to_string(option.name) << "="
                << winrt::to_string(SerializeConfigValue(option.value)) << "\n";
        }

        output.close();
        if (!output) return false;

        m_importedConfigPath = storage.wstring();
        m_importedOptions = options;
        return true;
    }

    std::vector<std::wstring> Manager::GetImportedProfileNames() const
    {
        std::vector<std::wstring> profiles;
        std::set<std::wstring> seen;
        for (auto const& option : m_importedOptions)
        {
            if (option.profile && seen.insert(option.section).second)
                profiles.push_back(option.section);
        }
        return profiles;
    }

    ImportedMpvConfig Manager::GetImportedConfig() const
    {
        ImportedMpvConfig result{};
        result.options = m_importedOptions;
        result.success = !m_importedConfigPath.empty();
        std::set<std::pair<std::wstring, bool>> sections;
        for (auto const& option : result.options)
        {
            if (option.builtIn)
                ++result.appliedBuiltInCount;
            else if (!option.profile)
                ++result.additionalCount;
            if (!option.builtIn) sections.emplace(option.section, option.profile);
        }
        result.sectionCount = static_cast<int>(sections.size());
        result.message = result.success
            ? std::to_wstring(result.appliedBuiltInCount) +
                L" opções incorporadas ao painel; " +
                std::to_wstring(result.additionalCount) + L" opções adicionais"
            : L"Nenhuma configuração pessoal foi importada.";
        return result;
    }

    ImportedMpvConfig Manager::ImportMpvConfig(
        std::wstring const& path,
        ValidateOption const& validate)
    {
        ImportedMpvConfig result{};

        std::ifstream input(std::filesystem::path{ path }, std::ios::binary);
        if (!input)
        {
            result.message = L"Não foi possível ler o arquivo selecionado.";
            return result;
        }

        std::string bytes{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{} };
        std::wistringstream lines{ DecodeConfigText(bytes) };

        std::wstring currentSection = L"GERAL";
        bool currentIsProfile{};
        std::wstring line;
        std::vector<std::pair<std::wstring, std::wstring>> ignoredGlobalOptions;

        while (std::getline(lines, line))
        {
            std::wstring trimmed = Trim(line);
            if (trimmed.empty()) continue;

            if (trimmed.front() == L'#')
            {
                std::wstring heading = Trim(trimmed.substr(1));
                if (!heading.empty() && heading.front() == L'=')
                {
                    while (!heading.empty() &&
                        (heading.front() == L'=' || iswspace(heading.front())))
                        heading.erase(heading.begin());
                    while (!heading.empty() &&
                        (heading.back() == L'=' || iswspace(heading.back())))
                        heading.pop_back();
                    heading = Trim(heading);
                    if (!heading.empty())
                    {
                        currentSection = heading;
                        currentIsProfile = false;
                    }
                }
                continue;
            }

            if (trimmed.size() > 2 &&
                trimmed.front() == L'[' && trimmed.back() == L']')
            {
                currentSection = Trim(trimmed.substr(1, trimmed.size() - 2));
                currentIsProfile = true;
                continue;
            }

            std::wstring optionLine = StripInlineComment(trimmed);
            if (optionLine.empty()) continue;

            size_t separator = optionLine.find(L'=');
            std::wstring name = Trim(separator == std::wstring::npos
                ? optionLine : optionLine.substr(0, separator));
            std::wstring value = separator == std::wstring::npos
                ? L"yes" : Trim(optionLine.substr(separator + 1));

            if (value.size() >= 2 &&
                ((value.front() == L'"' && value.back() == L'"') ||
                    (value.front() == L'\'' && value.back() == L'\'')))
            {
                value = value.substr(1, value.size() - 2);
            }

            if (name.empty())
            {
                ++result.invalidCount;
                continue;
            }

            name = CanonicalImportedOptionName(std::move(name));
            // Imported configurations receive the same one-way safety migration
            // as native settings, so dxva2 can never become active invisibly
            // after it has been removed from the Settings combobox.
            if (name == L"hwdec" && value == L"dxva2")
            {
                value = L"dxva2-copy";
            }
            if (name == L"screenshot-directory" &&
                (value == L"~/Pictures/mpv-screenshots" ||
                    value == L"~/Pictures/HCPlayer-screenshots"))
            {
                value = L"~/Pictures/Capturas do HC Player";
            }
            if (IsIgnoredImportedOption(name) ||
                (currentIsProfile && name == L"fullscreen"))
            {
                if (!currentIsProfile)
                    ignoredGlobalOptions.emplace_back(name, value);
                ++result.invalidCount;
                continue;
            }

            bool builtIn = !currentIsProfile && IsPlayerOwnedImportedOption(name);
            if (validate && validate(name, value, currentIsProfile))
            {
                result.options.push_back({
                    currentSection, name, value, currentIsProfile, builtIn });
                if (builtIn)
                    ++result.appliedBuiltInCount;
                else if (!currentIsProfile)
                    ++result.additionalCount;
            }
            else
            {
                ++result.invalidCount;
            }
        }

        if (result.options.empty())
        {
            result.message = L"Nenhuma opção válida foi encontrada.";
            return result;
        }

        for (auto const& [name, value] : ignoredGlobalOptions)
        {
            auto utf8Name = winrt::to_string(name);
            auto saved = m_overrides.find(utf8Name);
            if (saved != m_overrides.end() && saved->second == winrt::to_string(value))
            {
                m_overrides.erase(saved);
                m_dirty = true;
            }
        }

        std::set<std::pair<std::wstring, bool>> sections;
        for (auto const& option : result.options)
        {
            if (!option.builtIn) sections.emplace(option.section, option.profile);
        }
        result.sectionCount = static_cast<int>(sections.size());

        for (auto const& option : m_importedOptions)
        {
            if (!option.profile)
            {
                auto name = winrt::to_string(option.name);
                auto saved = m_overrides.find(name);
                if (saved != m_overrides.end() &&
                    saved->second == winrt::to_string(option.value))
                {
                    m_overrides.erase(saved);
                }
            }
        }

        for (auto const& option : result.options)
        {
            if (!option.profile)
                m_overrides[winrt::to_string(option.name)] = winrt::to_string(option.value);
        }

        if (!SaveImportedOptions(result.options))
        {
            result.message = L"As opções foram validadas, mas não foi possível salvar a configuração.";
            return result;
        }
        m_activeImportedProfile.clear();

        m_dirty = true;
        if (!SaveNativeOptions())
        {
            result.message = L"As opções foram importadas, mas não foi possível salvar os valores do painel.";
            return result;
        }

        result.success = true;
        result.message = std::to_wstring(result.appliedBuiltInCount) +
            L" opções incorporadas ao painel; " +
            std::to_wstring(result.additionalCount) + L" opções adicionais";
        if (result.invalidCount > 0)
        {
            result.message += L"; " + std::to_wstring(result.invalidCount) + L" ignoradas";
        }
        return result;
    }

    bool Manager::ResetImportedConfig()
    {
        bool changed{};
        for (auto const& option : m_importedOptions)
        {
            if (option.profile) continue;
            auto name = winrt::to_string(option.name);
            auto saved = m_overrides.find(name);
            if (saved != m_overrides.end() &&
                saved->second == winrt::to_string(option.value))
            {
                m_overrides.erase(saved);
                changed = true;
            }
        }
        if (changed)
            m_dirty = true;
        // Preserve any already-pending native save as well. Resetting an
        // imported file was historically also a persistence checkpoint.
        if (m_dirty && !SaveNativeOptions()) return false;

        std::error_code error;
        auto storage = ImportedConfigStoragePath();
        if (std::filesystem::exists(storage, error))
            std::filesystem::remove(storage, error);
        if (error) return false;

        m_importedConfigPath.clear();
        m_importedOptions.clear();
        m_activeImportedProfile.clear();
        return true;
    }

    bool Manager::UpdateImportedOption(
        std::wstring const& section,
        std::wstring const& name,
        std::wstring const& value,
        bool profile,
        ValidateOption const& validate)
    {
        if (!validate || !validate(name, value, profile)) return false;

        auto updated = m_importedOptions;
        auto item = std::find_if(updated.begin(), updated.end(),
            [&](ImportedMpvOption const& option)
            {
                return option.section == section &&
                    option.name == name && option.profile == profile;
            });
        if (item == updated.end()) return false;

        item->value = value;
        if (!SaveImportedOptions(updated)) return false;

        if (!profile)
        {
            m_overrides[winrt::to_string(name)] = winrt::to_string(value);
            m_dirty = true;
            if (!SaveNativeOptions()) return false;
        }
        return true;
    }
}
