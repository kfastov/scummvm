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
// Пока умеет ровно то, что разобрано в формате: открыть книгу, собрать список
// страниц с фонами и текстом и показывать их. Байт-код OpenScript не разобран,
// поэтому собственной логики игры (кнопки, мини-игры, MCI-команды) здесь нет —
// см. book.h и ledger/0015, там записано, что именно установлено и что нет.

#include "engines/engine.h"
#include "common/array.h"
#include "common/error.h"
#include "common/rect.h"

struct ADGameDescription;

namespace Graphics {
struct Surface;
class Palette;
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
	void showPage(int index);
	void drawPageInfo(Graphics::Surface *screen, int index, const struct Page &page);
	void handleEvents();

	const ADGameDescription *_desc;
	Book *_book = nullptr;

	void showImage(int index);
	Common::Array<int> _rawImages; ///< индексы картинок, которые умеем разворачивать
	bool _imageMode = false;
	int _currentImage = 0;

	int _currentPage = 0;
	bool _quit = false;
	bool _needsRedraw = true;
};

} // End of namespace ToolBook

#endif
