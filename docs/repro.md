# Как повторить

Все команды — из Windows PowerShell. Пути соответствуют этой машине.

---

## 1. Распаковать ISO

```powershell
# полный листинг
py -3.14 D:\CODE\SDNX\port\tools\extract_iso.py "D:\CODE\SDNX\Sleeping Dogs LT+3.0.iso" list

# извлечь всё в assets (6.09 ГиБ)
py -3.14 D:\CODE\SDNX\port\tools\extract_iso.py "D:\CODE\SDNX\Sleeping Dogs LT+3.0.iso" extract D:\CODE\SDNX\port\assets
```

Отдельный файл (например, только XEX):

```powershell
cd D:\CODE\SDNX\port\tools
node xdvdfs.mjs "D:\CODE\SDNX\Sleeping Dogs LT+3.0.iso" extract default.xex "D:\CODE\SDNX\port\xex\default.xex"
```

---

## 2. Кодогенерация

```powershell
cd D:\CODE\rexglue\bin
.\rexglue.exe codegen "D:\CODE\SDNX\port\probe\sdretail_manifest.toml" --log-file D:\CODE\SDNX\port\probe\codegen.log --log-level info
```

Манифест: `D:\CODE\SDNX\port\probe\sdretail_manifest.toml`.
Важно: таблица границ — **`[entrypoint.functions]`**, записи **без `end`**.

Ожидаемо: `Analyze: 0 errors`, ~114 с, 241 МБ C++ в `generated\default`.

---

## 3. Сборка десктопа

```powershell
$env:PATH = "C:\Program Files\LLVM\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;" + $env:PATH
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

cd D:\CODE\SDNX\port\probe
& $cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH="D:\CODE\rexglue"
& $cmake --build out/build/win-amd64-release -j 12
```

Только clang (MSVC не соберёт: `__VA_OPT__`, `__builtin_bswap*`).
`CMakeLists.txt` должен содержать `rexglue_setup_target(sdretail GPU_PLUGINS xenos)`.

---

## 4. Запуск

```powershell
$d = "D:\CODE\SDNX\port\probe\out\build\win-amd64-release"
& "$d\sdretail.exe" --game_data_root D:\CODE\SDNX\port\assets `
    --log_file D:\CODE\SDNX\port\run.log --log_level info `
    --gpu_plugin xenos
```

Полезные флаги:

| Флаг | Смысл |
|---|---|
| `--gpu_plugin xenos` | включает GPU-плагин (без него «no GPU emulation loaded») |
| `--mnk_mode` | клавиатура как геймпад |
| `--keybind_start Return`, `--keybind_a Return` | Enter = START/A |
| `--log_level debug` | подробный лог (осторожно: 90k+ строк) |
| `--render_target_path_d3d12 rtv` | альтернативный путь EDRAM |

---

## 5. Автоцикл разрешения свёрнутых адресов

```powershell
py -3.14 D:\CODE\SDNX\port\tools\autoloop.py 10
```

Делает: codegen → build → boot с прожатием START/OK/New Game → парсинг FATAL → split → повтор.
Состояние: `port\probe\folded_entries.json`, лог: `port\probe\autoloop.log`.

Пересобрать состояние из лога (если что-то разъехалось):

```powershell
py -3.14 D:\CODE\SDNX\port\tools\rebuild_state.py
```

---

## 6. Дамп распакованного образа (для Ghidra)

В `port\probe\src\sdretail_app.h` уже есть хук `OnPostLoadXexImage()`, который пишет
`D:\CODE\SDNX\port\image.bin` (16.7 МБ, база `0x82000000`). Достаточно запустить игру.

Анализ в Ghidra:

