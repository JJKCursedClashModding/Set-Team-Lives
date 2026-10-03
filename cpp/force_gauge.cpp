// SetTeamLives - standalone ASI plugin: raise the Team Gauge ("lives") in
// selected battle modes.
//
// This is the ASI (loader-injected) variant of the UE4SS mod.  It has no UE4SS
// dependency: the loader LoadLibrary's SetTeamLives.asi, DllMain spawns a
// thread which installs two inline detours, and everything else is plain Win32.
//
// Data files live flat next to the .asi itself (no subfolder):
// gauge_lives.ini (auto-written with defaults when missing) and force_gauge.log.
//
// Config (gauge_lives.ini, [Gauge]): Enabled=1, Lives=6, Modes=Free,PvP.
// F8 toggles at runtime.
//
// Static reverse-engineering summary (see lives-team-gauge-findings.md sections 6/9):
//
//   AbSingleton (getter FUN_141017BD0, RVA 0x1017BD0)
//     +0x1C0 -> ForceGauge manager object
//
//   Manager maps (keyed by team byte, 0 = player team, 1 = rival team):
//     +0x8A8  current value   (GetValue  FUN_1410176D0, RVA 0x10176D0)
//     +0x8F8  max value       (GetMax    FUN_141017790, RVA 0x1017790, default 4)
//     +0x948  barrier/extra   (SetExtra  FUN_14102AD10, RVA 0x102AD10)
//     +0x998  infinity flag   (SetInf    FUN_14102AE50, RVA 0x102AE50)
//
//   SetMax     FUN_14102ADB0  RVA 0x102ADB0  (manager, key, max)
//   SetCurrent FUN_14102AB90  RVA 0x102AB90  (manager, key, value, force)
//   Setup      FUN_1413B0970  RVA 0x13B0970  (phase object in RCX)
//       - the gauge initialiser.  It is virtual slot 106 of the battle phase
//         class, so the object passed to it is a UGamePhase_Battle* whose vtable
//         identifies the game mode exactly.
//
//   Mode -> phase class -> vtable (RVA, statically verified; see findings md 9):
//     Test         0x4B815C0    PvP          0x4B809F0
//     Demo         0x4B7FA10    PvESolo      0x4B801E0
//     Story        0x4B811C0    PvETag       0x4B805E8
//     Free         0x4B7FDF8    Replay       0x4B80DD8
//     VisualLobby  0x4B819A8    Arcade       0x4B7F610
//     AgingOnline  0x4B7F228    (base phases: BattleBase 0x4B7E638,
//                                BattleMissionBase 0x4B7EA20, BattlePvEBase 0x4B7EE20)
//
// The mod detours SetCurrent: when the game forces the initial team value
// (force=1, key 0/1, value >= 0) *and* the current battle's phase vtable is in
// the configured mode list, it raises max + current to `Lives`.  Decrements use
// force=0 and are untouched, so the gauge drains normally and the match still
// ends at 0 remaining points.  Negative values (the -1 gauge-off sentinel) are
// never modified.

#include "force_gauge.hpp"

#include "detour.hpp"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

namespace
{
    constexpr std::uintptr_t RVA_SetCurrent = 0x102AB90; // FUN_14102AB90
    constexpr std::uintptr_t RVA_SetMax = 0x102ADB0;     // FUN_14102ADB0
    constexpr std::uintptr_t RVA_Setup = 0x13B0970;      // FUN_1413B0970

    // FUN_14102AB90: push rbx; push rbp; push rdi; sub rsp,20; mov eax,[rcx+0x8B0]
    constexpr std::size_t kSetCurrentStolen = 14;
    constexpr std::size_t kSetCurrentPatch = 14;
    constexpr std::uint8_t kSetCurrentPrologue[kSetCurrentStolen] = {
        0x40, 0x53, 0x55, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x81, 0xB0, 0x08, 0x00, 0x00};

