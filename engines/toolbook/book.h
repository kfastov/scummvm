/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef TOOLBOOK_BOOK_H
#define TOOLBOOK_BOOK_H

// Разбор книги Asymetrix ToolBook 4.0 (.TBK).
//
// Формат закрытый и открытых реализаций не имеет; всё, что здесь закодировано,
// получено разбором самой книги «Башни знаний» (подробности и опровергнутые
// гипотезы — в ledger/0015 и tools/tbk.py, где живёт та же логика на Python).
//
// Устройство файла, коротко:
//
//   * сигнатура "\x03JBO";
//   * записи с заголовком `2a 00 01 02 <тип> 00 <размер32>`, внутри — два
//     поля-связки, вложенный заголовок и данные;
//   * перед заголовком записи лежит её шестнадцатибитное смещение от начала
//     сегмента: наследство 16-битного адресного пространства. Разница между
//     файловым смещением и этим числом даёт базу сегмента;
//   * картинки описаны обычными DIB (BITMAPINFOHEADER + палитра), а пиксели
//     сжаты своим RLE. Перед заголовком лежит пара размеров: распакованный и
//     сжатый. Разбор потока (ledger/0021, поправка в 0030): запись — пара
//     «счётчик, значение», счётчик c <= 0xf5 даёт серию длиной c+3; c > 0xf5
//     означает c-0xf5 литеральных байт следом. Серия **не** обрывается на конце
//     строки — прежнее правило обрезки давало наклон всей картинки.
//     Данные лежат не всегда сразу за палитрой, поэтому начало ищется перебором
//     со сверкой по обоим размерам;
//   * страницы опознаются по свойству `ASYM_TpID`: следом за ним лежит
//     полноэкранный DIB — фон страницы, а перед ним имена страницы и её
//     обработчиков;
//   * тексты — в CP1251;
//   * скрипты хранятся скомпилированными, опкоды не разобраны.

#include "common/array.h"
#include "common/rect.h"
#include "common/str.h"

namespace Graphics {
struct Surface;
class Palette;
}

namespace Common {
class SeekableReadStream;
}

namespace ToolBook {

/// Заголовок DIB, найденный в книге.
struct Image {
	uint32 offset = 0;   ///< смещение BITMAPINFOHEADER в книге
	int width = 0;
	int height = 0;
	int depth = 0;
	uint32 colors = 0;   ///< число цветов палитры
	uint32 pixels = 0;   ///< смещение пиксельных данных (0 — ещё не найдено)
	uint32 rawSize = 0;  ///< размер распакованных пикселей
	uint32 compSize = 0; ///< размер сжатого потока
	uint32 stride = 0;   ///< длина строки в байтах, выровненная на 4

	/// Правда, если перед заголовком стоит файловая шапка BM: тогда пиксели
	/// лежат несжатыми сразу за палитрой.
	bool raw = false;
	/// Начало данных уже искали и не нашли — второй раз не искать. Перебор
	/// стоит десятки тысяч распаковок, и без этого перерисовка страницы
	/// подвисает на секунды.
	bool searched = false;
	/// Поток найден в виде кусков `[u16 записей][записи]` — так его пишет сама
	/// среда ToolBook (ledger/0039).
	bool chunked = false;
	/// Большой поток хранится как huge-массив 16-битной среды: группы записей
	/// идут подряд, но после смещения > 0xfffc источник переносится на начало
	/// следующей 64-КБ страницы. Пропущенный хвост входит в compSize.
	bool hugeSpan = false;

