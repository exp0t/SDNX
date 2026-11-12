# Switch: инфраструктура сборки

Рабочие заметки по ARM64-части. Дополнение к [portlog.md](../portlog.md) §15 и
[docs/findings.md](findings.md) §11.

---

## 1. Что уже готово

| Компонент | Путь | Размер |
|---|---|---|
| Docker в WSL2 Ubuntu | `wsl -d Ubuntu` | 29.1.3 |
| Образ `devkitpro/devkita64` | docker image | 2.79 ГБ |
| Образ `devkitpro-mesa-rust` | docker image | 7.38 ГБ |
| Mesa NVK Horizon SDK | `D:\CODE\SDNX\mesa-sdk\opt\devkitpro\portlibs\switch\lib\libvulkan.a` | 124 738 466 Б (AArch64; 114–119.8 МБ — это прежние сборки с x86-64 `libnak_rs.a`) |
| Источник Mesa | `~/mesa-switch-src` (ext4) | 505 МБ |
| Hello-NRO | `D:\CODE\SDNX\nx\hello\sdnx-hello.nro` | 143 КБ |
| **Хостовый кодогенератор** | `~/masseffect-nx/sdk/out/host/rexglue` | ReXGlue v0.2.1 |
| **NRO Sleeping Dogs** | `D:\CODE\SDNX\nx\sdretail.nro` | **85 649 605 Б**, `NRO0` @ 0x10 |

Проверка артефактов:

```powershell
# NRO: байты 0..3 — заглушка перехода, "NRO0" по смещению 0x10
$b = [IO.File]::ReadAllBytes("D:\CODE\SDNX\nx\hello\sdnx-hello.nro")[0..31]

# ELF: e_machine = 0x00B7 (AArch64)
$e = [IO.File]::ReadAllBytes("D:\CODE\SDNX\nx\hello\sdnx-hello.elf")[0..19]
```

---

## 2. Почему не devkitPro напрямую

Все домены `*.devkitpro.org` отвечают **403**, а DNS подменяется на `198.18.26.17`
(это делает DPI-обход на хосте). DoH отдаёт настоящие адреса Cloudflare, но HTTP всё равно 403 —
блок на уровне запроса.

Поэтому используется официальный путь самих Switch-портов: **Docker-образ `devkitpro/devkita64`**
(Docker Hub доступен).

Если нужно именно нативное devkitPro — добавить `devkitpro.org` в исключения DPI-обхода.

---

## 3. Docker в WSL

```bash
# установка (в WSL Ubuntu)
sudo apt-get update && sudo apt-get install -y docker.io

# демон без systemd
sudo sh -c 'nohup dockerd --host=unix:///var/run/docker.sock >/tmp/dockerd.log 2>&1 &'
sudo docker info

# образ
sudo docker pull devkitpro/devkita64
```

Проверка содержимого образа:

```bash
sudo docker run --rm devkitpro/devkita64 bash -lc '
  echo $DEVKITPRO                      # /opt/devkitpro
  ls $DEVKITPRO/libnx/include/switch.h # есть
  cmake --version; ninja --version'
```

---

## 4. Hello-NRO

Исходники: `D:\CODE\SDNX\nx\hello\{Makefile, source\main.c}`.

```bash
sudo docker run --rm -v /mnt/d/CODE/SDNX/nx/hello:/work -w /work \
  devkitpro/devkita64 bash -lc 'make -j8'
```

Особенности Makefile (отличия от шаблона devkitPro):

- компиляторы задаются абсолютно: `$(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc`;
- `-I$(DEVKITPRO)/libnx/include` добавлен **до** строки `CFLAGS +=`, иначе раскрывается пустым;
- `elf2nro` без `--nacp` (NACP не генерируется, для homebrew не обязателен);
- используется современный ввод libnx: `padInitializeDefault` / `padUpdate` / `padGetButtonsDown`
  вместо старых `hidKeysDown` / `KEY_*`.

Запуск проверки: Ryujinx → «Файл → Загрузить приложение» → `sdnx-hello.nro`
(из библиотеки `.nro` не подхватывается).

---

## 5. Mesa NVK Horizon SDK

