#include "pch.h"
#include "ExternalToolsManager.h"
#include "StoragePaths.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    std::filesystem::path ImportedYtdlpStoragePath()
    {
        return hc::storage::UserDataRoot() / L"tools" / L"yt-dlp.exe";
    }

    std::filesystem::path ImportedDenoStoragePath()
    {
        return ImportedYtdlpStoragePath().parent_path() / L"deno.exe";
    }

    std::filesystem::path FindExecutableOnPath(wchar_t const* executableName)
    {
        std::vector<wchar_t> buffer(32768);
        DWORD length = SearchPathW(
            nullptr, executableName, nullptr,
            static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (length > 0 && length < buffer.size())
        {
            return std::filesystem::path{ buffer.data() };
        }
        return {};
    }

    bool IsPortableExecutable(std::filesystem::path const& path)
    {
        std::ifstream input(path, std::ios::binary);
        char signature[2]{};
        return input.read(signature, sizeof(signature)) &&
            signature[0] == 'M' && signature[1] == 'Z';
    }
}

namespace hc::tools
{
    std::filesystem::path Manager::ResolveYtdlpPath(bool* imported) const
    {
        auto local = ImportedYtdlpStoragePath();
        std::error_code error;
        if (std::filesystem::is_regular_file(local, error))
        {
            if (imported) *imported = true;
            return local;
        }
        if (imported) *imported = false;
        return FindExecutableOnPath(L"yt-dlp.exe");
    }

