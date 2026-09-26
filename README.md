# Animus Injector

An ASI injector for the multiplayer mode of Assassin's Creed Brotherhood, Revelations, and III.

## Features

- Redirects from Ubisoft servers to AnimusNetwork's servers
    - also works when security software has already hooked the network functions
    - redirects the AC3 friend list served by `uplay_r1_loader.dll`
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
UplayProxy=animusnetwork.com:21006

[Logging]
Enabled=true
Level=info

[Fixes]
FixCpuAffinity=true
FixSkipIntroVideos=true
```

`UplayProxy` (AC3 only) is the `host:port` that the friend list of `uplay_r1_loader.dll` is requested from: the game server's friend service (TCP, on the same port number as its UDP login server), which serves friends, friend requests and online states. Leave it empty to use `RedirectHost` instead.

## Troubleshooting

Check `AnimusInjector.log` in the game folder first.

**`already hooked by ...` or `falling back to import table patching`.**
Another program, usually security software such as Bitdefender, has already
hooked the Windows networking functions. The injector works around this
automatically. Continue if the log ends with `hostname redirect active`.

**`failed to install hostname redirect`.**
The game will still start, but it will not connect to the redirect host. Add
the game folder as an exception in your antivirus, or temporarily disable its
behaviour or exploit protection (e.g. Bitdefender *Advanced Threat Defense*).
Then start the game again. If it still fails, include the log when you report
the issue.

**`skip intro videos fix: ... not found` or `... is ambiguous` (AC3 only).**
The fix looks for the intro video code in any `AC3MP.exe` build. This message
means your build does not contain it in a recognisable form, so the intro
videos play as usual. The game is not affected otherwise.

**`the AC3 friend list will not be redirected` (AC3 only).**
The friend list is served by `uplay_r1_loader.dll` over WinINet, which the
hostname redirect does not cover, so the injector changes the proxy's URL
directly. This error means that step failed, for example because the proxy was
replaced with a build it does not recognise. Make sure the AnimusNetwork
`uplay_r1_loader.dll` is in place.

## Changelog

See [Changelog.md](Changelog.md).

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