Скрипт: `D:\CODE\masseffect-nx\mesa\build_mesa_docker.sh` (чужой репозиторий, ветка закреплена).

```bash
cd /mnt/d/CODE/masseffect-nx

# 1) клон источника в ext4 (в /mnt/d git падает: fetch-pack invalid index-pack output)
git -c http.version=HTTP/1.1 clone --depth 1 \
  https://github.com/danfromtico/mesa-switch.git ~/mesa-switch-src

# 2) патч в CRLF ломает git apply — привести к LF
sed -i 's/\r$//' mesa/mesa-switch-masseffect.patch
cd ~/mesa-switch-src && git apply /mnt/d/CODE/masseffect-nx/mesa/mesa-switch-masseffect.patch

# 3) сборка (час и больше, ~15 ГБ)
cd /mnt/d/CODE/masseffect-nx
sudo env SRC=$HOME/mesa-switch-src OUT=/mnt/d/CODE/SDNX/mesa-sdk \
  /bin/bash mesa/build_mesa_docker.sh
```

Результат:

```
/mnt/d/CODE/SDNX/mesa-sdk/opt/devkitpro/portlibs/switch/lib/libvulkan.a   (124 738 466 Б)
```

Скрипт сам печатает, как это подключать:

```
MESA_SDK=/mnt/d/CODE/SDNX/mesa-sdk tools/build_nro.sh
-DREXGLUE_SWITCH_NVK_SDK=/mnt/d/CODE/SDNX/mesa-sdk/opt/devkitpro/portlibs/switch
```

### ⚠️ Грабли: Rust-часть NVK собирается под хост

`libvulkan.a` — это C-часть NVK **плюс** `libnak_rs.a` (Rust-компилятор шейдеров), склеенные
`ar -M`. Их `rustc-switch-wrapper.sh` берёт целевую архитектуру из `MESA_SWITCH_RUST_TARGET`,
но на **Linux-пути** (`build-switch.sh` → Docker) переменная не выставляется: ветки-фолбэки
в обёртке рассчитаны на MSYS2/macOS. Итог — `libnak_rs.a` собирается под `x86_64`, и линковка
NRO падает с

```
ld: skipping incompatible …/libvulkan.a when searching for -lvulkan
ld: cannot find -lvulkan
```

Проверка (первый объект архива должен быть AArch64):

```bash
m=$(aarch64-none-elf-ar t libvulkan.a | head -1)
aarch64-none-elf-ar p libvulkan.a "$m" > /tmp/m.o
aarch64-none-elf-readelf -h /tmp/m.o | grep Machine   # нужно AArch64
```

Лечение (то, что делаем):

1. В `build-switch.sh` добавить в `docker run` контейнера
   `-e MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu`.
2. В образ `devkitpro-mesa-rust` поставить стандартную библиотеку под цель и зафиксировать
   изменения через `docker commit` (иначе каждый `docker run` — чистый образ):
   `rustup target add aarch64-unknown-linux-gnu`.
3. Пересобрать Mesa (полный прогон `mesa/build_mesa_docker.sh`), затем переклеить архив.

Симптом в сборке NRO, если этого не сделать: `cannot find -lvulkan` **при наличии** файла
на месте — ld отвергает архив из-за несовместимой архитектуры первого объекта.

### Что ещё пришлось починить в их Linux-пути (полный список)

| Симптом | Причина | Правка |
|---|---|---|
| `libnak_rs.a` из x86-64 объектов | `MESA_SWITCH_RUST_TARGET` не доходит до контейнера | `-e MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu` в `docker run` внутри `build-switch.sh` |
| `can't find crate for core/std (aarch64-unknown-linux-gnu)` | в образе нет rust-std под цель | `rustup target add` + `docker commit` образа |
| proc-macro `libnak_ir_proc.so` собирался под ARM64 | обёртка форсирует цель для всех вызовов | исключение в обёртке по `*proc-macro*` |
| `couldn't find crate unicode_ident … x86_64` | `syn`/`quote`/`proc_macro2`/`unicode_ident`/`paste`/`rustc_hash` собирались как гостевые | исключения в обёртке по `--crate-name` |
| `mesa_clc: 'opencl-c-base.h' file not found` | `mesa_clc` не передаёт builtin-include clang | копирование `opencl-c*.h` в `src/compiler/libcl` (он уже в `-I`) |
| LTO-warnings про opaque pointers | LLVM 14 в образе vs LLVM 23 у rustc | безвредно, линковка проходит |

