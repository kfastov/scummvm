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
// Общий префикс кода обработчика OpenScript. Размер кадра и раскладка
// аргументов после него различаются; целостность подтверждается полем strOff
// у маркера таблицы строк, а не длинным шаблоном пролога.
static const byte kHandlerPrefix[] = { 0x26, 0x0f };
static const uint kHandlerPrefixSize = sizeof(kHandlerPrefix);
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

uint16 Book::readUint16(uint32 offset) const {
	return offset + 2 <= _data.size() ? readU16(&_data[offset]) : 0;
}

uint32 Book::readUint32(uint32 offset) const {
	return offset + 4 <= _data.size() ? readU32(&_data[offset]) : 0;
}

const byte *Book::bytes(uint32 offset, uint32 size) const {
	return offset <= _data.size() && size <= _data.size() - offset ? &_data[offset] : nullptr;
}

Common::String Book::readString(uint32 offset, uint32 maxLength) const {
	if (offset >= _data.size())
		return Common::String();
	uint32 end = offset;
	while (end < _data.size() && _data[end] && end - offset <= maxLength)
		end++;
	if (end >= _data.size() || end == offset || end - offset > maxLength)
		return Common::String();
	return Common::String((const char *)&_data[offset], end - offset);
}

Common::String Book::handlerString(const Handler &handler, uint32 target) const {
	if (target < handler.code || target >= _data.size())
		return Common::String();
	// Named references point at a two-byte selector/hash followed by the
	// identifier. A literal points directly at its first character. Prefer the
	// formal reference spelling when the two bytes at target are not printable
	// identifier bytes; this keeps dispatch numeric and still exposes the name
	// for diagnostics/global lookup.
	if (target + 3 < _data.size() &&
			(!Common::isAlpha(_data[target]) && _data[target] != '_') &&
			(Common::isAlpha(_data[target + 2]) || _data[target + 2] == '_'))
		target += 2;
	return readString(target);
}

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
	scanHandlers();
	scanScriptObjects();
	scanScriptObjectIndex();
	scanHeapSegments();
	scanImages();
	scanObjects();
	scanPages();
	scanPageOrder();
	scanClassNames();

	debug(1, "ToolBook: записей %u (сегментов %u), картинок %u, страниц %u, обработчиков %u, top-level scripts %u, классов %u",
			_records.size(), _segmentCount, _images.size(), _pages.size(), _handlers.size(),
			_scriptObjects.size(), _classNames.size());
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
	// Do not scan the whole file for BITMAPINFOHEADER byte patterns: 91 such
	// patterns occur in compressed data.  Every real image is referenced by a
	// Picture block (+0x55 is a local handle to a type-0 DIB block), while the
	// authoritative raw/compressed sizes live in that Picture at +0x2d/+0x31.
	// This relation resolves exactly all 2393 images in this book.
	for (uint s = 0; s < _heapSegments.size(); s++) {
		const HeapSegment &seg = _heapSegments[s];
		for (uint32 p = seg.base + 0x11; p < seg.end; ) {
			uint32 next = seg.base + (readU16(&_data[p]) | 1);
			if (next <= p || next > seg.end)
				break;
			if (readU16(&_data[p + 2]) != 0x15 || next - p < 0x57) {
				p = next;
				continue;
			}

			uint16 dibHandle = readU16(&_data[p + 0x55]);
			uint32 dibBlock = dibHandle >= 3 ? seg.base + dibHandle - 3 : seg.end;
			uint32 dib = dibBlock + 4;
			if (dibBlock + 44 > seg.end || readU16(&_data[dibBlock + 2]) != 0 ||
					readU32(&_data[dib]) != 40) {
				p = next;
				continue;
			}

			int32 w = (int32)readU32(&_data[dib + 4]);
			int32 h = (int32)readU32(&_data[dib + 8]);
			uint16 planes = readU16(&_data[dib + 12]);
			uint16 bpp = readU16(&_data[dib + 14]);
			uint32 compression = readU32(&_data[dib + 16]);
			if (planes != 1 || compression > 2 ||
					!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32) ||
					w <= 0 || w > 2048 || h == 0 || ABS(h) > 2048) {
				p = next;
				continue;
			}

			Image img;
			img.offset = dib;
			img.width = w;
			img.height = ABS(h);
			img.depth = bpp;
			img.colors = readU32(&_data[dib + 32]);
			if (img.colors == 0 && bpp <= 8)
				img.colors = 1u << bpp;
			img.stride = ((w * bpp + 31) / 32) * 4;
			img.rawSize = readU32(&_data[p + 0x2d]);
			img.compSize = readU32(&_data[p + 0x31]);
			img.segBase = seg.base;
			if (img.rawSize != img.stride * img.height) {
				p = next;
				continue;
			}

			bool duplicate = false;
			for (uint i = 0; i < _images.size(); i++)
				if (_images[i].offset == img.offset) {
					duplicate = true;
					break;
				}
			if (!duplicate)
				_images.push_back(img);
			p = next;
		}
	}
}

// Единица координат книги: 1/1440 дюйма, книга 640×480 при 96 точках на дюйм.
static const int kUnit = 15;

static bool isObjectBlockType(uint16 type) {
	switch (type) {
	case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c:
	case 0x0d: case 0x0e: case 0x0f: case 0x10: case 0x12:
	case 0x13: case 0x15: case 0x1a:
		return true;
	default:
		return false;
	}
}

static int unitsToPixel(int16 units) {
	return units >= 0 ? units / kUnit : -((-units + kUnit - 1) / kUnit);
}

void Book::scanHeapSegments() {
	// У root-блока Page/Background всегда handle 0x14: он начинается по
	// segmentBase+0x11, а handle любого блока равен relativeOffset+3. Поле
	// segmentBase+0x0d задаёт конец цепочки. Ссылки нечётные, поэтому и конец,
	// и next block нормализуются OR 1 (проверено на всех 181 DIB-сегментах).
	const uint32 n = _data.size();
	for (uint32 p = 0x11; p + 20 < n; p++) {
		uint16 type = readU16(&_data[p + 2]);
		if (type != 4 && type != 5)
			continue;
		if (_data[p + 4] != 0 || _data[p + 5] != 2)
			continue;
		uint32 base = p - 0x11;
		uint32 firstEnd = base + (readU16(&_data[p]) | 1);
		uint32 heapEnd = base + (readU16(&_data[base + 0x0d]) | 1);
		if (firstEnd <= p || firstEnd > heapEnd || heapEnd > n)
			continue;
		// Не принимаем случайный шаблон в потоке: вся heap-цепочка обязана
		// закончиться ровно на формальном heapEnd.
		uint32 block = p;
		uint count = 0;
		while (block < heapEnd && count++ < 10000) {
			uint32 next = base + (readU16(&_data[block]) | 1);
			if (next <= block || next > heapEnd)
				break;
			block = next;
		}
		if (block != heapEnd)
			continue;

		HeapSegment seg;
		seg.base = base;
		seg.end = heapEnd;
		seg.type = type;
		seg.id = readU16(&_data[p + 6]);
		seg.selector = readU16(&_data[base + 7]);
		uint32 nameAt = p + 10, nameEnd = nameAt;
		while (nameEnd < firstEnd && _data[nameEnd] >= 32 && _data[nameEnd] < 127 &&
				nameEnd - nameAt <= 31)
			nameEnd++;
		if (nameEnd < firstEnd && _data[nameEnd] == 0)
			seg.name = Common::String((const char *)&_data[nameAt], nameEnd - nameAt);
		_heapSegments.push_back(seg);
		p = firstEnd - 1;
	}

	debug(1, "ToolBook: формальных heap-сегментов %u", _heapSegments.size());
}

