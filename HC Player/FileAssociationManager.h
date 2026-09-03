#pragma once

namespace hc::file_associations
{
    // Best-effort, per-user registration. This never changes Windows UserChoice
    // and therefore never forces HC Player to become the default application.
    void EnsureRegistered() noexcept;
}