Итог: `libvulkan.a` = **124 738 466 Б**, первый объект AArch64; NRO линкуется за один шаг.

---

## 6. Хостовый кодогенератор их форка

Собирается их же скриптом, но только на Linux-окружении, которое надо подготовить.

```bash
cd ~/masseffect-nx
export CC=clang-20 CXX=clang++-20 JOBS=8
export CFLAGS="-march=native" CXXFLAGS="-march=native"
bash tools/build_host.sh
# → sdk/out/host/rexglue  (ReXGlue v0.2.1)
```

### Грабли (все пройдены)

| Симптом | Причина | Решение |
|---|---|---|
| `pipefail: invalid option name` | скрипты склонированы с CRLF (git autocrlf на Windows) | `sed -i 's/\r$//'` по 321 файлу (`*.sh`, `*.py`, `Makefile`) |
| `Could NOT find PkgConfig` | нет `pkg-config` | `apt install pkg-config` |
| SDL3: нет `x11-xcb`, `wayland-client`, `XTEST` | нет dev-пакетов | `libx11-xcb-dev libwayland-dev libxtst-dev` + xcb-* |
| `_mm_shuffle_epi8 requires 'ssse3'` | сборка без `-march` | `-march=native` |
| `no member named 'expected' in namespace 'std'` | libstdc++ 13 + **clang 18** | нужен clang 20 (`apt.llvm.org`) |
| libc++: `no matching function from_chars(float)` | в libc++ нет FP-`from_chars` | не использовать `-stdlib=libc++` |
| libc++: `clock_time_conversion` undeclared | в libc++ нет `clock_time_conversion`/`clock_cast` (потому в их коде и есть Apple-ветка) | libstdc++ |
| `Could not get lock /var/lib/dpkg` | держит `unattended-upgrades` | остановить его, дождаться лока |
| `CMakeCache.txt ... different than the directory` | кэш переехал вместе с копией дерева | `rm -rf sdk/out` перед сборкой |
| `ninja: interrupted by user` | WSL убивает процессы при закрытии сессии | запускать как фоновую задачу харнесса |
| сборка очень медленная | дерево на 9p (`/mnt/d`) | работать в ext4 (`~/masseffect-nx`) |

⚠️ Их `CMakeLists.txt` **жёстко требует clang**: `ReXGlue requires the Clang compiler`. GCC 14
собрал бы (у него есть всё), но стоит явный `FATAL_ERROR`.

---

## 7. Что дальше

1. ~~Третьи стороны SDK~~ ✅ `18 898 файлов` в `sdk/thirdparty`.
2. ~~Хостовый кодогенератор~~ ✅ `rexglue` v0.2.1.
3. Кодогенерация нашего `default.xex` их сборкой (манифест — по образцу
   `app/masseffect_manifest.toml`, наши 30 границ переносим).
4. Свой Horizon-энтрипоинт: `sdk/src/ui/runtime_switch.cpp`, `window_switch.h`,
   `rexglue_switch.cmake` (`rexglue_switch_add_nro`).
5. Первый NRO Sleeping Dogs → Ryujinx.

**Ожидание:** штатный рендер ReXGlue на Switch медленный (у Mass Effect — 1.5–2.5 fps в меню).
Играбельность потребует нативного Vulkan-рендерера по образцу `masseffect-nx/app/src/native/`.

---

## 8. Первый запуск NRO в Ryujinx: что происходит

Артефакт: `D:\CODE\SDNX\nx\sdretail.nro` (85 649 605 Б). На SD-карту эмулятора положено:

```
%APPDATA%\Ryujinx\sdcard\switch\sdretail\
├── sdretail.nro
├── game_root\        (6.09 ГБ данных игры)
└── logs\             (гостевые логи самого NRO)
```

Запуск: `Ryujinx.exe <путь к sdretail.nro>` (из библиотеки `.nro` не подхватывается — либо
«Файл → Загрузить приложение»).

