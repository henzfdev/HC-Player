#include "pch.h"
#include "ShaderManager.h"
#include "StoragePaths.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace
{
    std::filesystem::path ManagedShadersDirectory()
    {
        return hc::storage::UserDataRoot() / L"shaders";
    }

    std::filesystem::path ManagedShadersRegistryPath()
    {
        return ManagedShadersDirectory() / L"shaders.dat";
    }

    std::filesystem::path ManagedShaderPath(std::wstring const& fileName)
    {
        return ManagedShadersDirectory() / fileName;
    }

    bool IsManagedShaderFile(std::filesystem::path const& path)
    {
        std::wstring extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(),
            extension.begin(), towlower);
        return extension == L".glsl";
    }

    bool IsSafeManagedShaderFileName(std::wstring const& fileName)
    {
        if (fileName.empty())
        {
            return false;
        }

        std::filesystem::path candidate{ fileName };
        // Registry entries are filenames only. Never let a manually edited
        // shaders.dat escape the private shader directory via .. or roots.
        return !candidate.has_root_path() &&
            candidate.parent_path().empty() &&
            candidate.filename().wstring() == fileName;
    }

    bool EqualsInsensitive(
        std::wstring const& left,
        std::wstring const& right)
    {
        return _wcsicmp(left.c_str(), right.c_str()) == 0;
    }

    bool StartsWithInsensitive(
        std::wstring const& value,
        std::wstring const& prefix)
    {
        return value.size() >= prefix.size() &&
            _wcsnicmp(value.c_str(), prefix.c_str(), prefix.size()) == 0;
    }

    bool IsAnime4KShaderName(std::wstring const& fileName)
    {
        return StartsWithInsensitive(fileName, L"Anime4K_") &&
            IsManagedShaderFile(std::filesystem::path{ fileName });
    }

    std::vector<std::wstring> Anime4KModes()
    {
        return { L"A", L"B", L"C", L"A+A", L"B+B", L"C+A" };
    }

    std::vector<std::wstring> Anime4KRecipeFiles(
        std::wstring const& profile,
        std::wstring const& mode)
    {
        bool const hq = EqualsInsensitive(profile, L"hq");
        bool const fast = EqualsInsensitive(profile, L"fast");
        if (!hq && !fast)
        {
            return {};
        }

        std::wstring const primary = hq ? L"VL" : L"M";
        std::wstring const secondary = hq ? L"M" : L"S";

        auto restore = [](std::wstring const& size)
        {
            return L"Anime4K_Restore_CNN_" + size + L".glsl";
        };
        auto restoreSoft = [](std::wstring const& size)
        {
            return L"Anime4K_Restore_CNN_Soft_" + size + L".glsl";
        };
        auto upscale = [](std::wstring const& size)
        {
            return L"Anime4K_Upscale_CNN_x2_" + size + L".glsl";
        };
        auto upscaleDenoise = [](std::wstring const& size)
        {
            return L"Anime4K_Upscale_Denoise_CNN_x2_" + size + L".glsl";
        };

        std::vector<std::wstring> files{
            L"Anime4K_Clamp_Highlights.glsl"
        };

        if (EqualsInsensitive(mode, L"A"))
        {
            files.push_back(restore(primary));
            files.push_back(upscale(primary));
            files.push_back(L"Anime4K_AutoDownscalePre_x2.glsl");
            files.push_back(L"Anime4K_AutoDownscalePre_x4.glsl");
            files.push_back(upscale(secondary));
        }
        else if (EqualsInsensitive(mode, L"B"))
        {
            files.push_back(restoreSoft(primary));
            files.push_back(upscale(primary));
            files.push_back(L"Anime4K_AutoDownscalePre_x2.glsl");
            files.push_back(L"Anime4K_AutoDownscalePre_x4.glsl");
            files.push_back(upscale(secondary));
        }
        else if (EqualsInsensitive(mode, L"C"))
        {
            files.push_back(upscaleDenoise(primary));
            files.push_back(L"Anime4K_AutoDownscalePre_x2.glsl");
            files.push_back(L"Anime4K_AutoDownscalePre_x4.glsl");
            files.push_back(upscale(secondary));
        }
        else if (EqualsInsensitive(mode, L"A+A"))
        {
            files.push_back(restore(primary));
            files.push_back(upscale(primary));
            files.push_back(restore(secondary));
            files.push_back(L"Anime4K_AutoDownscalePre_x2.glsl");
            files.push_back(L"Anime4K_AutoDownscalePre_x4.glsl");
            files.push_back(upscale(secondary));
        }
        else if (EqualsInsensitive(mode, L"B+B"))
        {
            files.push_back(restoreSoft(primary));
            files.push_back(upscale(primary));
            files.push_back(L"Anime4K_AutoDownscalePre_x2.glsl");
            files.push_back(L"Anime4K_AutoDownscalePre_x4.glsl");
            files.push_back(restoreSoft(secondary));
            files.push_back(upscale(secondary));
        }
        else if (EqualsInsensitive(mode, L"C+A"))
        {
            files.push_back(upscaleDenoise(primary));
            files.push_back(L"Anime4K_AutoDownscalePre_x2.glsl");
            files.push_back(L"Anime4K_AutoDownscalePre_x4.glsl");
            files.push_back(restore(secondary));
            files.push_back(upscale(secondary));
        }
        else
        {
            return {};
        }

        return files;
    }

    int ChangeList(
        hc::shaders::RuntimeAccess const& runtime,
        char const* action,
        std::filesystem::path const& path)
    {
        if (!runtime)
        {
            return 0;
        }

        std::string utf8Path = winrt::to_string(path.generic_wstring());
        const char* args[] = {
            "change-list", "glsl-shaders", action,
            utf8Path.c_str(), nullptr
        };
        return runtime.command(runtime.context, args);
    }
}