void Book::scanHandlers() {
	// Тот же строгий критерий, что в tools/osc.py: для каждого общего префикса
	// берём ближайший следующий маркер таблицы строк. Обработчик цел только
	// тогда, когда поле strOff указывает ровно за этот маркер.
	Common::Array<uint32> starts;
	Common::Array<uint32> marks;
	const uint32 n = _data.size();
	for (uint32 i = 0; i + 3 <= n; i++) {
		if (i + kHandlerPrefixSize <= n && memcmp(&_data[i], kHandlerPrefix, kHandlerPrefixSize) == 0)
			starts.push_back(i);
		if (_data[i] == 0x27 && _data[i + 1] == 0x1d && _data[i + 2] == 0x66)
			marks.push_back(i);
	}

	uint markIndex = 0;
	for (uint i = 0; i < starts.size(); i++) {
		const uint32 code = starts[i];
		while (markIndex < marks.size() && marks[markIndex] < code)
			markIndex++;
		if (markIndex >= marks.size())
			break;

		const uint32 marker = marks[markIndex];
		const uint32 codeSize = marker - code;
		if (marker + 5 > n || codeSize > 0xffff ||
				readU16(&_data[marker + 3]) != codeSize + 7)
			continue;

		Handler hd;
		hd.code = code;
		hd.codeSize = codeSize;
		readHandlerStrings(code, hd);
		_handlers.push_back(hd);
	}

	debug(1, "ToolBook: обработчиков с целой таблицей строк %u/%u",
			_handlers.size(), starts.size());
}

// Таблица строк обработчика: за кодом идёт маркер 27 1d 66, поле смещения и
// строки. Имена (сообщения, свойства) идут с двухбайтовым хешем впереди,
// строковые литералы — без него; последняя строка таблицы — имя обработчика.
void Book::readHandlerStrings(uint32 code, Handler &hd) const {
	uint32 m = code;
	while (m + 3 < _data.size() && m < code + 8000) {
		if (_data[m] == 0x27 && _data[m + 1] == 0x1d && _data[m + 2] == 0x66)
			break;
		m++;
	}
	if (m + 5 >= _data.size() || m >= code + 8000)
		return;
	if (readU16(&_data[m + 3]) != (m - code) + 7)
		return;
	hd.codeSize = m - code;

	uint32 p = m + 5;
	Common::Array<Common::String> all;
	Common::Array<bool> hashed;
	while (p < _data.size()) {
		uint32 e = p;
		while (e < _data.size() && _data[e])
			e++;
		if (e == p || e - p > 120)
			break;
		Common::String s((const char *)&_data[p], e - p);
		bool isName = true;
		for (uint i = 0; i < s.size(); i++)
			if (!Common::isAlnum(s[i]) && s[i] != '_')
				isName = false;
		if (!isName && s.size() > 2) {
			Common::String tail(s.c_str() + 2);
			bool ok = !tail.empty();
			for (uint i = 0; i < tail.size(); i++)
				if (!Common::isAlnum(tail[i]) && tail[i] != '_')
					ok = false;
			if (ok) {
				all.push_back(tail);
				hashed.push_back(true);
				p = e + 1;
				continue;
			}
		}
		all.push_back(s);
		hashed.push_back(isName);
		p = e + 1;
	}
	if (all.empty())
		return;
	hd.name = all[all.size() - 1];
	for (uint i = 0; i + 1 < all.size(); i++) {
		if (hashed[i])
			hd.messages.push_back(all[i]);
		else
			hd.literals.push_back(all[i]);
	}
}

void Book::scanScriptObjects() {
	// A top-level script is an outer type-1 record. The first six body bytes
	// identify the record inside its 16-bit segment, and the script header
	// follows at record+0x10. Requiring both the exact self handle and the
	// 0x0225 script signature rejects the other type-1 record variants.
	uint handlerCount = 0;
	for (uint i = 0; i < _records.size(); i++) {
		const Record &record = _records[i];
		if (record.type != 1 || record.offset + 0x28 > _data.size())
			continue;
		uint32 end = record.body + record.size;
		uint32 script = record.offset + 0x10;
		if (end > _data.size() || script + 0x18 > end ||
				_data[record.body] != 0 || _data[record.body + 3] != 0 ||
				readU16(&_data[record.body + 1]) != (uint16)(record.segmentOffset + 3) ||
				readU16(&_data[script + 6]) != 0x0225)
			continue;

		uint16 count = readU16(&_data[script + 0x16]);
		uint32 recordBase = script + 0x1a + (uint32)count * 4;
		if (count > 64 || recordBase > end)
			continue;
		bool valid = true;
		for (uint h = 0; h < count; h++) {
			uint32 handlerRecord = recordBase + readU16(&_data[script + 0x1a + h * 4]);
			if (handlerRecord + 29 >= end || _data[handlerRecord + 29] != 0x26) {
				valid = false;
				break;
			}
		}
		if (!valid)
			continue;

		ScriptObject object;
		object.record = record.offset;
		object.script = script;
		appendScriptHandlers(script, end, object.handlers, record.offset);
		handlerCount += object.handlers.size();
		_scriptObjects.push_back(object);
	}
	debug(1, "ToolBook: формальных top-level script records %u, handlers %u",
			_scriptObjects.size(), handlerCount);
}

const ScriptObject *Book::findScriptObject(uint16 handle, uint16 selector) const {
	for (uint i = 0; i < _scriptObjects.size(); i++)
		if (_scriptObjects[i].handle == handle && _scriptObjects[i].selector == selector)
			return &_scriptObjects[i];
	return nullptr;
}

const ScriptObject *Book::findScriptObjectByRecord(uint32 record) const {
	for (uint i = 0; i < _scriptObjects.size(); i++)
		if (_scriptObjects[i].record == record)
			return &_scriptObjects[i];
	return nullptr;
}

const Handler *Book::findScriptHandler(uint32 ownerRecord, uint16 selector) const {
	const ScriptObject *owner = findScriptObjectByRecord(ownerRecord);
	if (!owner)
		return nullptr;
	for (uint i = 0; i < owner->handlers.size(); i++)
		if (owner->handlers[i].eventHash == selector)
			return &owner->handlers[i];
	return nullptr;
}

Common::Array<Object> Book::objectsAround(const Common::String &objectName) const {
	Common::Array<Object> out;
	for (uint i = 0; i < _heapSegments.size(); i++) {
		Common::Array<Object> here;
		appendSegmentObjects(_heapSegments[i].base, here);
		for (uint k = 0; k < here.size(); k++)
			if (here[k].name.equalsIgnoreCase(objectName))
				return here;
	}
	return out;
}

