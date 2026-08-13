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
#include "common/debug.h"
#include "common/events.h"
#include "common/file.h"
#include "common/system.h"
#include "engines/advancedDetector.h"
#include "engines/util.h"
#include "graphics/font.h"
#include "graphics/fontman.h"
#include "graphics/palette.h"
#include "graphics/paletteman.h"
#include "graphics/surface.h"

#include "toolbook/book.h"
#include "toolbook/toolbook.h"

namespace ToolBook {

// Книга «Башни знаний» лежит внутри KNTOWER.EXE с этого смещения — там просто
// начинается файл книги, без обёртки.
static const uint32 kEmbeddedBookOffset = 0x29e0;

ToolBookEngine::ToolBookEngine(OSystem *syst, const ADGameDescription *desc)
		: Engine(syst), _desc(desc) {
}

ToolBookEngine::~ToolBookEngine() {
	delete _book;
}

Common::Error ToolBookEngine::run() {
	initGraphics(640, 480);
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

	showPage(0);

	while (!shouldQuit() && !_quit) {
		handleEvents();
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

// ЛОКАЛЬНАЯ ПРАВКА (не для апстрима): сценарий ввода для безоконных прогонов,
// такой же по смыслу, как в движке director. Строки вида
//     1500 click 232 215
//     4000 key 111
// где первое число — миллисекунды от старта. Нужен, чтобы обход игры был
// воспроизводимым и снимал одни и те же кадры для сверки с эталоном.
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
			ev.type = Common::EVENT_LBUTTONUP;
			ev.mouse = Common::Point(in.x, in.y);
		} else {
			ev.type = Common::EVENT_KEYDOWN;
			ev.kbd = Common::KeyState((Common::KeyCode)in.x, in.x);
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
		case Common::EVENT_KEYDOWN:
			switch (event.kbd.keycode) {
			case Common::KEYCODE_ESCAPE:
				_quit = true;
				break;
			case Common::KEYCODE_RIGHT:
			case Common::KEYCODE_SPACE:
			case Common::KEYCODE_PAGEDOWN:
				if (_imageMode) {
					if (_currentImage + 1 < (int)_rawImages.size())
						_currentImage++;
				} else if (_currentPage + 1 < (int)_book->pages().size()) {
					_currentPage++;
				}
				_needsRedraw = true;
				break;
			case Common::KEYCODE_LEFT:
			case Common::KEYCODE_PAGEUP:
				if (_imageMode) {
					if (_currentImage > 0)
						_currentImage--;
				} else if (_currentPage > 0) {
					_currentPage--;
				}
				_needsRedraw = true;
				break;
			case Common::KEYCODE_HOME:
				_currentPage = 0;
				_currentImage = 0;
				_needsRedraw = true;
				break;
			case Common::KEYCODE_i:
				// Просмотр ресурсов книги: показывает те картинки, которые
				// движок действительно разворачивает.
				_imageMode = !_imageMode;
				_needsRedraw = true;
				break;
			case Common::KEYCODE_o:
				// Рамки объектов страницы — проверка разбора прямоугольников.
				_showHotspots = !_showHotspots;
				_needsRedraw = true;
				break;
			default:
				break;
			}
			break;
		case Common::EVENT_LBUTTONUP: {
			// Сначала спрашиваем объекты страницы: у них теперь есть настоящие
			// прямоугольники. Пока это только сообщение в лог — исполнять
			// обработчик нечем, интерпретатора нет.
			const Object *hit = objectAt(event.mouse.x, event.mouse.y);
			if (hit) {
				debug(0, "ToolBook: щелчок по объекту «%s» (%d,%d)-(%d,%d)",
						hit->name.c_str(), hit->rect.left, hit->rect.top,
						hit->rect.right, hit->rect.bottom);
				break;
			}
			// Промах — листаем: правая половина вперёд, левая назад.
			if (event.mouse.x > 320) {
				if (_currentPage + 1 < (int)_book->pages().size()) {
					_currentPage++;
					_needsRedraw = true;
				}
			} else if (_currentPage > 0) {
				_currentPage--;
				_needsRedraw = true;
			}
			break;
		}
		default:
			break;
		}
	}
}

const Object *ToolBookEngine::objectAt(int x, int y) const {
	const Common::Array<Page> &pages = _book->pages();
	if (_imageMode || _currentPage < 0 || _currentPage >= (int)pages.size())
		return nullptr;

	// Меньший объект выигрывает: полноэкранные прямоугольники вроде popkaRec
	// лежат под всеми и не должны перехватывать щелчок.
	const Object *best = nullptr;
	int bestArea = 0;
	const Common::Array<Object> &objs = pages[_currentPage].objects;
	for (uint i = 0; i < objs.size(); i++) {
		if (!objs[i].rect.contains(x, y))
			continue;
		int area = objs[i].rect.width() * objs[i].rect.height();
		if (!best || area < bestArea) {
			best = &objs[i];
			bestArea = area;
		}
	}
	return best;
}

void ToolBookEngine::drawObjectFrames(Graphics::Surface *screen, const Page &page) {
	// Рамки объектов: пока интерпретатора нет, это единственный способ увидеть,
	// правильно ли разобраны прямоугольники (клавиша o).
	const uint32 color = screen->format.isCLUT8() ? 255 : screen->format.RGBToColor(0xff, 0, 0xff);
	for (uint i = 0; i < page.objects.size(); i++) {
		Common::Rect r = page.objects[i].rect;
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
	screen->fillRect(Common::Rect(0, 0, screen->w, screen->h), 0);

	bool drawn = false;
	if (page.background >= 0) {
		Graphics::Palette palette(256);
		Graphics::Surface *img = _book->decodeImage(const_cast<Image &>(_book->images()[page.background]), palette);
		if (img) {
			if (img->format.isCLUT8())
				_system->getPaletteManager()->setPalette(palette.data(), 0, 256);

			int x = MAX(0, (screen->w - (int16)img->w) / 2);
			int y = MAX(0, (screen->h - (int16)img->h) / 2);
			int rows = MIN((int)img->h, (int)screen->h - y);
			int cols = MIN((int)img->w, (int)screen->w - x);
			for (int row = 0; row < rows; row++) {
				memcpy(screen->getBasePtr(x, y + row), img->getBasePtr(0, row),
						cols * img->format.bytesPerPixel);
			}
			img->free();
			delete img;
			drawn = true;
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
			page.objects.size(), page.handlers.size(), page.text.size());
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