    // FUN_14102ADB0: mov [rsp+8],rbx; mov [rsp+10],rdi; mov eax,[rcx+0x900]
    constexpr std::uint8_t kSetMaxPrologue[16] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x8B, 0x81, 0x00, 0x09, 0x00, 0x00};

    // FUN_1413B0970: push r12; sub rsp,50; mov [rsp+68],rbx; mov [rsp+70],rbp
    constexpr std::size_t kSetupStolen = 16;
    constexpr std::size_t kSetupPatch = 16;
    constexpr std::uint8_t kSetupPrologue[kSetupStolen] = {
        0x41, 0x54, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x89, 0x5C, 0x24, 0x68, 0x48, 0x89, 0x6C, 0x24, 0x70};

    using SetCurrentFn = void (*)(void* manager, unsigned char key, int value, char force);
    using SetMaxFn = void (*)(void* manager, unsigned char key, int max_value);
    using SetupFn = char (*)(void* battle_phase);

    constexpr int kToggleKey = 0x77; // F8
    constexpr int kMaxModes = 16;

    struct ModeEntry
    {
        const wchar_t* name;
        std::uintptr_t vtable_rva;
    };

    constexpr ModeEntry kModes[] = {
        {L"Test", 0x4B815C0},   {L"Demo", 0x4B7FA10},    {L"Story", 0x4B811C0},
        {L"Free", 0x4B7FDF8},   {L"PvP", 0x4B809F0},     {L"PvESolo", 0x4B801E0},
        {L"PvETag", 0x4B805E8}, {L"Replay", 0x4B80DD8},  {L"VisualLobby", 0x4B819A8},
        {L"Arcade", 0x4B7F610}, {L"AgingOnline", 0x4B7F228},
    };

    std::atomic<bool> g_installed{false};
    std::atomic<bool> g_enabled{true};
    std::atomic<int> g_lives{6};
    std::atomic<bool> g_all_modes{true};
    std::atomic<std::uintptr_t> g_current_vtable_rva{0};
    std::atomic<int> g_current_mode{-1};
    const ModeEntry* g_allowed[kMaxModes]{};
    int g_allowed_count = 0;
    std::mutex g_log_mutex;

    SetCurrentFn g_orig_set_current = nullptr;
    SetMaxFn g_set_max = nullptr;
    SetupFn g_orig_setup = nullptr;
    void* g_set_current_trampoline = nullptr;
    void* g_setup_trampoline = nullptr;
    void* g_set_current_target = nullptr;
    void* g_setup_target = nullptr;
    std::uint8_t g_set_current_saved[kSetCurrentStolen]{};
    std::uint8_t g_setup_saved[kSetupStolen]{};

    auto game_base() -> std::uintptr_t
    {
        return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    }

    // INI + log live flat next to the .asi itself (no subfolder), so the mod
    // manager can deploy the plugin as scripts/SetTeamLives.asi +
    // scripts/gauge_lives.ini (+ scripts/force_gauge.log at runtime).
    auto data_root() -> const std::filesystem::path&
    {
        static const std::filesystem::path root = [] {
            wchar_t dll_path[MAX_PATH]{};
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&data_root), &self);
            GetModuleFileNameW(self, dll_path, MAX_PATH);
            return std::filesystem::path(dll_path).parent_path();
        }();
        return root;
    }

    auto log_line(const std::wstring& line) -> void
    {
        OutputDebugStringW((line + L"\n").c_str());
        std::lock_guard lock(g_log_mutex);
        std::wofstream out(data_root() / L"force_gauge.log", std::ios::app);
        out << line << L'\n';
        out.flush();
    }

    auto read_int_ini(const std::filesystem::path& ini, const wchar_t* section, const wchar_t* key, int fallback) -> int
    {
        wchar_t buf[64]{};
        GetPrivateProfileStringW(section, key, L"", buf, 64, ini.c_str());
        if (buf[0] == L'\0')
        {
            return fallback;
        }
        wchar_t* end = nullptr;
        return static_cast<int>(wcstol(buf, &end, 10));
    }

    auto read_string_ini(const std::filesystem::path& ini, const wchar_t* section, const wchar_t* key,
                         const wchar_t* fallback) -> std::wstring
    {
        wchar_t buf[512]{};
        GetPrivateProfileStringW(section, key, fallback, buf, 512, ini.c_str());
        return buf;
    }

    auto ensure_default_ini(const std::filesystem::path& ini) -> void
    {
        std::error_code ec;
        if (std::filesystem::exists(ini, ec))
        {
            return;
        }
        std::wofstream out(ini);
        if (!out)
        {
            return;
        }
        out << L"[Gauge]\n"
               L"; Raise the Team Gauge (\"lives\") to this value at battle start (vanilla is 4).\n"
               L"Lives=6\n\n"
               L"; 0 = leave the game completely vanilla. F8 toggles at runtime.\n"
               L"Enabled=1\n\n"
               L"; Comma-separated battle modes that get the extra lives (case-insensitive):\n"
               L";   All | Test, Demo, Story, Free, PvP, PvESolo, PvETag, Replay, VisualLobby, Arcade, AgingOnline\n"
               L"Modes=Free,PvP\n";
    }

    auto trim(std::wstring& s) -> void
    {
        const auto not_space = [](wchar_t c) { return std::iswspace(c) == 0; };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    }

    auto find_mode(const wchar_t* name) -> const ModeEntry*
    {
        for (const auto& mode : kModes)
        {
            if (_wcsicmp(mode.name, name) == 0)
            {
                return &mode;
            }
        }
        return nullptr;
    }

    auto mode_name(int index) -> const wchar_t*
    {
        if (index >= 0 && index < static_cast<int>(std::size(kModes)))
        {
            return kModes[index].name;
        }
        return L"unknown";
    }

    auto mode_index_for_vtable(std::uintptr_t vtable_rva) -> int
    {
        for (int i = 0; i < static_cast<int>(std::size(kModes)); ++i)
        {
            if (kModes[i].vtable_rva == vtable_rva)
            {
                return i;
            }
        }
        return -1;
    }

    auto current_mode_allowed() -> bool
    {
        if (g_all_modes.load(std::memory_order_relaxed))
        {
            return true;
        }
        const auto vtable = g_current_vtable_rva.load(std::memory_order_relaxed);
        for (int i = 0; i < g_allowed_count; ++i)
        {
            if (g_allowed[i]->vtable_rva == vtable)
            {
                return true;
            }
        }
        return false;
    }

    auto parse_modes(const std::filesystem::path& ini) -> void
    {
        std::wstring value = read_string_ini(ini, L"Gauge", L"Modes", L"All");
        trim(value);
        if (value.empty() || _wcsicmp(value.c_str(), L"All") == 0 || value == L"*")
        {
            g_all_modes.store(true);
            return;
        }
        g_all_modes.store(false);
        std::size_t start = 0;
        while (start <= value.size())
        {
            const std::size_t comma = value.find(L',', start);
            std::wstring token =
                value.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
            trim(token);
            if (!token.empty())
            {
                const auto* entry = find_mode(token.c_str());
                if (entry == nullptr)
                {
                    log_line(L"[SetTeamLives] unknown mode name in Modes=: '" + token + L"' (ignored)");
                }
                else
                {
                    bool duplicate = false;
                    for (int i = 0; i < g_allowed_count; ++i)
                    {
                        duplicate = duplicate || g_allowed[i] == entry;
                    }
                    if (!duplicate && g_allowed_count < kMaxModes)
                    {
                        g_allowed[g_allowed_count++] = entry;
                    }
                }
            }
            if (comma == std::wstring::npos)
            {
                break;
            }
            start = comma + 1;
        }
        if (g_allowed_count == 0)
        {
            log_line(L"[SetTeamLives] Modes= listed no valid mode; the gauge will not be modified");
        }
    }

    // The battle phase object is the first argument of the gauge setup, and the
    // setup is virtual slot 106 of the phase class, so its vtable is the mode id.
    __declspec(noinline) char hook_setup(void* battle_phase)
    {
        if (battle_phase != nullptr)
        {
            const auto base = game_base();
            const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            const auto vtable = *reinterpret_cast<std::uintptr_t*>(battle_phase);

            int flag = -1;
            float rate = 0.0f;
            if (vtable >= base + 0x1000 && vtable < base + 0x6000000)
            {
                const auto* slots = reinterpret_cast<std::uintptr_t*>(vtable);
                const auto flag_fn = reinterpret_cast<unsigned char (*)(void*)>(slots[0x390 / 8]);
                const auto rate_fn = reinterpret_cast<float (*)(void*)>(slots[0x388 / 8]);
                flag = static_cast<int>(flag_fn(battle_phase));
                rate = rate_fn(battle_phase);
            }

            const auto caller_rva = caller >= base ? caller - base : 0;
            const auto vtable_rva = vtable >= base ? vtable - base : 0;
            const int mode = mode_index_for_vtable(vtable_rva);
            g_current_vtable_rva.store(vtable_rva, std::memory_order_relaxed);
            g_current_mode.store(mode, std::memory_order_relaxed);

            struct Key
            {
                std::uintptr_t caller, vtable;
                int flag;
            };
            static Key logged[24]{};
            static int logged_count = 0;
            bool seen = false;
            for (int i = 0; i < logged_count; ++i)
            {
                if (logged[i].caller == caller_rva && logged[i].vtable == vtable_rva && logged[i].flag == flag)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen && logged_count < 24)
            {
                logged[logged_count++] = Key{caller_rva, vtable_rva, flag};
                wchar_t buf[256]{};
                swprintf_s(buf, L"[SetTeamLives] battle flow: mode=%s caller=0x%llX vtable=0x%llX flag=%d rate=%.3f %s",
                           mode_name(mode), static_cast<unsigned long long>(caller_rva),
                           static_cast<unsigned long long>(vtable_rva), flag, rate,
                           (g_enabled.load(std::memory_order_relaxed) && current_mode_allowed()) ? L"(applying)"
                                                                                              : L"(vanilla)");
                log_line(buf);
            }
        }
        return g_orig_setup(battle_phase);
    }

    __declspec(noinline) void hook_set_current(void* manager, unsigned char key, int value, char force)
    {
        const int lives = g_lives.load(std::memory_order_relaxed);
        if (force != '\0' && key <= 1 && value >= 0 && value < lives && g_enabled.load(std::memory_order_relaxed) &&
            current_mode_allowed())
        {
            // The map entry already exists (the game called SetMax just before),
            // so this only rewrites the clamp ceiling to the configured value.
            g_set_max(manager, key, lives);
            value = lives;
        }
        g_orig_set_current(manager, key, value, force);
    }

    DWORD WINAPI hotkey_thread(LPVOID param)
    {
        const int vk = static_cast<int>(reinterpret_cast<std::intptr_t>(param));
        bool was_down = false;
        for (;;)
        {
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (down && !was_down)
            {
                const bool enabled = !g_enabled.load();
                g_enabled.store(enabled);
                log_line(enabled ? L"[SetTeamLives] hotkey: ON" : L"[SetTeamLives] hotkey: OFF");
            }
            was_down = down;
            Sleep(50);
        }
    }
} // namespace