Common::Array<Object> Book::objectsOfSegment(const Common::String &name) const {
	Common::Array<Object> out;
	for (uint i = 0; i < _heapSegments.size(); i++)
		if (_heapSegments[i].name.equalsIgnoreCase(name)) {
			appendSegmentObjects(_heapSegments[i].base, out);
			break;
		}
	return out;
}

const Handler *Book::findHandlerByCode(uint32 code) const {
	for (uint i = 0; i < _scriptObjects.size(); i++)
		for (uint k = 0; k < _scriptObjects[i].handlers.size(); k++)
			if (_scriptObjects[i].handlers[k].code == code)
				return &_scriptObjects[i].handlers[k];
	for (uint i = 0; i < _pages.size(); i++) {
		for (uint k = 0; k < _pages[i].eventHandlers.size(); k++)
			if (_pages[i].eventHandlers[k].code == code)
				return &_pages[i].eventHandlers[k];
		for (uint o = 0; o < _pages[i].objects.size(); o++)
			for (uint k = 0; k < _pages[i].objects[o].handlers.size(); k++)
				if (_pages[i].objects[o].handlers[k].code == code)
					return &_pages[i].objects[o].handlers[k];
	}
	return nullptr;
}

const Handler *Book::findMessageHandler(const Common::String &receiver, uint16 selector) const {
	// Посылка 0x6c адресная: получатель лежит на стеке (RUN31:0x0194 кладёт его
	// в описатель посылки). Ищем сегмент кучи с таким именем и разбираем его
	// привязку скрипта тем же правилом, что и корневую (ledger/0055).
	for (uint i = 0; i < _heapSegments.size(); i++) {
		if (!_heapSegments[i].name.equalsIgnoreCase(receiver))
			continue;
		uint32 root = _heapSegments[i].base + 0x11;
		if (root + 0x30 > _data.size())
			continue;
		uint16 binding = readU16(&_data[root + 0x2e]);
		if (!binding)
			continue;
		for (uint k = 0; k < _scriptObjects.size(); k++) {
			if (_scriptObjects[k].id != (uint32)(binding - 1))
				continue;
			for (uint n = 0; n < _scriptObjects[k].handlers.size(); n++)
				if (_scriptObjects[k].handlers[n].eventHash == selector)
					return &_scriptObjects[k].handlers[n];
		}
	}
	return nullptr;
}

void Book::scanScriptObjectIndex() {
	if (_scriptObjects.empty())
		return;

	// Record segments encode their far selector as the high byte at +0x16.
	// A ScriptObject id-table entry is `{handle, selector, id, flags}` and its
	// target record is `segmentBase + handle + 0x0d`. This handle convention is
	// deliberately different from local heap handles. Build the set of exact
	// formal targets before looking for a table.
	Common::HashMap<uint32, int> targetByFarRef;
	for (uint i = 0; i < _scriptObjects.size(); i++) {
		const ScriptObject &object = _scriptObjects[i];
		const Record *record = nullptr;
		for (uint r = 0; r < _records.size(); r++)
			if (_records[r].offset == object.record) {
				record = &_records[r];
				break;
			}
		if (!record || record->segmentBase + 0x17 > _data.size() ||
				record->offset < record->segmentBase + 0x0d)
			continue;
		uint32 relative = record->offset - record->segmentBase - 0x0d;
		if (relative > 0xffff)
			continue;
		uint16 selector = (uint16)_data[record->segmentBase + 0x16] << 8;
		uint32 key = ((uint32)selector << 16) | relative;
		if (targetByFarRef.contains(key)) {
			warning("ToolBook: duplicate formal ScriptObject far reference %04x:%04x",
					selector, (uint16)relative);
			return;
		}
		targetByFarRef[key] = i;
	}
	if (targetByFarRef.size() != _scriptObjects.size())
		return;

	uint32 candidate = 0;
	uint candidateCount = 0;
	Common::Array<int> candidateOrder;
	for (uint32 start = 0; start + 8 <= _data.size(); start++) {
		// The overwhelming majority of bytes cannot start an id-table. Reject
		// them before allocating its per-candidate validation state.
		uint32 firstKey = ((uint32)readU16(&_data[start + 2]) << 16) |
				readU16(&_data[start]);
		if (!targetByFarRef.contains(firstKey) || readU16(&_data[start + 6]) > 1)
			continue;
		Common::Array<bool> seen;
		seen.resize(_scriptObjects.size());
		Common::Array<int> order;
		uint16 previousId = 0;
		bool first = true, terminated = false, valid = true;
		for (uint32 p = start; p + 8 <= _data.size() && order.size() <= _scriptObjects.size(); p += 8) {
			uint16 handle = readU16(&_data[p]);
			uint16 selector = readU16(&_data[p + 2]);
			uint16 id = readU16(&_data[p + 4]);
			uint16 flags = readU16(&_data[p + 6]);
			uint32 key = ((uint32)selector << 16) | handle;
			if (!targetByFarRef.contains(key) || flags > 1 || (!first && id <= previousId)) {
				valid = false;
				break;
			}
			int object = targetByFarRef[key];
			if (seen[object]) {
				valid = false;
				break;
			}
			seen[object] = true;
			order.push_back(object);
			previousId = id;
			first = false;
			if (flags == 1) {
				terminated = true;
				break;
			}
		}
		if (!valid || !terminated || order.size() != _scriptObjects.size())
			continue;
		candidate = start;
		candidateOrder = order;
		candidateCount++;
	}
	if (candidateCount != 1) {
		warning("ToolBook: expected one formal ScriptObject id table, found %u",
				candidateCount);
		return;
	}

	for (uint i = 0; i < candidateOrder.size(); i++) {
		uint32 entry = candidate + i * 8;
		ScriptObject &object = _scriptObjects[candidateOrder[i]];
		object.handle = readU16(&_data[entry]);
		object.selector = readU16(&_data[entry + 2]);
		object.id = readU16(&_data[entry + 4]);
	}
	debug(1, "ToolBook: formal ScriptObject id table @0x%x, %u entries",
			candidate, candidateOrder.size());
}