```powershell
$env:JAVA_HOME = "C:\Program Files\Eclipse Adoptium\jdk-21.0.12.101-hotspot"
& "D:\Tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" `
  D:\CODE\SDNX\port\ghidra_proj SdRaw `
  -import D:\CODE\SDNX\port\image.bin `
  -processor "PowerPC:BE:32:default" -loader BinaryLoader -loader-baseAddr 0x82000000 `
  -scriptPath D:\CODE\SDNX\port\tools\ghidra `
  -postScript ExportFunctions.java D:\CODE\SDNX\port\ghidra_funcs.txt `
  -deleteProject
```

Сравнение с таблицей ReXGlue:

```powershell
py -3.14 D:\CODE\SDNX\port\tools\diff_funcs.py `
  D:\CODE\SDNX\port\ghidra_funcs.txt `
  D:\CODE\SDNX\port\probe\generated\default\sdretail_register.cpp
```

---

## 7. Сборка SDK из исходников (нужна для инструментации и для Switch)

```powershell
git clone --depth 1 https://github.com/rexglue/rexglue-sdk.git D:\CODE\rexglue-src
cd D:\CODE\rexglue-src
git submodule update --init --recursive --depth 1

# git на Windows выгружает симлинки как текстовые файлы — материализовать:
py -3.14 D:\CODE\SDNX\port\tools\fix_symlinks.py

$env:PATH = "C:\Program Files\LLVM\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;" + $env:PATH
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
    -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF
& $cmake --build build -j 12
```

Артефакты: `D:\CODE\rexglue-src\out\win-amd64\{rexglue.exe,rexruntime.dll,rexgpu-xenos.dll}`.

---

## 8. Инструментация GPU-плагина

Патч уже внесён в `src\graphics\d3d12\command_processor.cpp::IssueDraw`.
Пересборка и подмена DLL рядом с игрой:

```powershell
$env:PATH = "C:\Program Files\LLVM\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;" + $env:PATH
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build D:\CODE\rexglue-src\build -j 12 --target rexgpu-xenos

$dst = "D:\CODE\SDNX\port\probe\out\build\win-amd64-release"
Copy-Item D:\CODE\rexglue-src\out\win-amd64\rexgpu-xenos.dll "$dst\rexgpu-xenos.dll" -Force
Copy-Item D:\CODE\rexglue-src\out\win-amd64\rexruntime.dll   "$dst\rexruntime.dll" -Force
```

Запуск с логом draw'ов:

```powershell
$env:REX_LOG_DRAWS = "400"
& "$dst\sdretail.exe" --game_data_root D:\CODE\SDNX\port\assets --gpu_plugin xenos `
    --log_file D:\CODE\SDNX\port\gameplay.log --log_level info
```

Анализ:

```powershell
py -3.14 D:\CODE\SDNX\port\tools\an_draws.py     # группировка по PS
py -3.14 D:\CODE\SDNX\port\tools\an_fmt.py       # группировка по формату RT
```

---

## 9. RenderDoc

### Захват нашего билда

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  D:\CODE\SDNX\port\tools\capture_frame.ps1 -Triggers 150,165 -Total 180
```

Скрипт сам прожает START и нажмёт F12 в указанные секунды; кадры кладутся в `port\captures\`.

### Headless-разбор

```powershell
$env:RD_CAPTURE = "D:\CODE\SDNX\port\captures2\frame_frame11834.rdc"
& "C:\Program Files\RenderDoc\qrenderdoc.exe" --python D:\CODE\SDNX\port\tools\rd_hdr.py
Get-Content D:\CODE\SDNX\port\rd_hdr.txt
```

Скрипты: `rd_actions2.py` (действия), `rd_hdr.py` (float-таргеты + PNG), `rd_dump_targets.py`
(colour-таргеты), `rd_pick.py` (пиксельные пробы).

### Эталон Xenia

```powershell
# путь без пробелов — через junction
New-Item -ItemType Junction -Path D:\sdproto -Target "D:\Sleeping Dogs (Nov 15 2011 prototype)"