    void Manager::AddExecutableDirectoryToPath(
        std::filesystem::path const& executable) const
    {
        if (executable.empty()) return;
        std::wstring directory = executable.parent_path().wstring();
        if (directory.empty()) return;

        DWORD required = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        std::wstring current;
        if (required > 1)
        {
            current.resize(required);
            DWORD written = GetEnvironmentVariableW(
                L"PATH", current.data(), static_cast<DWORD>(current.size()));
            if (written < current.size()) current.resize(written);
        }

        // Keep the imported tool discoverable by child processes even on MPV
        // builds that do not honor ytdl_hook-ytdl_path consistently on Windows.
        std::wstring lowerPath = current;
        std::wstring lowerDirectory = directory;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), towlower);
        std::transform(lowerDirectory.begin(), lowerDirectory.end(),
            lowerDirectory.begin(), towlower);
        if (lowerPath.find(lowerDirectory) != std::wstring::npos) return;

        std::wstring updated = directory;
        if (!current.empty()) updated += L";" + current;
        SetEnvironmentVariableW(L"PATH", updated.c_str());
    }

    std::filesystem::path Manager::FindDenoCandidate(bool* imported) const
    {
        auto local = ImportedDenoStoragePath();
        std::error_code error;
        if (std::filesystem::is_regular_file(local, error))
        {
            if (imported) *imported = true;
            return local;
        }
        if (imported) *imported = false;
        return FindExecutableOnPath(L"deno.exe");
    }

    bool Manager::ProbeDenoExecutable(std::filesystem::path const& path) const
    {
        std::error_code fileError;
        auto writeTime = std::filesystem::last_write_time(path, fileError);
        if (fileError) return false;
        if (m_hasCachedDenoResult &&
            path == m_cachedDenoPath &&
            writeTime == m_cachedDenoWriteTime)
        {
            return m_cachedDenoResult;
        }

        bool valid{};
        HANDLE outputRead{};
        HANDLE outputWrite{};
        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        if (CreatePipe(&outputRead, &outputWrite, &security, 0))
        {
            SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW startup{ sizeof(STARTUPINFOW) };
            startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            startup.hStdOutput = outputWrite;
            startup.hStdError = outputWrite;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            PROCESS_INFORMATION process{};
            std::wstring commandLine = L"\"" + path.wstring() + L"\" --version";
            if (CreateProcessW(path.c_str(), commandLine.data(), nullptr, nullptr,
                TRUE, CREATE_NO_WINDOW, nullptr, path.parent_path().c_str(),
                &startup, &process))
            {
                CloseHandle(outputWrite);
                outputWrite = nullptr;
                DWORD wait = WaitForSingleObject(process.hProcess, 5000);
                if (wait == WAIT_TIMEOUT)
                {
                    TerminateProcess(process.hProcess, 1);
                    WaitForSingleObject(process.hProcess, 1000);
                }

                DWORD exitCode = 1;
                GetExitCodeProcess(process.hProcess, &exitCode);
                std::string output;
                char buffer[512];
                DWORD read{};
                while (ReadFile(outputRead, buffer, sizeof(buffer), &read, nullptr) && read)
                    output.append(buffer, read);
                std::transform(output.begin(), output.end(), output.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                valid = wait == WAIT_OBJECT_0 && exitCode == 0 &&
                    output.find("deno ") != std::string::npos;
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
            if (outputWrite) CloseHandle(outputWrite);
            CloseHandle(outputRead);
        }

        m_cachedDenoPath = path;
        m_cachedDenoWriteTime = writeTime;
        m_cachedDenoResult = valid;
        m_hasCachedDenoResult = true;
        return valid;
    }

    std::filesystem::path Manager::ResolveDenoPath(bool* imported) const
    {
        auto candidate = FindDenoCandidate(imported);
        return !candidate.empty() && ProbeDenoExecutable(candidate)
            ? candidate : std::filesystem::path{};
    }

    Status Manager::GetStatus() const
    {
        bool ytdlpImported{};
        bool denoImported{};
        auto ytdlpPath = ResolveYtdlpPath(&ytdlpImported);
        auto denoCandidate = FindDenoCandidate(&denoImported);
        bool denoValid = !denoCandidate.empty() && ProbeDenoExecutable(denoCandidate);

        Status status{};
        status.ytdlpAvailable = !ytdlpPath.empty();
        status.ytdlpImported = ytdlpImported;
        status.denoAvailable = denoValid;
        status.denoImported = denoImported;
        status.denoInvalid = !denoCandidate.empty() && !denoValid;
        status.ytdlpPath = ytdlpPath.wstring();
        status.denoPath = denoCandidate.wstring();
        status.ytdlpMessage = status.ytdlpAvailable
            ? (ytdlpImported ? L"Executável gerenciado pelo player"
                : L"Executável encontrado automaticamente no PATH")
            : L"yt-dlp não encontrado";
        return status;
    }

    bool Manager::ImportYtdlpBinary(
        std::wstring const& sourcePath,
        std::function<bool()> const& restartEngine,
        std::wstring& error) const
    {
        error.clear();
        std::filesystem::path source{ sourcePath };
        std::wstring extension = source.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        std::error_code fileError;
        if (extension != L".exe" ||
            !std::filesystem::is_regular_file(source, fileError) ||
            !IsPortableExecutable(source))
        {
            error = L"Selecione um executável válido do yt-dlp para Windows.";
            return false;
        }

        auto destination = ImportedYtdlpStoragePath();
        std::filesystem::create_directories(destination.parent_path(), fileError);
        if (fileError)
        {
            error = L"Não foi possível criar a pasta privada do yt-dlp.";
            return false;
        }

        bool sameFile = std::filesystem::equivalent(source, destination, fileError);
        fileError.clear();
        if (!sameFile)
        {
            std::filesystem::copy_file(source, destination,
                std::filesystem::copy_options::overwrite_existing, fileError);
            if (fileError)
            {
                error = L"Não foi possível copiar o executável selecionado.";
                return false;
            }
        }

        if (!restartEngine || !restartEngine())
        {
            error = L"O yt-dlp foi importado, mas o mecanismo de reprodução não pôde ser reiniciado.";
            return false;
        }
        return true;
    }

    bool Manager::ResetImportedYtdlp(
        std::function<bool()> const& restartEngine,
        std::wstring& error) const
    {
        error.clear();
        std::error_code fileError;
        auto path = ImportedYtdlpStoragePath();
        if (std::filesystem::exists(path, fileError))
        {
            std::filesystem::remove(path, fileError);
            if (fileError)
            {
                error = L"Não foi possível remover o executável importado.";
                return false;
            }
        }
        if (!restartEngine || !restartEngine())
        {
            error = L"O executável foi removido, mas o mecanismo de reprodução não pôde ser reiniciado.";
            return false;
        }
        return true;
    }

    bool Manager::ImportDenoBinary(
        std::wstring const& sourcePath,
        std::function<bool()> const& restartEngine,
        std::wstring& error) const
    {
        error.clear();
        std::filesystem::path source{ sourcePath };
        std::wstring extension = source.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        std::error_code fileError;
        if (extension != L".exe" ||
            !std::filesystem::is_regular_file(source, fileError) ||
            !IsPortableExecutable(source))
        {
            error = L"Selecione um executável válido do Deno para Windows.";
            return false;
        }
        if (!ProbeDenoExecutable(source))
        {
            error = L"O arquivo selecionado não é um runtime Deno funcional. Use o deno.exe oficial e confirme que 'deno --version' funciona.";
            return false;
        }

        auto destination = ImportedDenoStoragePath();
        std::filesystem::create_directories(destination.parent_path(), fileError);
        if (fileError)
        {
            error = L"Não foi possível criar a pasta privada do runtime JavaScript.";
            return false;
        }

        bool sameFile = std::filesystem::equivalent(source, destination, fileError);
        fileError.clear();
        if (!sameFile)
        {
            std::filesystem::copy_file(source, destination,
                std::filesystem::copy_options::overwrite_existing, fileError);
            if (fileError)
            {
                error = L"Não foi possível copiar o executável do Deno.";
                return false;
            }
        }

        if (!restartEngine || !restartEngine())
        {
            error = L"O Deno foi importado, mas o mecanismo de reprodução não pôde ser reiniciado.";
            return false;
        }
        return true;
    }

    bool Manager::ResetImportedDeno(
        std::function<bool()> const& restartEngine,
        std::wstring& error) const
    {
        error.clear();
        std::error_code fileError;
        auto path = ImportedDenoStoragePath();
        if (std::filesystem::exists(path, fileError))
        {
            std::filesystem::remove(path, fileError);
            if (fileError)
            {
                error = L"Não foi possível remover o runtime JavaScript importado.";
                return false;
            }
        }
        if (!restartEngine || !restartEngine())
        {
            error = L"O Deno foi removido, mas o mecanismo de reprodução não pôde ser reiniciado.";
            return false;
        }
        return true;
    }
}
