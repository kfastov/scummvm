# Платформенные и MCI-пробы стартового OpenScript

## Проверено 13 августа 2026

Этот checkpoint продолжает исполнять настоящий `firstIdle` ScriptObject 463.
В движок не добавлялись заранее заданные переходы по страницам, игровые
сценарии обхода или подмена роликов. Реализованы только операции, до которых
последовательно дошёл байткод книги.

## Win16 native declarations и platform bridge

Builtin 8 теперь принимает формальный static descriptor: число записей и
10-байтовые определения thunk. Зарегистрированы ровно функции, объявленные
самой книгой из GDI, USER, KERNEL, TB40WIN и TB40DOS. `op6d` разрешает вызовы по
сериализованному selector/name, а не по имени страницы.

Достигнутые платформенные вызовы:

* `displayFonts` перечисляет зарегистрированные ToolBook font resources;
* `AddFontResource` разбирает настоящий `CBOOK2.FON` через Windows resources и
  возвращает 6 загруженных bitmap faces;
* `SendMessage(HWND_BROADCAST, WM_FONTCHANGE, ...)` сохраняет штатный return,
  отдельного OS side effect в ScummVM не требует;
* `getModulePath` и `getFileVersion` дают OpenScript совместимую запись
  Win98 `USER.EXE`, не сведения о host macOS;
* `getWinIniVar`/`setWinIniVar` используют отдельное виртуальное Win.ini
  состояние.

## Достигнутый MCI capability test

В shared handler `myMCITest` реализованы только две реально встреченные формы
packed opcode 71:

* `0x8315`: signed W -> десятибайтовое ToolBook numeric value;
* `0x8351`: десятибайтовое numeric value -> W.

Builtin 163 инкрементирует такой numeric value через scratch output, builtin
206 сравнивает `quantity >= iterator`. Поэтому `sysinfo all name i` вызывается
ровно для индексов `1..N`. Минимальный беззвучный MCI backend отвечает на
`close all`, `sysinfo all quantity` и `sysinfo all name N`, включая WaveAudio и
AVIVideo. Это перечень возможностей платформы, а не список игровых роликов.

Также реализованы достигнутые преобразования строк и проверка ToolBook
`is null`. Неизвестная достигнутая операция по-прежнему останавливает handler,
а не получает фиктивный успех.

## Repro и новый барьер

Тест выполнялся без вывода звука (`SDL_AUDIODRIVER=dummy`):

```
cd "/Users/konstantinfastov/Projects/old games/scummvm-src"
make -j4
cd ..
TARGET=bashnya-toolbook DEBUG=2 ./4-scummvm.sh 3
```

Сборка и `git diff --check` проходят. Лог показывает:

```
ToolBook: AddFontResource cbook2.fon -> 6
ToolBook: builtin 70 @0x130564 пока не реализован
```

То есть стартовый script прошёл font/version/MCI/Win.ini ветви. Следующий
фактически достигнутый барьер — builtin 70 по адресу `0x130564`. Коммит
ScummVM этого checkpoint: `090f853f`.
