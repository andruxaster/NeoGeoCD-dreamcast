# Выделение из dc-poker-ssrpg-r13

Новый проект: NGCD-DC 0.1.0. Исходный архив не изменён.

| Исходный путь в `dcp` | Новый путь |
| --- | --- |
| `src/ngcd/*.[ch]` | `src/core/` |
| `src/ngcd/dc/` | `src/dreamcast/` |
| `src/common/frontend_dc.c` | `src/dreamcast/main.c` |
| `src/common/machine.h` | `src/dreamcast/machine.h` |
| `src/ssrpg/machine_ssrpg.c` | `src/dreamcast/machine_ngcd.c` |
| `src/ngcd/host/{main,cpu_musashi,z80_zazu}.c` | `src/host/` |
| `src/ffight/fame/` | `third_party/fame/` |
| `src/ffight/raze/` | `third_party/raze/` |
| `src/ngcd/host/musashi/` | `third_party/musashi/` |
| `src/ngcd/host/z80.[ch]` | `third_party/z80/` |
| `src/ampoker2/LICENSE.z80` | `third_party/z80/LICENSE.txt` |
| `tools/ngcdconv/` | `tools/ngcdconv/` |

Убраны American Poker II, Magic Card II, Final Fight, TMNT, CPU benchmark
и общий AY, который Neo Geo CD не использует.

Изменения исходников ограничены путями include, комментариями, названием
эмулятора в логах/дескрипторе, переименованием `AMP_PERF`/`AMP_RELEASE` в
`NGCD_PERF`/`NGCD_RELEASE` и явным подключением заголовка BIOS-шрифта.
CPU-ядра, включая исправленный FAME, перенесены без изменения байтов.
Код конвертера и его Windows EXE также перенесены без изменений.

Ядро Neo Geo CD сохранено без функциональных изменений. Изменён только
вводный комментарий `ngcd.h`. Сохранены имена дисковых файлов, формат NGA,
частоты, настройки рендера, `NGCDBAK` и VMU app_id `dc-poker ngcd`.

Новые файлы: корневой Makefile, правила в `mk/`, Makefile конвертера,
интеграционный тест, VERSION, .gitignore и документация.

Создание CDI, игровой браузер, новые эмулируемые функции и исправления
унаследованных ошибок не входят в это выделение.
