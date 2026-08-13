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

#include "common/debug.h"
#include "common/stream.h"
#include "common/textconsole.h"
#include "graphics/palette.h"
#include "graphics/surface.h"

#include "toolbook/book.h"

namespace ToolBook {

static const byte kSignature[4] = { 0x03, 'J', 'B', 'O' };
static const char kPageAnchor[] = "ASYM_TpID";

static inline uint16 readU16(const byte *p) { return p[0] | (p[1] << 8); }
static inline uint32 readU32(const byte *p) {
	return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

// Отсеиваем «строки», которые на деле пиксельный мусор: у настоящего
// идентификатора есть гласная и хоть какое-то разнообразие символов.
static bool looksLikeIdentifier(const Common::String &s) {
	if (s.size() < 3)
		return false;

	bool vowel = false;
	uint distinct = 0;
	bool seen[256];
	memset(seen, 0, sizeof(seen));

	for (uint i = 0; i < s.size(); i++) {
		char c = s[i];
		if (strchr("aeiouyAEIOUY", c))
			vowel = true;
		if (!seen[(byte)c]) {
			seen[(byte)c] = true;
			distinct++;
		}
	}
	return vowel && distinct > 2;
}

static bool identifierAt(const Common::Array<byte> &data, uint pos, Common::String &out, uint &end) {
	if (pos >= data.size())
		return false;
	byte first = data[pos];
	if (!Common::isAlpha(first) && first != '_')
		return false;

	uint i = pos;
	while (i < data.size() && (Common::isAlnum(data[i]) || data[i] == '_'))
		i++;
	if (i >= data.size() || data[i] != 0 || i - pos < 3 || i - pos > 31)
		return false;

	out = Common::String((const char *)&data[pos], i - pos);
	end = i + 1;
	return looksLikeIdentifier(out);
}

Book::Book() {}
Book::~Book() {}

bool Book::load(Common::SeekableReadStream *stream, uint32 embeddedOffset) {
	if (!stream)
		return false;

	uint32 start = 0;
	byte sig[4];
	stream->seek(0);
	stream->read(sig, 4);
	if (memcmp(sig, kSignature, 4) != 0) {
		// В игре книга зашита внутрь исполняемого файла.
		stream->seek(embeddedOffset);
		if (stream->read(sig, 4) != 4 || memcmp(sig, kSignature, 4) != 0) {
			warning("ToolBook: сигнатуры книги нет ни по нулю, ни по 0x%x", embeddedOffset);
			return false;
		}
		start = embeddedOffset;
	}

	uint32 size = stream->size() - start;
	_data.resize(size);
	stream->seek(start);
	if (stream->read(_data.begin(), size) != size) {
		warning("ToolBook: книга не дочиталась");
		return false;
	}

	debug(1, "ToolBook: книга %u байт со смещения 0x%x", size, start);

	scanRecords();
	scanImages();
	scanObjects();
	scanPages();
	scanClassNames();

	debug(1, "ToolBook: записей %u (сегментов %u), картинок %u, страниц %u, классов %u",
			_records.size(), _segmentCount, _images.size(), _pages.size(), _classNames.size());
	return true;
}

void Book::scanRecords() {
	const uint n = _data.size();
	uint32 lastBase = 0xffffffff;

	for (uint i = 2; i + 10 < n;) {
		if (_data[i] == 0x2a && _data[i + 1] == 0x00 && _data[i + 2] == 0x01 &&
				_data[i + 3] == 0x02 && _data[i + 5] == 0x00) {
			uint32 recSize = readU32(&_data[i + 6]);
			if (recSize > 0 && recSize < 0x400000 && i + 10 + recSize <= n) {
				Record rec;
				rec.offset = i;
				rec.type = _data[i + 4];
				rec.size = recSize;
				rec.body = i + 10;
				rec.segmentOffset = readU16(&_data[i - 2]);
				rec.segmentBase = i - rec.segmentOffset;
				rec.hasBitmap = rec.body + 18 <= n &&
						_data[rec.body + 16] == 'B' && _data[rec.body + 17] == 'M';
				_records.push_back(rec);

				if (rec.segmentBase != lastBase) {
					lastBase = rec.segmentBase;
					_segmentCount++;
				}

				i += 10 + recSize;
				continue;
			}
		}
		i++;
	}
}

void Book::scanImages() {
	const uint n = _data.size();

	for (uint i = 0; i + 40 <= n; ) {
		if (!(_data[i] == 0x28 && _data[i + 1] == 0 && _data[i + 2] == 0 && _data[i + 3] == 0)) {
			i++;
			continue;
		}

		int32 w = (int32)readU32(&_data[i + 4]);
		int32 h = (int32)readU32(&_data[i + 8]);
		uint16 planes = readU16(&_data[i + 12]);
		uint16 bpp = readU16(&_data[i + 14]);
		uint32 comp = readU32(&_data[i + 16]);

		bool plausible = planes == 1 && comp <= 2 &&
				(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32) &&
				w > 0 && w <= 2048 && h != 0 && ABS(h) <= 2048;

		if (!plausible) {
			i++;
			continue;
		}

		Image img;
		img.offset = i;
		img.width = w;
		img.height = ABS(h);
		img.depth = bpp;
		img.colors = readU32(&_data[i + 32]);
		if (img.colors == 0 && bpp <= 8)
			img.colors = 1u << bpp;
		img.stride = ((w * bpp + 31) / 32) * 4;
		img.rawSize = img.stride * img.height;

		// Несжатой считаем только ту картинку, перед которой стоит файловая
		// шапка BM с согласованным bfOffBits: у таких пиксели лежат сразу за
		// палитрой как есть.
		if (i >= 14 && _data[i - 14] == 'B' && _data[i - 13] == 'M') {
			uint32 offBits = readU32(&_data[i - 14 + 10]);
			img.raw = (offBits == 14 + 40 + img.colors * 4);
		}
		if (img.raw)
			img.pixels = i + 40 + img.colors * 4;

		// Перед заголовком лежит пара размеров: распакованный и сжатый. По ней
		// и опознаётся сжатый поток — и она же служит проверкой при поиске
		// начала данных.
		for (uint32 back = 8; back <= 0x140 && back <= i; back++) {
			if (readU32(&_data[i - back]) != img.rawSize)
				continue;
			uint32 comp = readU32(&_data[i - back + 4]);
			if (comp > 0 && comp <= img.rawSize * 2) {
				img.compSize = comp;
				break;
			}
		}

		if (img.rawSize > 0 && (img.raw || img.compSize))
			_images.push_back(img);

		i += 40;
	}
}

// Единица координат книги: 1/1440 дюйма, книга 640×480 при 96 точках на дюйм.
static const int kUnit = 15;

void Book::scanObjects() {
	const uint n = _data.size();

	// Запись объекта кончается на `<u16 next><u32 self><имя\0>`; прямоугольник
	// из четырёх u16 лежит перед ней, но на расстоянии, зависящем от класса
	// объекта. Опознаётся по кратности координат пятнадцати (ledger/0028).
	for (uint i = 64; i + 2 < n; i++) {
		byte c = _data[i];
		if (!Common::isAlpha(c) && c != '_')
			continue;
		if (Common::isAlnum(_data[i - 1]) || _data[i - 1] == '_')
			continue;

		uint e = i;
		while (e < n && (Common::isAlnum(_data[e]) || _data[e] == '_'))
			e++;
		if (e >= n || _data[e] != 0 || e - i < 2 || e - i > 24)
			continue;

		uint32 self = readU32(&_data[i - 4]);
		if (self == 0 || self >= 0x10000)
			continue;

		for (uint back = 6; back < 48; back++) {
			const byte *p = &_data[i - 4 - back];
			int l = readU16(p), t = readU16(p + 2), r = readU16(p + 4), b = readU16(p + 6);
			if (l % kUnit || t % kUnit || r % kUnit || b % kUnit)
				continue;
			if (l >= r || t >= b || r > 640 * kUnit || b > 480 * kUnit)
				continue;
			if (r - l < 2 * kUnit || b - t < 2 * kUnit)
				continue;

			Object obj;
			obj.offset = i;
			obj.name = Common::String((const char *)&_data[i], e - i);
			obj.rect = Common::Rect(l / kUnit, t / kUnit, r / kUnit, b / kUnit);
			_objects.push_back(obj);
			break;
		}
		i = e;
	}
}

void Book::scanPages() {
	// Страницы разделяются полноэкранными фонами: объекты, лежащие между двумя
	// соседними фонами, принадлежат одной странице.
	Common::Array<uint> fullscreen;
	for (uint k = 0; k < _images.size(); k++)
		if (_images[k].width == 640 && _images[k].height == 480 && _images[k].depth == 8)
			fullscreen.push_back(k);

	uint obj = 0;
	for (uint f = 0; f < fullscreen.size(); f++) {
		Page page;
		page.background = fullscreen[f];
		uint32 from = _images[fullscreen[f]].offset;
		uint32 to = (f + 1 < fullscreen.size()) ? _images[fullscreen[f + 1]].offset : _data.size();

		while (obj < _objects.size() && _objects[obj].offset < from)
			obj++;
		for (uint k = obj; k < _objects.size() && _objects[k].offset < to; k++)
			page.objects.push_back(_objects[k]);

		// Имя страницы: объект во весь экран обычно назван по странице.
		for (uint k = 0; k < page.objects.size(); k++) {
			const Common::Rect &r = page.objects[k].rect;
			if (r.width() >= 620 && r.height() >= 460) {
				page.name = page.objects[k].name;
				break;
			}
		}
		if (page.name.empty() && !page.objects.empty())
			page.name = page.objects[0].name;

		_pages.push_back(page);
	}

	scanPageText();
}

void Book::scanPageText() {
	const uint n = _data.size();
	const uint anchorLen = sizeof(kPageAnchor) - 1;

	for (uint i = 0; i + anchorLen + 1 < n; i++) {
		if (_data[i] != 'A' || memcmp(&_data[i], kPageAnchor, anchorLen) != 0 || _data[i + anchorLen] != 0)
			continue;

		// Якорь ASYM_TpID есть не у всех страниц (51 против 57 фонов), но там,
		// где он есть, рядом лежат имена обработчиков и текст.
		Page *page = nullptr;
		for (uint k = 0; k < _pages.size(); k++) {
			if (_pages[k].background < 0)
				continue;
			if (_images[_pages[k].background].offset > i)
				break;
			page = &_pages[k];
		}
		if (!page || page->anchor)
			continue;
		page->anchor = i;

		// Имена перед якорем: имена обработчиков страницы.
		uint lo = i > 600 ? i - 600 : 0;
		for (uint p = lo; p < i; ) {
			Common::String id;
			uint end;
			if (identifierAt(_data, p, id, end)) {
				if (!id.equals("true") && !id.equals("false") && !id.equals("script") &&
						!id.hasPrefix("ASYM_"))
					page->handlers.push_back(id);
				p = end;
			} else {
				p++;
			}
		}

		// Текст страницы: русские строки в CP1251 рядом с якорем.
		uint tlo = i > 4096 ? i - 4096 : 0;
		Common::String run;
		for (uint p = tlo; p < MIN((uint)n, i + 4096); p++) {
			byte c = _data[p];
			bool cyr = c >= 0xc0;
			bool filler = c == ' ' || c == ',' || c == '.' || c == '!' || c == '?' ||
					c == '-' || c == ':' || c == ';' || c == '\r' || c == '\n';
			if (cyr || (!run.empty() && filler)) {
				run += (char)(c == '\r' || c == '\n' ? ' ' : c);
			} else {
				if (run.size() >= 24)
					page->text.push_back(cp1251ToUtf8(run));
				run.clear();
			}
		}
		if (run.size() >= 24)
			page->text.push_back(cp1251ToUtf8(run));
	}
}

void Book::scanClassNames() {
	// Таблица имён книги идёт следом за именами сегментов; классы объектов
	// (Page, Button, Field, Viewer…) лежат в ней же.
	int32 tableStart = -1;
	const char *marker = "*ClassTbl*";
	for (uint i = 0; i + 10 < _data.size(); i++) {
		if (_data[i] == '*' && memcmp(&_data[i], marker, 10) == 0) {
			tableStart = i;
			break;
		}
	}
	if (tableStart < 0)
		return;

	uint end = MIN((uint)_data.size(), (uint)tableStart + 0x1000);
	for (uint p = tableStart; p < end; ) {
		Common::String id;
		uint next;
		if (identifierAt(_data, p, id, next)) {
			if (Common::isUpper(id[0]) && id.size() > 3)
				_classNames.push_back(id);
			p = next;
		} else {
			p++;
		}
	}
}

// Распаковка потока картинки, см. book.h и ledger/0021.
static uint32 unpackRLE(const byte *src, uint32 srcLen, byte *dst, uint32 dstLen,
		uint32 stride, uint32 *usedOut) {
	uint32 in = 0, out = 0, col = 0;

	while (in < srcLen && out < dstLen) {
		byte c = src[in++];
		uint32 n;

		if (c <= 0xf5) {
			if (in >= srcLen)
				break;
			byte v = src[in++];
			n = MIN<uint32>(MIN<uint32>(c + 3, stride - col), dstLen - out);
			memset(dst + out, v, n);
			// Хвост серии, не влезший в строку, отбрасывается: серия не
			// переходит на следующую строку.
			out += n;
			n = MIN<uint32>(c + 3, stride - col);
		} else {
			uint32 k = c - 0xf5;
			n = MIN<uint32>(k, dstLen - out);
			for (uint32 j = 0; j < n && in < srcLen; j++)
				dst[out++] = src[in++];
			in += (k > n) ? (k - n) : 0;
			n = k;
		}
		col = (col + n) % stride;
	}

	if (usedOut)
		*usedOut = in;
	return out;
}

bool Book::locatePixels(Image &img) {
	if (img.pixels)
		return true;
	if (!img.compSize || !img.rawSize || img.searched)
		return false;
	img.searched = true;

	const uint32 afterPalette = img.offset + 40 + img.colors * 4;
	// У части картинок между палитрой и данными вклиниваются другие записи,
	// поэтому начало ищется перебором. Признак верного начала — распаковка
	// даёт ровно rawSize байт, израсходовав примерно compSize.
	//
	// Окно намеренно узкое: на корпусе из 400 картинок данные лежали сразу за
	// палитрой у 396, а каждая проверка стоит полной распаковки. С окном в
	// 64 КБ перелистывание страницы подвисало на секунды (ledger/0029).
	const uint32 kSearch = 0x600;
	Common::Array<byte> tmp;
	tmp.resize(img.rawSize);

	for (uint32 off = 0; off < kSearch; off++) {
		uint32 start = afterPalette + off;
		if (start + img.compSize > _data.size())
			break;

		uint32 used = 0;
		uint32 got = unpackRLE(&_data[start], MIN<uint32>(img.compSize + 512, _data.size() - start),
				tmp.begin(), img.rawSize, img.stride, &used);

		if (got == img.rawSize && used + img.height / 2 + 16 >= img.compSize &&
				used <= img.compSize + img.height / 2 + 16) {
			img.pixels = start;
			return true;
		}
	}
	return false;
}

Graphics::Surface *Book::decodeImage(Image &img, Graphics::Palette &palette) {
	if (!locatePixels(img))
		return nullptr;
	if (img.depth != 8 && img.depth != 24) {
		warning("ToolBook: картинка %d бит пока не разворачивается", img.depth);
		return nullptr;
	}
	Common::Array<byte> pixels;
	pixels.resize(img.rawSize);
	memset(pixels.begin(), 0, img.rawSize);

	if (img.raw) {
		if (img.pixels + img.rawSize > _data.size())
			return nullptr;
		memcpy(pixels.begin(), &_data[img.pixels], img.rawSize);
	} else {
		unpackRLE(&_data[img.pixels], MIN<uint32>(img.compSize + 512, _data.size() - img.pixels),
				pixels.begin(), img.rawSize, img.stride, nullptr);
	}

	Graphics::Surface *surf = new Graphics::Surface();
	uint32 stride = img.stride;

	if (img.depth == 8) {
		surf->create(img.width, img.height, Graphics::PixelFormat::createFormatCLUT8());
		palette.resize(256, false);
		for (uint c = 0; c < MIN<uint32>(img.colors, 256); c++) {
			const byte *e = &_data[img.offset + 40 + c * 4];
			// В DIB палитра лежит как BGRX.
			palette.set(c, e[2], e[1], e[0]);
		}
		// DIB хранится снизу вверх.
		for (int y = 0; y < img.height; y++) {
			const byte *src = pixels.begin() + (img.height - 1 - y) * stride;
			memcpy(surf->getBasePtr(0, y), src, MIN<uint32>(stride, (uint32)img.width));
		}
	} else {
		surf->create(img.width, img.height, Graphics::PixelFormat(4, 8, 8, 8, 8, 16, 8, 0, 24));
		for (int y = 0; y < img.height; y++) {
			const byte *src = pixels.begin() + (img.height - 1 - y) * stride;
			uint32 *dst = (uint32 *)surf->getBasePtr(0, y);
			for (int x = 0; x < img.width; x++, src += 3)
				*dst++ = surf->format.ARGBToColor(0xff, src[2], src[1], src[0]);
		}
	}

	return surf;
}

Common::String cp1251ToUtf8(const Common::String &s) {
	// Таблица для 0x80..0xFF. Русское издание пишет тексты в CP1251, а ScummVM
	// показывает UTF-8.
	static const uint16 kHigh[128] = {
		0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
		0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
		0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
		0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
		0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
		0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
		0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
		0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
		0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
		0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
		0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
		0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
		0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
		0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
		0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
		0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
	};

	Common::String out;
	for (uint i = 0; i < s.size(); i++) {
		byte c = (byte)s[i];
		if (c < 0x80) {
			out += (char)c;
			continue;
		}
		uint16 uc = kHigh[c - 0x80];
		if (!uc)
			continue;
		if (uc < 0x800) {
			out += (char)(0xc0 | (uc >> 6));
			out += (char)(0x80 | (uc & 0x3f));
		} else {
			out += (char)(0xe0 | (uc >> 12));
			out += (char)(0x80 | ((uc >> 6) & 0x3f));
			out += (char)(0x80 | (uc & 0x3f));
		}
	}
	return out;
}

} // End of namespace ToolBook