	/// База сегмента кучи, в котором лежит блок с этим DIB. Выводится из
	/// заголовка блока: длина блока известна (4 + 40 + палитра), значит
	/// база = конец блока − поле «конец» (ledger/0031).
	uint32 segBase = 0;
};

/// Формальная куча одной страницы/фона ToolBook. Ссылки внутри неё —
/// 16-битные handles; адрес блока равен ``segmentBase + handle - 3``.
struct HeapSegment {
	uint32 base = 0;
	uint32 end = 0;       ///< первый байт после цепочки блоков
	uint16 type = 0;      ///< 4 = Background, 5 = Page
	uint32 id = 0;
	uint16 selector = 0;  ///< дальний selector, уникален среди root одного типа
	Common::String name;
};

/// Обработчик события объекта: имя события и строки из его таблицы.
struct Handler {
	uint32 code = 0;                 ///< смещение кода в книге
	uint32 codeSize = 0;             ///< длина кода до маркера таблицы строк
	uint32 ownerScriptRecord = 0;    ///< formal top-level ScriptObject record, if any
	uint16 eventHash = 0;            ///< hash from the owning script directory
	bool returnsValue = false;       ///< record ends in the alternate return-D marker
	Common::String name;             ///< buttonClick, mouseEnter, enterPage…
	Common::Array<Common::String> literals;  ///< строковые литералы
	Common::Array<Common::String> messages;  ///< имена (с хешем) — сообщения и свойства
};

/// A top-level ToolBook script record. These records use the same event
/// directory as local page/object scripts, but are addressed through a far
/// handle/selector pair by the compiled OpenScript dispatcher.
struct ScriptObject {
	uint32 record = 0;       ///< outer type-1 record header
	uint32 script = 0;       ///< serialized script header at record + 0x10
	uint16 handle = 0;       ///< far handle from the formal ScriptObject id table
	uint16 selector = 0;     ///< filled from a formal far-reference table
	uint32 id = 0;           ///< serialized object id, if indexed
	Common::Array<Handler> handlers;
};

/// Объект на странице: кнопка, многоугольник, картинка.
///
/// Координаты в книге — знаковые величины в 1/1440 дюйма; книга 640×480
/// точек при 96 точках на дюйм, поэтому 15 единиц дают один пиксель. Реальные
/// объекты могут иметь субпиксельные и отрицательные координаты.
struct Object {
	uint32 offset = 0;      ///< смещение имени в книге
	uint32 block = 0;       ///< начало typed object block
	uint32 segmentBase = 0;
	uint16 handle = 0;
	uint16 parentHandle = 0;
	uint16 type = 0;
	uint32 id = 0;
	Common::String name;
	Common::Rect rect;      ///< в точках экрана
	/// Обвод для многоугольных областей (ledger/0037). Пусто у прямоугольных.
	Common::Array<Common::Point> outline;
	bool picture = false;   ///< за именем идёт блок DIB, а не список вершин
	int image = -1;         ///< индекс DIB для Picture
	bool field = false;
	uint32 textBlock = 0;
	uint16 textCapacity = 0;
	Common::String initialText;
	bool ownVisible = true; ///< serialized flags+0x23 bit 0, without ancestors
	bool visible = true;    ///< initial effective visibility, including ancestors
	/// Обработчики объекта: bytecode, event selector and string table,
	/// resolved through the object's formal local script handle.
	Common::Array<Handler> handlers;
};

/// Страница книги.
struct Page {
	uint32 segmentBase = 0;
	uint32 backgroundSegmentBase = 0;
	uint32 id = 0;
	uint16 selector = 0;       ///< far selector of the serialized Page root
	uint32 backgroundId = 0;
	int nextPage = -1;         ///< index resolved from the formal page-order table
	uint16 canvasWidth = 640;   ///< referenced Background client size, pixels
	uint16 canvasHeight = 480;
	/// Viewer placement is runtime state, not implied by a small Background's
	/// client size. It remains unset until a formal viewer/transition property
	/// supplies an origin.
	bool hasCanvasOrigin = false;
	int16 canvasX = 0;
	int16 canvasY = 0;
	uint32 anchor = 0;         ///< смещение свойства ASYM_TpID
	int background = -1;       ///< индекс в списке картинок, -1 если не найден
	Common::String name;       ///< имя страницы, если распознано
	Common::Array<Common::String> handlers; ///< имена обработчиков рядом с якорем
	Common::Array<Handler> eventHandlers;   ///< формально привязанные root handlers
	Common::Array<Common::String> text;     ///< текст страницы (CP1251 → UTF-8)
	Common::Array<Object> objects;          ///< объекты, лежащие до следующего фона
};

/// Запись книги.
struct Record {
	uint32 offset = 0;
	uint32 size = 0;
	uint32 body = 0;
	uint16 segmentOffset = 0;
	uint32 segmentBase = 0;
	byte type = 0;
	bool hasBitmap = false;
};

class Book {
public:
	Book();
	~Book();

	/// Загружает книгу. Внутри .EXE она лежит куском со смещения `embeddedOffset`,
	/// поэтому сигнатура ищется и по нулевому смещению, и по нему.
	bool load(Common::SeekableReadStream *stream, uint32 embeddedOffset);

	uint size() const { return _data.size(); }

