# Checkdsk Notes

- After every iteration of code changes, rebuild the affected artifacts before finishing the task.
- For utility changes, rebuild the modified utility with its local `Makefile`.
- When changes affect packaged outputs or release contents, regenerate `build/utils.img` and `dist/checkdsk.zip` via `run/create_floppy_image.sh`.
- If a rebuild fails, report the failure clearly and do not present the iteration as complete.

## Development platform
- Разработка ведётся с использованием `sdcc-sprinter-sdk` под Sprinter DSS.
- Если требуемой функциональности нет в SDK, не реализовывать обходные пути самостоятельно: согласовать решение с архитектором (человеком) и оформить feature request к SDK.

## Скрипты сборки и упаковки
- Любой скрипт, который попадает в репозиторий (`run/`, `tools/` и т. п.), **не должен содержать локальных путей**. Пути — относительные от `script_dir`/`repo_root`, либо принимаются через позиционные аргументы или переменные окружения с дефолтами `${VAR:-...}`.
- Для локально-специфичных значений (нестандартный SDCC, кастомные тулчейны и т. п.) делается **отдельный wrapper-скрипт** `*.local.sh` рядом с основным: он выставляет `export VAR=...` и `exec` основной репо-скрипт. Wrapper **не коммитится** — добавлять его явно в `.gitignore`.
- Эталоны для копирования: `/Users/dmitry/dev/zx/sprinter/utils/run/create_floppy_image.{sh,local.sh}` (с `.gitignore`-исключением для `local.sh`) и `/Users/dmitry/dev/zx/sprinter/kode/run/{create_floppy_image,make}.sh` (параметризация через args + env, без отдельного wrapper'а).

## printf и 32-битные значения
- `printf`/`fprintf`/`sprintf` в SDK поддерживают **только 16-битные** целые. Модификатор `l` парсится и игнорируется — `%ld`/`%lu`/`%lx` молча печатают только младшие 16 бит. См. `sdcc-sprinter-sdk/lib/src/stdio/printf.c:6-7,53-54` и `sdcc-sprinter-sdk/docs/en/04_standard_library.md:145-152`.
- **Не использовать** `%ld`/`%lu`/`%lx` в коде — это тихий баг без предупреждения компилятора.
- Для печати `long` / `uint32_t` использовать отдельный хелпер `_utoa32(unsigned long val, char *end, int base, int upper)` из `sdcc-sprinter-sdk/lib/src/stdio/printf_long.c`: записать в локальный буфер (минимум 11 байт для base=10, 9 байт для base=16) и вывести результат через `%s`. При его использовании линкер автоматически подтянет `printf_long.rel` и SDCC-рантайм 32-битного деления (~260 байт).
- На текущий момент `_vprintfmt` сам **не вызывает** `_utoa32` — комментарий в `printf.c:7` про «link with printf_long.rel» не работает без правки самого `printf.c`. Если для проекта нужен полноценный `%ld` напрямую через `printf` — оформить feature request к SDK на восстановление wiring; самописный printf-движок не делать.

## Memory usage
- Подходить к потреблению памяти консервативно и безопасно.
- Если для временной работы нужен большой буфер, запрашивать страничную память у DSS, а после использования обязательно возвращать её обратно.

## Ограничения ФС
Sprinter DSS имеет ограничения на длину имен файлов, поэтому создавай выходные файлы для целевой платформы с учетом маски 8.3

## External reference sources
- You may consult the following local sibling repositories/directories for answers, platform details, and implementation ideas:
  - `/Users/dmitry/dev/zx/sprinter/sdcc-sprinter-sdk`
  - `/Users/dmitry/dev/zx/sprinter/sprinter_bios`
  - `/Users/dmitry/dev/zx/sprinter/Estex-DSS/`
  - `/Users/dmitry/dev/zx/sprinter/utils`
  - `/Users/dmitry/dev/zx/sprinter/kode`
  - `/Users/dmitry/dev/zx/sprinter/sprinter_ai_doc/manual`
  - `/Users/dmitry/dev/zx/sprinter/sources/tasm_071/TASM`
  - `/Users/dmitry/dev/zx/sprinter/sources/fformat/src/fformat_v113`
  - `/Users/dmitry/dev/zx/sprinter/sources/fm/FM-SRC/FM`
- Treat them as reference material only; this repository remains the source of truth for changes you make here.

## Commit Message Notes
- When preparing a commit comment/message, use the format `type: summary`.
- Example: `feat: handle Ctrl+X/Ctrl+Z abort via DSS scan codes`
