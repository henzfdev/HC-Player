#pragma once

#include <string>

namespace hc::portable_registration
{
    // Explicit, per-user Portable integration. --register manages HC Player
    // Portable's private Start Menu shortcut plus private HCPlayerPortable.*
    // ProgIDs/capabilities for format icons. It never touches UserChoice, HKLM,
    // HCPlayer.* installed ProgIDs, or the installed HC Player registration.
    // --unregister removes only entries whose ownership can be proven.
    bool Register(std::wstring& statusMessage) noexcept;
    bool Unregister(std::wstring& statusMessage) noexcept;
}
