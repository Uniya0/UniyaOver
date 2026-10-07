UniyaOver — Instant Replay for Windows
======================================

License
-------
Copyright (c) 2026 UNIYA. All rights reserved (see LICENSE.txt).
Official binaries may be used personally; redistribution / rebranding
without permission is not allowed.
FFmpeg DLLs remain LGPL (see FFMPEG-LICENSE.txt / THIRD_PARTY_NOTICES.txt).

Requirements
------------
- Windows 10/11 x64
- NVIDIA GPU with NVENC (GeForce / RTX). AMD and Intel are NOT supported yet.
- Microsoft Visual C++ Redistributable 2015–2022 (x64):
  https://aka.ms/vs/17/release/vc_redist.x64.exe

Install
-------
Recommended: run UniyaOver-Setup.exe (single file). Pick a folder, click INSTALL.
Portable: keep this folder layout:

  UniyaOver.exe              (launcher)
  bin\UniyaOverCore.exe      (app)
  bin\*.dll                  (FFmpeg)
  assets\uniyaover.ico
  *.txt                      (licenses / readme)

What UniyaOver is / is not
--------------------------
Offline instant replay (ring buffer → hotkey save, full or region).
No account / no cloud upload.
NOT guaranteed with competitive anti-cheat (Valorant, etc.) — use at your own risk.
NOT a full NVIDIA App replacement.

Privacy paths
-------------
Config: %LOCALAPPDATA%\UniyaOver\
Clips:  Videos\instant-replay\ (or Settings)
Thumbs: %LOCALAPPDATA%\UniyaOver\thumbs\

Notes
-----
Unsigned builds may trigger SmartScreen until code-signed.
