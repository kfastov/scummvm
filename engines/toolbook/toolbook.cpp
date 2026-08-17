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

#include "common/config-manager.h"
#include "common/archive.h"
#include "common/debug.h"
#include "common/events.h"
#include "common/file.h"
#include "common/hashmap.h"
#include "common/formats/winexe.h"
#include "common/ptr.h"
#include "common/system.h"
#include "engines/advancedDetector.h"
#include "engines/util.h"
#include "graphics/font.h"
#include "graphics/fontman.h"
#include "graphics/fonts/ttf.h"
#include "graphics/palette.h"
#include "graphics/paletteman.h"
#include "graphics/surface.h"

#include "toolbook/book.h"
#include "toolbook/toolbook.h"
#include "audio/audiostream.h"
#include "audio/decoders/wave.h"

namespace ToolBook {

// Книга «Башни знаний» лежит внутри KNTOWER.EXE с этого смещения — там просто
// начинается файл книги, без обёртки.
static const uint32 kEmbeddedBookOffset = 0x29e0;
static const uint16 kEventEnterPage = 0x8d55;
static const uint16 kEventFirstIdle = 0x963c;
static const uint16 kEventMouseEnter = 0xd5cf;
static const uint16 kEventMouseLeave = 0xc7ec;
static const uint16 kEventButtonDown = 0xe148;
static const uint16 kEventButtonClick = 0xf56e;

static void blitTransparent(Graphics::Surface *dst, const Graphics::Surface &src,
		int x, int y, int transparent = -1) {
	for (int sy = 0; sy < src.h; sy++) {
		int dy = y + sy;
		if (dy < 0 || dy >= dst->h)
			continue;
		const byte *s = (const byte *)src.getBasePtr(0, sy);
		byte *d = (byte *)dst->getBasePtr(MAX(0, x), dy);
		int from = MAX(0, -x), to = MIN((int)src.w, (int)dst->w - x);
		for (int sx = from; sx < to; sx++) {
			if (transparent < 0 || s[sx] != transparent)
				d[sx - from] = s[sx];
		}
	}
}

// Ключ свойства в нашем подобии CDB: имя объекта плюс номер свойства.
// Имена объектов книга сравнивает без учёта регистра.
// Имена классов ToolBook по типу блока кучи.
//
// Массив лежит в `MTB40UTL.DLL` со смещения 0x0d1e4 — нуль-терминированные
// строки подряд, индекс равен типу блока. Соответствие проверено по четырём
// независимым якорям: тип сегмента 4 — Background и 5 — Page (book.cpp), тип
// блока 9 — кнопка и 10 — поле (объекты диалога). Пустые места в массиве среда
// занимает словом `Error`; здесь они пусты.
//
// Отсюда же видно, что 2393 объекта с DIB — это **PaintObject**, а не Picture
// (книга и сравнивает с `paintobject`), а 1067 областей карты —
// `irregularPolygon` (ledger/0079).
static const char *const kClassNames[] = {
	nullptr, "Book", nullptr, nullptr, "Background", "Page", nullptr, nullptr,
	"Rectangle", "Button", "Field", "Group", "Ellipse", "RoundedRectangle",
	"Line", "Polygon", "IrregularPolygon", "Arc", "Pie", "AngledLine", "Curve",
	"PaintObject", "RecordField", "Hotword", "Hotword", "Picture", "Stage",
	"Table", "Chart", "reportSection", "GroupedButton", "DataBaseField", nullptr,
	"MultipleSelection", "ComboBox", "Viewer", "OLE", "TableColumn"
};

static Common::String classNameOfType(uint16 type) {
	if (type >= ARRAYSIZE(kClassNames) || !kClassNames[type])
		return Common::String();
	return kClassNames[type];
}

static Common::String uppercased(const Common::String &s) {
	Common::String key = s;
	key.toUppercase();
	return key;
}

static Common::String propertyKey(const Common::String &object, uint32 property) {
	return uppercased(object) + Common::String::format("#%04x", property);
}

ToolBookEngine::ToolBookEngine(OSystem *syst, const ADGameDescription *desc)
		: Engine(syst), _desc(desc) {
}

ToolBookEngine::~ToolBookEngine() {
	FontMan.mayDeleteFont(_fieldFont);
	delete _book;
}

Common::Error ToolBookEngine::run() {
	initGraphics(640, 480);
	#ifdef USE_FREETYPE2
	_fieldFont = Graphics::loadTTFFontFromArchive("LiberationSans-Regular.ttf", 16,
			Graphics::kTTFSizeModeCharacter, 96, 96, Graphics::kTTFRenderModeMonochrome);
	#endif
	if (!_fieldFont)
		_fieldFont = FontMan.getFontByUsage(Graphics::FontManager::kGUIFont);
	_startMs = _system->getMillis();

	Common::File file;
	const char *candidates[] = { "KNTOWER.EXE", "kntower.tbk", "RESOURCE.TBK", nullptr };
	for (int i = 0; candidates[i]; i++) {
		if (file.open(Common::Path(candidates[i]))) {
			debug(1, "ToolBook: открыл %s", candidates[i]);
			break;
		}
	}
	if (!file.isOpen())
		return Common::Error(Common::kNoGameDataFoundError, "не найден KNTOWER.EXE");

	_book = new Book();
	if (!_book->load(&file, kEmbeddedBookOffset))
		return Common::Error(Common::kUnknownError, "книга не разобралась");
	if (_book->initialPage() >= 0)
		_currentPage = _book->initialPage();

	debug(0, "ToolBook: страниц %u, картинок %u, записей %u, сегментов %u",
			_book->pages().size(), _book->images().size(),
			_book->records().size(), _book->segmentCount());

	Common::String classes;
	for (uint i = 0; i < _book->classNames().size() && i < 20; i++)
		classes += _book->classNames()[i] + " ";
	debug(1, "ToolBook: классы объектов: %s", classes.c_str());

	for (uint i = 0; i < _book->images().size(); i++)
		_rawImages.push_back(i);
	debug(0, "ToolBook: картинок: %u, объектов с прямоугольником: %u",
			_rawImages.size(), _book->objects().size());

	for (uint i = 0; i < _book->pages().size(); i++) {
		const Page &p = _book->pages()[i];
		Common::String names;
		for (uint k = 0; k < p.objects.size() && k < 6; k++)
			names += p.objects[k].name + " ";
		debug(1, "ToolBook: страница %2u «%s»: объектов %u — %s",
				i, p.name.c_str(), p.objects.size(), names.c_str());
	}

	showPage(_currentPage);
	dispatchPageEvent(kEventEnterPage);
	_pendingFirstIdle = true;

	while (!shouldQuit() && !_quit) {
		handleEvents();
		// firstIdle is a Reader lifecycle event, not part of page navigation.
		// Deliver it once the page has returned to the event loop.
		if (_pendingFirstIdle) {
			_pendingFirstIdle = false;
			dispatchPageEvent(kEventFirstIdle);
		}
		if (_needsRedraw) {
			if (_imageMode)
				showImage(_currentImage);
			else
				showPage(_currentPage);
			_needsRedraw = false;
		}
		_system->updateScreen();
		_system->delayMillis(20);
	}

	return Common::kNoError;
}

int ToolBookEngine::findPageIndex(const Common::String &name) const {
	for (uint i = 0; i < _book->pages().size(); i++)
		if (_book->pages()[i].name.equalsIgnoreCase(name))
			return i;
	return -1;
}

void ToolBookEngine::navigateTo(const Common::String &name) {
	int index = findPageIndex(name);
	if (index < 0) {
		warning("ToolBook: страница «%s» не найдена", name.c_str());
		return;
	}
	_currentPage = index;
	_hoveredObject = 0;
	_focusedField = 0;
	_fieldSelectAll = false;
	_imageMode = false;
	_needsRedraw = true;
	debug(0, "ToolBook: OpenScript navigation -> «%s»", name.c_str());
	dispatchPageEvent(kEventEnterPage);
	_pendingFirstIdle = true;
}

void ToolBookEngine::dispatchPageEvent(uint16 eventHash) {
	if (_currentPage < 0 || _currentPage >= (int)_book->pages().size())
		return;
	const Common::Array<Handler> &handlers = _book->pages()[_currentPage].eventHandlers;
	for (uint i = 0; i < handlers.size(); i++)
		if (handlers[i].eventHash == eventHash) {
			runHandler(handlers[i]);
			return;
		}
}

void ToolBookEngine::dispatchObjectEvent(const Object &object, uint16 eventHash) {
	// Сообщение ToolBook идёт вверх по иерархии: объект, его страница, её фон.
	// Кнопки диалога своих обработчиков не имеют — `buttonClick` лежит на фоне
	// «message», и получателем остаётся нажатый объект (ledger/0078).
	ScriptValue receiver;
	receiver.string = object.name;
	receiver.isObject = true;
	receiver.width = 4;

	for (uint i = 0; i < object.handlers.size(); i++)
		if (object.handlers[i].eventHash == eventHash) {
			runHandler(object.handlers[i], Common::Array<ScriptValue>(), receiver, nullptr, 0);
			return;
		}

	int index = pageOfObject(object);
	if (index < 0)
		return;
	// В `eventHandlers` страницы лежат и её собственные root-обработчики, и
	// обработчики фона (book.cpp: appendRootHandlers для обоих сегментов).
	const Common::Array<Handler> &handlers = _book->pages()[index].eventHandlers;
	for (uint i = 0; i < handlers.size(); i++)
		if (handlers[i].eventHash == eventHash) {
			debug(2, "ToolBook: событие 0x%04x объекта «%s» обработано страницей «%s»",
					eventHash, object.name.c_str(), _book->pages()[index].name.c_str());
			runHandler(handlers[i], Common::Array<ScriptValue>(), receiver, nullptr, 0);
			return;
		}
}

int ToolBookEngine::pageOfObject(const Object &object) const {
	for (int which = 0; which < 2; which++) {
		int index = which == 0 ? shownViewerPage() : _currentPage;
		if (index < 0 || index >= (int)_book->pages().size())
			continue;
		const Common::Array<Object> &objects = _book->pages()[index].objects;
		for (uint i = 0; i < objects.size(); i++)
			if (objects[i].block == object.block)
				return index;
	}
	return -1;
}

// Initial executable subset of OpenScript. It contains only machine-confirmed
// VM operations and generic ToolBook services. An unknown operation stops the
// current handler: fabricating a return value could select a game branch that
// the serialized OpenScript never selected.
bool ToolBookEngine::runHandler(const Handler &handler) {
	Common::Array<ScriptValue> arguments;
	ScriptValue receiver;
	return runHandler(handler, arguments, receiver, nullptr, 0);
}

bool ToolBookEngine::runHandler(const Handler &handler,
		const Common::Array<ScriptValue> &arguments,
		const ScriptValue &receiver, ScriptValue *result, uint depth) {
	typedef ScriptValue Value;
	if (depth >= 64) {
		warning("ToolBook: слишком глубокая цепочка OpenScript @0x%x", handler.code);
		return false;
	}
	static const uint8 kTypeWidths[0x5f] = {
		4, 2, 4, 4, 4, 4, 4, 4, 4, 4, 8, 4, 4, 8, 4, 4,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2,
		4, 4, 10, 2, 2, 2, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 4,
		2, 2, 2, 4, 4, 4, 2, 4, 4, 2, 4, 4, 2, 4, 4, 4,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4
	};
	Common::Array<Value> stack;
	Common::HashMap<int, Value> locals;
	int argumentOffset = 8;
	for (uint i = 0; i < arguments.size(); i++) {
		locals[argumentOffset] = arguments[i];
		argumentOffset += arguments[i].width;
	}
	// Указатель инструкций — абсолютное смещение по книге, как `ds:si` у среды
	// (RUN34:0x1416 при возврате снимает со стека именно пару «сегмент,
	// смещение»). Это первый шаг к её модели: один поток кода вместо буфера на
	// каждый обработчик (ledger/0068).
	const byte *code = _book->bytes(0, _book->size());
	if (!code)
		return false;
	uint32 ip = handler.code;
	auto pushNumber = [&](uint32 n, uint8 width = 4) { Value v; v.number = n; v.width = width; stack.push_back(v); };
	auto pushString = [&](const Common::String &s, uint8 width = 4) { Value v; v.string = s; v.isString = true; v.width = width; stack.push_back(v); };
	auto pushReference = [&](uint32 reference, const Common::String &s, uint8 width) {
		Value v;
		v.reference = reference;
		v.hasReference = true;
		v.string = s;
		v.isString = !s.empty();
		v.width = width;
		stack.push_back(v);
	};
	auto pushObject = [&](const Common::String &s) { Value v; v.string = s; v.isObject = true; v.width = 4; stack.push_back(v); };
	// Пролог обработчика (`26` и следом `0f` с отрицательным сдвигом) отводит
	// кадр локальных; всё последующее отведение — временные значения в стеке.
	bool expectFrame = true;
	auto pop = [&]() { Value v; if (!stack.empty()) { v = stack.back(); stack.pop_back(); } return v; };
	auto popBytes = [&](uint bytes) {
		uint consumed = 0;
		while (!stack.empty() && consumed < bytes) {
			consumed += stack.back().width;
			stack.pop_back();
		}
		if (consumed != bytes)
			debug(1, "ToolBook: stack cleanup %u bytes consumed %u @0x%x",
					bytes, consumed, ip);
		return consumed == bytes;
	};
	auto isNullValue = [&](const Value &v) {
		return !v.isString && !v.isObject && !v.hasReference &&
				!v.isBookReference && !v.buffer && v.array.empty() &&
				(v.number == 0 || v.number == 0x04000001);
	};
	auto truth = [&](const Value &v) {
		// Логические значения ToolBook — канонические строки `true`/`false`.
		// Без этой проверки строка «false» считалась истиной, потому что она
		// не пуста (ledger/0054).
		if (v.isString && v.string.equalsIgnoreCase("false"))
			return false;
		if (v.isString && v.string.equalsIgnoreCase("true"))
			return true;
		return v.isString || v.isObject ? !v.string.empty() : !isNullValue(v);
	};
	auto valueString = [&](const Value &v) {
		if (v.buffer && _nativeBuffers.contains(v.buffer)) {
			const NativeBuffer &buffer = _nativeBuffers.getVal(v.buffer);
			Common::String string;
			for (uint i = 0; i < buffer.data.size() && buffer.data[i]; i++)
				string += (char)buffer.data[i];
			return string;
		}
		return v.string;
	};
	// Значение для лога. У чисел `valueString` пусто, и запись вида
	// «массив 1[10] := «»» прятала, легло ли туда посчитанное — а именно ради
	// этого лог и читают.
	auto describe = [&](const Value &v) {
		Common::String string = valueString(v);
		if (!string.empty() || v.isString)
			return Common::String::format("«%s»", string.c_str());
		return Common::String::format("%d", (int32)v.number);
	};
	auto local = [&](int off) { return locals.contains(off) ? locals[off] : Value(); };
	auto read16 = [&](uint32 at) { return (uint16)(code[at] | (code[at + 1] << 8)); };
	auto branch = [&](uint32 end, uint16 rel) { ip = (uint32)((int32)end + (int16)rel); };
	auto parseNativeDescriptor = [&](uint32 base, Common::Array<NativeBinding> *out) {
		uint16 count = _book->readUint16(base);
		if (!count || count > 64 || !_book->bytes(base + 2, (uint32)count * 10))
			return false;
		Common::Array<NativeBinding> parsed;
		uint32 nextName = 2 + (uint32)count * 10;
		for (uint i = 0; i < count; i++) {
			uint32 entry = base + 2 + i * 10;
			uint16 modeNameOffset = _book->readUint16(entry);
			uint16 registryNameOffset = _book->readUint16(entry + 2);
			uint16 ordinal = _book->readUint16(entry + 4);
			uint16 thunkOffset = _book->readUint16(entry + 6);
			const byte *lengths = _book->bytes(entry + 8, 2);
			if (!lengths)
				return false;
			uint8 thunkLength = lengths[0], signatureLength = lengths[1];
			Common::String name = _book->readString(base + registryNameOffset, 255);
			bool identifier = !name.empty() &&
					(Common::isAlpha(name[0]) || name[0] == '_');
			for (uint k = 1; identifier && k < name.size(); k++)
				identifier = Common::isAlnum(name[k]) || name[k] == '_';
			const byte *thunk = _book->bytes(base + thunkOffset, thunkLength);
			bool byName = (modeNameOffset & 0x8000) != 0;
			if (!identifier || registryNameOffset != nextName ||
					thunkOffset != registryNameOffset + name.size() + 1 ||
					thunkLength < 10 || thunkLength != signatureLength + 10 || !thunk ||
					thunk[0] != 0x29 || (thunk[1] & 3) != 0 ||
					thunk[thunkLength - 4] != 0x1c || thunk[thunkLength - 3] != 0x66 ||
					thunk[thunkLength - 2] != thunkLength || thunk[thunkLength - 1] != 0 ||
					(byName && ((modeNameOffset & 0x7fff) != registryNameOffset || ordinal != 0)))
				return false;
			NativeBinding binding;
			binding.name = name;
			binding.descriptor = base;
			binding.thunk = base + thunkOffset;
			binding.ordinal = ordinal;
			binding.argumentBytes = thunk[1];
			binding.thunkLength = thunkLength;
			binding.signatureLength = signatureLength;
			binding.byName = byName;
			parsed.push_back(binding);
			nextName = thunkOffset + thunkLength;
		}
		if (out)
			*out = parsed;
		return true;
	};
	auto globalName = [&](uint32 operand) {
		uint16 rel = read16(operand + 2);
		return _book->handlerString(handler, (uint32)((int32)(operand + 4) + (int16)rel) + 5);
	};
	const uint32 codeEnd = handler.code + handler.codeSize;
	for (uint steps = 0; ip < codeEnd && steps < 10000; steps++) {
		uint8 op = code[ip++];
		debug(6, "ToolBook: опкод %02x @0x%x (стек %u)", op, ip - 1, stack.size());
		switch (op) {
		case 0x02: { int16 off = (int16)read16(ip); ip += 2; Value value = local(off); value.width = 4; stack.push_back(value); break; }
		case 0x01: { int16 off = (int16)read16(ip); ip += 2; Value value = local(off); value.width = 2; stack.push_back(value); break; }
		case 0x04: pushNumber(code[ip++], 2); break;
		case 0x05: pushNumber(read16(ip), 2); ip += 2; break;
		case 0x06: pushNumber((uint32)read16(ip) | ((uint32)read16(ip + 2) << 16)); ip += 4; break;
		case 0x07: case 0x08: {
			uint16 rel = read16(ip);
			ip += 2;
			uint32 reference = (uint32)((int32)(ip) + (int16)rel);
			Common::String literal;
			if (op != 0x08 || !parseNativeDescriptor(reference, nullptr))
				// RUN34:0x0f38 и 0x0f60 кладут указатель ровно на байты по
				// ссылке (`lodsw; add ax,si; push ax`) — никакого пропуска
				// двухбайтового хеша тут нет. Прежняя догадка «если байт не
				// буква, пропустить два» ломала односимвольные литералы вроде
				// «\\»: сравнение брало соседнюю строку (ledger/0063).
				literal = _book->readString(reference, 255);
			pushReference(reference, literal,
					op == 0x07 ? 2 : 4);
			break;
		}
		case 0x0d: { int16 off = (int16)read16(ip); ip += 2; locals[off] = pop(); locals[off].width = 4; break; }
		case 0x0c: { int16 off = (int16)read16(ip); ip += 2; locals[off] = pop(); locals[off].width = 2; break; }
		case 0x0f: {
			int16 adjustment = (int16)read16(ip);
			ip += 2;
			// RUN34:0x10ec — это буквально `add sp, N`. Отрицательное значение
			// отводит место: в прологе под локальные, а посреди обработчика — под
			// временные десятибайтовые числа, адрес которых потом берёт опкод
			// 0x59. Поэтому отведённое место представлено значением нужной ширины
			// (ledger/0082); локальные при этом по-прежнему живут отдельно.
			if (adjustment > 0) {
				popBytes(adjustment);
			} else if (adjustment < 0 && expectFrame) {
				// Первое такое отведение в обработчике — кадр локальных: у нас они
				// живут отдельной таблицей, поэтому на стек операндов не кладутся.
				expectFrame = false;
			} else if (adjustment < 0) {
				Value scratch;
				scratch.width = (uint8)MIN(255, -adjustment);
				stack.push_back(scratch);
			}
			break;
		}
		case 0x10: {
			uint8 bytes = code[ip++];
			const uint end = stack.size();
			uint consumed = 0, first = end;
			while (first && consumed < bytes) {
				first--;
				consumed += stack[first].width;
			}
			if (consumed == bytes)
				for (uint i = first; i < end; i++)
					stack.push_back(stack[i]);
			break;
		}
		case 0x12: { uint16 rel = read16(ip); ip += 2; branch(ip, rel); break; }
		case 0x13: case 0x14: {
			uint16 rel = read16(ip); ip += 2;
			Value tested = pop();
			bool condition = truth(tested);
			bool taken = (op == 0x13 && condition) || (op == 0x14 && !condition);
			debug(5, "ToolBook: ветвление %02x @0x%x условие=%d («%s», число %u) переход=%d",
					op, ip - 3, condition, valueString(tested).c_str(),
					tested.number, taken);
			if (taken) branch(ip, rel);
			break;
		}
		case 0x15: {
			uint16 rel = read16(ip); ip += 2;
			Value tested = pop();
			bool condition = truth(tested);
			debug(5, "ToolBook: ветвление 15 @0x%x условие=%d («%s», число %u) переход=%d",
					ip - 3, condition, valueString(tested).c_str(),
					tested.number, !condition);
			if (!condition) branch(ip, rel);
			break;
		}
		case 0x1e:
		case 0x1f:
		case 0x20: {
			// Вызов builtin с переменным числом операндов: `[u16 номер][u8 сколько
			// байт снять]`. Три опкода различаются только судьбой результата:
			// RUN34:0x1436 после вызова делает `add sp, cleanup` и всё, 0x14b0
			// кладёт обратно слово (`push cx`), а 0x152d — двойное (`push dx;
			// push cx`). То же деление, что у обычных вызовов 0x21/0x22/0x23
			// (ledger/0081).
			uint16 id = read16(ip); ip += 2;
			uint8 cleanup = code[ip++];
			const uint8 resultWidth = op == 0x20 ? 4 : (op == 0x1f ? 2 : 0);
			const bool wantsResult = resultWidth != 0;
			if (id == 248) {
				// `ValueNewArray` (MTB40BAS.207 — имя стоит в таблице экспорта).
				// Обёртка RUN91:0x0f28 передаёт ей код типа, размер элемента
				// результата (таблица `ds:0x0ca0` в DGROUP: 2, 4, 8 и 10 байт),
				// число элементов **младшим байтом** второго операнда и указатель
				// на сами элементы. Внутри (MTB40BAS seg60:0x006e) элементы
				// читаются **словами с шагом 2** — размер из таблицы к источнику
				// отношения не имеет.
				//
				// Книга зовёт это двенадцатью формами с кодами типа 9, 35, 38 и
				// 128 и счётчиками 1..3; на всех двенадцати `cleanup` сходится с
				// `2 + 2 + 2×счётчик` (ledger/0081).
				Value type = pop();
				Value count = pop();
				uint elements = count.number & 0xff;
				if (4 + 2 * elements != cleanup) {
					debug(1, "ToolBook: 248 тип %u, элементов %u, но cleanup %u @0x%x",
							type.number, elements, cleanup, ip - 4);
					return false;
				}
				Common::Array<Value> items;
				for (uint i = 0; i < elements; i++)
					items.push_back(pop());
				Value array;
				array.width = 4;
				array.arrayId = _nextArray++;
				array.number = array.arrayId;
				Common::Array<Value> &store = _arrayStore[array.arrayId];
				for (int i = (int)items.size() - 1; i >= 0; i--)
					store.push_back(items[i]);
				debug(2, "ToolBook: массив %u типа %u из %u элементов",
						array.arrayId, type.number, elements);
				if (wantsResult) {
					array.width = resultWidth;
					stack.push_back(array);
				}
				break;
			}
			if (id == 249 || id == 251 || id == 262) {
				// Чтение элемента массива. Обёртки RUN91:0x11ba, 0x1212 и 0x112a
				// все приходят в `ValueArrayGet` (MTB40BAS.208) и различаются лишь
				// шириной элемента да тем, спрашивают ли сперва `ValueArrayInfo`.
				// На стеке, сверху вниз: массив (4), число измерений (2), индексы
				// по слову (ledger/0081).
				Value array = pop();
				Value dims = pop();
				uint count = dims.number & 0xff;
				Common::Array<uint32> indices;
				for (uint i = 0; i < count; i++)
					indices.push_back(pop().number);
				if (!array.arrayId || !_arrayStore.contains(array.arrayId) || count != 1) {
					debug(1, "ToolBook: чтение элемента не-массива (измерений %u) @0x%x",
							count, ip - 4);
					return false;
				}
				const Common::Array<Value> &store = _arrayStore[array.arrayId];
				uint32 at = indices[0];
				// Чтение за концом — не ошибка: общий поиск элемента
				// (MTB40BAS seg60:0x11ce) на читающем вызове доходит до 0x1394 и
				// возвращает нулевой дальний указатель, из которого получается
				// пустое значение (ledger/0082).
				Value element;
				if (at >= 1 && at <= store.size())
					element = store[at - 1];
				else
					debug(2, "ToolBook: массив %u[%u] за концом (%u элементов) -> пусто",
							array.arrayId, at, store.size());
				debug(2, "ToolBook: массив %u[%u] -> %s", array.arrayId, at,
						describe(element).c_str());
				if (wantsResult) {
					element.width = resultWidth;
					stack.push_back(element);
				}
				break;
			}
			if (id == 250 || id == 252 || id == 263) {
				// Запись элемента: все три обёртки (RUN91:0x135a, 0x13a8 и 0x12d8)
				// зовут `ValueArraySet` (MTB40BAS.209) как `(массив, &значение,
				// число измерений, &индексы)`; различаются шириной значения — два
				// байта против четырёх, — а со стека оно снимается как есть. На
				// стеке, сверху вниз: массив, значение, число измерений, индексы.
				// Нумерация ToolBook с единицы (ledger/0081).
				//
				// Что ширина — единственное различие, видно по общей проверке
				// RUN91:0x1066: она спрашивает у `ValueArrayInfo` размер элемента,
				// берёт его из той же таблицы ширин (DGROUP `0x0ca0`) и сверяет с
				// константой обёртки — 2 у 250, 4 у 252 (ledger/0082).
				Value array = pop();
				Value value = pop();
				Value dims = pop();
				uint count = dims.number & 0xff;
				Common::Array<uint32> indices;
				for (uint i = 0; i < count; i++)
					indices.push_back(pop().number);
				if (!array.arrayId || !_arrayStore.contains(array.arrayId)) {
					debug(1, "ToolBook: запись в не-массив @0x%x", ip - 4);
					return false;
				}
				if (count != 1) {
					debug(1, "ToolBook: массив в %u измерений пока не разобран @0x%x",
							count, ip - 4);
					return false;
				}
				Common::Array<Value> &store = _arrayStore[array.arrayId];
				uint32 at = indices[0];
				if (at < 1) {
					debug(1, "ToolBook: индекс %u в массиве %u @0x%x",
							at, array.arrayId, ip - 4);
					return false;
				}
				// Запись за концом **растит** массив. Это видно в общем поиске
				// элемента (MTB40BAS seg60:0x11ce): ветка 0x1279 берётся только
				// при взведённом признаке записи, и там из индекса считается новый
				// размер — `max(индекс, 1024 / размер элемента)`, то есть место
				// отводится под запрошенный индекс, но не меньше килобайтного
				// куска. Книга на это и рассчитывает: массивы заводятся из одного
				// элемента, а таблица результатов растёт по мере чтения
				// `Scores.inf` (ledger/0082).
				while (store.size() < at)
					store.push_back(Value());
				store[at - 1] = value;
				debug(2, "ToolBook: массив %u[%u] := %s", array.arrayId, at,
						describe(value).c_str());
				if (wantsResult)
					pushNumber(0, resultWidth);
				break;
			}
			if (id != 192) {
				debug(1, "ToolBook: variadic builtin %u (cleanup %u) @0x%x пока не реализован",
						id, cleanup, ip - 4);
				for (int v = (int)stack.size() - 1, n = 0; v >= 0 && n < 10; v--, n++)
					debug(1, "    операнд [-%d]: ширина %u тип %02x число %u стр «%s»",
							n, stack[v].width, stack[v].type, stack[v].number,
							valueString(stack[v]).c_str());
				return false;
			}
			// Aggregate constructor: cleanup includes flag/count words and N
			// dynamic values, but not the receiver beneath them.
			popBytes(cleanup);
			if (wantsResult)
				pushNumber(0, resultWidth);
			break;
		}
		case 0x21: case 0x22: case 0x23: {
			uint16 id = read16(ip); ip += 2;
			if (id == 8) {
				// RUN seg29:013c consumes a module path and a serialized native
				// procedure descriptor. The descriptor begins with a count and
				// fixed ten-byte entries; it is a static far value, not a string.
				Value descriptor = pop();
				Value module = pop();
				if ((!module.isString && !module.isObject) || !descriptor.hasReference) {
					debug(1, "ToolBook: builtin 8 binding operands пока не реализованы @0x%x",
							ip - 3);
					return false;
				}
				uint32 base = descriptor.reference;
				Common::Array<NativeBinding> bindings;
				if (!parseNativeDescriptor(base, &bindings)) {
					debug(1, "ToolBook: поврежден native descriptor @0x%x", base);
					return false;
				}
				Common::String key = module.string;
				key.toUppercase();
				_nativeBindings[key] = bindings;
				for (uint i = 0; i < bindings.size(); i++) {
					Common::String functionKey = bindings[i].name;
					functionKey.toUppercase();
					_nativeFunctions[functionKey] = bindings[i];
				}
				debug(2, "ToolBook: native module %s: %u bindings", key.c_str(), bindings.size());
			} else if (id == 180) { // page(context, name or index)
				Value name = pop();
				Value context = pop();
				Common::String wanted = name.string;
				if (!name.isString && !name.isObject) {
					// `page N of <фон>` — не имя, а **номер страницы внутри фона**.
					// Так книга перебирает message1…Message5, подбирая ту, где
					// сообщение помещается в поле (ledger/0078).
					Common::Array<int> pages = _book->pagesOfBackground(context.string);
					if (name.number >= 1 && name.number <= pages.size())
						wanted = _book->pages()[pages[name.number - 1]].name;
					else
						debug(1, "ToolBook: у фона «%s» нет страницы %u (всего %u) @0x%x",
								context.string.c_str(), name.number, pages.size(), ip - 3);
				}
				if (op != 0x21) pushString(wanted);
			} else if (id == 177) { // background(name)
				Value name = pop();
				pop(); // book/context
				if (op != 0x21) pushObject(name.string);
			} else if (id == 182) { // object(owner, name, class)
				// Класс — номер из таблицы классов книги: 10 — Field, 21 — Picture,
				// 38 — Viewer. Окна лежат отдельной таблицей и объектами страниц не
				// являются, поэтому имя разрешается по ней (ledger/0076, 0077).
				Value klass = pop();
				Value name = pop();
				Value owner = pop();
				Value resolved;
				resolved.string = name.string;
				resolved.isObject = true;
				resolved.width = 4;
				resolved.owner = valueString(owner);
				resolved.isViewer = klass.number == 38;
				if (resolved.isViewer && !_book->findViewer(name.string))
					debug(1, "ToolBook: окно «%s» не найдено в таблице окон @0x%x",
							name.string.c_str(), ip - 3);
				if (op != 0x21) stack.push_back(resolved);
			} else if (id == 329) { // resource(owner, name, class)
				pop();
				Value name = pop();
				pop();
				if (op != 0x21) pushObject(name.string);
			} else if (id == 194 || id == 198) {
				// Оба идут в общий RUN91:0x1da2, различаясь одним флагом: 194
				// кладёт 1, 198 — 0. Помощник считает равенство и в конце
				// сравнивает его с этим флагом (`cmp ax,[bp+0ch]`), возвращая 1
				// при совпадении. Значит **194 — равенство, а 198 —
				// неравенство** (ledger/0062). Раньше 198 считался равенством,
				// и книга уходила в ветку «не найден файл».
				Value b = pop(), a = pop();
				bool eq = a.isString || a.isObject || b.isString || b.isObject ?
						a.string.equalsIgnoreCase(b.string) : a.number == b.number;
				debug(4, "ToolBook: сравнение %u: «%s»(стр=%d,чис=%u) и «%s»(стр=%d,чис=%u) -> равны=%d",
						id, valueString(a).c_str(), a.isString, a.number,
						valueString(b).c_str(), b.isString, b.number, eq);
				pushNumber((id == 194 ? eq : !eq) ? 1 : 0, 2);
			} else if (id == 63) { // ToolBook `&&`: concatenate with one space
				Value right = pop();
				Value left = pop();
				if ((!left.isString && !isNullValue(left)) ||
						(!right.isString && !isNullValue(right))) {
					debug(1, "ToolBook: builtin 63 non-string operands пока не реализованы @0x%x",
							ip - 3);
					return false;
				}
				pushString((isNullValue(left) ? Common::String() : left.string) + " " +
						(isNullValue(right) ? Common::String() : right.string));
			} else if (id == 70) {
				// Логическое «не». RUN91:09bc берёт слово и возвращает слово:
				//     cmp word [bp+6],1  ; CF=1 ⟺ аргумент == 0
				//     sbb ax,ax          ; ax = -CF
				//     neg ax             ; ax = CF
				//     retf 2
				// То есть 1 возвращается ровно при нулевом аргументе. Раньше здесь
				// стояло обратное условие, и ветвление после myMCITest уходило не
				// туда (ledger/0052).
				Value value = pop();
				pushNumber((uint16)value.number == 0 ? 1 : 0, 2);
			} else if (id == 190) { // case-insensitive `does not contain`
				Value haystack = pop();
				Value needle = pop();
				if ((!haystack.isString && !haystack.isObject) ||
						(!needle.isString && !needle.isObject)) {
					debug(1, "ToolBook: builtin 190 non-string operands @0x%x",
							ip - 3);
					return false;
				}
				Common::String foldedHaystack = haystack.string;
				Common::String foldedNeedle = needle.string;
				foldedHaystack.toUppercase();
				foldedNeedle.toUppercase();
				pushNumber(foldedHaystack.find(foldedNeedle) == Common::String::npos, 2);
			} else if (id == 81) {
				// Длина строки: RUN87:0x0bc2 — это `lstrlen` (KERNEL.90) над дальним
				// указателем, у нулевого указателя ноль. Книга так проверяет имя
				// игрока, прочитанное из names.sav.
				Value value = pop();
				Common::String text = valueString(value);
				pushNumber(text.size(), op == 0x22 ? 2 : 4);
			} else if (id == 202) {
				// Сравнение «больше» над десятибайтовыми числами (RUN91:0x0c2e).
				// Целую ветвь видно прямо: старшие слова сравниваются знаково
				// (`jl`/`jg`), младшие — беззнаково (`jbe`), и единица получается,
				// когда **позже положенный операнд больше положенного раньше**.
				// Нецелые операнды та же функция гонит через сопроцессор.
				Value second = pop();
				Value first = pop();
				int32 a = first.isString ? atoi(first.string.c_str()) : (int32)first.number;
				int32 b = second.isString ? atoi(second.string.c_str()) : (int32)second.number;
				pushNumber(b > a ? 1 : 0, 2);
			} else if (id == 210 || id == 211) {
				// Пара «пусто» / «не пусто»: RUN91:0x08cc и 0x0944 различаются тем,
				// что при канонической пустой ссылке (`[bp+6]==1 && [bp+8]==0x400`)
				// первая возвращает 1, а вторая — 0.
				Value value = pop();
				bool null = isNullValue(value);
				pushNumber((id == 210 ? null : !null) ? 1 : 0, 2);
			} else if (id == 23) { // page navigation
				Value target = pop();
				if (target.isString || target.isObject)
					navigateTo(target.string);
			} else if (id == 122) { // system word property getter
				pop();
				// sysLevel is a ToolBook enum word. Keep the symbolic value until
				// enum coercion (2d) tags it as a dynamic value.
				pushString("Reader", 2);
			} else if (id == 69 || id == 71 || id == 72 || id == 73) {
				// Арифметика над десятибайтовыми числами. Все четыре — `retf 0x16`:
				// указатель на приёмник (2 байта) и два числа по 10. Само число это
				// `[u16 признак][8 байт]`: при нулевом признаке грузится
				// `fld qword`, иначе `fild dword`.
				//
				//   69 (RUN91:0x00b2) `fsubr`  — первое минус второе
				//   71 (RUN91:0x02e2) `fdiv`/`fmul`/`fsubr` — остаток от деления
				//   72 (RUN91:0x0012) `fadd`
				//   73 (RUN91:0x0152) `fmul`
				//
				// Приёмник — место в стеке операндов, отведённое опкодом `0x0f`
				// (`add sp, -10`) и адресуемое опкодом `0x59` (`push sp+N`). После
				// вызова это место и оказывается вершиной стека, поэтому следующая
				// же операция берёт результат оттуда. Раньше движок принимал
				// указатель за «признак» и клал результат сам — сходилось по
				// стеку, но ломалось, как только книга завела два временных места
				// сразу (ledger/0082).
				Value destination = pop();
				Value right = pop();
				Value left = pop();
				if (!destination.isStackRef || destination.stackRef >= stack.size() ||
						right.width != 10 || left.width != 10) {
					debug(1, "ToolBook: builtin %u неожиданная форма операндов @0x%x",
							id, ip - 3);
					return false;
				}
				// Элемент массива мог прийти строкой (`getIniVar` отдаёт текст),
				// поэтому число берётся так же, как в сравнении 202.
				int32 a = left.isString ? atoi(left.string.c_str()) : (int32)left.number;
				int32 b = right.isString ? atoi(right.string.c_str()) : (int32)right.number;
				int32 outcome = 0;
				switch (id) {
				case 69: outcome = a - b; break;
				case 71: outcome = b ? a % b : 0; break;
				case 72: outcome = a + b; break;
				default: outcome = a * b; break;
				}
				Value computed = left;
				computed.width = 10;
				computed.type = 0x22;
				computed.number = (uint32)outcome;
				computed.isString = false;
				computed.string.clear();
				stack[destination.stackRef] = computed;
				debug(3, "ToolBook: builtin %u: %d и %d -> %d", id, a, b, outcome);
			} else if (id == 138) {
				// Состояние окна. RUN66:0x0182 пропускает только класс 0x26 и зовёт
				// seg24:0x06c6; там для 0x4035, 0x4127 и 0x40d8 сначала разрешается
				// живая запись окна (seg23:0x13dc), и если её нет — ответ ложь.
				// То есть это вопросы «окно открыто?» и «показано?»: 324 пишет
				// 0x4035, а кнопка диалога спрашивает 0x4127 перед закрытием
				// (ledger/0078).
				Value selector = pop();
				Value object = pop();
				Common::String key = uppercased(object.string);
				bool state = false;
				if (_viewerStates.contains(key))
					state = selector.number == 0x4035 ? _viewerStates[key].visible :
							_viewerStates[key].open;
				if (selector.number != 0x4035 && selector.number != 0x4127) {
					debug(1, "ToolBook: builtin 138 свойство 0x%04x окна «%s» не разобрано @0x%x",
							selector.number, object.string.c_str(), ip - 3);
					return false;
				}
				debug(2, "ToolBook: окно «%s» свойство 0x%04x -> %d",
						object.string.c_str(), selector.number, state);
				pushNumber(state ? 1 : 0, op == 0x22 ? 2 : 4);
			} else if (id == 144) {
				// RUN seg66:02fc consumes a selector W and an input D, and
				// returns a D. The exact serialized selector 0x401b is its
				// identity branch, used to obtain the current object/context.
				Value selector = pop();
				Value value = pop();
				if (selector.number == 0x401c) {
					// Имя класса объекта. Свойство лежит в базе объектов (общий путь
					// RUN66:0x05d2 → CDBGetValueEx), но что в нём — видно по книге:
					// во всех шестидесяти с лишним сравнениях справа стоит имя класса
					// ToolBook — `button`, `field`, `group`, `rectangle`, `ellipse`,
					// `paintobject`, `irregularpolygon` (ledger/0078).
					//
					// Имена берутся из таблицы классов среды (см. classNameOfType).
					const Object *object = findCurrentObject(value.string);
					Common::String className = classNameOfType(object ? object->type : 0);
					if (className.empty()) {
						// Пустое имя класса — не «никакой класс», а «мы не знаем».
						// Книга сравнивает это значение с именем класса, и пустая
						// строка молча увела бы её в чужую ветку. Останавливаем
						// обработчик, как и на любой неразобранной операции.
						debug(1, "ToolBook: имя класса объекта «%s» (тип 0x%02x) не разобрано @0x%x",
								value.string.c_str(), object ? object->type : 0, ip - 3);
						return false;
					}
					pushString(className);
				} else if (selector.number == 0x401b) {
					stack.push_back(value);
				} else {
					debug(1, "ToolBook: builtin 144 selector %04x пока не реализован @0x%x",
							selector.number, ip - 3);
					return false;
				}
			} else if (id == 98) { // Win16 AnsiUpper(string copy)
				Value value = pop();
				if (!value.isString && !value.isObject) {
					debug(1, "ToolBook: builtin 98 non-string conversion пока не реализован @0x%x",
							ip - 3);
					return false;
				}
				value.string.toUppercase();
				value.isString = true;
				value.isObject = false;
				value.width = 4;
				stack.push_back(value);
			} else if (id == 62) { // dynamic string concatenation
				Value right = pop();
				Value left = pop();
				if ((!left.isString && !left.isObject && !isNullValue(left)) ||
						(!right.isString && !right.isObject && !isNullValue(right))) {
					debug(1, "ToolBook: builtin 62 non-string operands пока не реализованы @0x%x",
							ip - 3);
					return false;
				}
				pushString((isNullValue(left) ? Common::String() : left.string) +
						(isNullValue(right) ? Common::String() : right.string));
			} else if (id == 163) {
				// The reached form writes an incremented ten-byte numeric value to
				// an output pointer. Scratch storage is represented as a Value cell,
				// so return that cell on the abstract stack for the following dup10.
				Value outputPointer = pop();
				Value value = pop();
				if (value.width != 10) {
					debug(1, "ToolBook: builtin 163 non-extended operand @0x%x",
							ip - 3);
					return false;
				}
				(void)outputPointer;
				value.number++;
				value.width = 10;
				stack.push_back(value);
			} else if (id == 206) {
				// RUN91:0c9c compares two ten-byte numeric values. The bytecode
				// pushes the iterator first and the upper bound second, so the
				// returned word is true while upperBound >= iterator.
				Value upperBound = pop();
				Value iterator = pop();
				if (upperBound.width != 10 || iterator.width != 10) {
					debug(1, "ToolBook: builtin 206 non-extended operands @0x%x",
							ip - 3);
					return false;
				}
				pushNumber(iterator.number <= upperBound.number ? 1 : 0, 2);
						} else if (id == 139) {
				// Запись свойства объекта. RUN81:0x021c ветвится по классу объекта
				// (`байт[bp+0dh] and 0FCh`), неизвестные номера уходят общим путём в
				// `seg8:0x04ea`, а тот зовёт `MTB40BAS.CDBSetValueEx` — то есть
				// свойства это записи базы объектов. Аргументы (в порядке укладки):
				// объект, значение, номер свойства; `retf 8` (ledger/0056).
				Value propertyId = pop();
				Value propertyValue = pop();
				Value object = pop();
				if (object.string.empty()) {
					debug(1, "ToolBook: builtin 139 без объекта @0x%x", ip - 3);
					return false;
				}
				setFieldProperty(object, propertyId.number, propertyValue);
				_objectProperties[propertyKey(object.string, propertyId.number)] = propertyValue;
				debug(2, "ToolBook: свойство 0x%04x объекта «%s» := %u",
						propertyId.number, object.string.c_str(), propertyValue.number);
				if (op != 0x21)
					stack.push_back(propertyValue);
						} else if (id == 143) {
				// То же, что 139, но значение — четырёхбайтовое (RUN81:0x0440: тот же
				// разбор, только класс объекта читается на два слова дальше).
				Value propertyId = pop();
				Value propertyValue = pop();
				Value object = pop();
				if (object.string.empty()) {
					debug(1, "ToolBook: builtin 143 без объекта @0x%x", ip - 3);
					return false;
				}
				setFieldProperty(object, propertyId.number, propertyValue);
				_objectProperties[propertyKey(object.string, propertyId.number)] = propertyValue;
				debug(2, "ToolBook: свойство 0x%04x объекта «%s» := «%s»",
						propertyId.number, object.string.c_str(),
						valueString(propertyValue).c_str());
				if (op != 0x21)
					stack.push_back(propertyValue);
						} else if (id == 140) {
				// Чтение свойства: RUN66:0x0726, `retf 6` — объект и номер, значение
				// возвращается. Внутри это MTB40BAS.37 по той же базе объектов.
				// Пока отдаём то, что сами записали; если свойство не ставилось,
				// значение пустое, и это видно в логе — оригинал взял бы умолчание
				// из сериализованной записи объекта (ledger/0056).
				Value propertyId = pop();
				Value object = pop();
				if (propertyId.number == 0x4030 || propertyId.number == 0x41f8) {
					// Признаки переполнения поля. Среда держит их в базе объектов
					// (обычный `CDBGetValueEx` через seg8:0x0338), а заполняет их
					// разметка текста, поэтому в машинном коде смысла каждого из
					// двух не видно. Наблюдаемый договор книги однозначен: страница
					// годится, **когда оба нуля** — цикл `myMessage` перебирает
					// message1…Message5 и лишь потом уменьшает кегль до 10 и пишет
					// «Не влазит сообщение». Поэтому оба признака движок считает
					// одинаково: текст не помещается в прямоугольник поля.
					const Object *field = findFieldObject(object);
					bool overflow = field && !fieldTextFits(*field);
					debug(2, "ToolBook: поле «%s» (в «%s») переполнено: %d",
							object.string.c_str(), object.owner.c_str(), overflow);
					pushNumber(overflow ? 1 : 0, op == 0x22 ? 2 : 4);
				} else {
					Common::String storeKey = propertyKey(object.string, propertyId.number);
					if (_objectProperties.contains(storeKey)) {
						stack.push_back(_objectProperties[storeKey]);
					} else {
						debug(1, "ToolBook: свойство 0x%04x объекта «%s» не ставилось @0x%x",
								propertyId.number, object.string.c_str(), ip - 3);
						pushString(Common::String());
					}
				}
						} else if (id == 212) {
				// Материализация текстового диапазона. У оригинала опкод 0x33 строит
				// описатель (совокупность, единица измерения, номер), а RUN86:0x00ea
				// его исполняет: берёт длину исходной строки (KERNEL.90), ищет границы
				// (MTB40BAS.69) и собирает значение (MTB40BAS.109/106). По всей книге
				// 212 вызывается ровно после 0x33 — восемь раз с `char -1` и дважды с
				// единицей 1 (ledger/0063).
				//
				// Наш 0x33 вычисляет подстроку сразу, поэтому здесь остаётся только
				// убедиться, что на стеке материализованное строковое значение.
				if (stack.empty()) {
					debug(1, "ToolBook: builtin 212 без операнда @0x%x", ip - 3);
					return false;
				}
				Value range = pop();
				if (!range.isString)
					range.string = valueString(range);
				range.isString = true;
				range.width = 4;
				stack.push_back(range);
						} else if (id == 152) {
				// Ещё одно чтение свойства (RUN66:0x103a — тот же сегмент, что и
				// обычное чтение 140). `retf 6`: со стека снимаются только номер
				// свойства и объект. Раньше здесь снималось ещё и «вместилище», и
				// лишний операнд съедал объект, приготовленный для следующей записи
				// свойства — а та брала объект из получателя. Две ошибки гасили друг
				// друга, пока не понадобилось именно то, что съедалось (ledger/0077).
				Value propertyId = pop();
				Value object = pop();
				if (propertyId.number == 0x4020) {
					// «Страница этого объекта»: RUN66:0x177e разрешает вместилище
					// объекта. Книга так добирается от поля «Message» до страницы,
					// которую потом показывает окно.
					Common::String page = object.owner;
					if (page.empty()) {
						int index = _book->pageOfObject(object.string);
						if (index >= 0)
							page = _book->pages()[index].name;
					}
					if (page.empty())
						debug(1, "ToolBook: страница объекта «%s» не найдена @0x%x",
								object.string.c_str(), ip - 3);
					pushObject(page);
				} else {
					Common::String storeKey = propertyKey(object.string, propertyId.number);
					if (_objectProperties.contains(storeKey)) {
						stack.push_back(_objectProperties[storeKey]);
					} else {
						debug(1, "ToolBook: свойство 0x%04x объекта «%s» не ставилось (builtin 152) @0x%x",
								propertyId.number, object.string.c_str(), ip - 3);
						pushString(Common::String());
					}
				}
						} else if (id == 150) {
				// Ещё одна форма чтения свойства (RUN66:0x0dea, тот же сегмент разбора
				// свойств): со стека снимаются номер и значение, называющее объект.
				Value propertyId = pop();
				Value object = pop();
				Common::String storeKey = propertyKey(valueString(object), propertyId.number);
				if (_objectProperties.contains(storeKey)) {
					stack.push_back(_objectProperties[storeKey]);
				} else {
					debug(1, "ToolBook: свойство 0x%04x у «%s» не ставилось (builtin 150)",
							propertyId.number, valueString(object).c_str());
					pushString(Common::String());
				}
						} else if (id == 149 || id == 151) {
				// Запись свойства (RUN81:0x0c0a и 0x0e26 — сегмент записи свойств).
				// У обоих `retf 0xa`: номер свойства, четырёхбайтовое значение и
				// объект. Объект приходит со стека, а не из получателя обработчика:
				// именно так книга пишет `page of Dial` — окно кладётся вызовом 182
				// прямо перед значением (ledger/0077).
				Value propertyId = pop();
				Value propertyValue = pop();
				Value object = pop();
				Common::String target = object.string.empty() ? receiver.string : object.string;
				if (target.empty()) {
					debug(1, "ToolBook: builtin %u без объекта @0x%x", id, ip - 3);
					return false;
				}
				if (object.isViewer && propertyId.number == 0x40d7) {
					// «Страница окна»: единственное свойство класса 0x26 в разборе
					// чтения (RUN66:0x1318 проверяет класс перед разбором). Им книга
					// и говорит, что показывать в диалоге.
					Common::String page = valueString(propertyValue);
					_viewerStates[uppercased(target)].page = page;
					_needsRedraw = true;
					debug(2, "ToolBook: окно «%s» показывает страницу «%s»",
							target.c_str(), page.c_str());
				}
				_objectProperties[propertyKey(target, propertyId.number)] = propertyValue;
				debug(2, "ToolBook: свойство 0x%04x объекта «%s» := «%s» (форма %u)",
						propertyId.number, target.c_str(),
						valueString(propertyValue).c_str(), id);
				if (op != 0x21)
					stack.push_back(propertyValue);
			} else if (id == 324) {
				// Показать окно. RUN90:0x0276, `retf 0xc`: окно и три постоянных
				// операнда (2, 0, 0 — во всех одиннадцати вызовах книги). Исполнитель
				// RUN90:0x02d8 работает только с классом 0x26 и в конце пишет окну
				// свойство 0x4035 (ledger/0077).
				Value value = pop();
				Value second = pop();
				Value first = pop();
				Value object = pop();
				ViewerState &viewer = _viewerStates[uppercased(object.string)];
				viewer.visible = true;
				_needsRedraw = true;
				debug(2, "ToolBook: окно «%s» показано (%u, %u, «%s») @0x%x",
						object.string.c_str(), first.number, second.number,
						valueString(value).c_str(), ip - 3);
			} else if (id == 346 || id == 347) {
				// Открыть (346, RUN67:0x12d0) и закрыть (347, RUN67:0x1332) окно.
				// Обе `retf 4` и различаются одним операндом общего исполнителя
				// RUN67:0x15f6: тот работает только с классом 0x26 (`cmp byte
				// [bp+0xa], 0x26`) и зовёт seg24:0x0000 с режимом 1 либо 2 — режим 1
				// создаёт окно, дальше идёт `IsWindow` (ledger/0077).
				Value object = pop();
				ViewerState &viewer = _viewerStates[uppercased(object.string)];
				viewer.open = (id == 346);
				if (id == 347) {
					viewer.visible = false;
					viewer.page.clear();
				}
				_needsRedraw = true;
				debug(2, "ToolBook: окно «%s» %s", object.string.c_str(),
						id == 346 ? "открыто" : "закрыто");
						} else if (id == 175) {
				// Запуск внешней программы (RUN83:0x0000 -> MTB40BAS.157). Книга так
				// показывает вступительный ролик `.\demo\knt_demo.exe` — отдельный
				// 16-битный EXE, которого у движка нет и быть не может. Ролик
				// пропускается; результат — пустое значение, то есть «задача не
				// запущена». Это ограничение платформы, а не выбор игровой ветки.
				Value program = pop();
				debug(1, "ToolBook: внешняя программа «%s» не запускается 	0x%x",
						valueString(program).c_str(), ip - 3);
				if (op != 0x21)
					pushString(Common::String());
			} else if (id == 180) {
				// `page <имя>` — разрешение страницы по имени (класс 180 в таблице имён
				// OpenScript, tools/osc.py). Возвращаем объектное значение с этим именем:
				// дальше книга либо переходит на страницу, либо читает её свойства.
				Value pageName = pop();
				Common::String wanted = valueString(pageName);
				int found = findPageIndex(wanted);
				if (found < 0)
					debug(1, "ToolBook: страница «%s» не найдена 	0x%x", wanted.c_str(), ip - 3);
				Value pageValue;
				pageValue.string = wanted;
				pageValue.isObject = true;
				pageValue.width = 4;
				stack.push_back(pageValue);
						} else if (id == 234) {
				// RUN67:0x01a0: ставит признак `ds:[870h] = 1` и зовёт seg30:0x1048.
				// Без аргументов и без результата — состояние показа. Отмечаем.
				debug(2, "ToolBook: состояние показа 234 	0x%x", ip - 3);
						} else if (id == 121) {
				// RUN95:0x0b22, вызывается вариантом без результата с двумя словами.
				// Что делает, не прочитано; на ветвление не влияет — отмечаем.
				Value second = pop();
				Value first = pop();
				debug(1, "ToolBook: действие 121 (%u, %u) пока не выполняется 	0x%x",
						first.number, second.number, ip - 3);
						} else if (id == 124) {
				// Метрика среды по номеру (RUN85:0x0b48 разбирает номер и для 0x75
				// зовёт MTB40UTL.94 за метриками экрана, затем делит константу на
				// разрешение и собирает пару). Номер 117 — сколько единиц ToolBook
				// приходится на пиксель: при 96 точках на дюйм это 1440/96 = 15 по
				// обеим осям, и книга сравнивает результат ровно со строкой «15,15».
				// Наш экран — тот же 96 dpi, что и на эталонном стенде (ledger/0069).
				Value which = pop();
				if (which.number != 117) {
					debug(1, "ToolBook: метрика %u (builtin 124) пока не реализована 	0x%x",
							which.number, ip - 3);
					return false;
				}
				const uint32 unitsPerInch = 1440, dotsPerInch = 96;
				pushString(Common::String::format("%u,%u", unitsPerInch / dotsPerInch,
						unitsPerInch / dotsPerInch));
						} else if (id == 130) {
				// Метрика среды по номеру (RUN85:0x106e). Номер 39 уходит на 0x111c,
				// где вызывается seg1:0x06e6 — а это прямой вызов DOS `mov ah,2ch`,
				// «получить системное время». Из ответа берутся три байта (часы,
				// минуты, секунды) и складываются в трёхсимвольное значение
				// (ledger/0070). Отдаём то же самое по часам системы.
				Value which = pop();
				if (which.number != 39) {
					debug(1, "ToolBook: метрика %u (builtin 130) пока не реализована 	0x%x",
							which.number, ip - 3);
					return false;
				}
				TimeDate now;
				g_system->getTimeAndDate(now);
				char stamp[3] = { (char)now.tm_hour, (char)now.tm_min, (char)now.tm_sec };
				pushString(Common::String(stamp, 3));
						} else if (id == 168) {
				// Преобразование в единицы времени (RUN80:0x0144 через MTB40BAS.94).
				// Достигнутая форма — (значение, 0, «seconds»): книга берёт отсчёт
				// времени в секундах. Отдаём текущее время суток по часам системы —
				// разность двух таких вызовов и есть то, ради чего книга их делает
				// (ledger/0070).
				Value units = pop();
				Value amount = pop();
				Value source = pop();
				Common::String unitName = valueString(units);
				if (!unitName.equalsIgnoreCase("seconds")) {
					debug(1, "ToolBook: преобразование в «%s» (builtin 168) пока не реализовано 	0x%x",
							unitName.c_str(), ip - 3);
					return false;
				}
				TimeDate now;
				g_system->getTimeAndDate(now);
				uint32 seconds = now.tm_hour * 3600u + now.tm_min * 60u + now.tm_sec +
						amount.number;
				(void)source;
				pushString(Common::String::format("%u", seconds));
						} else if (id == 50) {
				// Засев датчика случайных чисел (RUN67:0x1054: значение уходит в
				// seg1:0x0588, затем десять прогревочных вызовов seg1:0x05a0).
				// Книга берёт зерно из времени (метрика 39 → секунды → остаток),
				// поэтому просто передаём его нашему датчику (ledger/0070).
				Value seed = pop();
				_random.setSeed(seed.number);
				debug(2, "ToolBook: датчик случайных чисел засеян %u", seed.number);
						} else if (id == 86) {
				// Число элементов списка (RUN87:0x0b76). Элементы в ToolBook разделены
				// запятыми; книга сразу после этого берёт случайный элемент, а `0x33`
				// с единицей измерения 1 обращается к ним по номеру (ledger/0070).
				Value list = pop();
				Common::String text = valueString(list);
				uint32 items = text.empty() ? 0 : 1;
				for (uint i = 0; i < text.size(); i++)
					if (text[i] == ',')
						items++;
				pushNumber(items, op == 0x22 ? 2 : 4);
			} else if (id == 91) {
				// Случайное число (RUN87:0x0a7c: тот же генератор seg1:0x05a0, что
				// прогревается при засеве, затем `fimul` на аргумент). В OpenScript
				// `random(n)` даёт от 1 до n.
				Value bound = pop();
				uint32 n = bound.number ? bound.number : 1;
				pushNumber(_random.getRandomNumberRng(1, n), op == 0x22 ? 2 : 4);
						} else if (id == 125) {
				// Настройка среды по номеру (RUN95:0x0000 разбирает номер длинной
				// цепочкой). Вызывается вариантом 0x21 — без результата, парой
				// «значение, номер». На ветвление не влияет: отмечаем и идём дальше.
				Value which = pop();
				Value value = pop();
				debug(1, "ToolBook: настройка %u := %u (builtin 125) пока не применяется 	0x%x",
						which.number, value.number, ip - 3);
						} else if (id == 106) {
				// RUN87:09fe, retf 8: два дальних указателя на строки. Зовёт
				// MTB40BAS.108 и, если та вернула непустой указатель, отдаёт
				// `результат - начало строки + 1`. То есть это `offset()` —
				// позиция подстроки, считая с единицы, или 0 (ledger/0054).
				// Искомое лежит в аргументе, уложенном первым, строка — во втором.
				Value haystack = pop();
				Value needle = pop();
				if (!haystack.isString || !needle.isString) {
					debug(1, "ToolBook: builtin 106 нестроковые операнды @0x%x",
							ip - 3);
					return false;
				}
				const char *at = needle.string.empty() ? nullptr :
						strstr(haystack.string.c_str(), needle.string.c_str());
				pushNumber(at ? (uint32)(at - haystack.string.c_str()) + 1 : 0,
						op == 0x22 ? 2 : 4);
						} else if (id == 116) {
				// RUN87:125c, retf 0xc. Аргументы (в порядке укладки): флаг копии,
				// смещение, приёмник, источник. Функция строит значение-строку по
				// дальнему указателю `приёмник + смещение` (MTB40BAS.109) и, если
				// флаг не ноль и источник не пуст, сперва копирует источник туда
				// (KERNEL.88 = lstrcpy). Возвращается всегда значение, построенное по
				// приёмнику, — поэтому чтение заполненного буфера идёт этой же
				// функцией с нулевым флагом (ledger/0054).
				Value source = pop();
				Value destination = pop();
				Value offset = pop();
				Value copyFlag = pop();
				if (!destination.buffer || !_nativeBuffers.contains(destination.buffer)) {
					debug(1, "ToolBook: builtin 116 приёмник не буфер (%u) @0x%x",
							destination.buffer, ip - 3);
					return false;
				}
				NativeBuffer &buffer = _nativeBuffers[destination.buffer];
				uint32 from = MIN<uint32>(offset.number, buffer.data.size());
				if (truth(copyFlag)) {
					if (!source.isString) {
						debug(1, "ToolBook: builtin 116 источник не строка @0x%x",
								ip - 3);
						return false;
					}
					uint32 count = from >= buffer.data.size() ? 0 :
							MIN<uint32>(source.string.size(), buffer.data.size() - from - 1);
					for (uint32 i = 0; i < count; i++)
						buffer.data[from + i] = (byte)source.string[i];
					if (from + count < buffer.data.size())
						buffer.data[from + count] = 0;
				}
				Common::String text;
				for (uint32 i = from; i < buffer.data.size() && buffer.data[i]; i++)
					text += (char)buffer.data[i];
				pushString(text);
			} else if (id == 33 || id == 56) { // hide/show object
				Value object = pop();
				if (object.isObject || object.isString)
					setObjectVisible(object.string, id == 56);
			} else if (id == 129) { // sysCursor setter
				pop();
				pop();
			} else if (id == 127 || id == 128) {
				// Ячейка среды: 127 (RUN95:0x02b0, `retf 6`) пишет, 128
				// (RUN85:0x061e, `retf 2`) читает — обе по номеру. У номера 12 это
				// буквально одна пара глобальных слов `ds:[0x8cc]`: ветка записи
				// кладёт туда значение (пустое даёт каноническую пустую ссылку
				// `{1, 0x400}`), ветка чтения его же и достаёт (ledger/0077).
				//
				// Книга пользуется парой как ловушкой ошибки: гасит ячейку, делает
				// вызов, потом смотрит, не появилось ли в ней сообщение.
				uint32 slot;
				Value stored;
				if (id == 127) {
					slot = pop().number;
					stored = pop();
				} else {
					slot = pop().number;
					stored = _systemSlots.contains(slot) ? _systemSlots[slot] : Value();
				}
				if (slot != 12) {
					debug(1, "ToolBook: ячейка среды %u (builtin %u) не разобрана @0x%x",
							slot, id, ip - 3);
					return false;
				}
				if (id == 127) {
					_systemSlots[slot] = stored;
					debug(2, "ToolBook: ячейка среды 12 := «%s»", valueString(stored).c_str());
				} else if (op != 0x21) {
					stack.push_back(stored);
				}
			} else if (id == 247) { // select a complete Field text range
				Value object = pop();
				const Object *field = findCurrentObject(object.string);
				if (field && field->field) {
					_focusedField = field->block;
					_fieldSelectAll = true;
					_needsRedraw = true;
				}
			} else if (id == 364) { // flushMessageQueue: none pending in this VM loop
				if (op != 0x21)
					pushNumber(0, op == 0x22 ? 2 : 4);
			} else if (id == 482) { // callMCI(command, optional notify receiver)
				Value notifyReceiver = pop();
				Value commandValue = pop();
				if (!commandValue.isString) {
					debug(1, "ToolBook: callMCI non-string command @0x%x", ip - 3);
					return false;
				}
				Common::String command = commandValue.string;
				Common::String folded = command;
				folded.toLowercase();
				while (folded.hasSuffix(" wait"))
					folded.erase(folded.size() - 5);
				if (folded == "close all") {
					_mciAliases.clear();
					pushString(Common::String());
				} else if (folded == "sysinfo all quantity") {
					// Win98 exposes the installed MCI driver classes. These are
					// platform capabilities, not media or page-specific state.
					pushString("4");
				} else if (folded.hasPrefix("sysinfo all name ")) {
					const char *drivers[] = { "CDAudio", "Sequencer", "WaveAudio", "AVIVideo" };
					uint index = atoi(folded.c_str() + 17);
					pushString(index >= 1 && index <= ARRAYSIZE(drivers) ?
							drivers[index - 1] : Common::String());
				} else if (folded.hasPrefix("open ")) {
					// `open <файл> [type <драйвер>] alias <имя>` — заводим псевдоним.
					// Разбираем ту же грамматику, что и MCI: имя файла идёт первым,
					// после `alias` — как его будут звать дальше (ledger/0070).
					Common::String rest(command.c_str() + 5), fileName, alias;
					uint32 space = rest.findFirstOf(' ');
					fileName = space == Common::String::npos ? rest : Common::String(rest.c_str(), space);
					Common::String lowered = rest;
					lowered.toLowercase();
					uint32 at = lowered.find("alias ");
					if (at != Common::String::npos) {
						alias = Common::String(rest.c_str() + at + 6);
						uint32 cut = alias.findFirstOf(' ');
						if (cut != Common::String::npos)
							alias.erase(cut);
					}
					if (alias.empty()) {
						debug(1, "ToolBook: MCI open без псевдонима: %s", command.c_str());
						return false;
					}
					Common::String key = alias;
					key.toLowercase();
					_mciAliases[key] = fileName;
					debug(2, "ToolBook: MCI открыт «%s» как «%s»", fileName.c_str(), alias.c_str());
					pushString(Common::String());
				} else if (folded.hasPrefix("close ")) {
					Common::String alias(folded.c_str() + 6);
					uint32 cut = alias.findFirstOf(' ');
					if (cut != Common::String::npos)
						alias.erase(cut);
					if (_mciAliases.contains(alias))
						_mciAliases.erase(alias);
					stopMedia(alias);
					debug(2, "ToolBook: MCI закрыт «%s»", alias.c_str());
					pushString(Common::String());
				} else if (folded.hasPrefix("play ") || folded.hasPrefix("stop ") ||
						folded.hasPrefix("seek ") || folded.hasPrefix("pause ")) {
					Common::String verb(folded.c_str(), folded.findFirstOf(' '));
					Common::String alias(folded.c_str() + verb.size() + 1);
					uint32 cut = alias.findFirstOf(' ');
					if (cut != Common::String::npos)
						alias.erase(cut);
					if (verb == "play")
						playMedia(alias);
					else if (verb == "stop" || verb == "pause")
						stopMedia(alias);
					debug(2, "ToolBook: MCI «%s» для «%s»", verb.c_str(), alias.c_str());
					pushString(Common::String());
				} else {
					debug(1, "ToolBook: MCI command пока не реализована: %s", command.c_str());
					return false;
				}
				(void)notifyReceiver;
			} else {
				debug(1, "ToolBook: builtin %u @0x%x пока не реализован",
						id, ip - 3);
				// Верхушка стека — операнды нереализованного builtin: печатаем их,
				// чтобы барьер сразу показывал, с чем его зовут.
				for (int v = (int)stack.size() - 1, n = 0; v >= 0 && n < 8; v--, n++)
					debug(1, "    операнд [-%d]: ширина %u тип %02x число %u стр «%s»",
							n, stack[v].width, stack[v].type, stack[v].number,
							valueString(stack[v]).c_str());
				return false;
			}
			break;
		}
		case 0x26: expectFrame = true; break;
		case 0x27: return true;
		case 0x28:
			if (result) {
				if (stack.empty())
					return false;
				*result = pop();
			}
			return true;
		case 0x2b: { // coerce a dynamic value to another dynamic type
			uint8 type = code[ip++];
			if (!stack.empty()) {
				if (type == 9 && stack.back().isBookReference) {
					// CDBQUERYFILEPATH on the current BookRef yields the directory
					// containing the opened book. ScummVM exposes that directory as
					// the root of SearchMan, so its DOS-visible relative spelling is
					// sufficient and keeps host paths out of OpenScript values.
					stack.back().string = ".\\";
					stack.back().isString = true;
					stack.back().isBookReference = false;
				}
				stack.back().type = type;
				stack.back().width = 4;
			}
			break;
		}
		case 0x2c: { // materialize a dynamic value in the requested raw type
			uint8 type = code[ip++];
			if (!stack.empty()) {
				if (type == 0x22 && stack.back().isString) {
					// MCI numeric responses are strings until the compiler asks for
					// ToolBook's ten-byte extended numeric representation.
					stack.back().number = (uint32)(int32)atoi(stack.back().string.c_str());
					stack.back().string.clear();
					stack.back().isString = false;
				} else if (stack.back().isString && type != 9 &&
						(stack.back().string.equalsIgnoreCase("true") ||
						 stack.back().string.equalsIgnoreCase("false"))) {
					// Логическое значение ToolBook хранится канонической строкой
					// `true`/`false`. При материализации в числовой тип оно должно
					// стать 1/0: стартовый скрипт отдаёт результат myMCITest именно
					// так, а builtin 70 (логическое «не», RUN91:09bc) читает слово.
					// Без этого «true» приходило нулём и книга уходила в ветку
					// «звуковое устройство не обнаружено» (ledger/0054).
					stack.back().number = stack.back().string.equalsIgnoreCase("true") ? 1 : 0;
				} else if (type == 9 && !stack.back().isString && !stack.back().isObject &&
						!stack.back().hasReference && !stack.back().isBookReference &&
						!stack.back().buffer && stack.back().array.empty()) {
					// Тип 9 — сишная строка. Опкод 0x2c (RUN34:0x2821) для целей
					// ниже 0x22 уходит в `MTB40BAS.97` — `ValueConvertToC`, то есть
					// значение материализуется в строку **независимо от исходного
					// типа**; матрица допустимых преобразований (`ss:[0x862]`)
					// проверяется только для целей 0x22..0x5f.
					//
					// Раньше здесь стояло условие «исходный тип 0x23», и книга не
					// могла собрать «Score_» & 1 — имя ключа в Scores.inf
					// (ledger/0081).
					stack.back().string = Common::String::format("%d", (int32)stack.back().number);
					stack.back().isString = true;
				}
				stack.back().type = type;
				stack.back().width = type < ARRAYSIZE(kTypeWidths) ? kTypeWidths[type] : 4;
			}
			break;
		}
		case 0x2d: { // tag the raw value already on the stack as a dynamic D
			uint8 type = code[ip++];
			if (!stack.empty()) {
				stack.back().type = type;
				stack.back().width = 4;
			}
			break;
		}
		case 0x2e: { // convert one dynamic D type to another
			uint8 type = code[ip++];
			if (!stack.empty()) {
				stack.back().type = type;
				stack.back().width = 4;
			}
			break;
		}
		case 0x2f: case 0x30: case 0x31: case 0x77: break;
		case 0x56: // 10-byte extended value -> dynamic D
			if (!stack.empty())
				stack.back().width = 4;
			break;
		case 0x33: {
			uint8 mode = code[ip++];
			Value index = pop();
			Value aggregate = pop();
			if (mode == 1 && aggregate.isString) {
				// Единица 1 — элементы, разделённые запятыми; нумерация с единицы.
				Common::String rest = aggregate.string, item;
				uint32 wanted = index.number, seen = 1;
				uint32 from = 0;
				for (uint i = 0; i <= rest.size(); i++) {
					if (i == rest.size() || rest[i] == ',') {
						if (seen == wanted) {
							item = Common::String(rest.c_str() + from, i - from);
							break;
						}
						seen++;
						from = i + 1;
					}
				}
				pushString(item);
			} else if (mode == 2 && !aggregate.array.empty() && index.number < aggregate.array.size())
				pushString(aggregate.array[index.number]);
			else if (mode == 0 && aggregate.isString && !aggregate.string.empty()) {
				int32 at = index.number == 0xffff ? -1 : (int32)index.number;
				if (at < 0)
					at += aggregate.string.size();
				pushString(at >= 0 && at < (int32)aggregate.string.size() ?
						Common::String(aggregate.string[at]) : Common::String());
			}
			else {
				debug(1, "ToolBook: aggregate index mode %u/%u пока не реализован @0x%x",
						mode, index.number, ip - 2);
				return false;
			}
			break;
		}
		case 0x35: {
			// RUN34:0x44bc: `lodsb` — единица измерения, затем со стека снимаются
			// два слова (`pop cx` — верхняя граница, `pop dx` — нижняя) и строка.
			// Дальше считается длина и вызывается вырезка. Достигнута только
			// единица 0 — символы; границы включительные и считаются с единицы,
			// что видно по результату: `chars 1 to 15` от
			// `C:\NMG\KNTOWER\KNTOWER.TBC` даёт каталог (ledger/0054).
			uint8 unit = code[ip++];
			Value to = pop();
			Value from = pop();
			Value text = pop();
			if (unit != 0 || !text.isString) {
				debug(1, "ToolBook: опкод 0x35 единица %u/строка %d @0x%x",
						unit, text.isString, ip - 2);
				return false;
			}
			uint32 first = from.number > 0 ? from.number - 1 : 0;
			uint32 last = MIN<uint32>(to.number, text.string.size());
			pushString(last > first ?
					Common::String(text.string.c_str() + first, last - first) :
					Common::String());
			break;
		}
		case 0x3b: {
			uint8 slot = code[ip++];
			if (slot == 0) {
				// CDB slot 0 is a BookRef (runtime tag class 0x68), not a Page
				// object and not yet a pathname. Type-9 coercion materializes its
				// containing directory through CDBQUERYFILEPATH.
				Value bookReference;
				bookReference.isBookReference = true;
				bookReference.type = 0x68;
				stack.push_back(bookReference);
			} else if (slot == 2)
				pushString(_book->pages()[_currentPage].name);
			else if (slot == 3 || slot == 4)
				stack.push_back(receiver);
			else
				pushNumber(0);
			break;
		}
		case 0x3c:
			if (!stack.empty())
				stack.back().width = 4;
			break;
		case 0x3e: { // cached global-symbol load
			Common::String name = globalName(ip);
			ip += 4;
			if (_scriptGlobals.contains(name))
				stack.push_back(_scriptGlobals.getVal(name));
			else
				pushNumber(0);
			break;
		}
		case 0x3f: { // cached global-symbol store
			Common::String name = globalName(ip);
			ip += 4;
			Value value = pop();
			_scriptGlobals[name] = value;
			break;
		}
		case 0x45: case 0x46: {
			// These opcodes move a five-word (10-byte) extended value. The
			// abstract VM keeps that aggregate in one Value cell. Do not confuse
			// this with the one-word 01/0c or two-word 02/0d families.
			int16 off = (int16)read16(ip);
			ip += 2;
			if (op == 0x45) {
				Value value = local(off);
				value.width = 10;
				stack.push_back(value);
			} else {
				locals[off] = pop();
				locals[off].width = 10;
			}
			break;
		}
		case 0x68: { // tagged dynamic value -> 4-byte local
			int16 off = (int16)read16(ip);
			ip += 2;
			locals[off] = pop();
			locals[off].width = 4;
			break;
		}
		case 0x71: {
			// 0x71 is a packed conversion family. Implement only the two
			// machine-verified forms reached by this book's MCI capability loop.
			uint16 form = read16(ip);
			ip += 2;
			if (stack.empty())
				return false;
			if ((form & 0xff) == 0x15) {
				// RUN34:0x30dd (по таблице seg34:0x2e2e, индекс
				// `(низкий & 0x0f) << 1 | (низкий & 0xf0)`): снимает слово и
				// выкладывает десятибайтовое число. Расширение зависит от
				// знакового бита старшего байта формы: `or ah,ah / jns` —
				// при взведённом бите идёт `cdq`, иначе dx обнуляется.
				// Поэтому 0x8315 — со знаком, 0x0315 — без (ledger/0054).
				stack.back().number = (form & 0x8000) ?
						(uint32)(int32)(int16)stack.back().number :
						(stack.back().number & 0xffff);
				stack.back().type = 0x22;
				stack.back().width = 10;
			} else if ((form & 0xff) == 0x25) {
				// RUN34:0x3162 — брат 0x15 для четырёхбайтового источника:
				// `pop ax; pop dx`, дальше та же выкладка `push 0; push 0;
				// push dx; push ax; push 1`, то есть десять байт
				// `[u16 признак=1][dword][4 нуля]`. Ветки `jns`/`cdq` здесь нет
				// вовсе — старший байт формы обработчик не смотрит, значение
				// берётся со стека как есть (ledger/0082).
				if (stack.back().isString) {
					stack.back().number = (uint32)(int32)atoi(stack.back().string.c_str());
					stack.back().isString = false;
					stack.back().string.clear();
				}
				stack.back().type = 0x22;
				stack.back().width = 10;
			} else if ((form & 0xff) == 0x11 || (form & 0xff) == 0x12) {
				// Проверка диапазона, а не преобразование. Подобработчик
				// RUN34:0x2ee5 разбирает старший байт формы целиком:
				//   * взведён знаковый бит и слово отрицательное — годится только
				//     разновидность 3, остальные дают ошибку;
				//   * 1 и 3 — слово обязано быть не больше 0x7fff (сравнение
				//     беззнаковое, `ja`);
				//   * 0 — не проверяется вовсе;
				//   * прочее — слово обязано быть ненулевым.
				// Книга доходит до 0x0211 (ledger/0070) и до 0x0311 — проверки
				// длины имени игрока, прочитанного из names.sav.
				uint8 kind = (uint8)(form >> 8);
				uint16 word = (uint16)stack.back().number;
				bool ok;
				if ((kind & 0x80) && (int16)word < 0)
					ok = (kind & 0x7f) == 3;
				else if ((kind & 0x7f) == 1 || (kind & 0x7f) == 3)
					ok = word <= 0x7fff;
				else if ((kind & 0x7f) == 0)
					ok = true;
				else
					ok = word != 0;
				if (!ok) {
					debug(1, "ToolBook: опкод 71 форма %04x: значение %u вне диапазона @0x%x",
							form, word, ip - 3);
					return false;
				}
				if ((form & 0xff) == 0x12) {
					// RUN34:0x3047 — та же проверка, а следом расширение слова до
					// двойного: `pop cx / push 0 / push cx`, и только при взведённом
					// знаковом бите с разновидностью 3 и отрицательном слове
					// вместо нуля кладётся -1. То есть знаковое расширение —
					// исключение, а не правило.
					bool signExtend = (kind & 0x80) && (int16)word < 0 && (kind & 0x7f) == 3;
					stack.back().number = signExtend ?
							(uint32)(int32)(int16)word : (uint32)word;
					stack.back().width = 4;
				}
			} else if ((form & 0xff) == 0x21) {
				// Обратное к 0x12: сужение двойного слова до слова (RUN34:0x2fef).
				// Успех кончается на `pop cx / add sp,2 / push cx` — старшее слово
				// просто выбрасывается, но перед этим оно обязано быть нулевым;
				// единственное исключение — знаковая форма разновидности 3, где
				// допускается ещё и `0xffff` (отрицательное число).
				uint8 kind = (uint8)(form >> 8);
				uint32 value = stack.back().number;
				uint16 high = (uint16)(value >> 16), low = (uint16)value;
				bool ok;
				if ((kind & 0x80) && high == 0xffff)
					ok = (kind & 0x7f) == 3;
				else if (high != 0)
					ok = false;
				else if ((kind & 0x7f) == 1 || (kind & 0x7f) == 3)
					ok = (int16)low >= 0;
				else if ((kind & 0x7f) == 0)
					ok = true;
				else
					ok = low != 0;
				if (!ok) {
					debug(1, "ToolBook: опкод 71 форма %04x: %u не сужается до слова @0x%x",
							form, value, ip - 3);
					return false;
				}
				stack.back().number = low;
				stack.back().width = 2;
			} else if ((form & 0xff) == 0x51 || (form & 0xff) == 0x52) {
				// Обратное преобразование: подобработчики RUN34:0x3207 и 0x3276
				// передают форму целиком в общий преобразователь seg30:0x078c и
				// различаются только тем, сколько кладут обратно — слово (`push
				// ax`) или двойное (`push dx; push ax`). Решение внутри
				// принимается по `младший & 7` и `старший & 3`, поэтому формы
				// 0x8351 и 0x8251 различать незачем (ledger/0054, 0082).
				if (stack.back().width != 10) {
					debug(1, "ToolBook: opcode 71 form %04x non-extended input @0x%x",
							form, ip - 3);
					return false;
				}
				if (stack.back().isString) {
					stack.back().number = (uint32)(int32)atoi(stack.back().string.c_str());
					stack.back().isString = false;
					stack.back().string.clear();
				}
				stack.back().width = (form & 0xff) == 0x51 ? 2 : 4;
			} else {
				debug(1, "ToolBook: opcode 71 form %04x @0x%x пока не реализована",
						form, ip - 3);
				return false;
			}
			break;
		}
case 0x48: {
			// RUN34:0x179e: `inc word ss:[bx]`, при переносе — `inc word ss:[bx+2]`.
			// То есть прибавление единицы к четырёхбайтовому значению на стеке.
			if (stack.empty())
				return false;
			stack.back().number++;
			break;
		}
		case 0x49: {
			// RUN34:0x1883: снимает два четырёхбайтовых значения и сравнивает
			// старшие слова со знаком, при равенстве младшие без знака; кладёт
			// слово 1, если верхнее не меньше следующего, иначе 0.
			Value upper = pop();
			Value lower = pop();
			pushNumber((int32)upper.number >= (int32)lower.number ? 1 : 0, 2);
			break;
		}
		case 0x4b: {
			// RUN34:0x190a: `lodsw`, затем на стек кладутся 0,0,0, операнд и 1 —
			// те же десять байт `[8 байт числа][u16 признак]`, что у 0x4a, только
			// константа шириной в слово.
			Value value;
			value.number = read16(ip);
			ip += 2;
			value.type = 0x22;
			value.width = 10;
			stack.push_back(value);
			break;
		}
				case 0x4a: { Value value; value.number = code[ip++]; value.type = 0x22; value.width = 10; stack.push_back(value); break; }
		case 0x59: {
			// RUN34:0x0fde: `lodsw; add ax, sp; push ax` — адрес байта в стеке
			// операндов со смещением N от вершины. Наш стек — это значения с
			// шириной, поэтому смещение отсчитывается по ним.
			uint16 offset = read16(ip);
			ip += 2;
			uint32 accumulated = 0, target = stack.size();
			for (uint i = stack.size(); i > 0; i--) {
				if (accumulated == offset) { target = i - 1; break; }
				accumulated += stack[i - 1].width;
			}
			if (target >= stack.size()) {
				debug(1, "ToolBook: 0x59 смещение %u не попало в стек @0x%x", offset, ip - 3);
				return false;
			}
			Value reference;
			reference.isStackRef = true;
			reference.stackRef = target;
			reference.width = 2;
			stack.push_back(reference);
			break;
		}
		case 0x6c: {
			// Посылка сообщения: RUN34:0x4960 читает u16 (смещение от следующего
			// байта) и байт числа аргументов, зовёт RUN31:0x0194 и **не исполняет
			// обработчик сам** — он лишь разрешает адрес. Передаёт управление
			// следующий за ним опкод 0x1b, которому число байт аргументов кладут
			// отдельной инструкцией. По всей книге это идёт связкой
			// `6c <смещение> <число> / 04 <байт аргументов> / 1b` (ledger/0069).
			//
			// Поэтому здесь мы только разрешаем обработчик и кладём ссылку на него.
			uint16 rel = read16(ip);
			uint32 nameTarget = (uint32)((int32)(ip + 2) + (int16)rel);
			uint8 count = code[ip + 2];
			ip += 3;
			uint16 selector = _book->readUint16(nameTarget);
			Common::String name = _book->readString(nameTarget + 2, 255);
			if (count) {
				// Ветка RUN31:0x01fb, до неё книга ещё не доходила.
				debug(1, "ToolBook: сообщение %s с %u аргументами пока не реализовано 	0x%x",
						name.c_str(), count, ip - 3);
				return false;
			}
			// Получателя и контекст кладут предыдущие инструкции (0055).
			Value messageContext = pop();
			Value messageReceiver = pop();
			Common::String receiverName = messageReceiver.string;
			const Handler *messageTarget = receiverName.empty() ? nullptr :
					_book->findMessageHandler(receiverName, selector);
			if (!messageTarget && handler.ownerScriptRecord)
				messageTarget = _book->findScriptHandler(handler.ownerScriptRecord, selector);
			if (!messageTarget) {
				debug(1, "ToolBook: сообщение %s (селектор %04x) получателю «%s» не разрешено 	0x%x",
						name.c_str(), selector, receiverName.c_str(), ip - 3);
				return false;
			}
			(void)messageContext;
			Value resolved;
			resolved.isHandlerRef = true;
			resolved.reference = messageTarget->code;
			resolved.hasReference = true;
			resolved.receiverName = receiverName;
			resolved.width = 4;
			stack.push_back(resolved);
			break;
		}
		case 0x1b: {
			// Передача управления по адресу со стека. RUN34:0x1392:
			//     mov dx,ds / mov cx,si   ; запомнить, где были
			//     pop ax / pop si / pop ds ; адрес — со стека
			//     push ax                 ; слово с числом байт аргументов вернуть
			//     push dx / push cx       ; и положить обратный адрес
			// У среды это переход в общем потоке кода; у нас обработчик пока
			// исполняется отдельным вызовом, поэтому здесь он и запускается —
			// с аргументами, снятыми со стека по числу байт из байт-кода, а не по
			// нашей догадке (ledger/0069).
			Value argumentBytes = pop();
			Value target = pop();
			if (!target.isHandlerRef) {
				debug(1, "ToolBook: 0x1b без разрешённого обработчика 	0x%x", ip - 1);
				return false;
			}
			const Handler *callee = _book->findHandlerByCode(target.reference);
			if (!callee) {
				debug(1, "ToolBook: обработчик по адресу 0x%x не найден 	0x%x",
						target.reference, ip - 1);
				return false;
			}
			Common::Array<Value> callArguments;
			uint consumed = 0;
			while (!stack.empty() && consumed < argumentBytes.number) {
				consumed += stack.back().width;
				callArguments.push_back(pop());
			}
			if (consumed != argumentBytes.number) {
				debug(1, "ToolBook: 0x1b: аргументов %u байт, снято %u 	0x%x",
						argumentBytes.number, consumed, ip - 1);
				return false;
			}
			Common::Array<Value> ordered;
			for (int a = (int)callArguments.size() - 1; a >= 0; a--)
				ordered.push_back(callArguments[a]);
			Value callReceiver;
			callReceiver.string = target.receiverName;
			callReceiver.isObject = !target.receiverName.empty();
			callReceiver.width = 4;
			Value callResult;
			debug(3, "ToolBook: вызов по адресу 0x%x («%s»), аргументов %u байт",
					target.reference, target.receiverName.c_str(), argumentBytes.number);
			if (!runHandler(*callee, ordered, callReceiver,
					callee->returnsValue ? &callResult : nullptr, depth + 1))
				return false;
			if (callee->returnsValue)
				stack.push_back(callResult);
			break;
		}
				case 0x6d: {
			uint16 rel = read16(ip);
			uint32 nameTarget = (uint32)((int32)(ip + 2) + (int16)rel);
			uint8 kind = code[ip + 2];
			uint16 argumentBytes = read16(ip + 3);
			ip += 5;
			// The target is formally a two-byte selector followed by the
			// identifier. It is never subject to the literal-string heuristic.
			Common::String name = _book->readString(nameTarget + 2, 255);
			Common::String key = name;
			key.toUppercase();
			uint16 selector = _book->readUint16(nameTarget);
			const Handler *scriptTarget = kind == 1 && handler.ownerScriptRecord ?
					_book->findScriptHandler(handler.ownerScriptRecord, selector) : nullptr;
			if (kind == 1 && !scriptTarget && _nativeFunctions.contains(key))
				debug(3, "ToolBook: платформенный вызов %s @0x%x", name.c_str(),
						ip - 6);
			if (kind == 1 && (scriptTarget || _nativeFunctions.contains(key))) {
				// The linked thunk has its own marshalling signature. It is not
				// equal to the wire's explicit byte count: a property-style call
				// may use the implicit receiver as its first native D argument.
				Common::Array<Value> args;
				pop(); // duplicated lookup context
				Value callReceiver = pop();
				uint consumed = 0;
				while (!stack.empty() && consumed < argumentBytes) {
					consumed += stack.back().width;
					args.push_back(pop());
				}
				if (consumed != argumentBytes) {
					debug(1, "ToolBook: native %s argument stack mismatch", name.c_str());
					return false;
				}
				if (scriptTarget) {
					Common::Array<Value> orderedArgs;
					for (int i = (int)args.size() - 1; i >= 0; i--)
						orderedArgs.push_back(args[i]);
					Value callResult;
					debug(3, "ToolBook: вызов обработчика %s @0x%x", name.c_str(), ip - 6);
					if (!runHandler(*scriptTarget, orderedArgs, callReceiver,
							scriptTarget->returnsValue ? &callResult : nullptr, depth + 1))
						return false;
					if (scriptTarget->returnsValue) {
						debug(3, "ToolBook: %s вернул «%s» (число %u)", name.c_str(),
								valueString(callResult).c_str(), callResult.number);
						stack.push_back(callResult);
					}
				} else if (key == "GLOBALALLOC") {
					// Win16 GlobalAlloc(flags, bytes). The compiled thunk exposes
					// the OpenScript source order as size followed by flags.
					if (argumentBytes != 8 || args.size() != 2)
						return false;
					Common::Array<Value> orderedArgs;
					for (int i = (int)args.size() - 1; i >= 0; i--)
						orderedArgs.push_back(args[i]);
					uint32 size = orderedArgs[0].number;
					uint32 handle = _nextNativeBuffer++;
					NativeBuffer buffer;
					buffer.data.resize(size);
					for (uint32 i = 0; i < size; i++)
						buffer.data[i] = 0;
					_nativeBuffers[handle] = buffer;
					Value allocated;
					allocated.number = handle;
					allocated.width = 4;
					stack.push_back(allocated);
					debug(2, "ToolBook: GlobalAlloc %u bytes -> %u", size, handle);
				} else if (key == "GLOBALLOCK") {
					if (argumentBytes != 4 || args.size() != 1)
						return false;
					Value locked;
					locked.number = args[0].number;
					locked.buffer = args[0].number;
					locked.width = 4;
					if (!_nativeBuffers.contains(locked.buffer))
						return false;
					stack.push_back(locked);
				} else if (key == "ADDFONTRESOURCE") {
					if (argumentBytes != 4 || args.size() != 1)
						return false;
					Common::String fileName = valueString(args[0]);
					for (uint i = 0; i < fileName.size(); i++)
						if (fileName[i] == '\\')
							fileName.setChar('/', i);
					while (fileName.hasPrefix("./"))
						fileName.erase(0, 2);
					Common::ScopedPtr<Common::SeekableReadStream> fontFile(
							SearchMan.createReadStreamForMember(Common::Path(fileName)));
					uint32 loaded = 0;
					if (fontFile) {
						Common::ScopedPtr<Common::WinResources> resources(
								Common::WinResources::createFromEXE(fontFile.get()));
						Common::ScopedPtr<Common::SeekableReadStream> directory(resources ?
								resources->getResource(Common::kWinFontDir,
										Common::WinResourceID("FONTDIR")) : nullptr);
						if (directory && directory->size() >= 2) {
							uint16 count = directory->readUint16LE();
							for (uint i = 0; i < count && !directory->eos(); i++) {
								uint16 id = directory->readUint16LE();
								if (directory->pos() + 113 > directory->size())
									break;
								directory->skip(68);
								uint16 points = directory->readUint16LE();
								directory->skip(43);
								directory->readString(); // device
								Common::String face = directory->readString();
								Common::ScopedPtr<Common::SeekableReadStream> font(
										resources->getResource(Common::kWinFont, id));
								if (!font || face.empty())
									continue;
								Common::String faceKey = face;
								faceKey.toUppercase();
								NativeFontFace &registered = _nativeFonts[faceKey];
								registered.name = face;
								bool duplicate = false;
								for (uint p = 0; p < registered.points.size(); p++)
									duplicate |= registered.points[p] == points;
								if (!duplicate)
									registered.points.push_back(points);
								loaded++;
							}
						}
					}
					pushNumber(loaded);
					debug(1, "ToolBook: AddFontResource %s -> %u",
							fileName.c_str(), loaded);
				} else if (key == "SENDMESSAGE") {
					// The reached call is HWND_BROADCAST / WM_FONTCHANGE. ScummVM
					// owns its font registry directly, so the Windows broadcast has
					// no additional consumers. Preserve SendMessage's numeric result.
					if (argumentBytes != 16 || args.size() != 4)
						return false;
					pushNumber(0);
				} else if (key == "FILEEXISTS") {
					// TB40DOS.1 (seg3:0x0106, `retf 4`): берёт дальний указатель на
					// имя и возвращает **число** в AX. Книга так его и проверяет — в
					// обоих вызовах стоит `если fileExists(путь) <> 1`. Раньше движок
					// отдавал строку «true», сравнение со единицей не сходилось, и
					// книга уходила в ветку «не обнаружен файл» (ledger/0077).
					if (args.size() != 1)
						return false;
					// Дерево игры плоское, поэтому от пути эталонной установки
					// (`C:\NMG\KNTOWER\…`) берём имя файла.
					Common::String probe = valueString(args[0]);
					while (probe.contains('\\'))
						probe.erase(0, probe.findFirstOf('\\') + 1);
					bool present = !probe.empty() && SearchMan.hasFile(Common::Path(probe));
					debug(2, "ToolBook: fileExists %s -> %d", probe.c_str(), present ? 1 : 0);
					pushNumber(present ? 1 : 0);
								} else if (key == "SETFILEATTRIBUTES") {
					// TB40DOS: смена атрибутов файла. Своей файловой системы у движка
					// нет — атрибуты DOS ни на что не влияют, а на эталонном стенде
					// вызов удаётся, поэтому возвращаем тот же успех. Ветку выбирает
					// книга, и она должна выбрать ту же, что на стенде.
					if (args.size() != 2)
						return false;
					debug(2, "ToolBook: setFileAttributes %s «%s» (пропущено)",
							valueString(args[1]).c_str(), valueString(args[0]).c_str());
					pushNumber(1);
								} else if (key == "DISPLAYBITSPERPIXEL") {
					// Глубина цвета экрана. Книга проверяет её на стартовых проверках и
					// требует 256 цветов; наш экран палитровый, восьмибитный — как и на
					// эталонном стенде (Cirrus в режиме 256 цветов).
					pushNumber(8);
								} else if (key == "GETINIVAR") {
					// TB40WIN: чтение переменной из INI-файла. Порядок аргументов взят не
					// из догадки, а из настоящего файла эталонного стенда
					// C:\WINDOWS\MTB40.INI: там секция [CACHE FILES], ключ KNTOWER.EXE
					// (ledger/0054). Путь приводим к имени файла — дерево игры плоское.
					if (args.size() != 3)
						return false;
					Common::String iniSection = valueString(args[0]);
					Common::String iniEntry = valueString(args[1]);
					Common::String iniFile = valueString(args[2]);
					while (iniFile.contains('\\'))
						iniFile.erase(0, iniFile.findFirstOf('\\') + 1);
					Common::ScopedPtr<Common::SeekableReadStream> iniStream(
							SearchMan.createReadStreamForMember(Common::Path(iniFile)));
					Common::String found;
					bool inSection = false;
					while (iniStream && !iniStream->eos()) {
						Common::String line = iniStream->readLine();
						while (!line.empty() && (line.lastChar() == ' ' || line.lastChar() == '\r'))
							line.deleteLastChar();
						if (line.empty())
							continue;
						if (line.firstChar() == '[') {
							Common::String header(line.c_str() + 1);
							if (header.contains(']'))
								header.erase(header.findFirstOf(']'));
							inSection = header.equalsIgnoreCase(iniSection);
							continue;
						}
						if (!inSection || !line.contains('='))
							continue;
						uint32 split = line.findFirstOf('=');
						Common::String name(line.c_str(), split);
						while (!name.empty() && name.lastChar() == ' ')
							name.deleteLastChar();
						if (name.equalsIgnoreCase(iniEntry)) {
							found = Common::String(line.c_str() + split + 1);
							break;
						}
					}
					debug(2, "ToolBook: getIniVar [%s] %s из %s -> «%s»", iniSection.c_str(),
							iniEntry.c_str(), iniFile.c_str(), found.c_str());
					pushString(found);
								} else if (key == "GETWINDOWSDIRECTORY") {
					// Win16 `UINT GetWindowsDirectory(LPSTR, UINT)`: заполняет буфер и
					// возвращает длину. Книга заранее берёт буфер через GlobalAlloc и
					// GlobalLock, поэтому один аргумент — наш буфер, второй его размер;
					// какой именно, определяем по метке буфера, а не по порядку.
					// Эталон — Windows 98, там это C:\WINDOWS.
					if (args.size() != 2)
						return false;
					const Value *destination = nullptr, *capacity = nullptr;
					for (uint i = 0; i < args.size(); i++) {
						if (args[i].buffer && _nativeBuffers.contains(args[i].buffer))
							destination = &args[i];
						else
							capacity = &args[i];
					}
					if (!destination || !capacity)
						return false;
					NativeBuffer &buffer = _nativeBuffers[destination->buffer];
					Common::String windowsDirectory = "C:\\WINDOWS";
					uint32 room = MIN<uint32>(capacity->number, buffer.data.size());
					if (room <= windowsDirectory.size())
						return false;
					for (uint i = 0; i < windowsDirectory.size(); i++)
						buffer.data[i] = (byte)windowsDirectory[i];
					buffer.data[windowsDirectory.size()] = 0;
					pushNumber(windowsDirectory.size(), 2);
					debug(2, "ToolBook: GetWindowsDirectory -> %s", windowsDirectory.c_str());
				} else if (key == "GETMODULEPATH") {
					if (argumentBytes != 4 || args.size() != 1)
						return false;
					Common::String moduleName = valueString(args[0]);
					if (moduleName.empty())
						return false;
					// Win16 returns the loaded module's path. It is only fed to the
					// generic version-resource query next; retain a DOS-visible name.
					pushString(moduleName + ".DLL");
				} else if (key == "GETFILEVERSION") {
					if (argumentBytes != 4 || args.size() != 1)
						return false;
					Common::String fileName = valueString(args[0]);
					for (uint i = 0; i < fileName.size(); i++)
						if (fileName[i] == '\\')
							fileName.setChar('/', i);
					while (fileName.hasPrefix("./"))
						fileName.erase(0, 2);
					Common::ScopedPtr<Common::SeekableReadStream> executable(
							SearchMan.createReadStreamForMember(Common::Path(fileName)));
					Common::ScopedPtr<Common::WinResources> resources(executable ?
							Common::WinResources::createFromEXE(executable.get()) : nullptr);
					Common::ScopedPtr<Common::WinResources::VersionInfo> version(resources ?
							resources->getVersionResource(1) : nullptr);
					Value fields;
					if (version) {
						const char *keys[] = { "Path", "FileVersion", "Language", "InternalName",
							"ProductName", "ProductVersion", "OriginalFilename",
							"LegalCopyright", "LegalTrademarks" };
						for (uint i = 0; i < ARRAYSIZE(keys); i++)
							fields.array.push_back(i == 0 ? fileName :
									version->hash[keys[i]].encode());
					} else if (fileName.equalsIgnoreCase("USER.DLL") ||
							fileName.equalsIgnoreCase("USER.EXE")) {
						// The original bridge queries the version resource of Win98's
						// loaded USER.EXE. ScummVM has no host USER.EXE, so expose a
						// compact compatible Win16 platform record. These are platform
						// properties; no game branch or page name is encoded here.
						fields.array.push_back("USER.EXE");       // Path
						fields.array.push_back("4.10.2222");     // FileVersion
						fields.array.push_back("Russian");       // Language
						fields.array.push_back("USER");          // InternalName
						fields.array.push_back("Microsoft Windows");
						fields.array.push_back("4.10.2222");     // ProductVersion
						fields.array.push_back("USER.EXE");
						fields.array.push_back("Microsoft Corporation");
						fields.array.push_back(Common::String());
					}
					fields.width = 4;
					stack.push_back(fields);
				} else if (key == "GETWININIVAR") {
					if (argumentBytes != 8 || args.size() != 2)
						return false;
					// Explicit arguments were popped from the VM stack in reverse
					// source order: key, then section.
					Common::String section = valueString(args[1]);
					Common::String setting = valueString(args[0]);
					Common::String iniKey = section + "\n" + setting;
					iniKey.toUppercase();
					pushString(_winIniValues.contains(iniKey) ?
							_winIniValues.getVal(iniKey) : Common::String());
				} else if (key == "SETWININIVAR") {
					if (argumentBytes != 12 || args.size() != 3)
						return false;
					// Reverse pop order: value, key, section.
					Common::String section = valueString(args[2]);
					Common::String setting = valueString(args[1]);
					Common::String iniKey = section + "\n" + setting;
					iniKey.toUppercase();
					_winIniValues[iniKey] = valueString(args[0]);
					pushNumber(1);
				} else if (key == "DISPLAYFONTS") {
					const NativeBinding &binding = _nativeFunctions.getVal(key);
					if (binding.argumentBytes != 4 || argumentBytes != 0) {
						debug(1, "ToolBook: unexpected displayFonts signature");
						return false;
					}
					Common::String family = valueString(callReceiver);
					Common::String fontList;
					for (Common::HashMap<Common::String, NativeFontFace>::const_iterator it =
							_nativeFonts.begin(); it != _nativeFonts.end(); ++it) {
						const NativeFontFace &face = it->_value;
						if (!family.empty() && !face.name.equalsIgnoreCase(family))
							continue;
						fontList += face.name;
						for (uint s = 0; s < face.points.size(); s++)
							fontList += Common::String::format(",%u", face.points[s]);
						fontList += "\r\n";
					}
					pushString(fontList);
				} else {
					debug(1, "ToolBook: native function %s пока не реализована @0x%x",
							name.c_str(), ip - 6);
					// Аргументы уже сняты со стека в порядке, обратном исходному:
					// печатаем их, чтобы следующий барьер сразу показывал, чего от него
					// хотят, и не требовал отдельного прогона с пробой.
					for (uint a = 0; a < args.size(); a++)
						debug(1, "    аргумент %u (исходный %u): ширина %u число %u стр «%s»%s",
								a, (uint)(args.size() - 1 - a), args[a].width, args[a].number,
								valueString(args[a]).c_str(), args[a].buffer ? " буфер" : "");
					return false;
				}
			} else {
				debug(1, "ToolBook: named dispatch %s kind %u/%u пока не реализован @0x%x",
						name.c_str(), kind, argumentBytes, ip - 6);
				return false;
			}
			break;
		}
		case 0x72: ip++; pop(); break;
		case 0x73:
			// Materialize/copy the top dynamic value in the current context.
			// The abstract VM already owns its Value, so stack and value stay put.
			if (stack.empty())
				return false;
			stack.back().width = 4;
			break;
		default:
			debug(1, "ToolBook: OpenScript opcode %02x @0x%x пока не реализован", op, ip - 1);
			// Верхушка стека — то, с чем зовут неизвестный опкод.
			for (int v = (int)stack.size() - 1, n = 0; v >= 0 && n < 6; v--, n++)
				debug(1, "    стек [-%d]: ширина %u тип %02x число %u стр «%s»",
						n, stack[v].width, stack[v].type, stack[v].number,
						valueString(stack[v]).c_str());
			return false;
		}
	}
	if (handler.returnsValue && result) {
		if (stack.empty())
			return false;
		*result = pop();
	}
	return true;
}

// ЛОКАЛЬНАЯ ПРАВКА (не для апстрима): сценарий ввода для безоконных прогонов,
// такой же по смыслу, как в движке director. Строки вида
//     1500 click 232 215
//     4000 key 111
// где первое число — миллисекунды от старта. Это только воспроизводимый
// пользовательский ввод для кадров сверки; переходы выбирает сама книга.
void ToolBookEngine::playMedia(const Common::String &alias) {
	// Псевдоним завёл `open` (команда MCI книги); в нём лежит путь так, как его
	// написала книга — с обратными косыми и, возможно, с буквой диска. Дерево
	// игры у нас плоское по каталогам установки, поэтому приводим путь к тому
	// виду, в котором его найдёт SearchMan (ledger/0070).
	if (!_mciAliases.contains(alias)) {
		debug(1, "ToolBook: играть нечего — псевдоним «%s» не открыт", alias.c_str());
		return;
	}
	Common::String path = _mciAliases[alias];
	for (uint i = 0; i < path.size(); i++)
		if (path[i] == '\\')
			path.setChar('/', i);
	while (path.hasPrefix("./"))
		path.erase(0, 2);
	Common::String lowered = path;
	lowered.toLowercase();
	stopMedia(alias);
	Common::SeekableReadStream *stream = SearchMan.createReadStreamForMember(Common::Path(path));
	if (!stream) {
		debug(1, "ToolBook: файл «%s» не найден", path.c_str());
		return;
	}
	if (lowered.hasSuffix(".wav")) {
		Audio::AudioStream *sound = Audio::makeWAVStream(stream, DisposeAfterUse::YES);
		if (sound) {
			_mixer->playStream(Audio::Mixer::kSFXSoundType, &_mediaHandle, sound);
			_playingAlias = alias;
			debug(2, "ToolBook: звук «%s» пошёл", path.c_str());
			return;
		}
	}
	delete stream;
	debug(1, "ToolBook: «%s» пока не проигрывается", path.c_str());
}

void ToolBookEngine::stopMedia(const Common::String &alias) {
	if (_playingAlias != alias)
		return;
	_mixer->stopHandle(_mediaHandle);
	_playingAlias.clear();
}

void ToolBookEngine::loadInputScript() {
	_inputScriptLoaded = true;
	if (!ConfMan.hasKey("inputscript"))
		return;

	Common::File f;
	if (!f.open(Common::FSNode(ConfMan.getPath("inputscript")))) {
		warning("ToolBook: не открывается сценарий ввода %s",
				ConfMan.get("inputscript").c_str());
		return;
	}
	while (!f.eos()) {
		Common::String line = f.readLine();
		if (line.empty() || line[0] == '#')
			continue;
		uint32 t = 0;
		char verb[16] = { 0 };
		int a = 0, b = 0;
		if (sscanf(line.c_str(), "%u %15s %d %d", &t, verb, &a, &b) < 2)
			continue;
		ScriptedInput in;
		in.timeMs = t;
		in.x = a;
		in.y = b;
		if (!strcmp(verb, "click"))
			in.action = kInputClick;
		else if (!strcmp(verb, "key"))
			in.action = kInputKey;
		else
			continue;
		_inputScript.push_back(in);
	}
	debug(0, "ToolBook: сценарий ввода: %u строк", _inputScript.size());
}

void ToolBookEngine::feedScriptedInput() {
	if (!_inputScriptLoaded)
		loadInputScript();

	uint32 now = _system->getMillis() - _startMs;
	while (_inputScriptPos < _inputScript.size() &&
			_inputScript[_inputScriptPos].timeMs <= now) {
		const ScriptedInput &in = _inputScript[_inputScriptPos++];
		Common::Event ev;
		if (in.action == kInputClick) {
			ev.type = Common::EVENT_MOUSEMOVE;
			ev.mouse = Common::Point(in.x, in.y);
			_injected.push_back(ev);
			ev.type = Common::EVENT_LBUTTONDOWN;
			ev.mouse = Common::Point(in.x, in.y);
			_injected.push_back(ev);
			ev.type = Common::EVENT_LBUTTONUP;
			ev.mouse = Common::Point(in.x, in.y);
		} else {
			ev.type = Common::EVENT_KEYDOWN;
			int ascii = in.x;
			Common::KeyCode keycode = (Common::KeyCode)in.x;
			if (in.x >= 'A' && in.x <= 'Z')
				keycode = (Common::KeyCode)(Common::KEYCODE_a + in.x - 'A');
			ev.kbd = Common::KeyState(keycode, ascii);
		}
		_injected.push_back(ev);
	}
}

void ToolBookEngine::handleEvents() {
	feedScriptedInput();

	Common::Event event;
	while (!_injected.empty() || _system->getEventManager()->pollEvent(event)) {
		if (!_injected.empty()) {
			event = _injected[0];
			_injected.remove_at(0);
		}
		switch (event.type) {
		case Common::EVENT_MOUSEMOVE: {
			const Object *hit = objectAt(event.mouse.x, event.mouse.y);
			uint32 next = hit ? hit->block : 0;
			if (next == _hoveredObject)
				break;
			const Object *previous = findCurrentObject(_hoveredObject);
			_hoveredObject = next;
			if (previous)
				dispatchObjectEvent(*previous, kEventMouseLeave);
			// A leave handler may navigate and clear the hover state.
			if (_hoveredObject == next && next) {
				hit = findCurrentObject(next);
				if (hit)
					dispatchObjectEvent(*hit, kEventMouseEnter);
			}
			break;
		}
		case Common::EVENT_KEYDOWN:
			if (_focusedField) {
				const Object *field = findCurrentObject(_focusedField);
				if (field && field->field) {
					Common::String value = fieldText(*field);
					if (event.kbd.keycode == Common::KEYCODE_BACKSPACE) {
						if (_fieldSelectAll) {
							value.clear();
							_fieldSelectAll = false;
						} else
						if (!value.empty())
							value.deleteLastChar();
						_fieldValues[field->block] = value;
						_needsRedraw = true;
						break;
					}
					if (event.kbd.ascii >= 32 && event.kbd.ascii < 127 &&
							value.size() < field->textCapacity) {
						if (_fieldSelectAll) {
							value.clear();
							_fieldSelectAll = false;
						}
						value += (char)event.kbd.ascii;
						_fieldValues[field->block] = value;
						_needsRedraw = true;
						break;
					}
				}
			}
			switch (event.kbd.keycode) {
			case Common::KEYCODE_ESCAPE:
				_quit = true;
				break;
			case Common::KEYCODE_RIGHT:
			case Common::KEYCODE_PAGEDOWN:
				// Resource browsing is an explicitly enabled diagnostic mode;
				// it never changes the current game page.
				if (_imageMode && _currentImage + 1 < (int)_rawImages.size()) {
					_currentImage++;
					_needsRedraw = true;
				}
				break;
			case Common::KEYCODE_LEFT:
			case Common::KEYCODE_PAGEUP:
				if (_imageMode) {
					if (_currentImage > 0)
						_currentImage--;
				}
				_needsRedraw = true;
				break;
			case Common::KEYCODE_HOME:
				if (_imageMode) {
					_currentImage = 0;
					_needsRedraw = true;
				}
				break;
			case Common::KEYCODE_i:
				// Просмотр ресурсов книги: показывает те картинки, которые
				// движок действительно разворачивает. It is disabled unless
				// explicitly requested and cannot drive page transitions.
				if (ConfMan.getBool("toolbook_resource_viewer")) {
					_imageMode = !_imageMode;
					_needsRedraw = true;
				}
				break;
			case Common::KEYCODE_m: {
				// ЛОКАЛЬНАЯ ПРАВКА (не для апстрима): открыть диалог вручную —
				// проверка модели окна глазами (ledger/0073, 0077).
				ViewerState &viewer = _viewerStates[uppercased("Dial")];
				bool shown = viewer.open && viewer.visible;
				viewer.open = viewer.visible = !shown;
				viewer.page = shown ? Common::String() : Common::String("message1");
				_needsRedraw = true;
				break;
			}
			case Common::KEYCODE_o:
				// Рамки объектов страницы — проверка разбора прямоугольников.
				if (ConfMan.getBool("toolbook_resource_viewer")) {
					_showHotspots = !_showHotspots;
					_needsRedraw = true;
				}
				break;
			default:
				break;
			}
			break;
		case Common::EVENT_LBUTTONDOWN: {
			const Object *hit = objectAt(event.mouse.x, event.mouse.y);
			if (hit) {
				_focusedField = hit->field ? hit->block : 0;
				_fieldSelectAll = false;
				dispatchObjectEvent(*hit, kEventButtonDown);
			}
			break;
		}
		case Common::EVENT_LBUTTONUP: {
			const Object *hit = objectAt(event.mouse.x, event.mouse.y);
			if (hit) {
				debug(0, "ToolBook: щелчок по объекту «%s» (%d,%d)-(%d,%d)",
						hit->name.c_str(), hit->rect.left, hit->rect.top,
						hit->rect.right, hit->rect.bottom);
				dispatchObjectEvent(*hit, kEventButtonClick);
			}
			break;
		}
		default:
			break;
		}
	}
}

const Object *ToolBookEngine::findCurrentObject(const Common::String &name) const {
	// Пока окно показано, его объекты — те, с которыми книга и игрок работают;
	// страница под ним остаётся запасным вариантом.
	for (int which = 0; which < 2; which++) {
		int index = which == 0 ? shownViewerPage() : _currentPage;
		if (index < 0 || index >= (int)_book->pages().size())
			continue;
		const Common::Array<Object> &objects = _book->pages()[index].objects;
		for (uint i = 0; i < objects.size(); i++)
			if (objects[i].name.equalsIgnoreCase(name))
				return &objects[i];
	}
	return nullptr;
}

const Object *ToolBookEngine::findCurrentObject(uint32 block) const {
	if (!block)
		return nullptr;
	for (int which = 0; which < 2; which++) {
		int index = which == 0 ? shownViewerPage() : _currentPage;
		if (index < 0 || index >= (int)_book->pages().size())
			continue;
		const Common::Array<Object> &objects = _book->pages()[index].objects;
		for (uint i = 0; i < objects.size(); i++)
			if (objects[i].block == block)
				return &objects[i];
	}
	return nullptr;
}

bool ToolBookEngine::objectVisible(const Page &page, const Object &object, uint depth) const {
	if (depth > 32)
		return false;
	bool own = _visibilityOverrides.contains(object.block) ?
			_visibilityOverrides.getVal(object.block) : object.ownVisible;
	if (!own)
		return false;
	if (object.parentHandle == 0x14)
		return true;
	for (uint i = 0; i < page.objects.size(); i++) {
		const Object &parent = page.objects[i];
		if (parent.segmentBase == object.segmentBase && parent.handle == object.parentHandle)
			return objectVisible(page, parent, depth + 1);
	}
	return false;
}

void ToolBookEngine::setObjectVisible(const Common::String &name, bool visible) {
	const Object *object = findCurrentObject(name);
	if (!object)
		return;
	_visibilityOverrides[object->block] = visible;
	_needsRedraw = true;
}

Common::String ToolBookEngine::fieldText(const Object &object) const {
	return _fieldValues.contains(object.block) ?
			_fieldValues.getVal(object.block) : object.initialText;
}

const Graphics::Font *ToolBookEngine::fieldFont(int points) const {
	// Книга задаёт кегль в пунктах (свойство 0x400f: 12, при переполнении 10).
	// При 96 точках на дюйм пункт — это 4/3 пикселя; шрифты кешируются, потому
	// что цикл подбора страницы переразмечает текст на каждом шаге.
	if (points <= 0)
		points = 12;
	int pixels = points * 4 / 3;
	if (_fontCache.contains(pixels))
		return _fontCache[pixels];
	const Graphics::Font *font = nullptr;
	#ifdef USE_FREETYPE2
	font = Graphics::loadTTFFontFromArchive("LiberationSans-Regular.ttf", pixels,
			Graphics::kTTFSizeModeCharacter, 96, 96, Graphics::kTTFRenderModeMonochrome);
	#endif
	if (!font)
		font = _fieldFont;
	_fontCache[pixels] = font;
	return font;
}

int ToolBookEngine::fieldPoints(const Object &object) const {
	return _fieldSizes.contains(object.block) ? _fieldSizes[object.block] : 12;
}

const Graphics::Font *ToolBookEngine::layoutField(const Object &object,
		Common::Array<Common::U32String> &lines) const {
	const Graphics::Font *font = fieldFont(fieldPoints(object));
	lines.clear();
	if (!font)
		return nullptr;
	Common::String value = (_fieldSelectAll && _focusedField == object.block) ?
			Common::String() : fieldText(object);
	// Поле ToolBook переносит текст по словам внутри своего прямоугольника;
	// без этого длинное сообщение рисовалось одной строкой поверх рамки.
	font->wordWrapText(value.decode(Common::kUtf8), MAX(1, object.rect.width() - 4), lines);
	return font;
}

bool ToolBookEngine::fieldTextFits(const Object &object) const {
	Common::Array<Common::U32String> lines;
	const Graphics::Font *font = layoutField(object, lines);
	if (!font)
		return true;
	if ((int)lines.size() * font->getFontHeight() > object.rect.height())
		return false;
	for (uint i = 0; i < lines.size(); i++)
		if (font->getStringWidth(lines[i]) > object.rect.width() - 4)
			return false;
	return true;
}

void ToolBookEngine::drawField(Graphics::Surface *screen, const Object &object,
		Common::Point origin) {
	Common::Array<Common::U32String> lines;
	const Graphics::Font *font = layoutField(object, lines);
	if (!font)
		return;
	int lineHeight = font->getFontHeight();
	int block = (int)lines.size() * lineHeight;
	int x = object.rect.left + origin.x + 2;
	int y = object.rect.top + origin.y + MAX(0, (object.rect.height() - block) / 2);
	// Выключка не читается: в сериализованной записи поля она есть, но книга её
	// не задаёт, а умолчание ToolBook — по левому краю.
	for (uint i = 0; i < lines.size(); i++)
		font->drawString(screen, lines[i], x, y + (int)i * lineHeight,
				MAX(0, object.rect.width() - 4), 0);
	if (_focusedField == object.block) {
		Common::U32String last = lines.empty() ? Common::U32String() : lines[lines.size() - 1];
		int caretY = y + MAX(0, (int)lines.size() - 1) * lineHeight;
		int caret = MIN(object.rect.right + origin.x - 2, x + font->getStringWidth(last));
		screen->drawLine(caret, caretY + 1, caret, caretY + lineHeight - 1, 0);
	}
}

const Object *ToolBookEngine::findFieldObject(const ScriptValue &value) const {
	// Имя поля неуникально — «Message» есть на каждой странице фона «Message», —
	// поэтому ищем в той странице, в которой книга объект и разрешила
	// (builtin 182 кладёт вместилище в `owner`).
	int pageIndex = value.owner.empty() ? _book->pageOfObject(value.string) :
			findPageIndex(value.owner);
	if (pageIndex < 0)
		return nullptr;
	const Common::Array<Object> &objects = _book->pages()[pageIndex].objects;
	for (uint i = 0; i < objects.size(); i++)
		if (objects[i].field && objects[i].name.equalsIgnoreCase(value.string))
			return &objects[i];
	return nullptr;
}

void ToolBookEngine::setFieldProperty(const ScriptValue &object, uint32 property,
		const ScriptValue &value) {
	// 0x402e — текст поля, 0x400f — его кегль в пунктах. Книга кладёт их
	// свойствами, а не вводом: обработчик `myMessage` так раскладывает сообщение
	// по полю «Message» той страницы, которую потом показывает окно
	// (ledger/0077).
	if (property != 0x402e && property != 0x400f)
		return;
	const Object *field = findFieldObject(object);
	if (!field) {
		debug(1, "ToolBook: поле «%s» (в «%s») не нашлось", object.string.c_str(),
				object.owner.c_str());
		return;
	}
	if (property == 0x400f) {
		_fieldSizes[field->block] = (int)value.number;
		debug(2, "ToolBook: кегль поля «%s» := %u", field->name.c_str(), value.number);
	} else {
		// Литералы книги — в CP1251, а в `_fieldValues` лежит UTF-8: там же
		// оказывается набранное игроком. Переводим на границе, как это делает
		// разбор текста страниц.
		Common::String text = value.isString || value.isObject ?
				cp1251ToUtf8(value.string) : Common::String();
		_fieldValues[field->block] = text;
		debug(2, "ToolBook: текст поля «%s» := «%s»", field->name.c_str(), text.c_str());
	}
	_needsRedraw = true;
}

int ToolBookEngine::shownViewerPage() const {
	// Показывается окно, которое книга открыла (346), показала (324) и которому
	// назначила страницу (свойство 0x40d7). Порядок именно такой: страница
	// приходит между открытием и показом (ledger/0077).
	for (Common::HashMap<Common::String, ViewerState>::const_iterator it = _viewerStates.begin();
			it != _viewerStates.end(); ++it) {
		if (!it->_value.open || !it->_value.visible || it->_value.page.empty())
			continue;
		int index = findPageIndex(it->_value.page);
		if (index >= 0)
			return index;
		debug(1, "ToolBook: окно «%s» показывает неизвестную страницу «%s»",
				it->_key.c_str(), it->_value.page.c_str());
	}
	return -1;
}

Common::Point ToolBookEngine::pageOrigin(const Page &page) const {
	return page.hasCanvasOrigin ? Common::Point(page.canvasX, page.canvasY) : Common::Point();
}

const Object *ToolBookEngine::objectAt(int x, int y) const {
	const Common::Array<Page> &pages = _book->pages();
	if (_imageMode)
		return nullptr;

	// Показанное окно лежит поверх страницы и ввод забирает себе: сначала
	// проверяем его объекты, и только потом — страницу под ним (ledger/0078).
	int viewer = shownViewerPage();
	if (viewer >= 0) {
		const Object *hit = objectIn(pages[viewer], viewerOrigin(pages[viewer]), x, y);
		if (hit)
			return hit;
	}
	if (_currentPage < 0 || _currentPage >= (int)pages.size())
		return nullptr;
	return objectIn(pages[_currentPage], pageOrigin(pages[_currentPage]), x, y);
}

const Object *ToolBookEngine::objectIn(const Page &page, Common::Point origin,
		int x, int y) const {
	// Objects are stored in formal back-to-front child-list order. Walk that
	// order backwards so the topmost visible object receives the event.
	const Common::Array<Object> &objs = page.objects;
	for (int i = (int)objs.size() - 1; i >= 0; i--) {
		Common::Rect rect = objs[i].rect;
		rect.translate(origin.x, origin.y);
		if (!objectVisible(page, objs[i]) || !rect.contains(x, y))
			continue;
		// У многоугольных областей рамка — только грубая отсечка: попадание
		// считается по обводу, иначе кнопки на карте перекрывают друг друга.
		if (!objs[i].outline.empty() && !pointInOutline(objs[i], x - origin.x, y - origin.y))
			continue;
		return &objs[i];
	}
	return nullptr;
}

Common::Point ToolBookEngine::viewerOrigin(const Page &page) const {
	// Своей геометрии у диалога в таблице окон нет — все её поля нулевые (у окон
	// с сохранённым положением там лежат 800×600 и 1024×768). Окно без положения
	// ToolBook разворачивает по странице и ставит по центру родительского окна.
	//
	// Размер берётся по объектам страницы, а не по клиентскому размеру фона:
	// у фона «message» он один (259×136), а страницы его разного размера —
	// от 259×136 у `message1` до 449×304 у `Message5`, и по фону крупные
	// свешивались за край экрана (ledger/0078).
	Common::Rect box;
	for (uint i = 0; i < page.objects.size(); i++) {
		if (!objectVisible(page, page.objects[i]))
			continue;
		if (box.isEmpty())
			box = page.objects[i].rect;
		else
			box.extend(page.objects[i].rect);
	}
	int width = box.isEmpty() ? (int)page.canvasWidth : box.right;
	int height = box.isEmpty() ? (int)page.canvasHeight : box.bottom;
	return Common::Point(((int)_system->getWidth() - width) / 2,
			((int)_system->getHeight() - height) / 2);
}

// Классический тест «луч вправо»: точка внутри, если пересечений нечётное число.
bool ToolBookEngine::pointInOutline(const Object &obj, int x, int y) {
	const Common::Array<Common::Point> &p = obj.outline;
	bool in = false;
	for (uint i = 0, j = p.size() - 1; i < p.size(); j = i++) {
		if ((p[i].y > y) == (p[j].y > y))
			continue;
		int dx = p[j].x - p[i].x, dy = p[j].y - p[i].y;
		if (dy && x < p[i].x + (int)((int64)(y - p[i].y) * dx / dy))
			in = !in;
	}
	return in;
}

void ToolBookEngine::drawObjectFrames(Graphics::Surface *screen, const Page &page) {
	// Рамки объектов: пока интерпретатора нет, это единственный способ увидеть,
	// правильно ли разобраны прямоугольники (клавиша o).
	const uint32 color = screen->format.isCLUT8() ? 255 : screen->format.RGBToColor(0xff, 0, 0xff);
	Common::Point origin = pageOrigin(page);
	for (uint i = 0; i < page.objects.size(); i++) {
		const Common::Array<Common::Point> &o = page.objects[i].outline;
		if (!o.empty()) {
			for (uint k = 0; k < o.size(); k++) {
				Common::Point a = o[k] + origin, b = o[(k + 1) % o.size()] + origin;
				if (a.x >= 0 && a.x < screen->w && b.x >= 0 && b.x < screen->w &&
						a.y >= 0 && a.y < screen->h && b.y >= 0 && b.y < screen->h)
					screen->drawLine(a.x, a.y, b.x, b.y, color);
			}
			continue;
		}
		Common::Rect r = page.objects[i].rect;
		r.translate(origin.x, origin.y);
		r.clip(Common::Rect(0, 0, screen->w, screen->h));
		if (r.isEmpty())
			continue;
		screen->drawLine(r.left, r.top, r.right - 1, r.top, color);
		screen->drawLine(r.left, r.bottom - 1, r.right - 1, r.bottom - 1, color);
		screen->drawLine(r.left, r.top, r.left, r.bottom - 1, color);
		screen->drawLine(r.right - 1, r.top, r.right - 1, r.bottom - 1, color);
	}
}

void ToolBookEngine::showPage(int index) {
	const Common::Array<Page> &pages = _book->pages();
	if (index < 0 || index >= (int)pages.size())
		return;

	const Page &page = pages[index];

	Graphics::Surface *screen = _system->lockScreen();
	// A Background whose client canvas is smaller than the book window is a
	// modal overlay. ToolBook retains the existing framebuffer around it; only
	// full-window backgrounds replace the page beneath them.
	if (page.canvasWidth >= screen->w && page.canvasHeight >= screen->h)
		screen->fillRect(Common::Rect(0, 0, screen->w, screen->h), 0);

	bool drawn = false;
	Common::Point origin = pageOrigin(page);
	for (uint o = 0; o < page.objects.size(); o++) {
		const Object &obj = page.objects[o];
		if (!objectVisible(page, obj) || !obj.picture || obj.image < 0)
			continue;
		Graphics::Palette palette(256);
		Graphics::Surface *img = _book->decodeImage(
				const_cast<Image &>(_book->images()[obj.image]), palette);
		if (!img || !img->format.isCLUT8()) {
			if (img) { img->free(); delete img; }
			continue;
		}
		_system->getPaletteManager()->setPalette(palette.data(), 0, 256);
		// ToolBook picture DIBs use palette index 253 (magenta) as the
		// transparent matte. A full-screen background is copied opaquely.
		int transparent = (obj.rect.width() >= 620 && obj.rect.height() >= 460) ? -1 : 253;
		blitTransparent(screen, *img, obj.rect.left + origin.x, obj.rect.top + origin.y, transparent);
		img->free();
		delete img;
		drawn = true;
	}

	// Окно книги рисуется поверх страницы: у ToolBook диалог живёт отдельной
	// страницей, которую показывает окно (ledger/0072, 0077). Страница под ним
	// остаётся, поэтому кадр не чистится.
	int viewerPage = shownViewerPage();
	if (viewerPage >= 0) {
		const Page &shown = _book->pages()[viewerPage];
		// Своей геометрии у диалога в таблице окон нет — все её поля нулевые
		// (у окон с сохранённым положением там лежат 800×600 и 1024×768). Окно
		// без положения ToolBook разворачивает по странице и ставит по центру
		// родительского окна.
		Common::Point windowOrigin = viewerOrigin(shown);
		debug(3, "ОКНО: страница «%s» %ux%u в (%d, %d), объектов %u",
				shown.name.c_str(), shown.canvasWidth, shown.canvasHeight,
				windowOrigin.x, windowOrigin.y, shown.objects.size());
		for (uint o = 0; o < shown.eventHandlers.size(); o++)
			debug(3, "  ОКНО: обработчик страницы/фона «%s» hash=0x%04x",
					shown.eventHandlers[o].name.c_str(), shown.eventHandlers[o].eventHash);
		for (uint o = 0; o < shown.objects.size(); o++)
			debug(3, "  ОКНО: объект «%s» (%d,%d)-(%d,%d)%s тип 0x%02x",
					shown.objects[o].name.c_str(), shown.objects[o].rect.left,
					shown.objects[o].rect.top, shown.objects[o].rect.right,
					shown.objects[o].rect.bottom, shown.objects[o].field ? " поле" : "",
					shown.objects[o].type);
		for (uint o = 0; o < shown.objects.size(); o++) {
			const Object &obj = shown.objects[o];
			if (!objectVisible(shown, obj) || !obj.picture || obj.image < 0)
				continue;
			Graphics::Palette palette(256);
			Graphics::Surface *img = _book->decodeImage(
					const_cast<Image &>(_book->images()[obj.image]), palette);
			if (!img || !img->format.isCLUT8()) {
				if (img) { img->free(); delete img; }
				debug(3, "ОКНО: декодер не дал картинку %d", obj.image);
				continue;
			}
			_system->getPaletteManager()->setPalette(palette.data(), 0, 256);
			blitTransparent(screen, *img, obj.rect.left + windowOrigin.x,
					obj.rect.top + windowOrigin.y, 253);
			img->free();
			delete img;
			drawn = true;
		}
		for (uint o = 0; o < shown.objects.size(); o++) {
			const Object &obj = shown.objects[o];
			if (obj.field && objectVisible(shown, obj))
				drawField(screen, obj, windowOrigin);
		}
	}

	// ToolBook Fields are native text overlays; their frame and labels are
	// already part of the surrounding Picture DIB. Draw only the mutable text.
	for (uint o = 0; o < page.objects.size(); o++) {
		const Object &obj = page.objects[o];
		if (obj.field && objectVisible(page, obj))
			drawField(screen, obj, origin);
	}

	if (!drawn) {
		// Фон этой страницы сжат неразобранным способом. Показываем то, что
		// про страницу действительно известно, а не шум вместо картинки.
		drawPageInfo(screen, index, page);
	}

	if (_showHotspots)
		drawObjectFrames(screen, page);

	_system->unlockScreen();

	debug(1, "ToolBook: страница %d/%d «%s», фон %d, объектов %u, обработчиков %u, строк текста %u",
			index + 1, (int)pages.size(), page.name.c_str(), page.background,
			page.objects.size(), page.eventHandlers.size(), page.text.size());
	for (uint i = 0; i < page.objects.size() && i < 8; i++)
		debug(2, "  объект %s (%d,%d)-(%d,%d)", page.objects[i].name.c_str(),
				page.objects[i].rect.left, page.objects[i].rect.top,
				page.objects[i].rect.right, page.objects[i].rect.bottom);
	for (uint i = 0; i < page.text.size() && i < 3; i++)
		debug(2, "  текст: %s", page.text[i].c_str());
}

void ToolBookEngine::showImage(int index) {
	if (index < 0 || index >= (int)_rawImages.size())
		return;

	Image &img = const_cast<Image &>(_book->images()[_rawImages[index]]);
	Graphics::Palette palette(256);
	Graphics::Surface *surf = _book->decodeImage(img, palette);

	Graphics::Surface *screen = _system->lockScreen();
	screen->fillRect(Common::Rect(0, 0, screen->w, screen->h), 0);

	if (surf) {
		if (surf->format.isCLUT8())
			_system->getPaletteManager()->setPalette(palette.data(), 0, 256);

		int x = MAX(0, (screen->w - (int16)surf->w) / 2);
		int y = MAX(0, (screen->h - (int16)surf->h) / 2);
		int rows = MIN((int)surf->h, (int)screen->h - y);
		int cols = MIN((int)surf->w, (int)screen->w - x);
		for (int row = 0; row < rows; row++) {
			memcpy(screen->getBasePtr(x, y + row), surf->getBasePtr(0, row),
					cols * surf->format.bytesPerPixel);
		}
		surf->free();
		delete surf;
	}

	_system->unlockScreen();
	debug(1, "ToolBook: картинка %d/%d — %dx%d %d бит @0x%x",
			index + 1, (int)_rawImages.size(), img.width, img.height, img.depth, img.offset);
}

void ToolBookEngine::drawPageInfo(Graphics::Surface *screen, int index, const Page &page) {
	const Graphics::Font *font = FontMan.getFontByUsage(Graphics::FontManager::kGUIFont);
	if (!font)
		return;

	// В 8-битном режиме заводим себе минимальную палитру: серый фон, белый текст.
	if (screen->format.isCLUT8()) {
		byte pal[3 * 256];
		memset(pal, 0, sizeof(pal));
		pal[3 * 1 + 0] = pal[3 * 1 + 1] = pal[3 * 1 + 2] = 0xff;
		pal[3 * 2 + 0] = pal[3 * 2 + 1] = pal[3 * 2 + 2] = 0x80;
		_system->getPaletteManager()->setPalette(pal, 0, 256);
	}
	const uint32 white = screen->format.isCLUT8() ? 1 : screen->format.RGBToColor(0xff, 0xff, 0xff);
	const uint32 grey = screen->format.isCLUT8() ? 2 : screen->format.RGBToColor(0x80, 0x80, 0x80);

	int y = 20;
	Common::String head = Common::String::format("%d/%d  %s", index + 1,
			(int)_book->pages().size(), page.name.c_str());
	font->drawString(screen, head, 20, y, screen->w - 40, white);
	y += font->getFontHeight() + 6;

	if (page.background >= 0) {
		const Image &img = _book->images()[page.background];
		font->drawString(screen, Common::String::format(
				"fon %dx%d %d bit - dannye ne naydeny", img.width, img.height, img.depth),
				20, y, screen->w - 40, grey);
	} else {
		font->drawString(screen, "fon ne nayden", 20, y, screen->w - 40, grey);
	}
	y += font->getFontHeight() + 10;

	Common::String handlers;
	for (uint i = 0; i < page.handlers.size() && i < 10; i++)
		handlers += page.handlers[i] + " ";
	font->drawString(screen, handlers, 20, y, screen->w - 40, white);
	y += font->getFontHeight() + 10;

	for (uint i = 0; i < page.text.size() && y < screen->h - 30; i++) {
		font->drawString(screen, page.text[i], 20, y, screen->w - 40, white);
		y += font->getFontHeight() + 2;
	}
}

} // End of namespace ToolBook
