# Происхождение и уведомления

NGCD-DC 0.1.0 выделен из предоставленного `dc-poker-ssrpg-r13.tar.gz`.
Имеющиеся авторские комментарии и лицензионные тексты сохранены.
Этот файл описывает содержимое исходного архива и не назначает всему
проекту новую общую лицензию.

| Компонент | Происхождение по исходнику | Уведомления в архиве |
| --- | --- | --- |
| FAME SH4 2.1 | Oscar Orallo Pelaez; версия из MAME4ALL DC с исправлениями r13 | Заголовок `third_party/fame/fame.s`; исходный README ссылается на FAME.HTML, которого в r13 нет |
| RAZE-SH4 | Richard Mitton; SH4-версия Oscar Orallo | Заголовок `third_party/raze/raze.s` ссылается на RAZE.TXT и полный пакет RAZE; RAZE.TXT в r13 отсутствует |
| Musashi | Karl Stenerud и участники проекта | `third_party/musashi/LICENSE.txt`, комментарии исходников |
| SoftFloat | John R. Hauser, версия внутри Musashi | `third_party/musashi/softfloat/README.txt`, комментарии исходников |
| superzazu Z80 | Nicolas Allemand | `third_party/z80/LICENSE.txt` (MIT) |
| Neo Geo CD | C-реализация из r13 по NeoCD / neocd_libretro, Fabrice Martinez | Комментарий `src/core/ngcd.h` указывает LGPL-3.0 |
| YM2610 | C-адаптация из r13 по NeoCD и MAME; Jarek Burczynski, Tatsuyuki Satoh | Комментарий `src/core/ym2610.c` указывает GPL-2.0-or-later и содержит авторские сведения |

Полных текстов лицензий для FAME/RAZE и общей лицензии dc-poker исходный
архив не содержал. Недостающие условия не заменялись придуманной лицензией.
Исторические пометки `dc-poker` в стороннем/перенесённом коде оставлены
как сведения о происхождении изменений.
