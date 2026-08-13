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
//     сжатый. Разбор потока (ledger/0020): запись — пара «счётчик, значение»,
//     счётчик c <= 0xf5 даёт серию длиной c+3; c > 0xf5 означает c-0xf5
//     литеральных байт следом. Серия обрывается на конце строки картинки.
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
};

/// Объект на странице: кнопка, многоугольник, картинка.
///
/// Координаты в книге — в 1/1440 дюйма; книга 640×480 точек при 96 точках на
/// дюйм, поэтому единица = 15 и **все координаты кратны 15**. Кратность и
/// служит проверкой при поиске прямоугольника (ledger/0028).
struct Object {
	uint32 offset = 0;      ///< смещение имени в книге
	Common::String name;
	Common::Rect rect;      ///< в точках экрана
};

/// Страница книги.
struct Page {
	uint32 anchor = 0;         ///< смещение свойства ASYM_TpID
	int background = -1;       ///< индекс в списке картинок, -1 если не найден
	Common::String name;       ///< имя страницы, если распознано
	Common::Array<Common::String> handlers; ///< имена обработчиков рядом с якорем
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
	const Common::Array<Record> &records() const { return _records; }
	const Common::Array<Object> &objects() const { return _objects; }
	const Common::Array<Common::String> &classNames() const { return _classNames; }
	uint segmentCount() const { return _segmentCount; }

	/// Разворачивает картинку в поверхность. Владение переходит вызывающему.
	/// Ноль — данные не нашлись или глубина не поддержана.
	Graphics::Surface *decodeImage(Image &img, Graphics::Palette &palette);

	/// Ищет начало пиксельных данных: у части картинок они лежат не сразу за
	/// палитрой. Признак верного начала — распаковка даёт ровно rawSize байт,
	/// израсходовав примерно compSize. Результат запоминается в img.pixels.
	bool locatePixels(Image &img);

private:
	void scanRecords();
	void scanImages();
	void scanPages();
	void scanPageText();
	void scanObjects();
	void scanClassNames();

	Common::Array<Object> _objects;

	Common::Array<byte> _data;
	Common::Array<Image> _images;
	Common::Array<Page> _pages;
	Common::Array<Record> _records;
	Common::Array<Common::String> _classNames;
	uint _segmentCount = 0;
};

/// Перекодировка строки из CP1251 (кодировка русского издания) в UTF-8.
Common::String cp1251ToUtf8(const Common::String &s);

} // End of namespace ToolBook

#endif
