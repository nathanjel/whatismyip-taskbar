# What Is My IP

A small Windows tray application that keeps track of the machine's externally visible IPv4 address.

The app runs quietly in the background, shows no main window, and does not require elevated permissions. It watches for Windows network changes such as routing updates, Wi-Fi changes, access point changes, and Ethernet connect/disconnect events. When a change is detected, it asks a public IP API for the current external IPv4 address.

## Features

- Background-only Win32 C++ application.
- System tray icon with a readable `IP` badge.
- Tray menu:
  - `Copy IPv4: ...` copies the current IP address to the clipboard.
  - `Exit` closes the app.
- Tray tooltip shows the current IPv4 address and last update time.
- Per-user self-install prompt on first normal launch.
- Optional registration in Windows Apps settings for uninstall.
- Optional Startup shortcut for automatic launch after sign-in.
- Single-instance guard: if the app is already running, a second launch exits immediately.
- No administrator rights required.

## Build

This is a CMake project intended for Windows with Visual Studio/MSVC.

```powershell
cmake --build out/build/x64-debug
```

The built executable is:

```text
out/build/x64-debug/whatismyip-tb.exe
```

## Install Behavior

When launched without command-line arguments, the app checks whether it is installed for the current user:

- executable exists under `%LOCALAPPDATA%\Programs\What Is My IP`
- uninstall registration exists under HKCU
- Startup shortcut exists under `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup`

If anything is missing, it offers to install itself locally. If declined, it simply continues running normally.

## Command-Line Arguments

```text
--startup
```

Runs normally but skips the install prompt. This is used by the Startup shortcut.

```text
--uninstall
```

Removes the Startup shortcut, unregisters the app from Windows Apps settings, and removes the per-user installed copy.
If the tray app is running, the uninstall command first asks that running instance to close and waits briefly before removing files.

## Attribution

This project was generated with GPT-5.5 and Codex.

## License

MIT License. Copyright (c) 2026 Marcin Gałczyński <marcin@galczynski.pl>.
