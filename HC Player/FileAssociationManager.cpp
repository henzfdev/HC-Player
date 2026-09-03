#include "pch.h"
#include "FileAssociationManager.h"
#include "Resource.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace hc::file_associations
{
    namespace
    {
        constexpr wchar_t ApplicationName[] = L"HC Player";
        constexpr wchar_t CapabilitiesPath[] = L"Software\\HCPlayer\\Capabilities";

        struct FileTypeRegistration
        {
            wchar_t const* extension;
            wchar_t const* progId;
            wchar_t const* description;
            int iconResourceId;
        };

        constexpr FileTypeRegistration FileTypes[]
        {
            { L".mkv",  L"HCPlayer.MKV.1",  L"Arquivo MKV",  IDI_FILETYPE_MKV },
            { L".aac",  L"HCPlayer.AAC.1",  L"Arquivo AAC",  IDI_FILETYPE_AAC },
            { L".avi",  L"HCPlayer.AVI.1",  L"Arquivo AVI",  IDI_FILETYPE_AVI },
            { L".flac", L"HCPlayer.FLAC.1", L"Arquivo FLAC", IDI_FILETYPE_FLAC },
            { L".m3u8", L"HCPlayer.M3U8.1", L"Arquivo M3U8", IDI_FILETYPE_M3U8 },
            { L".m4a",  L"HCPlayer.M4A.1",  L"Arquivo M4A",  IDI_FILETYPE_M4A },
            { L".mka",  L"HCPlayer.MKA.1",  L"Arquivo MKA",  IDI_FILETYPE_MKA },
            { L".mov",  L"HCPlayer.MOV.1",  L"Arquivo MOV",  IDI_FILETYPE_MOV },
            { L".mp3",  L"HCPlayer.MP3.1",  L"Arquivo MP3",  IDI_FILETYPE_MP3 },
            { L".mp4",  L"HCPlayer.MP4.1",  L"Arquivo MP4",  IDI_FILETYPE_MP4 },
            { L".mpeg", L"HCPlayer.MPEG.1", L"Arquivo MPEG", IDI_FILETYPE_MPEG },
            { L".ogg",  L"HCPlayer.OGG.1",  L"Arquivo OGG",  IDI_FILETYPE_OGG },
            { L".opus", L"HCPlayer.OPUS.1", L"Arquivo OPUS", IDI_FILETYPE_OPUS },
            { L".wav",  L"HCPlayer.WAV.1",  L"Arquivo WAV",  IDI_FILETYPE_WAV },
            { L".webm", L"HCPlayer.WEBM.1", L"Arquivo WEBM", IDI_FILETYPE_WEBM },
        };

        struct RegistryKey
        {
            HKEY value{};

            RegistryKey() = default;
            RegistryKey(RegistryKey const&) = delete;
            RegistryKey& operator=(RegistryKey const&) = delete;

            ~RegistryKey()
            {
                if (value) RegCloseKey(value);
            }
        };

        bool CreateKey(HKEY root, std::wstring const& path, RegistryKey& key) noexcept
        {
            DWORD disposition{};
            return RegCreateKeyExW(
                root,
                path.c_str(),
                0,
                nullptr,
                REG_OPTION_NON_VOLATILE,
                KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY,
                nullptr,
                &key.value,
                &disposition) == ERROR_SUCCESS;
        }

        bool StringValueMatches(
            HKEY key,
            wchar_t const* valueName,
            std::wstring_view expected) noexcept
        {
            DWORD type{};
            DWORD byteCount{};
            LONG result = RegQueryValueExW(
                key, valueName, nullptr, &type, nullptr, &byteCount);
            if (result != ERROR_SUCCESS ||
                (type != REG_SZ && type != REG_EXPAND_SZ) ||
                byteCount < sizeof(wchar_t))
            {
                return false;
            }

            std::vector<wchar_t> buffer(
                (byteCount / sizeof(wchar_t)) + 1, L'\0');
            result = RegQueryValueExW(
                key,
                valueName,
                nullptr,
                &type,
                reinterpret_cast<BYTE*>(buffer.data()),
                &byteCount);
            if (result != ERROR_SUCCESS) return false;

            return std::wstring_view(buffer.data()) == expected;
        }

        bool SetStringIfNeeded(
            HKEY key,
            wchar_t const* valueName,
            std::wstring_view value,
            bool& changed) noexcept
        {
            if (StringValueMatches(key, valueName, value))
            {
                return true;
            }

            std::wstring const storedValue(value);
            DWORD const byteCount = static_cast<DWORD>(
                (storedValue.size() + 1) * sizeof(wchar_t));
            LONG const result = RegSetValueExW(
                key,
                valueName,
                0,
                REG_SZ,
                reinterpret_cast<BYTE const*>(storedValue.c_str()),
                byteCount);
            if (result == ERROR_SUCCESS) changed = true;
            return result == ERROR_SUCCESS;
        }

        std::wstring ExecutablePath()
        {
            std::vector<wchar_t> buffer(512);
            for (;;)
            {
                DWORD const length = GetModuleFileNameW(
                    nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length == 0) return {};

                if (length < buffer.size())
                {
                    return std::wstring(buffer.data(), length);
                }

                if (buffer.size() >= 32768) return {};
                buffer.resize((std::min)(buffer.size() * 2, std::size_t{ 32768 }));
            }
        }

        bool RegisterApplicationIdentity(
            std::wstring const& executablePath,
            bool& changed) noexcept
        {
            auto const separator = executablePath.find_last_of(L"\\/");
            std::wstring const executableName =
                separator == std::wstring::npos
                    ? executablePath
                    : executablePath.substr(separator + 1);
            if (executableName.empty()) return false;

            RegistryKey applicationKey;
            std::wstring const path =
                L"Software\\Classes\\Applications\\" + executableName;
            if (!CreateKey(HKEY_CURRENT_USER, path, applicationKey)) return false;

            // Controls the friendly name shown by Windows for this executable
            // without adding another file-association route.
            return SetStringIfNeeded(
                applicationKey.value,
                L"FriendlyAppName",
                ApplicationName,
                changed);
        }

        bool RegisterProgId(
            wchar_t const* progId,
            wchar_t const* description,
            int iconResourceId,
            std::wstring const& executablePath,
            std::wstring const& openCommand,
            bool& changed) noexcept
        {
            std::wstring const base = L"Software\\Classes\\" +
                std::wstring(progId);

            RegistryKey progIdKey;
            if (!CreateKey(HKEY_CURRENT_USER, base, progIdKey)) return false;
            if (!SetStringIfNeeded(
                progIdKey.value, nullptr, description, changed)) return false;

            RegistryKey iconKey;
            if (!CreateKey(
                HKEY_CURRENT_USER, base + L"\\DefaultIcon", iconKey))
            {
                return false;
            }
            std::wstring const icon = executablePath + L"," +
                std::to_wstring(-iconResourceId);
            if (!SetStringIfNeeded(
                iconKey.value, nullptr, icon, changed)) return false;

            RegistryKey commandKey;
            if (!CreateKey(
                HKEY_CURRENT_USER,
                base + L"\\shell\\open\\command",
                commandKey))
            {
                return false;
            }
            return SetStringIfNeeded(
                commandKey.value, nullptr, openCommand, changed);
        }

        bool RegisterOpenWith(
            wchar_t const* extension,
            wchar_t const* progId,
            bool& changed) noexcept
        {
            RegistryKey key;
            std::wstring const path = L"Software\\Classes\\" +
                std::wstring(extension) + L"\\OpenWithProgids";
            if (!CreateKey(HKEY_CURRENT_USER, path, key)) return false;
            return SetStringIfNeeded(key.value, progId, L"", changed);
        }

        bool RegisterCapabilities(bool& changed) noexcept
        {
            RegistryKey capabilitiesKey;
            if (!CreateKey(
                HKEY_CURRENT_USER, CapabilitiesPath, capabilitiesKey))
            {
                return false;
            }
            if (!SetStringIfNeeded(
                capabilitiesKey.value,
                L"ApplicationName",
                ApplicationName,
                changed) ||
                !SetStringIfNeeded(
                    capabilitiesKey.value,
                    L"ApplicationDescription",
                    L"HC Player media player",
                    changed))
            {
                return false;
            }

            RegistryKey associationsKey;
            if (!CreateKey(
                HKEY_CURRENT_USER,
                std::wstring(CapabilitiesPath) + L"\\FileAssociations",
                associationsKey))
            {
                return false;
            }

            for (auto const& fileType : FileTypes)
            {
                if (!SetStringIfNeeded(
                    associationsKey.value,
                    fileType.extension,
                    fileType.progId,
                    changed))
                {
                    return false;
                }
            }

            RegistryKey registeredApplicationsKey;
            if (!CreateKey(
                HKEY_CURRENT_USER,
                L"Software\\RegisteredApplications",
                registeredApplicationsKey))
            {
                return false;
            }
            return SetStringIfNeeded(
                registeredApplicationsKey.value,
                ApplicationName,
                CapabilitiesPath,
                changed);
        }
    }

    void EnsureRegistered() noexcept
    {
        try
        {
            std::wstring const executablePath = ExecutablePath();
            if (executablePath.empty()) return;

            std::wstring const openCommand =
                L"\"" + executablePath + L"\" \"%1\"";

            bool changed = false;
            bool complete = RegisterApplicationIdentity(executablePath, changed);

            for (auto const& fileType : FileTypes)
            {
                if (!RegisterProgId(
                    fileType.progId,
                    fileType.description,
                    fileType.iconResourceId,
                    executablePath,
                    openCommand,
                    changed))
                {
                    complete = false;
                }

                if (!RegisterOpenWith(
                    fileType.extension,
                    fileType.progId,
                    changed))
                {
                    complete = false;
                }
            }

            if (!RegisterCapabilities(changed))
            {
                complete = false;
            }

            if (complete && changed)
            {
                SHChangeNotify(
                    SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
            }
        }
        catch (...)
        {
            // Association registration is optional shell integration. It must
            // never prevent the player from starting or affect playback.
        }
    }
}
