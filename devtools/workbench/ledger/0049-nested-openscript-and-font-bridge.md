# Вложенный OpenScript и мост ресурсов шрифта

## Проверено 13 августа 2026

После формального checkpoint `0048` стартовая страница `begin` уже получала
свой shared `firstIdle`, но VM останавливалась на named dispatch
`getWinPointer`. Этот этап продолжает именно исполнение сериализованных скриптов
книги. Переходы по именам страниц, заранее записанный маршрут игры и подмена
роликов не добавлялись.

## ScriptObject owner и вложенный вызов

У каждого handler из top-level ScriptObject теперь сохраняется formal owner
record. `op6d` читает числовой selector из `[u16 selector][identifier]` и сначала
ищет обработчик в directory текущего ScriptObject.

Для стартового `firstIdle` owner — ScriptObject id 463, record `0x13020e`.
Его selector `0x6224` разрешается в настоящий `getWinPointer` по адресу
`0x1316cf`. Явное D-значение аргумента попадает в local `+8`; receiver/context
сохраняется отдельно. Handler заканчивается альтернативным маркером
`28 1c 66`, поэтому parser помечает его как return-D и VM возвращает верхнее
Value родителю. Маркер в `codeSize` не включается.

Проверено headless-прогоном: старый барьер `getWinPointer` пройден, следующим
достигнутым вызовом стал `GlobalAlloc` внутри самого handler.

## Минимальный Win16 buffer bridge

Реализованы только достигнутые операции:

* `GlobalAlloc(0x42, size)` создаёт размерный zero-filled byte buffer и handle;
* `GlobalLock(handle)` возвращает логический far pointer на этот buffer;
* builtin 116 копирует materialized ToolBook string в buffer с NUL и сохраняет
  его capacity;
* globals OpenScript теперь хранят типизированные `ScriptValue`, поэтому
  `pFileName` не превращается в строку с десятичным числом.

CDB slot 0 представлен отдельным BookRef value (runtime tag class `0x68`). При
достигнутой type-9 coercion он материализуется в каталог открытой книги через
эквивалент `CDBQUERYFILEPATH`; для SearchMan это DOS-relative `.\\`, без утечки
абсолютного macOS-пути в VM.

## CBOOK2.FON и AddFontResource

Выяснилось, что локальная extraction была неполной: оригинальный
`downloaded/iso/bashnya.iso` содержит `CBOOK2.FON` и в корне, и в `DEMO`.
Корневая копия восстановлена в runtime tree. Обе копии побайтно одинаковы:

```
SHA256 85b2bf568083958b615ca80c7ccf7cb2a991857631ec9fea7a5649c97e92c9ab
```

Fallback на `DEMO`, имя игры или искусственный успех не добавлялись.
`AddFontResource` открывает сформированный OpenScript путь через SearchMan,
читает Windows `FONTDIR`, проверяет соответствующие FONT resources и
регистрирует face/point sizes. Headless smoke показал 6 загруженных ресурсов
`CBook # 2`: 8, 10, 12, 14, 18, 24 pt; вызов вернул 6.

## Repro и текущий барьер

Звук отключён через `SDL_AUDIODRIVER=dummy`:

```
cd "/Users/konstantinfastov/Projects/old games/scummvm-src"
make -j4
cd ..
TARGET=bashnya-toolbook DEBUG=2 ./4-scummvm.sh 2
```

Сборка и `git diff --check` проходят. Лог подтверждает:

```
ToolBook: GlobalAlloc 128 bytes -> 1
ToolBook: AddFontResource cbook2.fon -> 6
ToolBook: native function getModulePath пока не реализована @0x1304db
```

Следующий достигнутый барьер — generic native `getModulePath`, после успешной
ветки установки шрифта. Коммиты ScummVM этого этапа:

* `3401cb1d` — nested ScriptObject dispatch и return-D;
* `37932fcf` — Win16 buffers, CDB BookRef materialization и FONTDIR bridge.

