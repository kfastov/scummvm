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

#ifndef TOOLBOOK_TOOLBOOK_H
#define TOOLBOOK_TOOLBOOK_H

// Движок для книг Asymetrix Multimedia ToolBook 4.0.
//
// The engine reads the formal ToolBook heap, decodes the book's image streams
// and executes the machine-confirmed subset of OpenScript needed by the game.

#include "engines/engine.h"
#include "common/array.h"
#include "common/error.h"
#include "common/events.h"
#include "common/hashmap.h"
#include "common/rect.h"

struct ADGameDescription;

namespace Graphics {
struct Surface;
class Palette;
class Font;
}

namespace ToolBook {

class Book;

enum ToolBookDebugChannels {
	kDebugGeneral = 1,
	kDebugBook,
};

class ToolBookEngine : public Engine {
public:
	ToolBookEngine(OSystem *syst, const ADGameDescription *desc);
	~ToolBookEngine() override;

	Common::Error run() override;

	const char *getGameId() const;

private:
	struct ScriptValue {
		uint32 number = 0;
		Common::String string;
		bool isString = false;
		bool isObject = false;
		bool hasReference = false;
		uint32 reference = 0;
		uint32 buffer = 0;
		uint8 type = 0;
		uint8 width = 4;
	};
	struct NativeBinding {
		Common::String name;
		uint32 descriptor = 0;
		uint32 thunk = 0;
		uint16 ordinal = 0;
		uint8 argumentBytes = 0;
		uint8 thunkLength = 0;
		uint8 signatureLength = 0;
		bool byName = false;
	};
	struct NativeFontFace {
		Common::String name;
		Common::Array<uint16> points;
	};

	void showPage(int index);
	void drawPageInfo(Graphics::Surface *screen, int index, const struct Page &page);
	void drawObjectFrames(Graphics::Surface *screen, const struct Page &page);
	const struct Object *objectAt(int x, int y) const;
	const struct Object *findCurrentObject(const Common::String &name) const;
	const struct Object *findCurrentObject(uint32 block) const;
	bool objectVisible(const struct Page &page, const struct Object &object,
			uint depth = 0) const;
	void setObjectVisible(const Common::String &name, bool visible);
	Common::String fieldText(const struct Object &object) const;
	Common::Point pageOrigin(const struct Page &page) const;
	static bool pointInOutline(const struct Object &obj, int x, int y);
	void handleEvents();
	int findPageIndex(const Common::String &name) const;
	void navigateTo(const Common::String &name);
	bool runHandler(const struct Handler &handler);
	bool runHandler(const struct Handler &handler,
			const Common::Array<ScriptValue> &arguments,
			const ScriptValue &receiver, ScriptValue *result, uint depth);
	void dispatchPageEvent(uint16 eventHash);
	void dispatchObjectEvent(const struct Object &object, uint16 eventHash);

	const ADGameDescription *_desc;
	Book *_book = nullptr;
	const Graphics::Font *_fieldFont = nullptr;

	void showImage(int index);
	Common::Array<int> _rawImages; ///< индексы картинок, которые умеем разворачивать
	bool _imageMode = false;
	int _currentImage = 0;

	int _currentPage = 0;
	bool _quit = false;
	bool _needsRedraw = true;
	bool _showHotspots = false; ///< рамки объектов страницы, клавиша o
	Common::HashMap<uint32, bool> _visibilityOverrides;
	Common::HashMap<uint32, Common::String> _fieldValues;
	Common::HashMap<Common::String, Common::String> _scriptGlobals;
	Common::HashMap<Common::String, Common::Array<NativeBinding> > _nativeBindings;
	Common::HashMap<Common::String, NativeBinding> _nativeFunctions;
	Common::HashMap<Common::String, NativeFontFace> _nativeFonts;
	bool _runtimeAction12 = false;
	uint32 _hoveredObject = 0;
	uint32 _focusedField = 0;
	bool _fieldSelectAll = false;
	bool _pendingFirstIdle = false;

	// ЛОКАЛЬНАЯ ПРАВКА (не для апстрима): сценарий ввода для безоконных
	// прогонов; ключ конфига inputscript, см. toolbook.cpp.
	enum ScriptedInputAction { kInputClick, kInputKey };
	struct ScriptedInput {
		uint32 timeMs;
		ScriptedInputAction action;
		int x, y;
	};
	Common::Array<ScriptedInput> _inputScript;
	Common::Array<Common::Event> _injected;
	uint _inputScriptPos = 0;
	bool _inputScriptLoaded = false;
	uint32 _startMs = 0;

	void loadInputScript();
	void feedScriptedInput();
};

} // End of namespace ToolBook

#endif
