#pragma once

namespace fgl
{
    auto install() -> bool;
    auto uninstall() -> void;

    // True when the three hooked functions still carry the expected prologue bytes
    // (used by the ASI entry thread as a readiness check before installing).
    auto prologues_match() -> bool;
}