Что видно в гостевом логе (`logs\sdretail_00N.log`):

```
Vulkan: выбран NVIDIA Tegra X1 (GM20B) (NVK GM20B), расширения получены
nvkmd-switch: VA bind addr=0x1fffff0000, range=0x10000 … failed: 0x195c
nvkmd-switch: create channel failed: out of device memory (VK_ERROR_OUT_OF_DEVICE_MEMORY)
Failed to create a Vulkan logical device … ErrorOutOfDeviceMemory
Graphics presentation setup failed: C0000001
```

То есть NRO **стартует, находит данные игры и доходит до создания Vulkan-устройства NVK**, после
чего упирается в ограничение эмулятора: NVK просит VA по адресу `0x1fffff0000` (~137 ГБ), а
NV-сервис Ryujinx отказывается его маппить (`ServiceNv MapBufferEx: Failed to map fixed buffer
with offset 0x0000001fffff0000` в логе Ryujinx — тот же адрес).

С `dram_size = 8` (ГБ) Ryujinx вместо падения уходит в бесконечную загрузку («Deep diving for
11m+») и гостевой лог не пишет — то есть до инициализации NRO дело не доходит.

Вывод: это несовместимость **эмулятора** с NVK-драйвером, а не дефект NRO. Дальше имеет смысл:

1. **Реальная консоль** — NRO готов, достаточно положить на SD (`switch/sdretail/`) и запустить
   через hbmenu; там NVK работает с настоящим NV-сервисом.
2. Другой эмулятор (yuzu-семейство) — у них другая реализация NV.
3. Копать настройки Ryujinx (GPU-память, memory manager mode) — менее перспективно: адрес
   фиксирован в NVK.

### Eden (`D:\CODE\EDEN`) — продвинулся дальше

Eden Nightly (`master-885b333c08`, Clang 23.1.5), GUI `eden.exe` + CLI `eden-cli.exe`.
Данные: `%APPDATA%\eden`, SD-карта — `%APPDATA%\eden\sdmc`.

**GUI игнорирует аргумент-путь** (открывает пустой список). Запускать нужно CLI:

```powershell
& "D:\CODE\EDEN\eden-cli.exe" "$env:APPDATA\eden\sdmc\switch\sdretail\sdretail.nro"
```