void Book::appendScriptHandlers(uint32 script, uint32 scriptEnd,
		Common::Array<Handler> &out, uint32 ownerScriptRecord) const {
	if (script + 0x18 > scriptEnd)
		return;
	uint16 count = readU16(&_data[script + 0x16]);
	uint32 directoryEnd = script + 0x18 + (uint32)count * 4;
	// The word after the directory is a reserved/flags field, not a required
	// zero terminator. One real ButtonDown script stores 0x0200 there; handler
	// record offsets still use the byte immediately after it as their base.
	uint32 recordBase = directoryEnd + 2;
	if (!count || count > 64 || recordBase > scriptEnd)
		return;

	// Directory entries point to serialized handler records. Every record has a
	// 29-byte header; bytecode begins at record+29. This is equivalent to
	// script+0x37+4*count for the first record, while later offsets are relative
	// to recordBase (not to the first code address).
	if (readU16(&_data[script + 6]) != 0x0225)
		return;
	for (uint i = 0; i < count; i++) {
		uint16 eventHash = readU16(&_data[script + 0x18 + i * 4]);
		uint16 relative = readU16(&_data[script + 0x1a + i * 4]);
		uint32 record = recordBase + relative;
		uint32 recordEnd = scriptEnd;
		for (uint j = 0; j < count; j++) {
			uint16 candidate = readU16(&_data[script + 0x1a + j * 4]);
			if (candidate > relative)
				recordEnd = MIN(recordEnd, recordBase + candidate);
		}
		uint32 code = record + 29;
		if (code >= recordEnd || _data[code] != 0x26)
			continue;
		bool found = false;
		for (uint h = 0; h < _handlers.size(); h++) {
			if (_handlers[h].code != code)
				continue;
			Handler handler = _handlers[h];
			handler.ownerScriptRecord = ownerScriptRecord;
			handler.eventHash = eventHash;
			out.push_back(handler);
			found = true;
			break;
		}
		// The global scanner deliberately starts from the common `26 0f`
		// prologue, but a directory may also own a short handler whose second
		// opcode is something else. The directory supplies an authoritative
		// record bound, so accept the standard end marker here when its strOff
		// field points exactly past the marker. This is the same integrity check
		// as scanHandlers(), without assuming a particular function prologue.
		if (!found) {
			for (uint32 marker = code + 1; marker + 5 <= recordEnd; marker++) {
				if (_data[marker] != 0x27 || _data[marker + 1] != 0x1d ||
						_data[marker + 2] != 0x66 ||
						readU16(&_data[marker + 3]) != marker - code + 7)
					continue;
				Handler handler;
				handler.code = code;
				handler.codeSize = marker - code;
				handler.ownerScriptRecord = ownerScriptRecord;
				handler.eventHash = eventHash;
				readHandlerStrings(code, handler);
				out.push_back(handler);
				found = true;
				break;
			}
		}
		// Empty handlers compile to a bare `enterHandler` followed immediately
		// by the standard END/return marker, so they intentionally have no 0x0f
		// stack-frame prologue or string table.
		if (!found && code + 4 <= recordEnd && _data[code] == 0x26 &&
				_data[code + 1] == 0x27 && _data[code + 2] == 0x1d && _data[code + 3] == 0x66) {
			Handler handler;
			handler.code = code;
			handler.codeSize = 1;
			handler.ownerScriptRecord = ownerScriptRecord;
			handler.eventHash = eventHash;
			out.push_back(handler);
			found = true;
		}
		// A return-value handler may end with the VM's alternate
		// `returnDynamic/end` marker (28 1c 66) instead of the usual void
		// marker. Directory record bounds remain authoritative.
		if (!found) {
			for (uint32 marker = code; marker + 5 <= recordEnd; marker++) {
				if (_data[marker] != 0x28 || _data[marker + 1] != 0x1c ||
						_data[marker + 2] != 0x66 ||
						readU16(&_data[marker + 3]) != marker - code + 7)
					continue;
				Handler handler;
				handler.code = code;
				handler.codeSize = marker - code;
				handler.ownerScriptRecord = ownerScriptRecord;
				handler.eventHash = eventHash;
				handler.returnsValue = true;
				out.push_back(handler);
				break;
			}
		}
	}
}

bool Book::resolveLocalScript(uint32 segmentBase, uint32 segmentEnd, uint16 handle,
		uint32 &script, uint32 &scriptEnd) const {
	if (handle < 3)
		return false;
	script = segmentBase + handle - 3;
	if (script + 0x1a > segmentEnd || readU16(&_data[script + 2]) != 0 ||
			readU16(&_data[script + 6]) != 0x0225)
		return false;
	scriptEnd = segmentBase + (readU16(&_data[script]) | 1);
	if (scriptEnd <= script || scriptEnd > segmentEnd)
		return false;

	uint16 count = readU16(&_data[script + 0x16]);
	uint32 recordBase = script + 0x1a + (uint32)count * 4;
	if (count > 64 || recordBase > scriptEnd)
		return false;
	for (uint i = 0; i < count; i++) {
		uint32 record = recordBase + readU16(&_data[script + 0x1a + i * 4]);
		if (record + 29 >= scriptEnd || _data[record + 29] != 0x26)
			return false;
	}
	return true;
}

bool Book::appendScriptBinding(uint32 segmentBase, uint32 segmentEnd, uint16 binding,
		Common::Array<Handler> &out) const {
	if (!binding)
		return true;

	uint32 script = 0, scriptEnd = 0;
	if (resolveLocalScript(segmentBase, segmentEnd, binding, script, scriptEnd)) {
		// A valid local directory with count zero is still a resolved binding.
		// It must not fall through to an unrelated ScriptObject whose id happens
		// to be one less than the local handle.
		appendScriptHandlers(script, scriptEnd, out);
		return true;
	}

	uint16 externalId = binding - 1;
	for (uint i = 0; i < _scriptObjects.size(); i++) {
		if (_scriptObjects[i].id != externalId)
			continue;
		for (uint h = 0; h < _scriptObjects[i].handlers.size(); h++)
			out.push_back(_scriptObjects[i].handlers[h]);
		return true;
	}
	return false;
}

