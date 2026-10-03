# SetTeamLivesASI

Standalone **ASI plugin** version of SetTeamLives — same feature set as the UE4SS
mod, but with no UE4SS dependency. Raises the **Team Gauge** ("lives") to
`Lives` (default 6) at battle start, only in the battle modes listed in `Modes`
(shipped: `Free,PvP`; vanilla is 4).

Status: addresses verified statically against the installed build
(`Jujutsu Kaisen CC.exe`, 111,301,392 bytes, UE 5.1). **Not yet verified in play in
ASI form** — the plugin refuses to install and logs if the binary changes.

## How it differs from the UE4SS mod

| | UE4SS mod | this ASI |
|---|---|---|
| Loading | UE4SS calls `start_mod` | ASI loader `LoadLibrary`s `SetTeamLives.asi`, `DllMain` starts a thread |
| Layout | `ue4ss/Mods/ForceGaugeLives/dlls/main.dll` | `SetTeamLives.asi` in the game `scripts/` folder (Ultimate ASI Loader plugin dir) |
| Data | next to the mod folder | flat next to the `.asi` (`gauge_lives.ini`, `force_gauge.log`) |
| Depends on | UE4SS | any x64 ASI loader |

The hooking core (`cpp/force_gauge.cpp`, `cpp/detour.hpp`) is shared.

## Files

- `cpp/force_gauge.cpp` — detours, mode table, INI/log, F8 toggle.
- `cpp/asi_entry.cpp` — `DllMain` + install thread (readiness retry via prologue guards).
- `cpp/detour.hpp` — self-contained inline hook engine.
- `build.ps1` — builds `dist\SetTeamLives.asi`.
- `gauge_lives.ini` — default config (also auto-written on first run if missing).
- `dist\SetTeamLives.asi` — output.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

## Install

This game already ships **Ultimate ASI Loader 8.3.0** as
`Jujutsu Kaisen CC\Binaries\Win64\dsound.dll`, which loads every `*.asi` placed next
to it.

1. Copy `dist\SetTeamLives.asi` to `Jujutsu Kaisen CC\Binaries\Win64\scripts\` (create it if missing).
   Ultimate ASI Loader also loads `*.asi` placed next to the exe, but `scripts/` is what the mod manager deploys.
2. Optional: copy `gauge_lives.ini` next to the `.asi`
   (a default INI is created automatically on first run).
3. Launch the game. Check `force_gauge.log` next to the `.asi` for
   `[SetTeamLives] installed (asi): ...`.

If another mod occupies the loader slot, any UAL proxy name works (`dinput8.dll`,
`winmm.dll`, `version.dll`, `d3d11.dll`, ...). UAL also supports a `plugins\` folder
for `.asi` files on some versions — the exe folder is the guaranteed location.

## Config

`gauge_lives.ini` (next to the `.asi`):

```ini
[Gauge]
Lives=6            ; 1-99, vanilla is 4
Enabled=1          ; 0 = completely vanilla (F8 toggles at runtime)
Modes=Free,PvP     ; All or a comma-separated mode list
```

Mode names: `Test`, `Demo`, `Story`, `Free`, `PvP`, `PvESolo`, `PvETag`, `Replay`,
`VisualLobby`, `Arcade`, `AgingOnline`. A mode not listed is left completely vanilla;
the mode comes from the battle phase object's vtable (`lives-team-gauge-findings.md` §9).

## Uninstall

Delete `SetTeamLives.asi` (and optionally `gauge_lives.ini` / `force_gauge.log` next to it). No
game files are touched.

## Caveats

- **Online: both players must run the mod.** Only the current value is synced between
  peers; the max is computed locally. One-sided use shows different gauge sizes and can
  desync the match.
- The plugin is pinned to this exe build. After a game update the prologue check fails
  and the log says `install failed: ... prologue mismatch (exe build changed?)`.
- No anti-cheat (EAC/BattlEye) is present in this build, but use it in private lobbies.
- See `lives-team-gauge-findings.md` (sections 5-9) for the reverse engineering and the
  full mode -> vtable map.