namespace hc::shaders
{
    bool Manager::Save() const
    {
        auto directory = ManagedShadersDirectory();
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            return false;
        }

        std::ofstream output(
            ManagedShadersRegistryPath(),
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return false;
        }

        for (auto const& entry : m_entries)
        {
            output << std::quoted(winrt::to_string(entry.fileName))
                << ' ' << (entry.enabled ? 1 : 0) << '\n';
        }
        return output.good();
    }

    void Manager::Load()
    {
        m_entries.clear();

        std::ifstream input(ManagedShadersRegistryPath(), std::ios::binary);
        std::string fileName;
        int enabled{};
        bool cleaned{};

        while (input >> std::quoted(fileName) >> enabled)
        {
            Entry entry{
                winrt::to_hstring(fileName).c_str(),
                enabled != 0
            };

            auto path = ManagedShaderPath(entry.fileName);
            std::error_code error;
            if (!IsSafeManagedShaderFileName(entry.fileName) ||
                !IsManagedShaderFile(path) ||
                !std::filesystem::is_regular_file(path, error))
            {
                cleaned = true;
                continue;
            }

            m_entries.push_back(std::move(entry));
        }

        if (cleaned)
        {
            Save();
        }
    }

    std::vector<ShaderInfo> Manager::GetShaders() const
    {
        std::vector<ShaderInfo> shaders;
        shaders.reserve(m_entries.size());
        for (auto const& entry : m_entries)
        {
            auto path = ManagedShaderPath(entry.fileName);
            shaders.push_back({
                entry.fileName,
                path.wstring(),
                entry.enabled
            });
        }
        return shaders;
    }

    Anime4KStatus Manager::GetAnime4KStatus() const
    {
        Anime4KStatus status{};

        for (auto const& entry : m_entries)
        {
            if (IsAnime4KShaderName(entry.fileName))
            {
                ++status.detectedShaderCount;
                if (entry.enabled)
                {
                    status.anyActive = true;
                }
            }
        }

        auto findByFileName = [this](std::wstring const& fileName)
        {
            return std::find_if(
                m_entries.begin(), m_entries.end(),
                [&fileName](Entry const& entry)
                {
                    return EqualsInsensitive(entry.fileName, fileName);
                });
        };

        auto recipeAvailable = [&](
            std::wstring const& profile,
            std::wstring const& mode,
            std::vector<std::wstring>* missingFiles)
        {
            auto recipe = Anime4KRecipeFiles(profile, mode);
            if (recipe.empty())
            {
                return false;
            }

            bool available = true;
            if (missingFiles)
            {
                missingFiles->clear();
            }
            for (auto const& fileName : recipe)
            {
                if (findByFileName(fileName) == m_entries.end())
                {
                    available = false;
                    if (missingFiles)
                    {
                        missingFiles->push_back(fileName);
                    }
                }
            }
            return available;
        };

        auto recipeIsActive = [&](
            std::wstring const& profile,
            std::wstring const& mode)
        {
            auto recipe = Anime4KRecipeFiles(profile, mode);
            if (recipe.empty())
            {
                return false;
            }

            std::vector<std::wstring> active;
            for (auto const& entry : m_entries)
            {
                if (entry.enabled && IsAnime4KShaderName(entry.fileName))
                {
                    active.push_back(entry.fileName);
                }
            }
            if (active.size() != recipe.size())
            {
                return false;
            }
            for (size_t index = 0; index < recipe.size(); ++index)
            {
                if (!EqualsInsensitive(active[index], recipe[index]))
                {
                    return false;
                }
            }
            return true;
        };

        auto buildModes = [&](std::wstring const& profile)
        {
            std::vector<Anime4KModeInfo> result;
            for (auto const& mode : Anime4KModes())
            {
                Anime4KModeInfo info{};
                info.mode = mode;
                info.available = recipeAvailable(
                    profile, mode, &info.missingFiles);
                result.push_back(std::move(info));
            }
            return result;
        };

        status.fastModes = buildModes(L"fast");
        status.hqModes = buildModes(L"hq");

        for (auto const& profile : {
            std::wstring{ L"fast" }, std::wstring{ L"hq" } })
        {
            for (auto const& mode : Anime4KModes())
            {
                if (recipeIsActive(profile, mode))
                {
                    status.activeProfile = profile;
                    status.activeMode = mode;
                    return status;
                }
            }
        }

        status.customActive = status.anyActive;
        return status;
    }

    void Manager::ApplyStartup(RuntimeAccess const& runtime) const noexcept
    {
        if (!runtime)
        {
            return;
        }

        // Remove only paths owned by HC Player, then append the currently
        // enabled ones in persisted order. Return values are intentionally
        // ignored here so a bad shader cannot make engine startup fail.
        for (auto const& entry : m_entries)
        {
            ChangeList(runtime, "remove", ManagedShaderPath(entry.fileName));
        }
        for (auto const& entry : m_entries)
        {
            if (entry.enabled)
            {
                ChangeList(runtime, "append", ManagedShaderPath(entry.fileName));
            }
        }
    }

    bool Manager::RefreshRuntime(
        RuntimeAccess const& runtime,
        std::wstring& error) const
    {
        error.clear();
        if (!runtime)
        {
            return true;
        }

        // Remove only paths owned by HC Player. This keeps glsl-shaders that
        // came from an imported mpv.conf intact.
        for (auto const& entry : m_entries)
        {
            if (ChangeList(
                runtime, "remove", ManagedShaderPath(entry.fileName)) < 0)
            {
                error = L"O mecanismo de reprodução não pôde atualizar a lista de shaders.";
                return false;
            }
        }

        for (auto const& entry : m_entries)
        {
            if (!entry.enabled)
            {
                continue;
            }

            auto shaderPath = ManagedShaderPath(entry.fileName);
            std::error_code fileError;
            if (!std::filesystem::is_regular_file(shaderPath, fileError))
            {
                error = L"O arquivo do shader não foi encontrado: " +
                    entry.fileName;
                return false;
            }

            if (ChangeList(runtime, "append", shaderPath) < 0)
            {
                error = L"O mecanismo de reprodução rejeitou o shader: " +
                    entry.fileName;
                return false;
            }
        }

        return true;
    }

    bool Manager::SetAnime4KMode(
        std::wstring const& profile,
        std::wstring const& mode,
        RuntimeAccess const& runtime,
        std::wstring& error)
    {
        error.clear();
        auto recipe = Anime4KRecipeFiles(profile, mode);
        if (recipe.empty())
        {
            error = L"Perfil ou modo Anime4K inválido.";
            return false;
        }

        auto findByFileName = [this](std::wstring const& fileName)
        {
            return std::find_if(
                m_entries.begin(), m_entries.end(),
                [&fileName](Entry const& entry)
                {
                    return EqualsInsensitive(entry.fileName, fileName);
                });
        };

        std::vector<std::wstring> missing;
        for (auto const& fileName : recipe)
        {
            if (findByFileName(fileName) == m_entries.end())
            {
                missing.push_back(fileName);
            }
        }
        if (!missing.empty())
        {
            std::wostringstream message;
            message << L"Este modo não pode ser ativado porque faltam "
                << missing.size() << L" shader(s)";
            message << L". Primeiro arquivo ausente: " << missing.front();
            error = message.str();
            return false;
        }

        auto previous = m_entries;
        std::vector<Entry> reordered;
        reordered.reserve(m_entries.size());

        // Unrelated custom shaders keep their relative order and enabled state.
        for (auto const& entry : m_entries)
        {
            if (!IsAnime4KShaderName(entry.fileName))
            {
                reordered.push_back(entry);
            }
        }

        // Keep every imported Anime4K file in the registry, but disable files
        // that are not part of the selected official recipe.
        for (auto const& entry : m_entries)
        {
            if (!IsAnime4KShaderName(entry.fileName))
            {
                continue;
            }

            bool used = std::any_of(
                recipe.begin(), recipe.end(),
                [&entry](std::wstring const& fileName)
                {
                    return EqualsInsensitive(entry.fileName, fileName);
                });
            if (!used)
            {
                auto disabled = entry;
                disabled.enabled = false;
                reordered.push_back(std::move(disabled));
            }
        }

        // The selected recipe is appended in the exact order documented by
        // Anime4K v4.x. mpv processes glsl-shaders in list order.
        for (auto const& fileName : recipe)
        {
            auto found = findByFileName(fileName);
            if (found == m_entries.end())
            {
                m_entries = std::move(previous);
                error = L"O shader necessário deixou de estar disponível.";
                return false;
            }

            auto enabled = *found;
            enabled.enabled = true;
            reordered.push_back(std::move(enabled));
        }

        m_entries = std::move(reordered);
        if (!Save())
        {
            m_entries = std::move(previous);
            error = L"Não foi possível salvar o modo Anime4K selecionado.";
            return false;
        }

        if (!RefreshRuntime(runtime, error))
        {
            m_entries = std::move(previous);
            Save();
            std::wstring ignored;
            RefreshRuntime(runtime, ignored);
            return false;
        }

        return true;
    }

    bool Manager::DisableAnime4K(
        RuntimeAccess const& runtime,
        std::wstring& error)
    {
        error.clear();
        auto previous = m_entries;
        bool changed{};

        for (auto& entry : m_entries)
        {
            if (IsAnime4KShaderName(entry.fileName) && entry.enabled)
            {
                entry.enabled = false;
                changed = true;
            }
        }

        if (!changed)
        {
            return true;
        }

        if (!Save())
        {
            m_entries = std::move(previous);
            error = L"Não foi possível salvar o estado do Anime4K.";
            return false;
        }

        if (!RefreshRuntime(runtime, error))
        {
            m_entries = std::move(previous);
            Save();
            std::wstring ignored;
            RefreshRuntime(runtime, ignored);
            return false;
        }

        return true;
    }

    bool Manager::DisableAllPreservingFiles(
        RuntimeAccess const& runtime,
        std::wstring& error)
    {
        error.clear();
        auto previous = m_entries;
        bool changed{};

        for (auto& entry : m_entries)
        {
            if (entry.enabled)
            {
                entry.enabled = false;
                changed = true;
            }
        }

        if (!changed)
        {
            return true;
        }

        if (!Save())
        {
            m_entries = std::move(previous);
            error = L"Não foi possível salvar o estado padrão dos shaders.";
            return false;
        }

        if (!RefreshRuntime(runtime, error))
        {
            m_entries = std::move(previous);
            Save();
            std::wstring ignored;
            RefreshRuntime(runtime, ignored);
            return false;
        }

        return true;
    }

    bool Manager::ImportShader(
        std::wstring const& sourcePath,
        RuntimeAccess const& runtime,
        std::wstring& error)
    {
        error.clear();
        std::filesystem::path source{ sourcePath };
        std::error_code fileError;
        if (!std::filesystem::is_regular_file(source, fileError) ||
            !IsManagedShaderFile(source))
        {
            error = L"Selecione um arquivo de shader GLSL válido (.glsl).";
            return false;
        }

        auto directory = ManagedShadersDirectory();
        std::filesystem::create_directories(directory, fileError);
        if (fileError)
        {
            error = L"Não foi possível criar a pasta privada de shaders do HC Player.";
            return false;
        }

        // Preserve the original filename. Anime4K v4.x recipes are defined by
        // exact shader filenames, and re-importing one of those files should
        // update the managed copy instead of creating unusable aliases.
        std::filesystem::path destination = directory / source.filename();
        auto existing = std::find_if(
            m_entries.begin(), m_entries.end(),
            [&destination](Entry const& entry)
            {
                return EqualsInsensitive(
                    entry.fileName, destination.filename().wstring());
            });

        bool sameFile = false;
        std::error_code sourceCanonicalError;
        std::error_code destinationCanonicalError;
        auto sourceCanonical = std::filesystem::weakly_canonical(
            source, sourceCanonicalError);
        auto destinationCanonical = std::filesystem::weakly_canonical(
            destination, destinationCanonicalError);
        if (!sourceCanonicalError && !destinationCanonicalError)
        {
            sameFile = _wcsicmp(
                sourceCanonical.c_str(),
                destinationCanonical.c_str()) == 0;
        }

        if (!sameFile)
        {
            fileError.clear();
            std::filesystem::copy_file(
                source, destination,
                std::filesystem::copy_options::overwrite_existing,
                fileError);
            if (fileError)
            {
                error = L"Não foi possível copiar o shader para a pasta privada do HC Player.";
                return false;
            }
        }

        if (existing != m_entries.end())
        {
            // Re-import is conservative. If this file participates in an active
            // Anime4K chain, disable the whole chain rather than leave a partial
            // official recipe running after one member was replaced.
            auto previous = m_entries;
            bool runtimeChanged{};
            if (existing->enabled &&
                IsAnime4KShaderName(existing->fileName))
            {
                for (auto& entry : m_entries)
                {
                    if (IsAnime4KShaderName(entry.fileName) && entry.enabled)
                    {
                        entry.enabled = false;
                        runtimeChanged = true;
                    }
                }
            }
            else
            {
                runtimeChanged = existing->enabled;
                existing->enabled = false;
            }

            if (!Save())
            {
                m_entries = std::move(previous);
                error = L"O shader foi atualizado, mas a configuração não pôde ser salva.";
                return false;
            }

            if (runtimeChanged)
            {
                std::wstring runtimeError;
                if (!RefreshRuntime(runtime, runtimeError))
                {
                    error = runtimeError;
                    return false;
                }
            }
            return true;
        }

        // Imported shaders start disabled by design. Merely importing a file
        // must never alter image quality or GPU cost until explicitly enabled.
        m_entries.push_back({
            destination.filename().wstring(),
            false
        });

        if (!Save())
        {
            m_entries.pop_back();
            std::filesystem::remove(destination, fileError);
            error = L"O shader foi copiado, mas a configuração não pôde ser salva.";
            return false;
        }

        return true;
    }

    bool Manager::SetShaderEnabled(
        std::wstring const& shaderPath,
        bool enabled,
        RuntimeAccess const& runtime,
        std::wstring& error)
    {
        error.clear();
        auto found = std::find_if(
            m_entries.begin(), m_entries.end(),
            [&shaderPath](Entry const& entry)
            {
                return _wcsicmp(
                    ManagedShaderPath(entry.fileName).c_str(),
                    shaderPath.c_str()) == 0;
            });
        if (found == m_entries.end())
        {
            error = L"O shader não está mais disponível na lista.";
            return false;
        }

        bool previous = found->enabled;
        found->enabled = enabled;
        if (!Save())
        {
            found->enabled = previous;
            error = L"Não foi possível salvar o estado do shader.";
            return false;
        }

        if (!RefreshRuntime(runtime, error))
        {
            found->enabled = previous;
            Save();
            std::wstring ignored;
            RefreshRuntime(runtime, ignored);
            return false;
        }

        return true;
    }

    bool Manager::MoveShader(
        std::wstring const& shaderPath,
        int direction,
        RuntimeAccess const& runtime,
        std::wstring& error)
    {
        error.clear();
        if (direction == 0)
        {
            return true;
        }

        auto found = std::find_if(
            m_entries.begin(), m_entries.end(),
            [&shaderPath](Entry const& entry)
            {
                return _wcsicmp(
                    ManagedShaderPath(entry.fileName).c_str(),
                    shaderPath.c_str()) == 0;
            });
        if (found == m_entries.end())
        {
            error = L"O shader não está mais disponível na lista.";
            return false;
        }

        size_t index = static_cast<size_t>(
            std::distance(m_entries.begin(), found));
        if ((direction < 0 && index == 0) ||
            (direction > 0 && index + 1 >= m_entries.size()))
        {
            return true;
        }

        size_t target = direction < 0 ? index - 1 : index + 1;
        std::swap(m_entries[index], m_entries[target]);

        if (!Save())
        {
            std::swap(m_entries[index], m_entries[target]);
            error = L"Não foi possível salvar a nova ordem dos shaders.";
            return false;
        }

        if (!RefreshRuntime(runtime, error))
        {
            std::swap(m_entries[index], m_entries[target]);
            Save();
            std::wstring ignored;
            RefreshRuntime(runtime, ignored);
            return false;
        }

        return true;
    }

    bool Manager::RemoveShader(
        std::wstring const& shaderPath,
        RuntimeAccess const& runtime,
        std::wstring& error)
    {
        error.clear();
        auto found = std::find_if(
            m_entries.begin(), m_entries.end(),
            [&shaderPath](Entry const& entry)
            {
                return _wcsicmp(
                    ManagedShaderPath(entry.fileName).c_str(),
                    shaderPath.c_str()) == 0;
            });
        if (found == m_entries.end())
        {
            error = L"O shader não está mais disponível na lista.";
            return false;
        }

        auto previous = m_entries;
        std::filesystem::path file = ManagedShaderPath(found->fileName);

        // Remove the shader that is about to leave the registry before erasing
        // its identity; RefreshRuntime() only knows entries that remain.
        if (runtime && ChangeList(runtime, "remove", file) < 0)
        {
            error = L"O mecanismo de reprodução não pôde remover o shader ativo.";
            return false;
        }

        m_entries.erase(found);

        if (!Save())
        {
            m_entries = std::move(previous);
            std::wstring ignored;
            RefreshRuntime(runtime, ignored);
            error = L"Não foi possível salvar a remoção do shader.";
            return false;
        }

        if (!RefreshRuntime(runtime, error))
        {
            m_entries = std::move(previous);
            Save();
            std::wstring ignored;
            RefreshRuntime(runtime, ignored);
            return false;
        }

        std::error_code fileError;
        std::filesystem::remove(file, fileError);
        if (fileError)
        {
            // Runtime/configuration are already clean. An orphaned private file
            // is preferable to restoring a shader the user explicitly removed.
            error = L"O shader foi removido da lista, mas o arquivo não pôde ser apagado.";
        }

        return true;
    }

    bool Manager::RemoveAllShaders(
        RuntimeAccess const& runtime,
        std::wstring& error)
    {
        error.clear();
        if (m_entries.empty())
        {
            return true;
        }

        auto previous = m_entries;

        // Remove only HC Player-owned paths from mpv. External glsl-shaders
        // that came from mpv.conf are never cleared or replaced.
        if (runtime)
        {
            for (auto const& entry : previous)
            {
                if (ChangeList(
                    runtime, "remove", ManagedShaderPath(entry.fileName)) < 0)
                {
                    std::wstring ignored;
                    RefreshRuntime(runtime, ignored);
                    error = L"O mecanismo de reprodução não pôde remover todos os shaders gerenciados.";
                    return false;
                }
            }
        }

        m_entries.clear();
        if (!Save())
        {
            m_entries = previous;
            Save();
            std::wstring ignored;
            RefreshRuntime(runtime, ignored);
            error = L"Não foi possível salvar a limpeza da lista de shaders.";
            return false;
        }

        size_t filesNotRemoved{};
        for (auto const& entry : previous)
        {
            std::error_code fileError;
            auto path = ManagedShaderPath(entry.fileName);
            std::filesystem::remove(path, fileError);
            std::error_code existsError;
            if (fileError && std::filesystem::exists(path, existsError))
            {
                ++filesNotRemoved;
            }
        }

        // An empty registry is already a valid committed state. Remove it and
        // the directory only as final housekeeping, matching pre-import state.
        std::error_code cleanupError;
        std::filesystem::remove(ManagedShadersRegistryPath(), cleanupError);
        cleanupError.clear();
        std::filesystem::remove(ManagedShadersDirectory(), cleanupError);

        if (filesNotRemoved > 0)
        {
            std::wostringstream message;
            message << L"A lista e o pipeline foram limpos, mas "
                << filesNotRemoved
                << L" arquivo(s) privado(s) não puderam ser apagados do disco.";
            error = message.str();
        }

        return true;
    }
}
