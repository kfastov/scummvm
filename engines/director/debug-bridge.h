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

#ifndef DIRECTOR_DEBUG_BRIDGE_H
#define DIRECTOR_DEBUG_BRIDGE_H

// ЛОКАЛЬНАЯ ПРАВКА (не для апстрима): отладочный мост.
//
// Живой движок слушает TCP-порт и отвечает на текстовые команды. Нужен потому,
// что безоконный прогон нечем расспросить: логи приходится растить до гигабайтов
// и вылавливать в них нужный кадр, а консоль ScummVM в режиме `dummy` открыть
// негде. Мост даёт прицельные вопросы («что под курсором», «какие поведения у
// канала 9», «выполни Lingo-выражение») и прицельный ввод в нужный момент.
//
// Порт задаётся ключом `debugbridge_port` в конфиге. Протокол: одна команда в
// строке, ответ — одна строка JSON {"ok":bool,"out":"..."}.

#include "common/array.h"
#include "common/str.h"

namespace Director {

class DebugBridge {
public:
	DebugBridge();
	~DebugBridge();

	/// Открывает слушающий сокет на localhost. Ложь — порт занят или сокет не создался.
	bool listen(int port);

	/// Вызывается из главного цикла: принимает клиента, читает и выполняет команды,
	/// доигрывает отложенные действия (отпускание кнопки мыши). Не блокирует.
	void poll();

	bool isRunning() const { return _listenFd >= 0; }

private:
	void acceptClient();
	void closeClient();
	void send(const Common::String &payload, bool ok);
	void readCommands();

	Common::String execute(const Common::String &line);

	// Собственные команды моста (всё остальное уходит в консоль ScummVM).
	Common::String cmdState();
	Common::String cmdSprites(bool all);
	Common::String cmdHit(int x, int y);
	Common::String cmdBehaviors(int channel);
	Common::String cmdShot(const Common::String &path);
	Common::String cmdEvent(int channel, const Common::String &eventName);
	Common::String cmdCastInfo(const Common::String &spec);
	Common::String cmdSound();

	void pushMouse(int type, int x, int y);
	void schedule(uint32 atMs, int type, int x, int y);

	int _listenFd;
	int _clientFd;
	Common::String _inbuf;

	// Очередь отложенного ввода. Мгновенный клик Director не замечает — он
	// опрашивает состояние мыши в своём цикле, — а перетаскивание вообще живёт
	// в цикле `repeat while the stilldown`, которому нужны движения мыши между
	// нажатием и отпусканием.
	struct PendingInput {
		uint32 atMs;
		int type;
		int x, y;
	};
	Common::Array<PendingInput> _pending;
};

} // End of namespace Director

#endif