void Book::scanObjects() {
	// Typed object block: +8 parent handle, +0a name/payload handle, +0f
	// script handle. Для Picture (0x15) +0x55 указывает на DIB block. Все эти
	// ссылки разрешаются как segmentBase + handle - 3.
	for (uint s = 0; s < _heapSegments.size(); s++) {
		const HeapSegment &seg = _heapSegments[s];
		Common::Array<uint16> handles, blockTypes;
		for (uint32 q = seg.base + 0x11; q < seg.end; ) {
			uint32 next = seg.base + (readU16(&_data[q]) | 1);
			if (next <= q || next > seg.end)
				break;
			handles.push_back((uint16)(q - seg.base + 3));
			blockTypes.push_back(readU16(&_data[q + 2]));
			q = next;
		}
		uint32 p = seg.base + 0x11;
		while (p < seg.end) {
			uint32 next = seg.base + (readU16(&_data[p]) | 1);
			if (next <= p || next > seg.end)
				break;
			uint16 type = readU16(&_data[p + 2]);
			uint32 len = next - p;
			uint16 parentHandle = readU16(&_data[p + 8]);
			bool parentValid = parentHandle == 0x14;
			for (uint b = 0; !parentValid && b < handles.size(); b++)
				if (handles[b] == parentHandle && isObjectBlockType(blockTypes[b]))
					parentValid = true;
			if (isObjectBlockType(type) && len >= 20 && parentValid) {
				Object obj;
				obj.offset = p;
				obj.block = p;
				obj.segmentBase = seg.base;
				obj.handle = (uint16)(p - seg.base + 3);
				obj.parentHandle = parentHandle;
				obj.type = type;
				obj.id = readU32(&_data[p + 4]);
				obj.ownVisible = len <= 0x23 || (_data[p + 0x23] & 1) != 0;
				obj.visible = obj.ownVisible;

				// У всех визуальных объектов прямоугольник лежит невыравненно с +0x13.
				if (len >= 0x1b) {
					uint32 q = p + 0x13;
					int16 l = (int16)readU16(&_data[q]), t = (int16)readU16(&_data[q + 2]);
					int16 r = (int16)readU16(&_data[q + 4]), bt = (int16)readU16(&_data[q + 6]);
					if (l < r && t < bt)
						obj.rect = Common::Rect(unitsToPixel(l), unitsToPixel(t),
								unitsToPixel(r), unitsToPixel(bt));
				}

				uint16 nameHandle = readU16(&_data[p + 0x0a]);
				uint32 nameBlock = nameHandle >= 3 ? seg.base + nameHandle - 3 : seg.end;
				bool validNameBlock = false;
				for (uint b = 0; b < handles.size(); b++)
					if (handles[b] == nameHandle && blockTypes[b] == 0)
						validNameBlock = true;
				if (validNameBlock && nameBlock + 5 < seg.end) {
					uint32 e = nameBlock + 4;
					while (e < seg.end && _data[e] >= 32 && _data[e] < 127)
						e++;
					if (e > nameBlock + 4 && e < seg.end && _data[e] == 0) {
						obj.offset = nameBlock + 4;
						obj.name = Common::String((const char *)&_data[nameBlock + 4], e - nameBlock - 4);
					}
				}

				if (type == 0x15 && len >= 0x57) {
					uint16 dibHandle = readU16(&_data[p + 0x55]);
					uint32 dib = seg.base + dibHandle - 3 + 4;
					for (uint i = 0; i < _images.size(); i++) {
						if (_images[i].offset == dib) {
							obj.picture = true;
							obj.image = i;
							break;
						}
					}
				}

				// Field text is two formal local references deep:
				// Field+0x28 -> descriptor type0, descriptor+6 -> text type0.
				// The text payload is [capacity:u16][logicalLen:u16][bytes].
				if (type == 0x0a && len >= 0x2a) {
					obj.field = true;
					uint16 descriptorHandle = readU16(&_data[p + 0x28]);
					uint32 descriptor = descriptorHandle >= 3 ?
							seg.base + descriptorHandle - 3 : seg.end;
					bool validDescriptor = false;
					for (uint b = 0; b < handles.size(); b++)
						if (handles[b] == descriptorHandle && blockTypes[b] == 0)
							validDescriptor = true;
					if (validDescriptor && descriptor + 8 <= seg.end) {
						uint16 textHandle = readU16(&_data[descriptor + 6]);
						uint32 text = textHandle >= 3 ? seg.base + textHandle - 3 : seg.end;
						bool validText = false;
						for (uint b = 0; b < handles.size(); b++)
							if (handles[b] == textHandle && blockTypes[b] == 0)
								validText = true;
						if (validText && text + 8 <= seg.end) {
							uint32 textEnd = seg.base + (readU16(&_data[text]) | 1);
							uint16 capacity = readU16(&_data[text + 4]);
							uint16 logicalLen = readU16(&_data[text + 6]);
							if (logicalLen <= capacity && text + 8 + capacity <= textEnd) {
								obj.textBlock = text;
								obj.textCapacity = capacity;
								obj.initialText = cp1251ToUtf8(Common::String(
										(const char *)&_data[text + 8], logicalLen));
							}
						}
					}
				}

				uint16 scriptBinding = readU16(&_data[p + 0x0f]);
				if (!appendScriptBinding(seg.base, seg.end, scriptBinding, obj.handlers))
					warning("ToolBook: unresolved object script binding %u at block 0x%x",
							scriptBinding, p);
				_objects.push_back(obj);
			}
			p = next;
		}
	}
	debug(1, "ToolBook: формальных дочерних объектов %u", _objects.size());
}

void Book::appendObjectTree(uint32 segmentBase, uint16 handle, bool parentVisible,
		Common::Array<Object> &out, uint depth) const {
	if (depth > 32)
		return;
	const Object *source = nullptr;
	for (uint i = 0; i < _objects.size(); i++) {
		if (_objects[i].segmentBase == segmentBase && _objects[i].handle == handle) {
			source = &_objects[i];
			break;
		}
	}
	if (!source)
		return;

	Object object = *source;
	object.visible = parentVisible && object.ownVisible;
	out.push_back(object);
	if (object.type != 0x0b || object.block + 0x34 > _data.size())
		return;

	// A Group owns an ordered type-0 child-list block. Each ten-byte entry is
	// `[handle, layoutRect]`; coordinates in the child object itself remain the
	// authoritative absolute page coordinates. The list order is back-to-front.
	uint16 count = readU16(&_data[object.block + 0x28]);
	uint16 listHandle = readU16(&_data[object.block + 0x32]);
	if (!count || listHandle < 3)
		return;
	uint32 list = segmentBase + listHandle - 3;
	if (list + 4 + (uint32)count * 10 > _data.size() || readU16(&_data[list + 2]) != 0)
		return;
	for (uint i = 0; i < count; i++)
		appendObjectTree(segmentBase, readU16(&_data[list + 4 + i * 10]),
				object.visible, out, depth + 1);
}

void Book::appendSegmentObjects(uint32 segmentBase, Common::Array<Object> &out) const {
	const HeapSegment *segment = nullptr;
	for (uint i = 0; i < _heapSegments.size(); i++) {
		if (_heapSegments[i].base == segmentBase) {
			segment = &_heapSegments[i];
			break;
		}
	}
	if (!segment)
		return;

	// Root +0x30 points to the type-6 child service. Its +0x06 count and
	// +0x10 type-0 list handle enumerate every direct child in z-order.
	uint32 root = segmentBase + 0x11;
	uint16 serviceHandle = readU16(&_data[root + 0x30]);
	if (serviceHandle < 3)
		return;
	uint32 service = segmentBase + serviceHandle - 3;
	if (service + 0x12 > segment->end || readU16(&_data[service + 2]) != 6)
		return;
	uint16 count = readU16(&_data[service + 0x06]);
	uint16 listHandle = readU16(&_data[service + 0x10]);
	if (!count || listHandle < 3)
		return;
	uint32 list = segmentBase + listHandle - 3;
	if (list + 4 + (uint32)count * 2 > segment->end || readU16(&_data[list + 2]) != 0)
		return;
	for (uint i = 0; i < count; i++)
		appendObjectTree(segmentBase, readU16(&_data[list + 4 + i * 2]), true, out, 0);
}

void Book::appendRootHandlers(uint32 segmentBase, Common::Array<Handler> &out) const {
	const HeapSegment *segment = nullptr;
	for (uint i = 0; i < _heapSegments.size(); i++)
		if (_heapSegments[i].base == segmentBase) {
			segment = &_heapSegments[i];
			break;
		}
	if (!segment)
		return;

	// Root +0x2e is a serialized script binding. A strict local type-0 script
	// handle is stored directly. Otherwise a non-zero value is a one-based
	// reference to the formal top-level ScriptObject id table. This partition is
	// exact across all 261 heaps in this book (144 null, 27 local, 90 external).
	// Validate the local script fully before choosing it: six external ids also
	// happen to land on an unrelated type-0 block numerically.
	uint32 root = segmentBase + 0x11;
	uint16 handle = readU16(&_data[root + 0x2e]);
	if (!handle)
		return;
	if (!appendScriptBinding(segmentBase, segment->end, handle, out))
		warning("ToolBook: unresolved root script binding %u at heap 0x%x", handle, segmentBase);
}

