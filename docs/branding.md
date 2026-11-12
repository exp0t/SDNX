# Брендинг Switch: текст «Nintendo Switch» и кнопки Switch

Патч данных игры (`game_root`), а не кода: меняются только записи архивов `UI.big` и `SeekFreeBoot.big`.
Инструмент: `port/tools/apply_branding.py`. Проверено на копии данных (применение → чтение → откат
байт-в-байт). **В игре не проверено** — нужен прогон на консоли или десктопной сборке.

> **2026-11-10: раскладка по умолчанию — по букве.** Switch A (правая) = «принять», B (нижняя) = «назад»,
> X = X, Y = Y; NRO больше не включает `input_xbox_layout`. Данные патчить с **`--by-letter`**, иначе иконки не
> совпадут с кнопками. Раскладка по положению — `input_xbox_layout = true` в `sdretail.toml` и патч без ключа.

## Как применить

```powershell
# к своей распакованной папке данных (та, что потом копируется на SD как game_root)
py -3.14 D:\CODE\SDNX\port\tools\apply_branding.py D:\путь\к\game_root

# откат (возвращает файлы байт-в-байт)
py -3.14 D:\CODE\SDNX\port\tools\apply_branding.py D:\путь\к\game_root --restore

# проверка: какие строки и картинки сейчас в архивах
py -3.14 D:\CODE\SDNX\port\tools\apply_branding.py D:\путь\к\game_root --verify D:\tmp\verify
```

Нужны Python 3 + `pillow` + `numpy`. Ключи: `--by-letter` (буквы как на Xbox, для `input_xbox_layout = false`),
`--no-text`, `--no-buttons`, `--no-fps-menu`. Повторный запуск сначала откатывает прошлый патч.
Тот же инструмент добавляет строку «ОГРАНИЧЕНИЕ FPS» в «Настройки → Экран» — см. [fps-limit.md](fps-limit.md).

## Что меняется

| | Где | Сколько |
|---|---|---|
| Текст | таблицы локализации, все 7 языков | 10 строк × 7 языков в `UI.big` + 10 английских в `SeekFreeBoot.big` |
| Кнопки | текстуры `BUTTON_*` | 15 из 25 (A/B/X/Y и их HOLD, L/R/ZL/ZR, «+»/«−», пары L R и ZL ZR) |

Правила замены текста (`port/branding/switch_text.json`):

| Было | Стало |
|---|---|
| консоль Xbox 360 | консоль Nintendo Switch |
| Магазин / Marketplace Xbox LIVE | Nintendo eShop |
| кнопка Xbox Guide | кнопка HOME |
| войдите в Xbox LIVE | подключите консоль Nintendo Switch к Интернету |

Других упоминаний Xbox в текстах нет: проверены все таблицы `UI`, `SeekFreeBoot`, `Global`, `Game`.
В XEX пользовательских строк с Xbox нет (только отладочные).

Кнопки — свои рисунки (`port/tools/make_switch_buttons.py`, штриховой шрифт в коде, без чужих шрифтов и
картинок). Стики, нажатия стиков и D-pad оставлены: на Switch они означают то же самое.

| Текстура игры | Xbox | Switch (по умолчанию, по положению) | `--by-letter` |
|---|---|---|---|
| `BUTTON_ACCEPT` | A (нижняя) | B | A |
| `BUTTON_BACK` (+`_HOLD`) | B (правая) | A | B |
| `BUTTON_BUTTON1` (+`_HOLD`) | X (левая) | Y | X |
| `BUTTON_BUTTON2` (+`_HOLD`) | Y (верхняя) | X | Y |
| `BUTTON_L1` / `BUTTON_R1` | LB / RB | L / R | L / R |
| `BUTTON_L2` / `BUTTON_R2` | LT / RT | ZL / ZR | ZL / ZR |
| `BUTTON_SELECT` / `BUTTON_START` | Back / Start | − / + | − / + |

Управление тоже по положению: NRO по умолчанию включает `input_xbox_layout = true` (нижняя кнопка Switch B
работает как Xbox A, правая A — как B, левая Y — как X, верхняя X — как Y), и иконки называют кнопку Switch на
этом месте. Явное `input_xbox_layout = false` в `sdretail.toml` возвращает раскладку по букве — тогда данные
патчить с `--by-letter`.

## Форматы (найдено на retail-данных)

**BIX/BIG** (`port/tools/sdbig.py`). `NAME.bix` — один qChunk (big-endian) с `BIGFileIndex`:
число записей и смещение таблицы по `0x58/0x5C`; запись 24 байта
`{uid, offset/4, load_offset, compressed_size, compressed_extra, uncompressed_size}`, отсортирована по uid.
Данные записи — `big[offset*4 + (load_offset & 0xFFF)]`. При `compressed_size = 0` данные лежат как есть,
иначе это поток UFG LZ с **little-endian** заголовком `PMCQ` (QCMP) — алгоритм из
`D:\CODE\SDDENX\TheoryEngine-main\quark\compresslz.hh`. Распаковка проверена на всех 4097 записях `UI.big`.

Патчер дописывает изменённые записи в конец `.big` (выравнивание 4 КБ, без сжатия — как родные несжатые записи)
и перенаправляет на них запись `.bix`. Оригинальные байты остаются на месте, откат = обрезать `.big` и вернуть
`.bix.orig`.

**Таблица локализации** (`port/tools/sdtext.py`), чанк `0x90CE6B7A`, имя `<язык>_<таблица>`
(`russian_Global`, `english_Front-End`, …): `+0x50` число строк, `+0x54` смещение таблицы (всегда `0x58`),
записи `{hash, rel}` (хэш одинаков во всех языках), текст UTF-8 с нулём, каждая строка выровнена на 4.
Пересборка совпадает с оригиналом байт-в-байт на всех 2856 таблицах `UI.big`.

Кнопки в тексте — `<img src='BUTTON_ACCEPT' …>`; ролик Scaleform `Buttons` (`CFX`) ссылается на внешние
текстуры `BUTTON_*.tga`, которые движок берёт из ресурсов-текстур с тем же именем.

**Текстуры** (`port/tools/sdtex.py`, `port/tools/xtex.py`). Заголовки `Illusion::Texture` (чанк `0xCDBFA090`)
кнопок — в записи `0x14B0D2FB`, пиксели — в парной temp-записи `0xFBEDD2FF` (обе есть в `UI.big` и
`SeekFreeBoot.big`). Формат `+0x54` (3 = DXT5), размер `+0x5C`, позиция/размер данных `+0x80/+0x84`.
Пиксели: тайлинг Xenos (`XGAddress2DTiled`, по блокам 4×4, выравнивание 32×32 блока) и перестановка байт
в 16-битных словах. Декодирование проверено визуально по всем 25 кнопкам.