& "C:\Program Files\RenderDoc\renderdoccmd.exe" capture `
  -c D:\CODE\SDNX\port\captures_xenia\x -d D:\xenia -w `
  D:\xenia\xenia_canary.exe D:\sdproto\default.xex
```

Важно: `renderdoccmd` **не понимает `--`** перед программой.

---

## 10. Инструменты проекта

| Файл | Назначение |
|---|---|
| `tools\xdvdfs.mjs` | чтение XDVDFS, извлечение одного файла |
| `tools\extract_iso.py` | полная распаковка ISO |
| `tools\xexinfo.mjs` | заголовок XEX |
| `tools\autoloop.py` | автоцикл поиска свёрнутых адресов |
| `tools\rebuild_state.py` | пересборка состояния границ из лога |
| `tools\fix_symlinks.py` | материализация git-симлинков в thirdparty |
| `tools\run_with_input.ps1` | прогон со скриншотами и вводом |
| `tools\capture_frame.ps1` | захват кадров под RenderDoc |
| `tools\rd_*.py` | headless-анализ захватов |
| `tools\an_draws.py`, `tools\an_fmt.py` | анализ палитры draw'ов |
| `tools\diff_funcs.py` | сверка Ghidra ↔ ReXGlue |
| `tools\ghidra\ExportFunctions.java` | экспорт функций из Ghidra |

---

## 11. Switch (ARM64)

Полные команды, грабли и текущее состояние — в [switch-bringup.md](switch-bringup.md).

Короткая версия:

```powershell
# Docker в WSL (демон без systemd)
wsl -d Ubuntu -- bash -lc "echo <pw> | sudo -S sh -c 'nohup dockerd >/tmp/dockerd.log 2>&1 &'"

# hello-NRO
wsl -d Ubuntu -- bash -lc "sudo docker run --rm -v /mnt/d/CODE/SDNX/nx/hello:/work -w /work devkitpro/devkita64 bash -lc 'make -j8'"

# Mesa NVK SDK
wsl -d Ubuntu -- bash -lc "cd /mnt/d/CODE/masseffect-nx && sudo env SRC=\$HOME/mesa-switch-src OUT=/mnt/d/CODE/SDNX/mesa-sdk /bin/bash mesa/build_mesa_docker.sh"
```

Проверка бэкендов на десктопе (переключение — копированием DLL):

```powershell
$d = "D:\CODE\SDNX\port\probe\out\build\win-amd64-release"
Copy-Item D:\CODE\SDNX\port\bin-vulkan\*.dll $d -Force   # Vulkan
Copy-Item D:\CODE\SDNX\port\bin-d3d12\*.dll  $d -Force   # D3D12
```

Vulkan-путь рендера выбирается флагом:

```powershell
& "$d\sdretail.exe" --game_data_root D:\CODE\SDNX\port\assets --gpu_plugin xenos --render_target_path_vulkan fbo
```

---

## 12. Данные игры и брендинг Switch

```powershell
py -3.14 D:\CODE\SDNX\port\tools\sdbig.py list    D:\CODE\SDNX\port\assets\UI          # записи архива
py -3.14 D:\CODE\SDNX\port\tools\sdbig.py chunks  D:\CODE\SDNX\port\assets\UI          # все чанки с именами
py -3.14 D:\CODE\SDNX\port\tools\sdbig.py extract D:\CODE\SDNX\port\assets\UI out 14B0D2FB
py -3.14 D:\CODE\SDNX\port\tools\sdtext.py dump   D:\CODE\SDNX\port\assets\UI xbox     # строки локализации
py -3.14 D:\CODE\SDNX\port\tools\check_text_roundtrip.py                               # пересборка таблиц = оригинал

py -3.14 D:\CODE\SDNX\port\tools\apply_branding.py <game_root>              # текст + кнопки Switch
py -3.14 D:\CODE\SDNX\port\tools\apply_branding.py <game_root> --restore    # откат
```

Подробности и форматы — [branding.md](branding.md). Нужны `pip install pillow numpy`.
