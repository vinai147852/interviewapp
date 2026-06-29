# Shipping StealthOverlay — No Source Code Required

This guide explains how to give friends and colleagues a ready-to-run copy of StealthOverlay without sharing any source code. Three tiers: raw EXE, simple installer, and signed installer for a professional feel.

---

## Option 1 — Bare EXE (quickest, 5 minutes)

This works fine for trusted people (family, close colleagues). They will see a Windows SmartScreen warning on the first run.

**Steps:**

1. Build `Release | x64` in Visual Studio.
2. Find the output:
   ```
   StealthOverlay\x64\Release\StealthOverlayApp.exe
   ```
3. Zip it:
   ```
   StealthOverlayApp_v1.0.zip
   ```
4. Share via Google Drive, OneDrive, iCloud Drive, or WeTransfer.
5. Tell the recipient: *"When SmartScreen says 'Unknown publisher', click More info → Run anyway."*

**Recipient requirements:** Windows 10 build 19041 (May 2020 Update) or Windows 11. No installer, no runtime, no dependencies.

---

## Option 2 — Inno Setup Installer (recommended, ~1 hour)

A real `.exe` installer that copies the app to `Program Files`, creates a Start Menu shortcut, and adds an entry to Add/Remove Programs. Removes the SmartScreen warning if you code-sign (see Option 3).

### Install Inno Setup

Download free from: https://jrsoftware.org/isinfo.php

### Create the install script

Create `installer\setup.iss`:

```iss
[Setup]
AppName=StealthOverlay
AppVersion=1.0
DefaultDirName={autopf}\StealthOverlay
DefaultGroupName=StealthOverlay
OutputBaseFilename=StealthOverlay_Setup_v1.0
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64

[Files]
Source: "..\StealthOverlay\x64\Release\StealthOverlayApp.exe"; \
        DestDir: "{app}"; DestName: "StealthOverlay.exe"; Flags: ignoreversion

[Icons]
Name: "{group}\StealthOverlay"; Filename: "{app}\StealthOverlay.exe"
Name: "{commondesktop}\StealthOverlay"; Filename: "{app}\StealthOverlay.exe"

[Run]
Filename: "{app}\StealthOverlay.exe"; Description: "Launch StealthOverlay"; \
          Flags: nowait postinstall skipifsilent
```

### Build the installer

Open Inno Setup Compiler, load `setup.iss`, press **Build → Compile** (or F9).

Output: `installer\Output\StealthOverlay_Setup_v1.0.exe` (~500 KB)

Share this single file. Recipients double-click it, click through the wizard, and the app is installed.

---

## Option 3 — Code Signing (professional, removes SmartScreen)

Without a signature, Windows SmartScreen shows a warning to every new recipient. A code signing certificate removes it.

### Get a certificate

| Provider | Price | Notes |
|---|---|---|
| Certum | ~$60/yr | Cheapest OV cert |
| Sectigo | ~$150/yr | Widely trusted |
| DigiCert | ~$500/yr | EV cert, instant SmartScreen trust |

For a personal project starting out: skip signing, use Option 1 or 2, and tell users to click "Run anyway". Invest in a cert when you have paying users.

### Sign the EXE (once you have a cert)

```bat
signtool sign ^
  /fd SHA256 ^
  /tr http://timestamp.digicert.com ^
  /td SHA256 ^
  /n "Your Name or Company" ^
  StealthOverlayApp.exe
```

Sign both the EXE and the Inno Setup output installer.

---

## Option 4 — Microsoft Store (widest reach, no SmartScreen at all)

Package as MSIX and publish to the Microsoft Store. Users install with one click, no warnings, and you get automatic updates.

**Rough steps:**

1. Create a developer account at https://partner.microsoft.com/dashboard ($19 one-time fee).
2. Package as MSIX using Visual Studio: **Project → Publish → Create App Packages**.
3. Submit for certification (typically 3–5 business days for a Win32 app).

This is overkill for a POC but the right path if you plan to monetise.

---

## What to share vs. what to keep private

| Artefact | Share? |
|---|---|
| `StealthOverlayApp.exe` (Release build) | ✅ Yes |
| Inno Setup installer `.exe` | ✅ Yes |
| `.pdb` (debug symbols) | ❌ No — reveals internal symbols |
| `*.vcxproj`, `*.sln`, `*.cpp`, `*.h` | ❌ No — that's your source |
| `x64\Release\*.lib` (static libs) | ❌ No — not needed by recipients |

---

## Quick checklist before sharing

- [ ] Build in **Release** configuration (not Debug — Debug binaries are ~10× larger and include symbols)
- [ ] Run the EXE on a **clean machine** (or a clean Windows Sandbox) to verify it works without Visual Studio installed
- [ ] Test on Windows 10 build 19041 if your audience may not be on Windows 11
- [ ] If distributing widely, test with Windows Defender: `.\StealthOverlayApp.exe` in a fresh folder

---

## Windows Sandbox quick test (no clean machine needed)

1. Enable Windows Sandbox: **Turn Windows features on or off → Windows Sandbox**
2. Start Sandbox, copy your EXE into it.
3. Run it — this mimics a fresh Windows 11 install.
