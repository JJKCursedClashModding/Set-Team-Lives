// ASI entry point.
//
// Ultimate ASI Loader (and compatible loaders) LoadLibrary's *.asi files.  All
// work is done from a thread created in DllMain so nothing heavy runs under the
// loader lock; the thread waits (if needed) until the game image matches the
// expected prologue bytes, then installs the detours once.

#include "force_gauge.hpp"

#include <Windows.h>

namespace
{
    auto init_thread(LPVOID) -> DWORD
    {
        // The loader injects at process start.  If the exe is not ready yet
        // (e.g. a packed/decrypting image), retry until the prologue guards
        // match, then install.  120 x 250 ms = 30 s worst case.
        for (int attempt = 0; attempt < 120; ++attempt)
        {
            if (fgl::prologues_match())
            {
                fgl::install();
                return 0;
            }
            Sleep(250);
        }
        fgl::install(); // final attempt: logs the mismatch reason
        return 0;
    }
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        CreateThread(nullptr, 0, &init_thread, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        fgl::uninstall();
    }
    return TRUE;
}