Раскладка на SD Eden (данные игры — junction'ом, чтобы не копировать 6 ГБ второй раз):

```
%APPDATA%\eden\sdmc\switch\sdretail\
├── sdretail.nro
├── game_root\   → junction на %APPDATA%\Ryujinx\sdcard\switch\sdretail\game_root
└── logs\
```

Результат первого запуска (гостевой лог `logs\sdretail_001.log`, 16 КБ):

* **NVK создаёт Vulkan-устройство успешно** — в отличие от Ryujinx:
  `driverID: MesaNvk`, `driverName: NVK`, `driverInfo: Mesa 26.2.3 (git-d4a00ea0ab)`,
  перечислены все свойства и включённые расширения (`VK_EXT_fragment_shader_interlock`,
  `VK_EXT_memory_budget`, `VK_EXT_robustness2`, …), создан swapchain 1920×1080.
* Падение произошло **не** из-за NVK, а из-за путей:

```
[error] --game_data_root was not provided.
Switch: app initialization failed
```

То есть `OnConfigurePaths` не нашёл `game_root`: `rex::filesystem::GetExecutableFolder()` под
эмулятором возвращает пусто, когда NRO запускается по хостовому пути. Нужен запасной вариант —
жёстко заданный `sdmc:/switch/sdretail` при пустом executable-folder.

После раскладки на SD Eden приложение завершается за ~1.2 с и **гостевой лог не пишет** — падает
раньше логирования; требует отладки (см. следующий шаг).

### Решено: запуск через GUI Eden + путь приложения

**hello-NRO в Eden работает** — виден наш текст, 60 FPS, Vulkan: тулчейн и эмулятор в порядке.

Диагностика (файл `sdmc:/sdretail_paths.txt`, который пишет `OnConfigurePaths`) дала главное:

```
hook ran
game_data_root=''
exe_folder='/switch/sdretail'
sdmc probe: missing  → позже: found
```

`GetExecutableFolder()` на Switch — это **`/switch/sdretail`** (без префикса `sdmc:`), то есть путь
приложения внутри системы, а не хостовый. Значит данные игры должны лежать в
`/switch/sdretail/game_root` = `sdmc:/switch/sdretail/game_root` эмулятора (у нас — junction на уже
скопированные 6 ГБ).

После этого в логе:

```
[io] mounted '/switch/sdretail/game_root' at \Device\Harddisk0\Partition1: 92 entries in 32.1 ms
Vulkan sparse binding is not used for shared memory emulation - video memory usage may increase
Guest memory arena mapped: virtual base 0x…1000, physical base 0x…1000
```

и на экране — **собственный экран загрузки Sleeping Dogs**: чёрный фон, надпись «Запуск…» и полоса
прогресса. Скриншоты: `nx\eden_game1.png`, `nx\eden_game2.png`.

Рабочая команда (GUI Eden грузит NRO по аргументу; `eden-cli.exe` работает без окна):

```powershell
& "D:\CODE\EDEN\eden.exe" "D:\CODE\SDNX\nx\sdretail.nro"
```

### Что осталось

Игра стоит на «Запуск…» — прогресс-бар двигается, процесс CPU-активен, но кадров пока нет
(`0 FPS`, `Кадр: nan мс`) и новых строк в логе после монтирования нет. Дальше разбирать:
ожидание ассетов, тредов или инициализации звука.

Резервные пути: реальная консоль (NRO и раскладка готовы) либо отладка загрузки через раннюю
трассировку `fopen` в фиксированный файл.

### Почему эмуляторы принципиально не подходят: NVDRV

Включили подробный лог (`sdretail.toml` рядом с NRO: `log_level = "debug"`). Видно, что рантайм
поднимается полностью:

```
Audio system initialized
[io] mounted '/switch/sdretail/game_root' … 92 entries
Registered symbolic links: game:, d:
GPU system initialized (presentation=true)
Runtime initialized successfully
Initializing function table at 830B0000, size 01AB80B0 …   ← 20 секунд (82 тыс. регистраций)
Function table initialized for module: code=821B0000-82EFC058
```

Дальше — тишина в гостевом логе, а лог **Eden** вырастает до 105 МБ за 5 минут:

```
878 979 строк
869 490 × HW.Memory <Error> Unmapped Write64 @ 0x00000b4d88101c = 0x…
  9 105 × Unmapped Write32 / 55 × Write8 / 12 × Read32
```

Первая такая запись идёт на 2.33 с — сразу после инициализации NVDRV:

```
Service.NVDRV Initialize: (STUBBED) called
Service.NVDRV SetChannelPriority: called, priority=64
Service.NVDRV SetErrorNotifier: (STUBBED) called
```

Адреса вида `0x00000b4d…` — это **GPU-адреса виртуальной памяти NVK**: драйвер пишет в своё
адресное пространство, ожидая, что `nvMap`/`nvHost` реально замапят его. Эмулятор эти операции не
реализует (STUBBED), поэтому записи уходят в никуда, а драйвер бесконечно их повторяет.

Вывод: **NVK нельзя запустить под эмулятором.** Ryujinx падал на VA bind, Eden проходит дальше, но
его NVDRV — заглушка. Оба эмулятора реализуют GPU на своей стороне (HLE) и не дают гостевому
драйверу настоящий NV-сервис.

Значит проверять NRO можно только на **реальной консоли**. Артефакт и раскладка готовы:

```
SD:/switch/sdretail/sdretail.nro     (85.7 МБ)
SD:/switch/sdretail/game_root/       (6.09 ГБ данных Steam-free retail-версии)
```

Запуск — **только с полной памятью приложения**: через hbmenu, открытый поверх игры с зажатой R (title override),
или через форвардер NSP (39-bit address space). Из альбома (applet mode) процесс получает ~400 МБ, а одной
физической памяти гостя нужно 512 МБ — игра не стартует (masseffect-nx `docs/platform-notes.md`; в прежней
версии этого документа было просто «через hbmenu» — это ошибка).

FAT32 на SD: самый большой файл данных — `Game.big` 1.69 ГБ, лимит 4 ГБ не мешает.

---

## 9. Полезные пути

```
D:\CODE\masseffect-nx\           референсный порт (sdk/, mesa/, tools/, docs/)
D:\CODE\rexglue-src\             наш ReXGlue SDK (исходники + инструментация)
D:\CODE\SDNX\mesa-sdk\           собранный NVK SDK
D:\CODE\SDNX\nx\hello\           hello-NRO
D:\CODE\SDNX\port\tools\wsl_*.sh обёртки для сборок в WSL
```

---

## 10. Быстрая кодогенерация и сборка NRO (2026-11-08)

Исходники NRO теперь лежат в `D:\CODE\SDNX\nx\sdretail\` (раньше были только в WSL `~/masseffect-nx/sdretail`).
Два скрипта (запускать в WSL):

```bash
bash /mnt/d/CODE/SDNX/nx/tools/sd_codegen.sh      # синхронизация → codegen → пост-патчи → проверки регистров
SUDO_PW=... bash /mnt/d/CODE/SDNX/nx/tools/sd_build_nro.sh   # сборка в devkitpro/devkita64 → nx/sdretail.nro
```

Что добавлено к кодогенерации (`nx/sdretail/perf_overrides.toml`, по образцу masseffect-nx, где это дало
~−13 % инструкций): регистры гостя как локальные переменные C++ (`cr/xer/ctr/non_volatile/non_argument_as_local`),
без `mfmsr/mtmsrd` и глобальной блокировки вокруг атомарных пар (`skip_msr`, `reserved_as_local`), без записи `lr`
перед вызовами, `dcbt` → prefetch. Пост-патчи: `direct_calls.py` (прямые вызовы, их может инлайнить компилятор) и
`pch_no_volatile.py` (доступ к памяти гостя без `volatile`).

Проверки, без которых эти опции опасны (результат записан в `perf_overrides.toml`):

| Проверка | Нашла | Решение |
|---|---|---|
| все `REX_ENTER_GLOBAL_LOCK` охватывают одну пару lwarx/stwcx. | 1121 из 1121, вызовов внутри нет | `skip_msr` безопасен (атомарность — через CAS) |
| `read_before_write.py` (r14–r31, cr, f, v) | 5 функций: 2 × `xor r30,r30,r30`, `subfe r,r29,r29`, guest setjmp, 4-байтный фрагмент без вызовов | безвредно, без пометок |
| `read_before_write.py --volatile` (r0/r11/r12) | `sub_82C0AAA4` (`_alloca_probe`, размер в r12) и 5 свёрнутых фрагментов `0x8261EBB0…EE00` (читают r11 родителя) | `share_registers = true` |

Предыдущий NRO (без этих опций) сохранён как `nx/sdretail_v010_xenos.nro` — для сравнения на консоли.

### Запуск на консоли (порядок проверки)

1. На SD: `sdmc:/switch/sdretail/sdretail.nro` и `sdmc:/switch/sdretail/game_root/` (распакованные данные;
   перед копированием можно применить `port/tools/apply_branding.py`, см. [branding.md](branding.md)).
2. Рядом с NRO `sdretail.toml` с `log_level = "debug"` для первого прогона.
3. Запуск **с полной памятью**: hbmenu через любую игру с зажатой R (title override) или форвардер (39-bit).
4. Принести `sdmc:/switch/sdretail/logs/sdretail_*.log`. Ожидание по опыту masseffect-nx: штатный Xenos-рендер
   даёт единицы fps; дальше — собственный рендерер.

## 11. Патчи SDK (применять после каждого обновления `~/masseffect-nx/sdk` и `D:\CODE\me-sdk`)

```bash
for f in fix_tfetch_lod fix_draw_extent fix_stencil_bit fix_resolve_uint fix_tfetch_numformat; do
  python3 /mnt/d/CODE/SDNX/nx/tools/$f.py ~/masseffect-nx/sdk
done
```

Все идемпотентны. Что чинит каждый — в docstring скрипта и в `portlog.md` §16.5–16.9.

Запуск ПК-стенда (тот же рендер, что на Switch): `nx\desktop\out\run_sdretail.bat` (геймпад) или
`run_sdretail_keyboard.bat`.
