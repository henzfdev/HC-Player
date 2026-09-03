#include "pch.h"
#include "PortableRegistrationManager.h"
#include "Resource.h"
#include "StoragePaths.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace hc::portable_registration
{
    namespace
    {
        constexpr wchar_t ShortcutFolderName[] = L"HC Player Portable";
        constexpr wchar_t ShortcutFileName[] = L"HC Player.lnk";
        constexpr wchar_t ShortcutDescription[] =
            L"HC Player Portable shell integration - HCPlayer.Portable.v1";

        constexpr wchar_t RegistryRoot[] = L"Software\\HCPlayerPortable";
        constexpr wchar_t RegistryOwnerValue[] = L"RegistrationOwner";
        constexpr wchar_t RegistryExecutableValue[] = L"ExecutablePath";
        constexpr wchar_t RegistryOwnerMarker[] =
            L"HC Player Portable shell integration - HCPlayer.Portable.v1";
        constexpr wchar_t CapabilitiesPath[] =
            L"Software\\HCPlayerPortable\\Capabilities";
        constexpr wchar_t RegisteredApplicationName[] = L"HC Player Portable";
        constexpr wchar_t ApplicationName[] = L"HC Player Portable";

        struct FileTypeRegistration
        {
            wchar_t const* extension;
            wchar_t const* progId;
            wchar_t const* description;
            int iconResourceId;
        };

        constexpr FileTypeRegistration FileTypes[]
        {
            { L".mkv",  L"HCPlayerPortable.MKV.1",  L"Arquivo MKV",  IDI_FILETYPE_MKV },
            { L".aac",  L"HCPlayerPortable.AAC.1",  L"Arquivo AAC",  IDI_FILETYPE_AAC },
            { L".avi",  L"HCPlayerPortable.AVI.1",  L"Arquivo AVI",  IDI_FILETYPE_AVI },
            { L".flac", L"HCPlayerPortable.FLAC.1", L"Arquivo FLAC", IDI_FILETYPE_FLAC },
            { L".m3u8", L"HCPlayerPortable.M3U8.1", L"Arquivo M3U8", IDI_FILETYPE_M3U8 },
            { L".m4a",  L"HCPlayerPortable.M4A.1",  L"Arquivo M4A",  IDI_FILETYPE_M4A },
            { L".mka",  L"HCPlayerPortable.MKA.1",  L"Arquivo MKA",  IDI_FILETYPE_MKA },
            { L".mov",  L"HCPlayerPortable.MOV.1",  L"Arquivo MOV",  IDI_FILETYPE_MOV },
            { L".mp3",  L"HCPlayerPortable.MP3.1",  L"Arquivo MP3",  IDI_FILETYPE_MP3 },
            { L".mp4",  L"HCPlayerPortable.MP4.1",  L"Arquivo MP4",  IDI_FILETYPE_MP4 },
            { L".mpeg", L"HCPlayerPortable.MPEG.1", L"Arquivo MPEG", IDI_FILETYPE_MPEG },
            { L".ogg",  L"HCPlayerPortable.OGG.1",  L"Arquivo OGG",  IDI_FILETYPE_OGG },
            { L".opus", L"HCPlayerPortable.OPUS.1", L"Arquivo OPUS", IDI_FILETYPE_OPUS },
            { L".wav",  L"HCPlayerPortable.WAV.1",  L"Arquivo WAV",  IDI_FILETYPE_WAV },
            { L".webm", L"HCPlayerPortable.WEBM.1", L"Arquivo WEBM", IDI_FILETYPE_WEBM },
        };

        struct ComApartment
        {
            HRESULT result{ CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) };
            bool ownsInitialization{ result == S_OK || result == S_FALSE };

            ~ComApartment()
            {
                if (ownsInitialization) CoUninitialize();
            }

            bool usable() const noexcept
            {
                return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
            }
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

        enum class RegistryNamespaceState
        {
            Absent,
            Owned,
            Conflict,
            Error,
        };

        std::filesystem::path ExecutablePath() noexcept
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
                            std::wstring(buffer.data(), length) };
                    }

                    if (buffer.size() >= 32768) return {};
                    buffer.resize((std::min)(
                        buffer.size() * 2, std::size_t{ 32768 }));
                }
            }
            catch (...)
            {
                return {};
            }
        }

        std::filesystem::path ShortcutPath() noexcept
        {
            PWSTR programsRaw{};
            if (FAILED(SHGetKnownFolderPath(
                FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, &programsRaw)))
            {
                return {};
            }

            std::filesystem::path result;
            try
            {
                result = std::filesystem::path{ programsRaw } /
                    ShortcutFolderName / ShortcutFileName;
            }
            catch (...)
            {
                result.clear();
            }
            CoTaskMemFree(programsRaw);
            return result;
        }

        bool LoadShortcut(
            std::filesystem::path const& path,
            winrt::com_ptr<IShellLinkW>& link) noexcept
        {
            try
            {
                winrt::check_hresult(CoCreateInstance(
                    CLSID_ShellLink,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(link.put())));

                auto persist = link.as<IPersistFile>();
                return SUCCEEDED(persist->Load(path.c_str(), STGM_READ));
            }
            catch (...)
            {
                link = nullptr;
                return false;
            }
        }

        bool IsOwnedShortcut(std::filesystem::path const& path) noexcept
        {
            try
            {
                std::error_code error;
                if (!std::filesystem::is_regular_file(path, error))
                {
                    return false;
                }

                ComApartment apartment;
                if (!apartment.usable()) return false;

                winrt::com_ptr<IShellLinkW> link;
                if (!LoadShortcut(path, link)) return false;

                wchar_t description[256]{};
                if (FAILED(link->GetDescription(
                    description, static_cast<int>(ARRAYSIZE(description)))))
                {
                    return false;
                }

                return std::wstring_view(description) == ShortcutDescription;
            }
            catch (...)
            {
                return false;
            }
        }

        bool CreateOrUpdateShortcut(
            std::filesystem::path const& executable,
            std::filesystem::path const& shortcut) noexcept
        {
            try
            {
                std::error_code error;
                std::filesystem::create_directories(shortcut.parent_path(), error);
                if (error) return false;

                ComApartment apartment;
                if (!apartment.usable()) return false;

                winrt::com_ptr<IShellLinkW> link;
                winrt::check_hresult(CoCreateInstance(
                    CLSID_ShellLink,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(link.put())));

                auto const workingDirectory = executable.parent_path();
                winrt::check_hresult(link->SetPath(executable.c_str()));
                winrt::check_hresult(
                    link->SetWorkingDirectory(workingDirectory.c_str()));
                winrt::check_hresult(link->SetDescription(ShortcutDescription));
                winrt::check_hresult(link->SetIconLocation(executable.c_str(), 0));

                auto persist = link.as<IPersistFile>();
                winrt::check_hresult(persist->Save(shortcut.c_str(), TRUE));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool RemoveOwnedShortcut(std::filesystem::path const& shortcut) noexcept
        {
            try
            {
                std::error_code error;
                if (!std::filesystem::exists(shortcut, error))
                {
                    return !error;
                }
                if (error || !IsOwnedShortcut(shortcut)) return false;

                if (!std::filesystem::remove(shortcut, error) || error)
                {
                    return false;
                }

                // Remove only our private Start Menu folder, and only when empty.
                error.clear();
                std::filesystem::remove(shortcut.parent_path(), error);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool KeyExists(
            HKEY root,
            std::wstring const& path,
            bool& exists) noexcept
        {
            exists = false;
            HKEY key{};
            LONG const result = RegOpenKeyExW(
                root, path.c_str(), 0, KEY_QUERY_VALUE, &key);
            if (result == ERROR_SUCCESS)
            {
                RegCloseKey(key);
                exists = true;
                return true;
            }
            if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
            {
                return true;
            }
            return false;
        }

        bool CreateKey(
            HKEY root,
            std::wstring const& path,
            RegistryKey& key) noexcept
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

        bool ReadStringValue(
            HKEY root,
            std::wstring const& path,
            wchar_t const* valueName,
            std::wstring& value,
            bool& exists) noexcept
        {
            value.clear();
            exists = false;

            RegistryKey key;
            LONG const openResult = RegOpenKeyExW(
                root, path.c_str(), 0, KEY_QUERY_VALUE, &key.value);
            if (openResult == ERROR_FILE_NOT_FOUND ||
                openResult == ERROR_PATH_NOT_FOUND)
            {
                return true;
            }
            if (openResult != ERROR_SUCCESS) return false;

            DWORD type{};
            DWORD byteCount{};
            LONG result = RegQueryValueExW(
                key.value, valueName, nullptr, &type, nullptr, &byteCount);
            if (result == ERROR_FILE_NOT_FOUND)
            {
                return true;
            }
            if (result != ERROR_SUCCESS ||
                (type != REG_SZ && type != REG_EXPAND_SZ) ||
                byteCount < sizeof(wchar_t))
            {
                return false;
            }

            std::vector<wchar_t> buffer(
                (byteCount / sizeof(wchar_t)) + 1, L'\0');
            result = RegQueryValueExW(
                key.value,
                valueName,
                nullptr,
                &type,
                reinterpret_cast<BYTE*>(buffer.data()),
                &byteCount);
            if (result != ERROR_SUCCESS) return false;

            value.assign(buffer.data());
            exists = true;
            return true;
        }

        bool ValueExists(
            HKEY root,
            std::wstring const& path,
            wchar_t const* valueName,
            bool& exists) noexcept
        {
            exists = false;
            RegistryKey key;
            LONG const openResult = RegOpenKeyExW(
                root, path.c_str(), 0, KEY_QUERY_VALUE, &key.value);
            if (openResult == ERROR_FILE_NOT_FOUND ||
                openResult == ERROR_PATH_NOT_FOUND)
            {
                return true;
            }
            if (openResult != ERROR_SUCCESS) return false;

            LONG const result = RegQueryValueExW(
                key.value, valueName, nullptr, nullptr, nullptr, nullptr);
            if (result == ERROR_FILE_NOT_FOUND) return true;
            if (result != ERROR_SUCCESS) return false;

            exists = true;
            return true;
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

        RegistryNamespaceState InspectRegistryNamespace(
            std::wstring& previousExecutable) noexcept
        {
            previousExecutable.clear();

            bool rootExists{};
            if (!KeyExists(HKEY_CURRENT_USER, RegistryRoot, rootExists))
            {
                return RegistryNamespaceState::Error;
            }

            if (rootExists)
            {
                std::wstring owner;
                bool ownerExists{};
                if (!ReadStringValue(
                    HKEY_CURRENT_USER,
                    RegistryRoot,
                    RegistryOwnerValue,
                    owner,
                    ownerExists))
                {
                    return RegistryNamespaceState::Error;
                }
                if (!ownerExists || owner != RegistryOwnerMarker)
                {
                    return RegistryNamespaceState::Conflict;
                }

                bool executableExists{};
                if (!ReadStringValue(
                    HKEY_CURRENT_USER,
                    RegistryRoot,
                    RegistryExecutableValue,
                    previousExecutable,
                    executableExists))
                {
                    return RegistryNamespaceState::Error;
                }
                if (!executableExists) previousExecutable.clear();
                return RegistryNamespaceState::Owned;
            }

            // Without our ownership marker, never claim leftovers or another
            // program's data even if it happens to use our reserved names.
            for (auto const& fileType : FileTypes)
            {
                bool exists{};
                std::wstring const progIdPath =
                    L"Software\\Classes\\" + std::wstring(fileType.progId);
                if (!KeyExists(HKEY_CURRENT_USER, progIdPath, exists))
                {
                    return RegistryNamespaceState::Error;
                }
                if (exists) return RegistryNamespaceState::Conflict;

                std::wstring const openWithPath =
                    L"Software\\Classes\\" + std::wstring(fileType.extension) +
                    L"\\OpenWithProgids";
                if (!ValueExists(
                    HKEY_CURRENT_USER,
                    openWithPath,
                    fileType.progId,
                    exists))
                {
                    return RegistryNamespaceState::Error;
                }
                if (exists) return RegistryNamespaceState::Conflict;
            }

            bool registeredValueExists{};
            if (!ValueExists(
                HKEY_CURRENT_USER,
                L"Software\\RegisteredApplications",
                RegisteredApplicationName,
                registeredValueExists))
            {
                return RegistryNamespaceState::Error;
            }
            if (registeredValueExists)
            {
                return RegistryNamespaceState::Conflict;
            }

            return RegistryNamespaceState::Absent;
        }

        bool RegisterProgId(
            FileTypeRegistration const& fileType,
            std::wstring const& executablePath,
            std::wstring const& openCommand,
            bool& changed) noexcept
        {
            std::wstring const base =
                L"Software\\Classes\\" + std::wstring(fileType.progId);

            RegistryKey progIdKey;
            if (!CreateKey(HKEY_CURRENT_USER, base, progIdKey)) return false;
            if (!SetStringIfNeeded(
                progIdKey.value, nullptr, fileType.description, changed))
            {
                return false;
            }

            RegistryKey iconKey;
            if (!CreateKey(
                HKEY_CURRENT_USER, base + L"\\DefaultIcon", iconKey))
            {
                return false;
            }
            std::wstring const icon = executablePath + L"," +
                std::to_wstring(-fileType.iconResourceId);
            if (!SetStringIfNeeded(
                iconKey.value, nullptr, icon, changed))
            {
                return false;
            }

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
            FileTypeRegistration const& fileType,
            bool& changed) noexcept
        {
            RegistryKey key;
            std::wstring const path =
                L"Software\\Classes\\" + std::wstring(fileType.extension) +
                L"\\OpenWithProgids";
            if (!CreateKey(HKEY_CURRENT_USER, path, key)) return false;
            return SetStringIfNeeded(
                key.value, fileType.progId, L"", changed);
        }

        bool RegisterCapabilities(
            std::wstring const& executablePath,
            bool& changed) noexcept
        {
            RegistryKey capabilitiesKey;
            if (!CreateKey(
                HKEY_CURRENT_USER, CapabilitiesPath, capabilitiesKey))
            {
                return false;
            }

            std::wstring const applicationIcon = executablePath + L",0";
            if (!SetStringIfNeeded(
                capabilitiesKey.value,
                L"ApplicationName",
                ApplicationName,
                changed) ||
                !SetStringIfNeeded(
                    capabilitiesKey.value,
                    L"ApplicationDescription",
                    L"HC Player Portable media player",
                    changed) ||
                !SetStringIfNeeded(
                    capabilitiesKey.value,
                    L"ApplicationIcon",
                    applicationIcon,
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
                RegisteredApplicationName,
                CapabilitiesPath,
                changed);
        }

        bool ApplyRegistryRegistration(
            std::wstring const& executablePath,
            bool& changed,
            bool newNamespace = false) noexcept
        {
            RegistryKey ownerKey;
            if (!CreateKey(HKEY_CURRENT_USER, RegistryRoot, ownerKey))
            {
                return false;
            }
            if (!SetStringIfNeeded(
                ownerKey.value,
                RegistryOwnerValue,
                RegistryOwnerMarker,
                changed))
            {
                // The namespace was proven absent immediately before this call.
                // If even the ownership marker cannot be written, remove only
                // the root this invocation just created rather than leaving an
                // unowned private key behind.
                if (newNamespace)
                {
                    RegDeleteTreeW(HKEY_CURRENT_USER, RegistryRoot);
                }
                return false;
            }
            if (!SetStringIfNeeded(
                ownerKey.value,
                RegistryExecutableValue,
                executablePath,
                changed))
            {
                return false;
            }

            std::wstring const openCommand =
                L"\"" + executablePath + L"\" \"%1\"";

            for (auto const& fileType : FileTypes)
            {
                if (!RegisterProgId(
                    fileType, executablePath, openCommand, changed) ||
                    !RegisterOpenWith(fileType, changed))
                {
                    return false;
                }
            }

            return RegisterCapabilities(executablePath, changed);
        }

        bool DeleteTreeIfPresent(
            HKEY root,
            std::wstring const& path) noexcept
        {
            LONG const result = RegDeleteTreeW(root, path.c_str());
            return result == ERROR_SUCCESS ||
                result == ERROR_FILE_NOT_FOUND ||
                result == ERROR_PATH_NOT_FOUND;
        }

        bool DeleteValueIfPresent(
            HKEY root,
            std::wstring const& path,
            wchar_t const* valueName) noexcept
        {
            RegistryKey key;
            LONG const openResult = RegOpenKeyExW(
                root,
                path.c_str(),
                0,
                KEY_QUERY_VALUE | KEY_SET_VALUE,
                &key.value);
            if (openResult == ERROR_FILE_NOT_FOUND ||
                openResult == ERROR_PATH_NOT_FOUND)
            {
                return true;
            }
            if (openResult != ERROR_SUCCESS) return false;

            LONG const result = RegDeleteValueW(key.value, valueName);
            return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
        }

        bool DeleteRegisteredApplicationIfOwned() noexcept
        {
            RegistryKey key;
            LONG const openResult = RegOpenKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\RegisteredApplications",
                0,
                KEY_QUERY_VALUE | KEY_SET_VALUE,
                &key.value);
            if (openResult == ERROR_FILE_NOT_FOUND ||
                openResult == ERROR_PATH_NOT_FOUND)
            {
                return true;
            }
            if (openResult != ERROR_SUCCESS) return false;

            if (!StringValueMatches(
                key.value, RegisteredApplicationName, CapabilitiesPath))
            {
                LONG const queryResult = RegQueryValueExW(
                    key.value,
                    RegisteredApplicationName,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr);
                if (queryResult == ERROR_FILE_NOT_FOUND) return true;
                return false;
            }

            LONG const result = RegDeleteValueW(
                key.value, RegisteredApplicationName);
            return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
        }

        bool RemoveOwnedRegistryRegistration() noexcept
        {
            std::wstring previousExecutable;
            if (InspectRegistryNamespace(previousExecutable) !=
                RegistryNamespaceState::Owned)
            {
                return false;
            }

            bool complete = true;
            for (auto const& fileType : FileTypes)
            {
                std::wstring const openWithPath =
                    L"Software\\Classes\\" + std::wstring(fileType.extension) +
                    L"\\OpenWithProgids";
                if (!DeleteValueIfPresent(
                    HKEY_CURRENT_USER, openWithPath, fileType.progId))
                {
                    complete = false;
                }

                std::wstring const progIdPath =
                    L"Software\\Classes\\" + std::wstring(fileType.progId);
                if (!DeleteTreeIfPresent(HKEY_CURRENT_USER, progIdPath))
                {
                    complete = false;
                }
            }

            if (!DeleteRegisteredApplicationIfOwned())
            {
                complete = false;
            }

            // Keep the ownership marker when any cleanup step failed so a later
            // --unregister can safely retry. Never leave unowned residue on purpose.
            if (complete && !DeleteTreeIfPresent(HKEY_CURRENT_USER, RegistryRoot))
            {
                complete = false;
            }
            return complete;
        }

        void RollBackRegistryRegistration(
            RegistryNamespaceState previousState,
            std::wstring const& previousExecutable) noexcept
        {
            try
            {
                if (previousState == RegistryNamespaceState::Absent)
                {
                    RemoveOwnedRegistryRegistration();
                    return;
                }

                if (previousState == RegistryNamespaceState::Owned &&
                    !previousExecutable.empty())
                {
                    bool ignoredChanged{};
                    ApplyRegistryRegistration(previousExecutable, ignoredChanged, false);
                }
            }
            catch (...)
            {
            }
        }

        void NotifyShell() noexcept
        {
            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        }
    }

    bool Register(std::wstring& statusMessage) noexcept
    {
        statusMessage.clear();
        try
        {
            if (!hc::storage::IsPortableMode())
            {
                statusMessage =
                    L"--register is available only in HC Player Portable Mode.";
                return false;
            }

            auto const executable = ExecutablePath();
            auto const shortcut = ShortcutPath();
            if (executable.empty() || shortcut.empty())
            {
                statusMessage =
                    L"HC Player could not resolve the executable or Start Menu path.";
                return false;
            }

            std::error_code error;
            if (std::filesystem::exists(shortcut, error))
            {
                if (error || !IsOwnedShortcut(shortcut))
                {
                    statusMessage =
                        L"HC Player found an existing shortcut at its Portable registration path and left it untouched.";
                    return false;
                }
            }
            else if (error)
            {
                statusMessage =
                    L"HC Player could not inspect the Portable registration path.";
                return false;
            }

            std::wstring previousExecutable;
            RegistryNamespaceState const previousRegistryState =
                InspectRegistryNamespace(previousExecutable);
            if (previousRegistryState == RegistryNamespaceState::Conflict)
            {
                statusMessage =
                    L"HC Player found registry entries at its reserved Portable integration namespace but could not prove ownership. Nothing was changed.";
                return false;
            }
            if (previousRegistryState == RegistryNamespaceState::Error)
            {
                statusMessage =
                    L"HC Player could not safely inspect its Portable registry namespace. Nothing was changed.";
                return false;
            }

            bool registryChanged{};
            std::wstring const executableString = executable.wstring();
            if (!ApplyRegistryRegistration(
                executableString,
                registryChanged,
                previousRegistryState == RegistryNamespaceState::Absent))
            {
                RollBackRegistryRegistration(
                    previousRegistryState, previousExecutable);
                statusMessage =
                    L"HC Player Portable registration failed while creating its per-user file-type integration. Windows defaults were not changed.";
                return false;
            }

            if (!CreateOrUpdateShortcut(executable, shortcut))
            {
                RollBackRegistryRegistration(
                    previousRegistryState, previousExecutable);
                statusMessage =
                    L"HC Player could not create its Portable Start Menu shortcut. Registry changes were rolled back as far as safely possible.";
                return false;
            }

            NotifyShell();
            statusMessage =
                L"HC Player Portable registered successfully for the current user.\n"
                L"SMTC identity and HC Player Portable file-type icons are registered. Windows default-app choices were not changed.\n"
                L"If you move this Portable folder, run --register again from the new location.";
            return true;
        }
        catch (...)
        {
            statusMessage =
                L"HC Player Portable registration failed. No Windows defaults were changed.";
            return false;
        }
    }

    bool Unregister(std::wstring& statusMessage) noexcept
    {
        statusMessage.clear();
        try
        {
            if (!hc::storage::IsPortableMode())
            {
                statusMessage =
                    L"--unregister is available only in HC Player Portable Mode.";
                return false;
            }

            auto const shortcut = ShortcutPath();
            if (shortcut.empty())
            {
                statusMessage =
                    L"HC Player could not resolve the Portable registration path.";
                return false;
            }

            std::wstring previousExecutable;
            RegistryNamespaceState const registryState =
                InspectRegistryNamespace(previousExecutable);
            if (registryState == RegistryNamespaceState::Conflict)
            {
                statusMessage =
                    L"HC Player did not remove the registry entries because it could not prove ownership of the reserved Portable namespace.";
                return false;
            }
            if (registryState == RegistryNamespaceState::Error)
            {
                statusMessage =
                    L"HC Player could not safely inspect its Portable registry namespace.";
                return false;
            }

            bool registryRemoved = true;
            if (registryState == RegistryNamespaceState::Owned)
            {
                registryRemoved = RemoveOwnedRegistryRegistration();
            }

            bool const shortcutRemoved = RemoveOwnedShortcut(shortcut);
            if (!registryRemoved || !shortcutRemoved)
            {
                statusMessage =
                    L"HC Player Portable unregistration could not complete every owned cleanup step. Unrelated Windows entries were left untouched; run --unregister again to retry.";
                return false;
            }

            NotifyShell();
            statusMessage =
                L"HC Player Portable registration removed successfully for the current user.";
            return true;
        }
        catch (...)
        {
            statusMessage =
                L"HC Player Portable unregistration failed. Unrelated Windows entries were left untouched.";
            return false;
        }
    }
}
