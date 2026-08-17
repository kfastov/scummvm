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
#include "common/random.h"
#include "audio/mixer.h"
#include "common/rect.h"
#include "common/ustr.h"

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
	bool isHandlerRef = false;   ///< разрешённый адрес обработчика (посылка 0x6c)
	Common::String receiverName;  ///< кому послано
		uint32 number = 0;
		Common::String string;
		bool isString = false;
		bool isObject = false;
		bool hasReference = false;
		bool isBookReference = false;
		uint32 reference = 0;
		uint32 buffer = 0;
		Common::Array<Common::String> array;
		/// Вместилище, в котором объект был разрешён по имени (builtin 182):
		/// `field "Message" of page "message3"` даёт owner = «message3».
		/// Через него читается свойство 0x4020 — «страница этого объекта».
		Common::String owner;
		bool isViewer = false;   ///< разрешён как класс 38 (Viewer)
		/// Номер массива в `_arrayStore`. У среды массив — ссылка (`ValueNewArray`
		/// отдаёт хендл, `ValueArraySet` меняет содержимое по нему), поэтому в
		/// значении лежит номер, а не копия элементов (ledger/0081).
		uint32 arrayId = 0;
		/// Ссылка на место в стеке операндов: опкод 0x59 — это `push (sp + N)`,
		/// адрес внутри стека, куда арифметика пишет результат (ledger/0082).
		bool isStackRef = false;
		uint32 stackRef = 0;   ///< индекс значения в стеке
		uint8 type = 0;
		uint8 width = 4;
	};
	/// Состояние окна книги. 346 открывает окно, 324 показывает, 347 закрывает;
	/// показываемую страницу кладёт свойство 0x40d7 (ledger/0077).
	struct ViewerState {
		bool open = false;
		bool visible = false;
		Common::String page;
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
	struct NativeBuffer {
		Common::Array<byte> data;
	};

	void showPage(int index);
	void drawPageInfo(Graphics::Surface *screen, int index, const struct Page &page);
	void drawObjectFrames(Graphics::Surface *screen, const struct Page &page);
	const struct Object *objectAt(int x, int y) const;
	const struct Object *objectIn(const struct Page &page, Common::Point origin,
			int x, int y) const;
	/// Начало координат показанного окна на экране.
	Common::Point viewerOrigin(const struct Page &page) const;
	const struct Object *findCurrentObject(const Common::String &name) const;
	const struct Object *findCurrentObject(uint32 block) const;
	bool objectVisible(const struct Page &page, const struct Object &object,
			uint depth = 0) const;
	void setObjectVisible(const Common::String &name, bool visible);
	Common::String fieldText(const struct Object &object) const;
	/// Поле по значению скрипта: имя плюс вместилище из builtin 182.
	const struct Object *findFieldObject(const ScriptValue &value) const;
	/// Шрифт нужного кегля (в пунктах) и разметка текста поля по словам.
	const Graphics::Font *fieldFont(int points) const;
	int fieldPoints(const struct Object &object) const;
	const Graphics::Font *layoutField(const struct Object &object,
			Common::Array<Common::U32String> &lines) const;
	/// Помещается ли текст в прямоугольник поля: этим книга выбирает страницу
	/// диалога (свойства 0x4030 и 0x41f8, ledger/0078).
	bool fieldTextFits(const struct Object &object) const;
	void drawField(Graphics::Surface *screen, const struct Object &object,
			Common::Point origin);
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
	/// Страница, на которой лежит объект: сначала показанное окно, потом книга.
	int pageOfObject(const struct Object &object) const;

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
	/// Кегль поля в пунктах (свойство 0x400f), по блоку объекта.
	Common::HashMap<uint32, int> _fieldSizes;
	mutable Common::HashMap<int, const Graphics::Font *> _fontCache;
	Common::HashMap<Common::String, ScriptValue> _scriptGlobals;
	Common::HashMap<uint32, NativeBuffer> _nativeBuffers;
	uint32 _nextNativeBuffer = 1;
	Common::HashMap<Common::String, Common::Array<NativeBinding> > _nativeBindings;
	Common::HashMap<Common::String, NativeBinding> _nativeFunctions;
	Common::HashMap<Common::String, NativeFontFace> _nativeFonts;
	Common::HashMap<Common::String, Common::String> _mciAliases;
	Common::HashMap<Common::String, Common::String> _winIniValues;
	/// Свойства объектов книги. У оригинала это CDB — база записей, куда
	/// `builtin 139` пишет через `MTB40BAS.CDBSetValueEx`. Своей CDB у движка
	/// нет, но наблюдаемое поведение то же: значение по паре
	/// «объект, номер свойства» кладётся и потом читается (ledger/0056).
	Common::HashMap<Common::String, ScriptValue> _objectProperties;
	/// Датчик случайных чисел книги: она сама засевает его временем (builtin 50).
	Common::RandomSource _random{"toolbook"};
	/// Звук книги: команды MCI заводят псевдонимы, по ним и играем.
	Audio::SoundHandle _mediaHandle;
	Common::String _playingAlias;
	/// Звук в обход MCI: `sndPlaySound` (builtin 316) зовут прямо путём.
	Audio::SoundHandle _effectHandle;
	/// Путь книги (`.\wav\null.wav`) -> поток движка. Возвращает «сыграли ли».
	bool playSoundFile(const Common::String &bookPath, Audio::SoundHandle *handle);
	/// Окна книги по имени (регистр приведён к верхнему).
	Common::HashMap<Common::String, ViewerState> _viewerStates;
	/// Открытое и показанное окно с назначенной страницей, иначе -1.
	int shownViewerPage() const;
	/// Свойство 0x402e — текст поля; книга задаёт его свойством, а не вводом.
	void setFieldProperty(const ScriptValue &object, uint32 property,
			const ScriptValue &value);
	void playMedia(const Common::String &alias);
	void stopMedia(const Common::String &alias);
	/// Ячейки среды по номеру: пара builtin 127 (запись) и 128 (чтение).
	Common::HashMap<uint32, ScriptValue> _systemSlots;
	/// Массивы книги по номеру: `ValueNewArray` заводит, `ValueArraySet` меняет.
	Common::HashMap<uint32, Common::Array<ScriptValue> > _arrayStore;
	uint32 _nextArray = 1;
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
