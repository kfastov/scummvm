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
	for (uint i = 0; i < object.handlers.size(); i++)
		if (object.handlers[i].eventHash == eventHash) {
			runHandler(object.handlers[i]);
			return;
		}
}

// Initial executable subset of OpenScript. It contains only machine-confirmed
// VM operations and generic ToolBook services. An unknown operation stops the
// current handler: fabricating a return value could select a game branch that
// the serialized OpenScript never selected.
bool ToolBookEngine::runHandler(const Handler &handler) {
	struct Value {
		uint32 number = 0;
		Common::String string;
		bool isString = false;
		bool isObject = false;
		bool hasReference = false;
		uint32 reference = 0; // serialized near/far reference target in the book
		uint8 type = 0; // ToolBook runtime type byte (when known)
		uint8 width = 4; // physical VM stack width: W=2, D=4, extended=10
	};
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
	const byte *code = _book->bytes(handler.code, handler.codeSize);
	if (!code)
		return false;
	uint32 ip = 0;
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
	auto pop = [&]() { Value v; if (!stack.empty()) { v = stack.back(); stack.pop_back(); } return v; };
	auto popBytes = [&](uint bytes) {
		uint consumed = 0;
		while (!stack.empty() && consumed < bytes) {
			consumed += stack.back().width;
			stack.pop_back();
		}
		if (consumed != bytes)
			debug(1, "ToolBook: stack cleanup %u bytes consumed %u @0x%x",
					bytes, consumed, handler.code + ip);
		return consumed == bytes;
	};
	auto truth = [&](const Value &v) { return v.isString || v.isObject ? !v.string.empty() : v.number != 0; };
	auto local = [&](int off) { return locals.contains(off) ? locals[off] : Value(); };
	auto read16 = [&](uint32 at) { return (uint16)(code[at] | (code[at + 1] << 8)); };
	auto branch = [&](uint32 end, uint16 rel) { ip = (uint16)(end + rel); };
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
		return _book->handlerString(handler, handler.code + (uint16)(operand + 4 + rel) + 5);
	};
	for (uint steps = 0; ip < handler.codeSize && steps < 10000; steps++) {
		uint8 op = code[ip++];
		switch (op) {
		case 0x02: { int16 off = (int16)read16(ip); ip += 2; Value value = local(off); value.width = 4; stack.push_back(value); break; }
		case 0x01: { int16 off = (int16)read16(ip); ip += 2; Value value = local(off); value.width = 2; stack.push_back(value); break; }
		case 0x04: pushNumber(code[ip++], 2); break;
		case 0x05: pushNumber(read16(ip), 2); ip += 2; break;
		case 0x06: pushNumber((uint32)read16(ip) | ((uint32)read16(ip + 2) << 16)); ip += 4; break;
		case 0x07: case 0x08: {
			uint16 rel = read16(ip);
			ip += 2;
			uint32 reference = handler.code + (uint16)(ip + rel);
			Common::String literal;
			if (op != 0x08 || !parseNativeDescriptor(reference, nullptr))
				literal = _book->handlerString(handler, reference);
			pushReference(reference, literal,
					op == 0x07 ? 2 : 4);
			break;
		}
		case 0x0d: { int16 off = (int16)read16(ip); ip += 2; locals[off] = pop(); locals[off].width = 4; break; }
		case 0x0c: { int16 off = (int16)read16(ip); ip += 2; locals[off] = pop(); locals[off].width = 2; break; }
		case 0x0f: {
			int16 adjustment = (int16)read16(ip);
			ip += 2;
			// A negative adjustment reserves frame scratch space, represented by
			// locals in this abstract VM. A positive one discards real values.
			if (adjustment > 0)
				popBytes(adjustment);
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
		case 0x13: case 0x14: { uint16 rel = read16(ip); ip += 2; bool condition = truth(pop()); if ((op == 0x13 && condition) || (op == 0x14 && !condition)) branch(ip, rel); break; }
		case 0x15: { uint16 rel = read16(ip); ip += 2; if (!truth(pop())) branch(ip, rel); break; }
		case 0x20: {
			uint16 id = read16(ip); ip += 2;
			uint8 cleanup = code[ip++];
			if (id != 192) {
				debug(1, "ToolBook: variadic builtin %u @0x%x пока не реализован",
						id, handler.code + ip - 4);
				return false;
			}
			// Aggregate constructor: cleanup includes flag/count words and N
			// dynamic values, but not the receiver beneath them.
			popBytes(cleanup);
			pushNumber(0, 4);
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
							handler.code + ip - 3);
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
			} else if (id == 180) { // page(name)
				Value name = pop();
				pop(); // book/context
				if (op != 0x21) pushString(name.string);
			} else if (id == 177) { // background(name)
				Value name = pop();
				pop(); // book/context
				if (op != 0x21) pushObject(name.string);
			} else if (id == 182) { // object(owner, name, class)
				pop(); // class selector
				Value name = pop();
				pop(); // page/background owner
				if (op != 0x21) pushObject(name.string);
			} else if (id == 329) { // resource(owner, name, class)
				pop();
				Value name = pop();
				pop();
				if (op != 0x21) pushObject(name.string);
			} else if (id == 194 || id == 198) { // typed `is` / ordinary equality
				Value b = pop(), a = pop();
				bool eq = a.isString || a.isObject || b.isString || b.isObject ?
						a.string.equalsIgnoreCase(b.string) : a.number == b.number;
				pushNumber(eq ? 1 : 0, 2);
			} else if (id == 190) { // case-insensitive `does not contain`
				Value haystack = pop();
				Value needle = pop();
				if ((!haystack.isString && !haystack.isObject) ||
						(!needle.isString && !needle.isObject)) {
					debug(1, "ToolBook: builtin 190 non-string operands @0x%x",
							handler.code + ip - 3);
					return false;
				}
				Common::String foldedHaystack = haystack.string;
				Common::String foldedNeedle = needle.string;
				foldedHaystack.toUppercase();
				foldedNeedle.toUppercase();
				pushNumber(foldedHaystack.find(foldedNeedle) == Common::String::npos, 2);
			} else if (id == 23) { // page navigation
				Value target = pop();
				if (target.isString || target.isObject)
					navigateTo(target.string);
			} else if (id == 122) { // system word property getter
				pop();
				// sysLevel is a ToolBook enum word. Keep the symbolic value until
				// enum coercion (2d) tags it as a dynamic value.
				pushString("Reader", 2);
			} else if (id == 144) {
				// RUN seg66:02fc consumes a selector W and an input D, and
				// returns a D. The exact serialized selector 0x401b is its
				// identity branch, used to obtain the current object/context.
				Value selector = pop();
				Value value = pop();
				if (selector.number != 0x401b) {
					debug(1, "ToolBook: builtin 144 selector %04x пока не реализован @0x%x",
							selector.number, handler.code + ip - 3);
					return false;
				}
				stack.push_back(value);
			} else if (id == 98) { // Win16 AnsiUpper(string copy)
				Value value = pop();
				if (!value.isString && !value.isObject) {
					debug(1, "ToolBook: builtin 98 non-string conversion пока не реализован @0x%x",
							handler.code + ip - 3);
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
				if ((!left.isString && !left.isObject) ||
						(!right.isString && !right.isObject)) {
					debug(1, "ToolBook: builtin 62 non-string operands пока не реализованы @0x%x",
							handler.code + ip - 3);
					return false;
				}
				pushString(left.string + right.string);
			} else if (id == 33 || id == 56) { // hide/show object
				Value object = pop();
				if (object.isObject || object.isString)
					setObjectVisible(object.string, id == 56);
			} else if (id == 129) { // sysCursor setter
				pop();
				pop();
			} else if (id == 127) {
				// RUN seg95:02b0 is a void dispatcher over a W action and a D
				// value (retf 6). The serialized startup uses action 12 with a
				// null value; that exact branch performs no import/callback and
				// sets the runtime's internal dynamic slot to canonical true.
				Value action = pop();
				Value value = pop();
				if (action.number != 12 || truth(value)) {
					debug(1, "ToolBook: builtin 127 action %u/value пока не реализован @0x%x",
							action.number, handler.code + ip - 3);
					return false;
				}
				_runtimeAction12 = true;
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
			} else {
				debug(1, "ToolBook: builtin %u @0x%x пока не реализован",
						id, handler.code + ip - 3);
				return false;
			}
			break;
		}
		case 0x26: break;
		case 0x27: return true;
		case 0x28: return true;
		case 0x2b: { // coerce a dynamic value to another dynamic type
			uint8 type = code[ip++];
			if (!stack.empty()) {
				stack.back().type = type;
				stack.back().width = 4;
			}
			break;
		}
		case 0x2c: { // materialize a dynamic value in the requested raw type
			uint8 type = code[ip++];
			if (!stack.empty()) {
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
		case 0x33: ip++; break;
		case 0x3b: { uint8 slot = code[ip++]; if (slot == 2) pushString(_book->pages()[_currentPage].name); else pushNumber(0); break; }
		case 0x3c:
			if (!stack.empty())
				stack.back().width = 4;
			break;
		case 0x3e: { // cached global-symbol load
			Common::String name = globalName(ip);
			ip += 4;
			if (_scriptGlobals.contains(name))
				pushString(_scriptGlobals.getVal(name));
			else
				pushNumber(0);
			break;
		}
		case 0x3f: { // cached global-symbol store
			Common::String name = globalName(ip);
			ip += 4;
			Value value = pop();
			_scriptGlobals[name] = value.isString || value.isObject ?
					value.string : Common::String::format("%u", value.number);
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
		case 0x4a: { Value value; value.number = code[ip++]; value.type = 0x22; value.width = 10; stack.push_back(value); break; }
		case 0x59: ip += 2; pushNumber(0, 2); break;
		case 0x6d: {
			uint16 rel = read16(ip);
			uint32 nameTarget = handler.code + (uint16)(ip + 2 + rel);
			uint8 kind = code[ip + 2];
			uint16 argumentBytes = read16(ip + 3);
			ip += 5;
			// The target is formally a two-byte selector followed by the
			// identifier. It is never subject to the literal-string heuristic.
			Common::String name = _book->readString(nameTarget + 2, 255);
			Common::String key = name;
			key.toUppercase();
			if (kind == 1 && _nativeFunctions.contains(key)) {
				const NativeBinding &binding = _nativeFunctions.getVal(key);
				// The linked thunk has its own marshalling signature. It is not
				// equal to the wire's explicit byte count: a property-style call
				// may use the implicit receiver as its first native D argument.
				Common::Array<Value> args;
				pop(); // duplicated lookup context
				Value receiver = pop();
				uint consumed = 0;
				while (!stack.empty() && consumed < argumentBytes) {
					consumed += stack.back().width;
					args.push_back(pop());
				}
				if (consumed != argumentBytes) {
					debug(1, "ToolBook: native %s argument stack mismatch", name.c_str());
					return false;
				}
				if (key == "DISPLAYFONTS") {
					if (binding.argumentBytes != 4 || argumentBytes != 0) {
						debug(1, "ToolBook: unexpected displayFonts signature");
						return false;
					}
					Common::String family = receiver.string;
					Common::String result;
					for (Common::HashMap<Common::String, NativeFontFace>::const_iterator it =
							_nativeFonts.begin(); it != _nativeFonts.end(); ++it) {
						const NativeFontFace &face = it->_value;
						if (!family.empty() && !face.name.equalsIgnoreCase(family))
							continue;
						result += face.name;
						for (uint s = 0; s < face.points.size(); s++)
							result += Common::String::format(",%u", face.points[s]);
						result += "\r\n";
					}
					pushString(result);
				} else {
					debug(1, "ToolBook: native function %s пока не реализована @0x%x",
							name.c_str(), handler.code + ip - 6);
					return false;
				}
			} else {
				debug(1, "ToolBook: named dispatch %s kind %u/%u пока не реализован @0x%x",
						name.c_str(), kind, argumentBytes, handler.code + ip - 6);
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
			debug(1, "ToolBook: OpenScript opcode %02x @0x%x пока не реализован", op, handler.code + ip - 1);
			return false;
		}
	}
	return true;
}

// ЛОКАЛЬНАЯ ПРАВКА (не для апстрима): сценарий ввода для безоконных прогонов,
// такой же по смыслу, как в движке director. Строки вида
//     1500 click 232 215
//     4000 key 111
// где первое число — миллисекунды от старта. Это только воспроизводимый
// пользовательский ввод для кадров сверки; переходы выбирает сама книга.
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
	if (_currentPage < 0 || _currentPage >= (int)_book->pages().size())
		return nullptr;
	const Common::Array<Object> &objects = _book->pages()[_currentPage].objects;
	for (uint i = 0; i < objects.size(); i++)
		if (objects[i].name.equalsIgnoreCase(name))
			return &objects[i];
	return nullptr;
}

const Object *ToolBookEngine::findCurrentObject(uint32 block) const {
	if (!block || _currentPage < 0 || _currentPage >= (int)_book->pages().size())
		return nullptr;
	const Common::Array<Object> &objects = _book->pages()[_currentPage].objects;
	for (uint i = 0; i < objects.size(); i++)
		if (objects[i].block == block)
			return &objects[i];
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

Common::Point ToolBookEngine::pageOrigin(const Page &page) const {
	return page.hasCanvasOrigin ? Common::Point(page.canvasX, page.canvasY) : Common::Point();
}

const Object *ToolBookEngine::objectAt(int x, int y) const {
	const Common::Array<Page> &pages = _book->pages();
	if (_imageMode || _currentPage < 0 || _currentPage >= (int)pages.size())
		return nullptr;

	// Objects are stored in formal back-to-front child-list order. Walk that
	// order backwards so the topmost visible object receives the event.
	const Common::Array<Object> &objs = pages[_currentPage].objects;
	Common::Point origin = pageOrigin(pages[_currentPage]);
	for (int i = (int)objs.size() - 1; i >= 0; i--) {
		Common::Rect rect = objs[i].rect;
		rect.translate(origin.x, origin.y);
		if (!objectVisible(pages[_currentPage], objs[i]) || !rect.contains(x, y))
			continue;
		// У многоугольных областей рамка — только грубая отсечка: попадание
		// считается по обводу, иначе кнопки на карте перекрывают друг друга.
		if (!objs[i].outline.empty() && !pointInOutline(objs[i], x - origin.x, y - origin.y))
			continue;
		return &objs[i];
	}
	return nullptr;
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

	// ToolBook Fields are native text overlays; their frame and labels are
	// already part of the surrounding Picture DIB. Draw only the mutable text.
	const Graphics::Font *fieldFont = _fieldFont;
	if (fieldFont) {
		for (uint o = 0; o < page.objects.size(); o++) {
			const Object &obj = page.objects[o];
			if (!obj.field || !objectVisible(page, obj))
				continue;
			Common::String value = (_fieldSelectAll && _focusedField == obj.block) ?
					Common::String() : fieldText(obj);
			Common::U32String text = value.decode(Common::kUtf8);
			int x = obj.rect.left + origin.x + 2;
			int y = obj.rect.top + origin.y + MAX(0, (obj.rect.height() - fieldFont->getFontHeight()) / 2);
			fieldFont->drawString(screen, text, x, y, MAX(0, obj.rect.width() - 4), 0);
			if (_focusedField == obj.block) {
				int caret = MIN(obj.rect.right + origin.x - 2, x + fieldFont->getStringWidth(text));
				screen->drawLine(caret, obj.rect.top + origin.y + 3,
						caret, obj.rect.bottom + origin.y - 4, 0);
			}
		}
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