	const Common::Array<Image> &images() const { return _images; }
	const Common::Array<Page> &pages() const { return _pages; }
	int initialPage() const { return _initialPage; }
	const Common::Array<int> &pageOrder() const { return _pageOrder; }
	const Common::Array<Record> &records() const { return _records; }
	const Common::Array<Object> &objects() const { return _objects; }
	const Common::Array<HeapSegment> &heapSegments() const { return _heapSegments; }
	/// Все обработчики с целой таблицей строк. Пока не привязаны к страницам:
	/// одного соседства в файле для доказуемой привязки недостаточно.
	const Common::Array<Handler> &handlers() const { return _handlers; }
	const Common::Array<ScriptObject> &scriptObjects() const { return _scriptObjects; }
	const ScriptObject *findScriptObject(uint16 handle, uint16 selector) const;
	const ScriptObject *findScriptObjectByRecord(uint32 record) const;
	const Handler *findScriptHandler(uint32 ownerRecord, uint16 selector) const;
	/// Обработчик по адресу его кода (посылка разрешается в адрес, ledger/0069).
	const Handler *findHandlerByCode(uint32 code) const;
	/// Объекты сегмента кучи с таким именем (фон-диалог показывается поверх страницы).
	Common::Array<Object> objectsOfSegment(const Common::String &name) const;
	/// Объекты того сегмента, где лежит объект с таким именем.
	Common::Array<Object> objectsAround(const Common::String &objectName) const;
	/// Обработчик сообщения у названного объекта (посылка с явным получателем).
	const Handler *findMessageHandler(const Common::String &receiver, uint16 selector) const;
	const Common::Array<Common::String> &classNames() const { return _classNames; }
	uint segmentCount() const { return _segmentCount; }
	uint16 readUint16(uint32 offset) const;
	uint32 readUint32(uint32 offset) const;
	const byte *bytes(uint32 offset, uint32 size = 1) const;
	Common::String readString(uint32 offset, uint32 maxLength = 4096) const;
	Common::String handlerString(const Handler &handler, uint32 target) const;

	/// Разворачивает картинку в поверхность. Владение переходит вызывающему.
	/// Ноль — данные не нашлись или глубина не поддержана.
	Graphics::Surface *decodeImage(Image &img, Graphics::Palette &palette);

	/// Ищет начало пиксельных данных. Пиксели лежат не в блоке DIB, а сплошным
	/// потоком за сегментом объектов (ledger/0032), поэтому раскладываются
	/// сразу на весь сегмент: потоки идут подряд и опознаются по паре
	/// размеров. Результат запоминается в img.pixels.
	bool locatePixels(Image &img);

private:
	void scanRecords();
	void scanImages();
	void scanPages();
	void scanPageOrder();
	void scanPageText();
	void scanObjects();
	void scanHeapSegments();
	void scanHandlers();
	void scanScriptObjects();
	void scanScriptObjectIndex();
	void readHandlerStrings(uint32 code, Handler &hd) const;
	void appendScriptHandlers(uint32 script, uint32 scriptEnd,
			Common::Array<Handler> &out, uint32 ownerScriptRecord = 0) const;
	bool resolveLocalScript(uint32 segmentBase, uint32 segmentEnd, uint16 handle,
			uint32 &script, uint32 &scriptEnd) const;
	bool appendScriptBinding(uint32 segmentBase, uint32 segmentEnd, uint16 binding,
			Common::Array<Handler> &out) const;
	void scanClassNames();
	void appendSegmentObjects(uint32 segmentBase, Common::Array<Object> &out) const;
	void appendObjectTree(uint32 segmentBase, uint16 handle, bool parentVisible,
			Common::Array<Object> &out, uint depth) const;
	void appendRootHandlers(uint32 segmentBase, Common::Array<Handler> &out) const;

	Common::Array<Object> _objects;
	Common::Array<HeapSegment> _heapSegments;
	Common::Array<Handler> _handlers;
	Common::Array<ScriptObject> _scriptObjects;

	Common::Array<byte> _data;
	Common::Array<Image> _images;
	Common::Array<Page> _pages;
	Common::Array<int> _pageOrder;
	int _initialPage = -1;
	Common::Array<Record> _records;
	Common::Array<Common::String> _classNames;
	uint _segmentCount = 0;
};

/// Перекодировка строки из CP1251 (кодировка русского издания) в UTF-8.
Common::String cp1251ToUtf8(const Common::String &s);

} // End of namespace ToolBook

#endif
