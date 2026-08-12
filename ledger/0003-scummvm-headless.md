# 0003 — Семь ведьм в ScummVM: детект и безоконные прогоны

**Дата:** 2026-08-11
**Статус:** ✅
**Тип:** инфраструктура

Своя сборка ScummVM (`scummvm-src/`), только с движком `director`.

**Детект.** Запись в `engines/director/detection_tables.h`: идентификатор
`sevenwitches`, цель `sevenwitches-win-ru`,
`WINGAME1_l("sevenwitches", "", "Prince.exe", "t:0e1975bcf5c000ce3d55e864fc0d8c05", 1851314, Common::RU_RUS, 600)`.
В конфиг обязателен `enable_unsupported_game_warning=false`, иначе модальное
предупреждение ждёт нажатия кнопки.

**Прогон вслепую** (`4-scummvm.sh`). Каждая часть рецепта обязательна:
`SDL_VIDEODRIVER=dummy` убирает окно; `SDL_RENDER_DRIVER=software` и
`SDL_FRAMEBUFFER_ACCELERATION=0` нужны потому, что поверх `dummy` новая SDL не
создаёт рендерер; `-g surfacesdl` — потому что OpenGL в этом режиме недоступен.

**Локальные правки для стенда** (не для апстрима): периодический сброс кадра в
PNG по ключу `framedump_ms`; защиты от падения на отсутствующих поверхностях в
`drawMouse`, `clearOverlay`, `internUpdateScreen`; сценарий ввода из файла
(ключ `inputscript`) с командами `click`, `key` и `sprite <n> <событие>`.

Остаток нестабильности лечится повтором запуска в скрипте. Убивать прогон только
`kill -9`: при фатальной ошибке ScummVM уходит в свою отладочную консоль и
обычный сигнал игнорирует. Один такой висел семь часов и записал 27 тысяч кадров
на 5 ГБ.
