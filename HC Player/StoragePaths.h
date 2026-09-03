#pragma once

#include <filesystem>

namespace hc::storage
{
    // The portable variant is opt-in and deterministic: a file named
    // portable.flag beside HC Player.exe selects .\Data as the entire
    // application-owned data root. Without that flag, the installed build
    // keeps the existing %LOCALAPPDATA%\HC Player behavior.
    bool IsPortableMode() noexcept;
    std::filesystem::path ExecutableDirectory() noexcept;
    std::filesystem::path UserDataRoot();
}
