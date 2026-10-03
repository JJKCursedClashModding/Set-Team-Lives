#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fgl::detour
{
    inline auto write_abs_jmp(std::uint8_t* dest, void* target) -> void
    {
        dest[0] = 0x48;
        dest[1] = 0xB8;
        std::memcpy(dest + 2, &target, sizeof(target));
        dest[10] = 0xFF;
        dest[11] = 0xE0;
    }

    inline auto install(void* target, void* hook_fn, std::size_t stolen_bytes, std::size_t patch_bytes,
                        std::uint8_t* saved_bytes, void** out_trampoline) -> bool
    {
        if (target == nullptr || hook_fn == nullptr || saved_bytes == nullptr || out_trampoline == nullptr ||
            stolen_bytes < 12 || patch_bytes < stolen_bytes)
        {
            return false;
        }

        void* const trampoline = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (trampoline == nullptr)
        {
            return false;
        }

        DWORD old_protect = 0;
        if (!VirtualProtect(target, patch_bytes, PAGE_EXECUTE_READWRITE, &old_protect))
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        std::memcpy(saved_bytes, target, stolen_bytes);

        auto* const stub = static_cast<std::uint8_t*>(trampoline);
        std::memcpy(stub, saved_bytes, stolen_bytes);
        write_abs_jmp(stub + stolen_bytes, static_cast<std::uint8_t*>(target) + stolen_bytes);

        auto* const patch = static_cast<std::uint8_t*>(target);
        write_abs_jmp(patch, hook_fn);
        for (std::size_t i = 12; i < patch_bytes; ++i)
        {
            patch[i] = 0x90;
        }

        FlushInstructionCache(GetCurrentProcess(), target, patch_bytes);
        FlushInstructionCache(GetCurrentProcess(), trampoline, stolen_bytes + 12);
        VirtualProtect(target, patch_bytes, old_protect, &old_protect);

        *out_trampoline = trampoline;
        return true;
    }

    inline auto uninstall(void* target, const std::uint8_t* saved_bytes, std::size_t stolen_bytes,
                          std::size_t patch_bytes, void* trampoline) -> void
    {
        if (target == nullptr || saved_bytes == nullptr)
        {
            return;
        }

        DWORD old_protect = 0;
        if (VirtualProtect(target, patch_bytes, PAGE_EXECUTE_READWRITE, &old_protect))
        {
            std::memcpy(target, saved_bytes, stolen_bytes);
            FlushInstructionCache(GetCurrentProcess(), target, patch_bytes);
            VirtualProtect(target, patch_bytes, old_protect, &old_protect);
        }

        if (trampoline != nullptr)
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
        }
    }
} // namespace fgl::detour