void Book::scanPages() {
	for (uint s = 0; s < _heapSegments.size(); s++) {
		if (_heapSegments[s].type != 5)
			continue;
		Page page;
		page.segmentBase = _heapSegments[s].base;
		page.id = _heapSegments[s].id;
		page.selector = _heapSegments[s].selector;
		page.name = _heapSegments[s].name;

		// Page root +0x4b хранит far reference на Background root. Handle там
		// всегда 0x14; selector однозначно разрешается среди Background.
		uint32 root = page.segmentBase + 0x11;
		uint16 bgHandle = readU16(&_data[root + 0x4b]);
		uint16 bgSelector = readU16(&_data[root + 0x4d]);
		if (bgHandle == 0x14) {
			for (uint b = 0; b < _heapSegments.size(); b++) {
				if (_heapSegments[b].type != 4 || _heapSegments[b].selector != bgSelector)
					continue;
				page.backgroundSegmentBase = _heapSegments[b].base;
				page.backgroundId = _heapSegments[b].id;
				uint32 backgroundRoot = page.backgroundSegmentBase + 0x11;
				uint16 widthUnits = readU16(&_data[backgroundRoot + 0x63]);
				uint16 heightUnits = readU16(&_data[backgroundRoot + 0x65]);
				if (widthUnits && heightUnits) {
					// Client extents are stored in 1/1440-inch units. Some
					// full-width backgrounds serialize 9597 rather than 9600;
					// round to the nearest pixel instead of shrinking them.
					page.canvasWidth = (widthUnits + kUnit / 2) / kUnit;
					page.canvasHeight = (heightUnits + kUnit / 2) / kUnit;
				}
				appendSegmentObjects(page.backgroundSegmentBase, page.objects);
				break;
			}
		}
		appendSegmentObjects(page.segmentBase, page.objects);
		// A Page is a child of its Background in the ToolBook message
		// hierarchy. Search its own/shared script first, then the parent
		// Background script when the Page does not handle the selector.
		appendRootHandlers(page.segmentBase, page.eventHandlers);
		appendRootHandlers(page.backgroundSegmentBase, page.eventHandlers);
		for (uint i = 0; i < page.objects.size(); i++)
			if (page.objects[i].picture && page.objects[i].rect.width() >= 620 &&
					page.objects[i].rect.height() >= 460)
				page.background = page.objects[i].image;

		_pages.push_back(page);
	}
}

