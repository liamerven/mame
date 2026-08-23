# Packages a built mame.exe and its support files into a distributable zip.
# Run from the repository root after building:  powershell -File package-release.ps1
# Produces dist\mame-screenreader-<date>.zip

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not (Test-Path "$root\mame.exe")) {
    Write-Error "mame.exe not found - build it first (make -j16 from the MSYS2 MinGW64 environment)"
}

$stage = "$root\dist\mame-screenreader"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# the emulator itself
Copy-Item "$root\mame.exe" $stage

# NVDA controller client so announcements go through NVDA when it's running
# (without it, or without NVDA, MAME falls back to Windows SAPI voices)
if (Test-Path "$root\nvdaControllerClient.dll") {
    Copy-Item "$root\nvdaControllerClient.dll" $stage
}

# support folders shipped with official MAME releases (plus cheat and roms) -
# deliberately NOT ini, cfg, nvram or sta, so personal configuration and play
# state never ship in a package
foreach ($dir in @("artwork", "bgfx", "cheat", "ctrlr", "hash", "hlsl", "language", "plugins", "roms", "samples")) {
    if (Test-Path "$root\$dir") {
        Copy-Item "$root\$dir" $stage -Recurse
    }
}

# make sure a roms folder exists in the package even if the source has none
New-Item -ItemType Directory -Force "$stage\roms" | Out-Null

# cheat collection archive (e.g. Pugsy's cheats), if present - MAME looks
# for it inside the cheat folder (the default cheatpath), not the root
foreach ($cheatfile in @("cheat.7z", "cheats.7z", "cheat\cheat.7z", "cheat\cheats.7z")) {
    if (Test-Path "$root\$cheatfile") {
        New-Item -ItemType Directory -Force "$stage\cheat" | Out-Null
        Copy-Item "$root\$cheatfile" "$stage\cheat\"
    }
}

# short read-me for the accessibility features
@"
MAME with screen reader accessibility
=====================================

This build of MAME speaks its internal UI (the menus reached with Tab,
the system selection list, startup warnings and popup messages).

* If the NVDA screen reader is running and nvdaControllerClient.dll is
  next to mame.exe, announcements use your NVDA voice.
* Otherwise announcements use the Windows SAPI text-to-speech voices -
  no screen reader needs to be installed.
* Speech can be turned off under Settings > Miscellaneous Options
  ("Speak menu selections and messages aloud"), or with ui_speech 0
  in ui.ini.

Put your ROM zips in the roms folder, run mame.exe, and use the arrow
keys and Enter to pick a system.  Press Tab in a running system for
the main menu.

Source: https://github.com/liamerven/mame/tree/screenreader-accessibility
"@ | Out-File -Encoding utf8 "$stage\README-ACCESSIBILITY.txt"

# zip it up
$date = Get-Date -Format "yyyy-MM-dd"
$zip = "$root\dist\mame-screenreader-$date.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Output "Created $zip"
