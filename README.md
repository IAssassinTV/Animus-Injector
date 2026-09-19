# Animus Injector

An ASI injector for the multiplayer mode of Assassin's Creed Brotherhood, Revelations, and III.

## Features

- Redirects from Ubisoft servers to AnimusNetwork's servers
- Fixes modern controllers by enforcing XInput
- Fixes CPU affinity
- Enforces PunkBuster to be disabled
- Skips intro videos

## Supported Games

| Game | Executable | XInput Fix | PunkBuster Fix | Skip Intro Videos |
|------|-----------|------------|----------------|-------------------|
| Assassins Creed: Brotherhood | `ACBMP.exe` | Yes | Yes | No |
| Assassins Creed: Revelations | `ACRMP.exe` | Yes | No | No |
| Assassins Creed III | `AC3MP.exe` | No | No | Yes |

## Installation
- Copy `AnimusInjector.asi` and `dinput8.dll` into the game folder (e.g. "**steamapps\common\Assassins Creed Brotherhood**" for ACB on Steam).
- Start the game. On first launch the matching `AnimusInjector.ini` defaults are created and the game exits immediately (see the log) so you can review them, then start the game a second time.

## Requirements
- [Animus-Launcher](https://github.com/IAssassinTV/Animus-Launcher) (optional)
- Ubi Orbit API (mandatory; required for ACR and AC3)
    - authenticates players and exchanges credentials with the game servers
    - saves necessary multiplayer data locally in an `orbit` subfolder

## Configuration
On first launch `AnimusInjector.ini` is written with the correct defaults for the detected game, and every fix can be toggled per game:

**ACBMP (`ACBMP.exe`):**

```ini
[Credentials]
Username=your_username
Password=your_password

[Network]
RedirectHost=animusnetwork.com

[Logging]
Enabled=true
Level=info

[Fixes]
FixCpuAffinity=true
FixXInputDetection=true
FixDisablePunkBuster=true
```

> **ACB login:** Once `AnimusInjector.ini` exists, if you fill in `[Credentials]` `Username`/`Password`, the injector relaunches
> `ACBMP.exe` on start with ` /onlineUser:USERNAME /onlinePassword:PASSWORD` appended
> automatically. If `[Credentials]` is empty, the launch is aborted (see below). You can also
> launch it manually from a console:
>
> ```
> ACBMP.exe /onlineUser:USERNAME /onlinePassword:PASSWORD
> ```
>
> This launches the multiplayer client and logs into the server set in `[Network] RedirectHost`.
> Starting the game without credentials (either via `[Credentials]` or the console arguments)
> is aborted and logged instead of launching into an unauthenticated session.

**ACRMP (`ACRMP.exe`):**

```ini
[Network]
RedirectHost=animusnetwork.com

[Logging]
Enabled=true
Level=info

[Fixes]
FixCpuAffinity=true
FixXInputDetection=true
```

**AC3MP (`AC3MP.exe`):**

```ini
[Network]
RedirectHost=animusnetwork.com

[Logging]
Enabled=true
Level=info

[Fixes]
FixCpuAffinity=true
FixSkipIntroVideos=true
```

## Building from source

Requirements:
- Windows 10 or Windows 11
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.20 or newer

Build from a Visual Studio 2022 developer prompt (or run `build_windows.bat`):

```
cmake -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

The resulting files:
- `build\bin\Release\AnimusInjector.asi`
- `build\bin\Release\dinput8.dll`

There is no native Linux build; `build_linux.sh` only cross-compiles the Win32 MSVC build
inside Docker as a convenience.

## Credits
- [Claudia](https://github.com/siohaza/claudia) — Core codebase for this injector
- [ACRMP-ControllerFix](https://github.com/SimoneDevkt/ACRMP-ControllerFix) — Codebase of XInput fix for ACR
- [AC3MP-skipintro](https://github.com/SimoneDevkt/AC3MP-skipintro) — Codebase of AC3MP intro video skip

## License

[MIT](LICENSE)