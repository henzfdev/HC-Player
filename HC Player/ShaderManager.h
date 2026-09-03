#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hc::shaders
{
    struct ShaderInfo
    {
        std::wstring name;
        std::wstring path;
        bool enabled{};
    };

    struct Anime4KModeInfo
    {
        std::wstring mode;
        bool available{};
        std::vector<std::wstring> missingFiles;
    };

    struct Anime4KStatus
    {
        uint32_t detectedShaderCount{};
        bool anyActive{};
        bool customActive{};
        std::wstring activeProfile;
        std::wstring activeMode;
        std::vector<Anime4KModeInfo> fastModes;
        std::vector<Anime4KModeInfo> hqModes;
    };

    // Tiny non-owning adapter used by the manager to update mpv's
    // glsl-shaders list without depending on mpv headers or the player engine.
    // An empty adapter means the engine is not running; persistence operations
    // still succeed and will be applied when mpv starts later.
    struct RuntimeAccess
    {
        void* context{};
        int (*command)(void* context, const char* const* args){};

        explicit operator bool() const noexcept
        {
            return context != nullptr && command != nullptr;
        }
    };

    class Manager
    {
    public:
        void Load();

        std::vector<ShaderInfo> GetShaders() const;
        Anime4KStatus GetAnime4KStatus() const;

        // Startup is deliberately best-effort: a broken third-party shader
        // must never prevent the mpv engine itself from starting.
        void ApplyStartup(RuntimeAccess const& runtime) const noexcept;

        bool SetAnime4KMode(
            std::wstring const& profile,
            std::wstring const& mode,
            RuntimeAccess const& runtime,
            std::wstring& error);
        bool DisableAnime4K(
            RuntimeAccess const& runtime,
            std::wstring& error);
        bool DisableAllPreservingFiles(
            RuntimeAccess const& runtime,
            std::wstring& error);
        bool ImportShader(
            std::wstring const& sourcePath,
            RuntimeAccess const& runtime,
            std::wstring& error);
        bool SetShaderEnabled(
            std::wstring const& shaderPath,
            bool enabled,
            RuntimeAccess const& runtime,
            std::wstring& error);
        bool MoveShader(
            std::wstring const& shaderPath,
            int direction,
            RuntimeAccess const& runtime,
            std::wstring& error);
        bool RemoveShader(
            std::wstring const& shaderPath,
            RuntimeAccess const& runtime,
            std::wstring& error);
        bool RemoveAllShaders(
            RuntimeAccess const& runtime,
            std::wstring& error);

    private:
        struct Entry
        {
            std::wstring fileName;
            bool enabled{};
        };

        bool Save() const;
        bool RefreshRuntime(
            RuntimeAccess const& runtime,
            std::wstring& error) const;

        std::vector<Entry> m_entries;
    };
}