void Book::scanPageOrder() {
	// Compiled books serialize an ordered far-reference table separately from
	// the heap. Each 13-byte entry contains the current Page root, followed by
	// the Background/Page ids of the next entry. Resolve it structurally: every
	// Page selector must occur exactly once and every next-id pair must agree
	// with the following formal heap root. Names and file adjacency play no role.
	const uint count = _pages.size();
	if (!count || count > 0xffff || _data.size() < count * 13)
		return;

	Common::HashMap<uint16, int> bySelector;
	for (uint i = 0; i < count; i++)
		bySelector[_pages[i].selector] = i;

	for (uint32 start = 0; start + (uint32)count * 13 <= _data.size(); start++) {
		if (readU16(&_data[start]) != 0x14 || _data[start + 4] != 0)
			continue;
		uint16 firstSelector = readU16(&_data[start + 2]);
		if (!bySelector.contains(firstSelector))
			continue;

		Common::Array<int> order;
		Common::Array<bool> seen;
		seen.resize(count);
		bool valid = true;
		for (uint i = 0; i < count; i++) {
			uint32 entry = start + i * 13;
			uint16 selector = readU16(&_data[entry + 2]);
			if (readU16(&_data[entry]) != 0x14 || _data[entry + 4] != 0 ||
					!bySelector.contains(selector)) {
				valid = false;
				break;
			}
			int page = bySelector[selector];
			if (page < 0 || page >= (int)count || seen[page]) {
				valid = false;
				break;
			}
			seen[page] = true;
			order.push_back(page);

			// The last entry points to itself; every earlier one points to the
			// next far reference. This validates all 13 bytes of every entry.
			uint32 nextEntry = start + (i + 1 < count ? i + 1 : i) * 13;
			uint16 nextSelector = readU16(&_data[nextEntry + 2]);
			if (!bySelector.contains(nextSelector)) {
				valid = false;
				break;
			}
			const Page &next = _pages[bySelector[nextSelector]];
			if (readU32(&_data[entry + 5]) != next.backgroundId ||
					readU32(&_data[entry + 9]) != next.id) {
				valid = false;
				break;
			}
		}
		if (!valid || order.size() != count)
			continue;

		_pageOrder = order;
		_initialPage = order[0];
		for (uint i = 0; i < count; i++)
			_pages[order[i]].nextPage = order[i + 1 < count ? i + 1 : i];
		debug(1, "ToolBook: formal page-order table @0x%x, %u entries, initial selector %04x",
				start, count, _pages[_initialPage].selector);
		return;
	}

	warning("ToolBook: formal page-order table was not found");
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
// Распаковка ровно так, как делает MTB40BAS.DLL (ledger/0039, 0042): поток —
// это куски вида `[u16 число записей][записи…]`, цикл идёт заданное число раз,
// а не «пока не заполнится приёмник».
static uint32 unpackChunks(const byte *src, uint32 srcLen, byte *dst, uint32 dstLen,
		uint32 *usedOut) {
	uint32 in = 0, out = 0;
	while (out < dstLen && in + 2 <= srcLen) {
		uint32 records = src[in] | (src[in + 1] << 8);
		in += 2;
		if (!records || records > 0xfff0)
			break;
		for (uint32 k = 0; k < records && in < srcLen; k++) {
			byte c = src[in++];
			if (c <= 0xf5) {
				if (in >= srcLen)
					break;
				byte v = src[in++];
				uint32 n = MIN<uint32>(c + 3, dstLen - out);
				memset(dst + out, v, n);
				out += n;
			} else {
				uint32 need = c - 0xf5;
				uint32 n = MIN<uint32>(need, dstLen - out);
				for (uint32 j = 0; j < n && in < srcLen; j++)
					dst[out++] = src[in++];
				in += (need > n) ? (need - n) : 0;
			}
			if (out >= dstLen)
				break;
		}
	}
	if (usedOut)
		*usedOut = in;
	return out;
}

// Большие картинки ToolBook передаются распаковщику как huge-указатель. После
// каждой группы оригинал проверяет SI и, если он стал 0 или больше 0xfffc,
// переносит источник на следующий 64-КБ сегмент. Поэтому хвост страницы может
// быть слаком, хотя он входит в объявленный compSize (MTB40BAS, seg 101:0x41).
//
// Здесь страница считается от начала src: живой рантайм передаёт первый кусок
// с нулевым SI. Проверки строгие — должны сойтись и весь compSize, и rawSize.
static bool unpackHugeChunks(const byte *src, uint32 srcLen, byte *dst, uint32 dstLen,
		uint32 *usedOut) {
	uint32 in = 0, out = 0;
	while (in < srcLen && out < dstLen) {
		uint32 pageOffset = in & 0xffff;
		if (pageOffset > 0xfffc) {
			uint32 skip = 0x10000 - pageOffset;
			if (skip > srcLen - in)
				break;
			in += skip;
			continue;
		}

		if (srcLen - in < 2)
			break;
		uint32 records = src[in] | (src[in + 1] << 8);
		in += 2;
		if (!records || records > 0xfff0)
			break;

		const uint32 pageEnd = (in & 0xffff0000) + 0x10000;
		bool valid = true;
		for (uint32 k = 0; k < records; k++) {
			if (in >= srcLen || in >= pageEnd) {
				valid = false;
				break;
			}
			byte c = src[in++];
			if (c <= 0xf5) {
				uint32 count = c + 3;
				if (in >= srcLen || in >= pageEnd || count > dstLen - out) {
					valid = false;
					break;
				}
				byte value = src[in++];
				if (dst)
					memset(dst + out, value, count);
				out += count;
			} else {
				uint32 count = c - 0xf5;
				if (count > srcLen - in || in + count > pageEnd || count > dstLen - out) {
					valid = false;
					break;
				}
				if (dst)
					memcpy(dst + out, src + in, count);
				in += count;
				out += count;
			}
		}
		if (!valid)
			break;
	}

	if (usedOut)
		*usedOut = in;
	return in == srcLen && out == dstLen;
}

static uint32 unpackRLE(const byte *src, uint32 srcLen, byte *dst, uint32 dstLen,
		uint32 stride, uint32 *usedOut) {
	(void)stride;
	uint32 in = 0, out = 0;

	while (in < srcLen && out < dstLen) {
		byte c = src[in++];
		uint32 n;

		if (c <= 0xf5) {
			if (in >= srcLen)
				break;
			byte v = src[in++];
			n = MIN<uint32>(c + 3, dstLen - out);
			memset(dst + out, v, n);
			out += n;
		} else {
			uint32 k = c - 0xf5;
			n = MIN<uint32>(k, dstLen - out);
			for (uint32 j = 0; j < n && in < srcLen; j++)
				dst[out++] = src[in++];
			in += (k > n) ? (k - n) : 0;
		}
	}

	if (usedOut)
		*usedOut = in;
	return out;
}

// Сколько байт выхода даст поток, если считать его кусками с числом записей.
static uint32 probeChunks(const byte *src, uint32 srcLen, uint32 want) {
	uint32 in = 0, out = 0;
	while (out < want && in + 2 <= srcLen) {
		uint32 records = src[in] | (src[in + 1] << 8);
		in += 2;
		if (!records || records > 0xfff0)
			return 0;
		for (uint32 k = 0; k < records; k++) {
			if (in >= srcLen)
				return 0;
			byte c = src[in++];
			if (c <= 0xf5) {
				in++;
				out += c + 3;
			} else {
				uint32 n = c - 0xf5;
				in += n;
				out += n;
			}
			if (out > want)
				return 0;
		}
	}
	return out;
}

// Строгая проверка одного обычного chunk stream: поток обязан израсходовать
// ровно объявленный compressed size и произвести ровно raw size.
static bool probeChunkStream(const byte *src, uint32 srcLen, uint32 dstLen) {
	uint32 in = 0, out = 0;
	while (in < srcLen && out < dstLen) {
		if (srcLen - in < 2)
			return false;
		uint32 records = src[in] | (src[in + 1] << 8);
		in += 2;
		if (!records || records > 0xfff0)
			return false;
		for (uint32 k = 0; k < records; k++) {
			if (in >= srcLen)
				return false;
			byte c = src[in++];
			if (c <= 0xf5) {
				if (in >= srcLen)
					return false;
				in++;
				out += c + 3;
			} else {
				uint32 count = c - 0xf5;
				if (count > srcLen - in)
					return false;
				in += count;
				out += count;
			}
			if (out > dstLen)
				return false;
		}
	}
	return in == srcLen && out == dstLen;
}

bool Book::locatePixels(Image &img) {
	if (img.pixels)
		return true;
	if (!img.rawSize)
		return false;

	// Основной путь (ledger/0044): поток лежит блоком сразу за блоком DIB и
	// начинается с числа записей. Проверяем несколько ближайших положений.
	const uint32 afterPal = img.offset + 40 + img.colors * 4;

	// Большой поток начинается после десятибайтового заголовка
	// `01 00 00 00 01 00 00 00 00 00`. Его группы живут в huge-span и на
	// границе source page могут оставлять 1..3 байта слака. Совпадение обоих
	// размеров обязательно: у фона @0x1757a9 это 232673 -> 307200 байт.
	static const byte kHugePrefix[10] = { 1, 0, 0, 0, 1, 0, 0, 0, 0, 0 };
	if (img.compSize && afterPal + sizeof(kHugePrefix) <= _data.size() &&
			memcmp(&_data[afterPal], kHugePrefix, sizeof(kHugePrefix)) == 0) {
		uint32 s = afterPal + sizeof(kHugePrefix);
		if (img.compSize <= _data.size() - s &&
				unpackHugeChunks(&_data[s], img.compSize, nullptr, img.rawSize, nullptr)) {
			img.pixels = s;
			img.chunked = true;
			img.hugeSpan = true;
			debug(1, "ToolBook: huge-span картинки @0x%x: поток @0x%x, %u -> %u байт",
					img.offset, s, img.compSize, img.rawSize);
			return true;
		}
	}

	// Ближний случай: поток начинается сразу за блоком DIB.
	for (uint32 shift = 4; shift <= 64; shift += 2) {
		uint32 s = afterPal + shift;
		if (s + 8 >= _data.size())
			break;
		if (probeChunks(&_data[s], MIN<uint32>(_data.size() - s, img.rawSize + 65536),
				img.rawSize) == img.rawSize) {
			img.pixels = s;
			img.chunked = true;
			return true;
		}
	}

	// Поток лежит блоком кучи неподалёку за блоком DIB (0044), но не обязательно
	// следующим: у рамки диалога между ними оказался ещё один блок, и поток начался
	// на +1028 от палитры. Поэтому идём **по заголовкам блоков**, а не по мелкому
	// окну сдвигов: каждый блок это `[u16 конец в сегменте][u16 тип][данные]`,
	// и данные очередного блока проверяются как начало потока (ledger/0075).
	for (uint32 s = afterPal, guard = 0; guard < 64 && s + 8 < _data.size(); guard++) {
		uint32 got = probeChunks(&_data[s + 4],
				MIN<uint32>(_data.size() - s - 4, img.rawSize + 65536), img.rawSize);
		if (got == img.rawSize) {
			img.pixels = s + 4;
			img.chunked = true;
			return true;
		}
		uint32 end = readU16(&_data[s]);
		if (!img.segBase || end < 4)
			break;
		uint32 next = img.segBase + end;
		if (next <= s || next + 8 >= _data.size())
			break;
		s = next;
	}

	// В сегменте с несколькими Picture потоки лежат после всей heap-цепочки,
	// в порядке object ID (а не DIB/file order), вплотную по compSize. Поля
	// raw/comp находятся в Picture block +0x2d/+0x31. Normal/hover variants
	// and other sibling pictures therefore share one ordered stream tail.
	int imageIndex = -1;
	for (uint i = 0; i < _images.size(); i++)
		if (_images[i].offset == img.offset) {
			imageIndex = i;
			break;
		}
	uint32 segmentBase = 0, segmentEnd = 0;
	if (imageIndex >= 0) {
		for (uint i = 0; i < _objects.size(); i++)
			if (_objects[i].image == imageIndex) {
				segmentBase = _objects[i].segmentBase;
				break;
			}
	}
	for (uint i = 0; i < _heapSegments.size(); i++)
		if (_heapSegments[i].base == segmentBase) {
			segmentEnd = _heapSegments[i].end;
			break;
		}
	if (segmentEnd) {
		Common::Array<uint> pictures;
		for (uint i = 0; i < _objects.size(); i++) {
				// The tail contains storage for every Picture block. Keep unresolved
				// pictures in the cursor walk too: they still consume compSize, or
				// rawSize when the formal compressed-size field is zero.
			if (_objects[i].segmentBase != segmentBase || _objects[i].type != 0x15)
				continue;

			// A segment tail contains only pictures which do not already have a
			// stream beside their DIB. Mixed segments may keep one Picture at
			// afterPalette+4 while sibling streams live in the ordered tail.
			// Counting its compSize in both places shifts every subsequent tail
			// stream. Use the declared sizes as a strict test;
			// the old size-only probe is deliberately not sufficient here.
			const Object &object = _objects[i];
			bool hasLocalStream = false;
			if (object.image >= 0) {
				const Image &localImage = _images[object.image];
				const uint32 localAfterPal = localImage.offset + 40 + localImage.colors * 4;
				const uint32 raw = readU32(&_data[object.block + 0x2d]);
				const uint32 comp = readU32(&_data[object.block + 0x31]);
				for (uint32 shift = 4; shift <= 64 && !hasLocalStream; shift += 2) {
					const uint32 candidate = localAfterPal + shift;
					if (candidate <= _data.size() && comp <= _data.size() - candidate)
						hasLocalStream = probeChunkStream(&_data[candidate], comp, raw);
				}
			}
			if (hasLocalStream)
				continue;

			uint at = pictures.size();
			while (at > 0 && _objects[pictures[at - 1]].id > _objects[i].id)
				at--;
			pictures.insert_at(at, i);
		}

		if (!pictures.empty()) {
			// A zero compressed size is the formal marker for an uncompressed
			// tail image; it still occupies rawSize bytes. Raw spans may precede
			// the first compressed stream, so anchor validation starts after all
			// leading raw images while preserving their total byte count.
			uint anchor = 0;
			uint32 rawPrefix = 0;
			while (anchor < pictures.size()) {
				const Object &obj = _objects[pictures[anchor]];
				uint32 comp = readU32(&_data[obj.block + 0x31]);
				if (comp)
					break;
				uint32 raw = readU32(&_data[obj.block + 0x2d]);
				if (!raw || raw > 0xffffffffU - rawPrefix) {
					rawPrefix = 0;
					anchor = pictures.size();
					break;
				}
				rawPrefix += raw;
				anchor++;
			}

			if (anchor < pictures.size()) {
				const Object &first = _objects[pictures[anchor]];
				uint32 firstRaw = readU32(&_data[first.block + 0x2d]);
				uint32 firstComp = readU32(&_data[first.block + 0x31]);
				debug(2, "ToolBook: segment stream probe @0x%x end=0x%x: %u pictures, anchor id=%u raw=%u comp=%u, raw prefix=%u",
						segmentBase, segmentEnd, pictures.size(), first.id, firstRaw, firstComp, rawPrefix);
				uint32 stream = 0;
				for (uint32 shift = 0; shift <= 64; shift++) {
					uint32 candidate = segmentEnd + shift;
					if (candidate > _data.size() || rawPrefix > _data.size() - candidate)
						continue;
					uint32 packed = candidate + rawPrefix;
					if (firstComp <= _data.size() - packed &&
							(probeChunkStream(&_data[packed], firstComp, firstRaw) ||
							 unpackHugeChunks(&_data[packed], firstComp, nullptr, firstRaw, nullptr))) {
						stream = candidate;
						break;
					}
				}

				uint32 cursor = stream;
				bool valid = stream != 0;
				Common::Array<byte> storage;
				for (uint i = 0; valid && i < pictures.size(); i++) {
					const Object &obj = _objects[pictures[i]];
					uint32 raw = readU32(&_data[obj.block + 0x2d]);
					uint32 comp = readU32(&_data[obj.block + 0x31]);
					byte kind = 0; // raw tail bytes
					if (!comp) {
						valid = raw && cursor <= _data.size() && raw <= _data.size() - cursor;
					} else if (cursor <= _data.size() && comp <= _data.size() - cursor &&
							probeChunkStream(&_data[cursor], comp, raw)) {
						kind = 1; // ordinary chunk stream
					} else if (cursor <= _data.size() && comp <= _data.size() - cursor &&
							unpackHugeChunks(&_data[cursor], comp, nullptr, raw, nullptr)) {
						kind = 2; // source-page-spanning chunk stream
					} else {
						valid = false;
					}
					if (valid && obj.image >= 0)
						valid = raw == _images[obj.image].rawSize;
					if (valid) {
						storage.push_back(kind);
						cursor += comp ? comp : raw;
					}
				}
				if (valid) {
					cursor = stream;
					for (uint i = 0; i < pictures.size(); i++) {
						const Object &obj = _objects[pictures[i]];
						uint32 raw = readU32(&_data[obj.block + 0x2d]);
						uint32 comp = readU32(&_data[obj.block + 0x31]);
						if (obj.image >= 0) {
							Image &candidate = _images[obj.image];
							candidate.compSize = comp ? comp : raw;
							candidate.pixels = cursor;
							candidate.raw = storage[i] == 0;
							candidate.chunked = storage[i] != 0;
							candidate.hugeSpan = storage[i] == 2;
						}
						cursor += comp ? comp : raw;
					}
					debug(1, "ToolBook: %u потоков сегмента @0x%x начинаются @0x%x",
							pictures.size(), segmentBase, stream);
					return img.pixels != 0;
				}
			}
		}
	}

	// Запасного пути нет намеренно. Жадный подбор по размерам находил чужие
	// данные и рисовал неправильные картинки — видимость работы, которая мешала
	// увидеть настоящую (ledger/0047). Лучше честно не нарисовать ничего.
	debug(2, "ToolBook: поток картинки @0x%x %dx%d не найден", img.offset, img.width, img.height);
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
	} else if (img.chunked) {
		if (img.hugeSpan) {
			if (!unpackHugeChunks(&_data[img.pixels], img.compSize,
					pixels.begin(), img.rawSize, nullptr))
				return nullptr;
		} else {
			unpackChunks(&_data[img.pixels], _data.size() - img.pixels,
					pixels.begin(), img.rawSize, nullptr);
		}
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
