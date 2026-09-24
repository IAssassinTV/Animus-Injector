# Changelog

All notable changes to this project will from now on (2026-09-24) be documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## v1.0.4 — 2026-09-24

### Fixed
- **Hostname redirect failing on some PCs.** When security software had already
  hooked the Windows networking functions (e.g. Bitdefender), the injector
  could not install its own hook. It logged `failed to install hostname redirect`
  and the game kept connecting to Ubisoft's servers.
  - If the normal hook cannot be installed, the injector now falls back to
    patching the game's import table instead, which works alongside other
    software's hooks.
  - Calls where the game looks up these functions while running (`GetProcAddress`)
    are redirected as well.
- **Game fixes skipped when the redirect failed.** Previously a failed redirect
  stopped initialization, so the CPU affinity, XInput and PunkBuster fixes were not
  applied either. They are now applied regardless.
- **AC3 friend list with long redirect hosts.** The friend proxy's URL was
  overwritten in place, so it only worked for redirect hosts of up to 27
  characters. With a longer host the friend list silently stayed on Ubisoft's
  servers, and the log wrongly claimed the hostname redirect would cover it (the
  proxy uses WinINet, which the hostname redirect does not intercept). The
  proxy's code now points to a URL owned by the injector, so any host length
  works.
- **AC3 friend list retry.** When a request failed, the proxy retried a
  hardcoded private address (`10.163.216.209`) that players cannot reach. It
  now retries the redirect host.

### Changed
- **AC3 Skip Intro Videos on other `AC3MP.exe` builds.** The fix only ran on
  one exact build (checked by PE timestamp and image size) and used fixed
  addresses, so other builds were rejected with `unsupported AC3MP.exe build`.
  This included a build whose code is identical and only the timestamp differs.
  The game code is now located by signature on any build. The fix is skipped,
  not forced, if the signatures are missing or ambiguous.
- The log now explains why a hook could not be installed, and names the module
  if another program has already hooked the function (`already hooked by ...`).
- The log only mentions fixes that exist for the running game. For example,
  ACR and AC3 no longer log `punkbuster fix disabled`, and ACB and ACR no
  longer log anything about skipping intro videos.
- If the AC3 friend list cannot be redirected, the log now says so as an error
  (`the AC3 friend list will not be redirected`).

---

## v1.0.3 — 2026-09-21

### Added
- **AC3 friend list:** the friend service URL inside `uplay_r1_loader.dll` is
  rewritten to the configured `RedirectHost`. If the DLL is loaded after the
  injector, the injector waits for it. Only the copy in memory is changed,
  never the file on disk.

### Fixed
- **AC3 Skip Intro Videos** now targets the updated `AC3MP.exe` build.

---

## v1.0.2 — 2026-09-20

### Fixed
- **ACR on Linux (Wine/Proton):** fixed a deadlock at startup when the XInput
  fix was enabled. v1.0.1 still also ran initialization inside `DllMain`; ACR
  now initializes only on a separate thread.
- The log now shows the correct injector version.

---

## v1.0.1 — 2026-09-20

### Fixed
- ACR failing to launch on Linux when the XInput fix was enabled (partial fix,
  completed in 1.0.2).

---

## v1.0.0 — 2026-09-19

First release as **Animus Injector** (previously *Claudia*).

### Added
- Support for **Assassin's Creed Revelations** (`ACRMP.exe`) and
  **Assassin's Creed III** (`AC3MP.exe`) alongside Brotherhood (`ACBMP.exe`).
- Bundled `dinput8.dll` loader that loads `AnimusInjector.asi`.
- Per-game `AnimusInjector.ini`, generated with the correct defaults for the
  detected game on first launch.
- **ACB login:** relaunches `ACBMP.exe` with the `[Credentials]` from the ini;
  refuses to start without credentials.
- **PunkBuster fix** (ACB), **XInput fix** (ACB, ACR) and **Skip Intro Videos**
  (AC3), each of which can be switched on or off in the ini.

### Removed
- The Claudia launcher GUI and `claudia.ini`.