namespace fgl
{
    auto prologues_match() -> bool
    {
        const auto base = game_base();
        if (base == 0)
        {
            return false;
        }
        return std::memcmp(reinterpret_cast<const void*>(base + RVA_SetMax), kSetMaxPrologue,
                           sizeof(kSetMaxPrologue)) == 0 &&
               std::memcmp(reinterpret_cast<const void*>(base + RVA_SetCurrent), kSetCurrentPrologue,
                           sizeof(kSetCurrentPrologue)) == 0 &&
               std::memcmp(reinterpret_cast<const void*>(base + RVA_Setup), kSetupPrologue, sizeof(kSetupPrologue)) ==
                   0;
    }

    auto install() -> bool
    {
        if (g_installed.load())
        {
            return true;
        }

        const auto base = game_base();
        if (base == 0)
        {
            log_line(L"[SetTeamLives] install failed: game module not found");
            return false;
        }

        const auto ini_path = data_root() / L"gauge_lives.ini";
        {
            ensure_default_ini(ini_path);
            const int lives = read_int_ini(ini_path, L"Gauge", L"Lives", 6);
            g_lives.store((lives >= 1 && lives <= 99) ? lives : 6, std::memory_order_relaxed);
            g_enabled.store(read_int_ini(ini_path, L"Gauge", L"Enabled", 1) != 0, std::memory_order_relaxed);
            parse_modes(ini_path);
        }

        const auto set_max_addr = base + RVA_SetMax;
        const auto set_current_addr = base + RVA_SetCurrent;
        const auto setup_addr = base + RVA_Setup;

        if (std::memcmp(reinterpret_cast<const void*>(set_max_addr), kSetMaxPrologue, sizeof(kSetMaxPrologue)) != 0)
        {
            log_line(L"[SetTeamLives] install failed: SetMax prologue mismatch (exe build changed?)");
            return false;
        }
        if (std::memcmp(reinterpret_cast<const void*>(set_current_addr), kSetCurrentPrologue,
                        sizeof(kSetCurrentPrologue)) != 0)
        {
            log_line(L"[SetTeamLives] install failed: SetCurrent prologue mismatch (exe build changed?)");
            return false;
        }
        if (std::memcmp(reinterpret_cast<const void*>(setup_addr), kSetupPrologue, sizeof(kSetupPrologue)) != 0)
        {
            log_line(L"[SetTeamLives] install failed: Setup prologue mismatch (exe build changed?)");
            return false;
        }

        g_set_max = reinterpret_cast<SetMaxFn>(set_max_addr);
        g_set_current_target = reinterpret_cast<void*>(set_current_addr);
        g_setup_target = reinterpret_cast<void*>(setup_addr);

        if (!fgl::detour::install(g_set_current_target, reinterpret_cast<void*>(&hook_set_current), kSetCurrentStolen,
                                  kSetCurrentPatch, g_set_current_saved, &g_set_current_trampoline))
        {
            log_line(L"[SetTeamLives] install failed: SetCurrent detour could not be written");
            return false;
        }
        if (!fgl::detour::install(g_setup_target, reinterpret_cast<void*>(&hook_setup), kSetupStolen, kSetupPatch,
                                  g_setup_saved, &g_setup_trampoline))
        {
            log_line(L"[SetTeamLives] install failed: Setup detour could not be written");
            fgl::detour::uninstall(g_set_current_target, g_set_current_saved, kSetCurrentStolen, kSetCurrentPatch,
                                   g_set_current_trampoline);
            g_set_current_target = nullptr;
            g_set_current_trampoline = nullptr;
            return false;
        }

        g_orig_set_current = reinterpret_cast<SetCurrentFn>(g_set_current_trampoline);
        g_orig_setup = reinterpret_cast<SetupFn>(g_setup_trampoline);
        g_installed.store(true);

        CreateThread(nullptr, 0, &hotkey_thread, reinterpret_cast<LPVOID>(static_cast<std::intptr_t>(kToggleKey)), 0,
                     nullptr);

        std::wstring modes;
        if (g_all_modes.load())
        {
            modes = L"All";
        }
        else
        {
            for (int i = 0; i < g_allowed_count; ++i)
            {
                if (i > 0)
                {
                    modes += L",";
                }
                modes += g_allowed[i]->name;
            }
        }
        wchar_t buf[320]{};
        swprintf_s(buf, L"[SetTeamLives] installed (asi): enabled=%d lives=%d modes=%s data=%s (F8 toggles)",
                   g_enabled.load() ? 1 : 0, g_lives.load(), modes.c_str(), data_root().c_str());
        log_line(buf);
        return true;
    }

    auto uninstall() -> void
    {
        if (!g_installed.load())
        {
            return;
        }
        if (g_setup_target != nullptr)
        {
            fgl::detour::uninstall(g_setup_target, g_setup_saved, kSetupStolen, kSetupPatch, g_setup_trampoline);
        }
        if (g_set_current_target != nullptr)
        {
            fgl::detour::uninstall(g_set_current_target, g_set_current_saved, kSetCurrentStolen, kSetCurrentPatch,
                                   g_set_current_trampoline);
        }
        g_setup_target = nullptr;
        g_set_current_target = nullptr;
        g_setup_trampoline = nullptr;
        g_set_current_trampoline = nullptr;
        g_orig_setup = nullptr;
        g_orig_set_current = nullptr;
        g_set_max = nullptr;
        g_installed.store(false);
        log_line(L"[SetTeamLives] uninstalled");
    }
} // namespace fgl
