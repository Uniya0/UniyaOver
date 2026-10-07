# UniyaOver (instant-replay)

Фоновый Instant Replay для Windows (NVIDIA NVENC). Offline, без аккаунта.

**Лицензия:** © UNIYA, все права защищены ([LICENSE](LICENSE)).  
FFmpeg DLL — **LGPL** ([THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)).

**Важно:** только NVIDIA с NVENC. AMD/Intel пока нет. Совместимость с античитом
(Valorant и т.п.) **не гарантируется** — не для соревновательного режима.

## Запуск

```bat
build\Release\UniyaOver.exe
build\Release\UniyaOver.exe --fps=240
build\Release\UniyaOver.exe --console
```

- Лог: `%LOCALAPPDATA%\UniyaOver\uniyaover.log`
- Конфиг: `%LOCALAPPDATA%\UniyaOver\config.json`
- Клипы: `%USERPROFILE%\Videos\instant-replay\`
- Портативка: `dist\UniyaOver` — `UniyaOver.exe` + `bin\` + `assets\`
- Один файл установки: `dist\UniyaOver-Setup.exe` (~125 MB)

## Хоткеи

По умолчанию: **Ctrl+Shift+R/X/S** (+ регион). Меняются в **Настройки**.

## GUI

- Главная / Библиотека / Настройки
- Сворачивание в **трей**
- Выбор монитора и playback-устройства, длина буфера, битрейт, превью клипов

## Сборка

FFmpeg: BtbN **win64-lgpl-shared** в `third_party/ffmpeg`.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File scripts\pack_dist.ps1
```
