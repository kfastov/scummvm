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

// ЛОКАЛЬНАЯ ПРАВКА (не для апстрима): см. debug-bridge.h.

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/file.h"
#include "common/events.h"
#include "common/system.h"
#include "common/memstream.h"
#include "common/tokenizer.h"
#include "graphics/paletteman.h"
#include "graphics/surface.h"
#include "image/png.h"

#include "director/director.h"
#include "director/debugger.h"
#include "director/debug-bridge.h"
#include "director/cast.h"
#include "director/castmember/castmember.h"
#include "director/channel.h"
#include "director/frame.h"
#include "director/movie.h"
#include "director/score.h"
#include "director/sound.h"
#include "director/sprite.h"
#include "director/types.h"
#include "director/window.h"
#include "director/lingo/lingo.h"
#include "director/lingo/lingo-object.h"

namespace Director {

static Common::String jsonEscape(const Common::String &s) {
	Common::String out;
	for (uint i = 0; i < s.size(); i++) {
		byte c = (byte)s[i];
		switch (c) {
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			// Управляющие байты пропускаем через \u, всё остальное — как есть.
			// Строки движка бывают в кодировке игры, а не в UTF-8; клиент
			// декодирует их сам, поэтому старшие байты не трогаем.
			if (c < 0x20)
				out += Common::String::format("\\u%04x", c);
			else
				out += (char)c;
		}
	}
	return out;
}

DebugBridge::DebugBridge() {
	_listenFd = -1;
	_clientFd = -1;
}

DebugBridge::~DebugBridge() {
	closeClient();
	if (_listenFd >= 0)
		::close(_listenFd);
}

bool DebugBridge::listen(int port) {
	_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (_listenFd < 0) {
		warning("DebugBridge: сокет не создан: %s", strerror(errno));
		return false;
	}

	int yes = 1;
	::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16)port);
	addr.sin_addr.s_addr = htonl(0x7f000001); // только localhost

	if (::bind(_listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		warning("DebugBridge: порт %d занят: %s", port, strerror(errno));
		::close(_listenFd);
		_listenFd = -1;
		return false;
	}

	if (::listen(_listenFd, 1) < 0) {
		warning("DebugBridge: listen(%d): %s", port, strerror(errno));
		::close(_listenFd);
		_listenFd = -1;
		return false;
	}

	::fcntl(_listenFd, F_SETFL, O_NONBLOCK);
	warning("DebugBridge: слушаю 127.0.0.1:%d", port);
	return true;
}

void DebugBridge::acceptClient() {
	int fd = ::accept(_listenFd, nullptr, nullptr);
	if (fd < 0)
		return;

	// Один клиент за раз: новое подключение вытесняет старое.
	closeClient();

	int yes = 1;
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
	::fcntl(fd, F_SETFL, O_NONBLOCK);
	_clientFd = fd;
	_inbuf.clear();
}

void DebugBridge::closeClient() {
	if (_clientFd >= 0) {
		::close(_clientFd);
		_clientFd = -1;
	}
	_inbuf.clear();
}

void DebugBridge::send(const Common::String &payload, bool ok) {
	if (_clientFd < 0)
		return;

	Common::String line = Common::String::format("{\"ok\":%s,\"out\":\"%s\"}\n",
			ok ? "true" : "false", jsonEscape(payload).c_str());

	const char *p = line.c_str();
	size_t left = line.size();
	// Ответы короткие, но клиент может читать медленно. Крутимся с потолком,
	// чтобы залипший клиент не остановил движок навсегда.
	for (int spins = 0; left > 0 && spins < 100000; spins++) {
		ssize_t n = ::write(_clientFd, p, left);
		if (n > 0) {
			p += n;
			left -= n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		break;
	}
}

void DebugBridge::poll() {
	if (_listenFd < 0)
		return;

	// Отложенный ввод: всё, чему пришло время.
	uint32 now = g_system->getMillis();
	while (!_pending.empty() && _pending[0].atMs <= now) {
		PendingInput in = _pending.remove_at(0);
		if (in.type != Common::EVENT_MOUSEMOVE)
			pushMouse(Common::EVENT_MOUSEMOVE, in.x, in.y);
		pushMouse(in.type, in.x, in.y);
	}

	acceptClient();
	readCommands();
}

void DebugBridge::readCommands() {
	if (_clientFd < 0)
		return;

	char buf[4096];
	for (;;) {
		ssize_t n = ::read(_clientFd, buf, sizeof(buf));
		if (n > 0) {
			for (ssize_t i = 0; i < n; i++)
				_inbuf += buf[i];
			continue;
		}
		if (n == 0) { // клиент отключился
			closeClient();
			return;
		}
		break; // EAGAIN
	}

	for (;;) {
		uint nl = 0;
		while (nl < _inbuf.size() && _inbuf[nl] != '\n')
			nl++;
		if (nl >= _inbuf.size())
			break;

		Common::String line(_inbuf.c_str(), nl);
		_inbuf = Common::String(_inbuf.c_str() + nl + 1);
		line.trim();
		if (line.empty())
			continue;

		Common::String out = execute(line);
		send(out, true);
	}
}

void DebugBridge::pushMouse(int type, int x, int y) {
	// Кладём в сам менеджер событий: Lingo читает положение и кнопки мыши
	// напрямую (getMousePos/getButtonState), поэтому события в обход менеджера
	// для игры не существуют.
	Common::Event ev;
	ev.type = (Common::EventType)type;
	ev.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(ev);
}

void DebugBridge::schedule(uint32 atMs, int type, int x, int y) {
	PendingInput in;
	in.atMs = atMs;
	in.type = type;
	in.x = x;
	in.y = y;
	_pending.push_back(in);
}

// ---------------------------------------------------------------- команды

Common::String DebugBridge::cmdState() {
	Common::String out;
	Window *window = g_director->getCurrentWindow();
	Movie *movie = window ? window->getCurrentMovie() : nullptr;

	out += Common::String::format("millis: %u\n", g_system->getMillis());
	out += Common::String::format("mouse: %d,%d buttons: %d\n",
			g_system->getEventManager()->getMousePos().x,
			g_system->getEventManager()->getMousePos().y,
			g_system->getEventManager()->getButtonState());

	if (!movie) {
		out += "movie: (нет)\n";
		return out;
	}

	Score *score = movie->getScore();
	out += Common::String::format("movie: %s\n", movie->getArchive() ?
			movie->getArchive()->getPathName().toString('/').c_str() : "(без архива)");
	out += Common::String::format("path: %s\n", window->getCurrentPath().c_str());
	out += Common::String::format("frame: %d of %d\n", score->getCurrentFrameNum(), score->getFramesNum());
	out += Common::String::format("playState: %d\n", (int)score->_playState);
	out += Common::String::format("channels: %d\n", (int)score->_channels.size());
	out += Common::String::format("version: %d\n", g_director->getVersion());

	// Метка кадра, если есть — по ней удобно понимать, где мы в сценарии.
	Common::String label = score->getFrameLabel(score->getCurrentFrameNum());
	if (!label.empty())
		out += Common::String::format("label: %s\n", label.c_str());

	return out;
}

Common::String DebugBridge::cmdSprites(bool all) {
	Window *window = g_director->getCurrentWindow();
	Movie *movie = window ? window->getCurrentMovie() : nullptr;
	if (!movie)
		return "нет фильма";

	Score *score = movie->getScore();
	Common::Point mouse = g_system->getEventManager()->getMousePos();
	Common::String out;

	out += Common::String::format("кадр %d, каналов %d, мышь %d,%d\n",
			score->getCurrentFrameNum(), (int)score->_channels.size(), mouse.x, mouse.y);

	for (uint i = 0; i < score->_channels.size(); i++) {
		Channel *ch = score->_channels[i];
		if (!ch || !ch->_sprite)
			continue;
		Sprite *sp = ch->_sprite;
		if (!all && !sp->_castId.member)
			continue;

		Common::Rect bbox = ch->getBbox();
		CastMember *cast = movie->getCastMember(sp->_castId);
		CastMemberInfo *info = movie->getCastMemberInfo(sp->_castId);

		out += Common::String::format(
				"ch %-3d cast %-10s %-9s %-16s bbox (%d,%d)-(%d,%d) vis %d puppet %d/%04x move %d "
				"script %s beh %d inst %d frames [%d..%d] mouse %d\n",
				i, sp->_castId.asString().c_str(),
				cast ? castType2str(cast->_type) : "—",
				info && !info->name.empty() ? info->name.c_str() : "",
				bbox.left, bbox.top, bbox.right, bbox.bottom,
				ch->_visible, sp->_puppet, sp->_autoPuppet, sp->_moveable,
				sp->_scriptId.asString().c_str(),
				(int)sp->_behaviors.size(), (int)ch->_scriptInstanceList.size(),
				ch->_startFrame, ch->_endFrame,
				sp->respondsToMouse());
	}

	return out;
}

Common::String DebugBridge::cmdHit(int x, int y) {
	Window *window = g_director->getCurrentWindow();
	Movie *movie = window ? window->getCurrentMovie() : nullptr;
	if (!movie)
		return "нет фильма";

	Score *score = movie->getScore();
	Common::Point pos(x, y);
	Common::String out;
	int winner = 0;

	for (int i = score->_channels.size() - 1; i >= 0; i--) {
		Channel *ch = score->_channels[i];
		if (!ch || !ch->_sprite)
			continue;

		CollisionTest test = ch->isMouseIn(pos);
		if (test == kCollisionNo && !ch->_sprite->_castId.member)
			continue;

		bool responds = ch->_sprite->respondsToMouse();
		if (test != kCollisionNo) {
			Common::Rect bbox = ch->getBbox();
			out += Common::String::format("ch %-3d тест %d отвечает %d bbox (%d,%d)-(%d,%d) "
					"beh %d inst %d script %s\n",
					i, (int)test, responds, bbox.left, bbox.top, bbox.right, bbox.bottom,
					(int)ch->_sprite->_behaviors.size(), (int)ch->_scriptInstanceList.size(),
					ch->_sprite->_scriptId.asString().c_str());
		}

		if (test == kCollisionYes && responds && !winner)
			winner = i;
		else if (test == kCollisionHole && !winner)
			break;
	}

	out += Common::String::format("getMouseSpriteIDFromPos(%d,%d) = %d\n",
			x, y, score->getMouseSpriteIDFromPos(pos));
	return out;
}

Common::String DebugBridge::cmdBehaviors(int channel) {
	Window *window = g_director->getCurrentWindow();
	Movie *movie = window ? window->getCurrentMovie() : nullptr;
	if (!movie)
		return "нет фильма";

	Score *score = movie->getScore();
	if (channel < 0 || channel >= (int)score->_channels.size())
		return Common::String::format("нет канала %d", channel);

	Channel *ch = score->_channels[channel];
	Sprite *sp = ch->_sprite;
	Common::String out;

	out += Common::String::format("канал %d: cast %s, кадры канала [%d..%d], текущий %d\n",
			channel, sp->_castId.asString().c_str(), ch->_startFrame, ch->_endFrame,
			score->getCurrentFrameNum());
	out += Common::String::format("поведений в спрайте: %d, созданных экземпляров: %d\n",
			(int)sp->_behaviors.size(), (int)ch->_scriptInstanceList.size());
	out += Common::String::format("spriteListIdx %d\n  %s\n", sp->_spriteListIdx,
			sp->_spriteInfo.toString().c_str());

	// То же самое, но из партитуры текущего кадра, а не из живого канала.
	// Расхождение между ними и есть ответ на вопрос «кто отстал».
	if (score->_currentFrame && channel < (int)score->_currentFrame->_sprites.size()) {
		Sprite *fs = score->_currentFrame->_sprites[channel];
		out += Common::String::format("в партитуре кадра %d: cast %s, spriteListIdx %d\n  %s\n",
				score->getCurrentFrameNum(), fs->_castId.asString().c_str(),
				fs->_spriteListIdx, fs->_spriteInfo.toString().c_str());
	}

	// Сырые блоки деталей спрайта из файла: описание, поведения, имя. Разбор
	// именно этих байтов и решает, чему верить — файлу или движку.
	for (int d = 0; d < 3 && sp->_spriteListIdx; d++) {
		Common::MemoryReadStreamEndian *st = score->getSpriteDetailsStream(sp->_spriteListIdx + d);
		if (!st) {
			out += Common::String::format("  блок [%d]: нет\n", sp->_spriteListIdx + d);
			continue;
		}
		out += Common::String::format("  блок [%d], %d байт:", sp->_spriteListIdx + d, (int)st->size());
		for (uint b = 0; b < st->size(); b++) {
			if (b % 16 == 0)
				out += "\n    ";
			out += Common::String::format("%02x ", st->readByte());
		}
		out += "\n";
		delete st;
	}

	for (uint i = 0; i < sp->_behaviors.size(); i++) {
		BehaviorElement &b = sp->_behaviors[i];
		out += Common::String::format("  [%d] %s\n", i, b.toString().c_str());

		ScriptContext *scr = movie->getScriptContext(kScoreScript, b.memberID);
		if (!scr) {
			out += "       скрипта нет (getScriptContext вернул ноль)\n";
			continue;
		}

		out += "       обработчики:";
		for (auto &it : scr->_eventHandlers)
			out += Common::String::format(" %s", g_lingo->_eventHandlerTypes[(LEvent)it._key]);
		for (auto &it : scr->_functionHandlers)
			out += Common::String::format(" %s()", it._key.c_str());
		out += "\n";
	}

	for (uint i = 0; i < ch->_scriptInstanceList.size(); i++) {
		out += Common::String::format("  экземпляр [%d]: %s\n", i,
				ch->_scriptInstanceList[i].asString(true).c_str());
	}

	return out;
}

Common::String DebugBridge::cmdShot(const Common::String &path) {
	Graphics::Surface *screen = g_system->lockScreen();
	if (!screen) {
		return "экрана нет";
	}

	Common::DumpFile out;
	if (!out.open(Common::Path(path), true)) {
		g_system->unlockScreen();
		return Common::String::format("не открывается %s", path.c_str());
	}

	bool ok;
	if (screen->format.isCLUT8()) {
		byte pal[768];
		g_system->getPaletteManager()->grabPalette(pal, 0, 256);
		ok = Image::writePNG(out, *screen, pal, 256);
	} else {
		ok = Image::writePNG(out, *screen);
	}

	out.finalize();
	out.close();
	g_system->unlockScreen();

	return ok ? Common::String::format("%s %dx%d", path.c_str(), screen->w, screen->h)
			  : Common::String("writePNG не смог");
}

Common::String DebugBridge::cmdEvent(int channel, const Common::String &eventName) {
	Movie *movie = g_director->getCurrentMovie();
	if (!movie)
		return "нет фильма";

	if (!g_lingo->_eventHandlerTypeIds.contains(eventName))
		return Common::String::format("неизвестное событие %s", eventName.c_str());

	LEvent ev = (LEvent)g_lingo->_eventHandlerTypeIds[eventName];
	movie->processEvent(ev, channel);
	return Common::String::format("processEvent(%s, %d) выполнен", eventName.c_str(), channel);
}

Common::String DebugBridge::cmdCastInfo(const Common::String &spec) {
	Movie *movie = g_director->getCurrentMovie();
	if (!movie)
		return "нет фильма";

	int member = atoi(spec.c_str());
	int castLib = DEFAULT_CAST_LIB;
	const char *colon = strchr(spec.c_str(), ':');
	if (colon) {
		castLib = member;
		member = atoi(colon + 1);
	}

	CastMemberID id(member, castLib);
	CastMember *cast = movie->getCastMember(id);
	if (!cast)
		return Common::String::format("нет члена каста %s", id.asString().c_str());

	CastMemberInfo *info = movie->getCastMemberInfo(id);
	Common::String out = Common::String::format("%s: тип %s, имя \"%s\"\n",
			id.asString().c_str(), castType2str(cast->_type),
			info ? info->name.c_str() : "");

	if (info && !info->script.empty())
		out += Common::String::format("исходник скрипта:\n%s\n", info->script.c_str());

	static const ScriptType kTypes[] = { kScoreScript, kCastScript, kMovieScript, kParentScript };
	for (uint t = 0; t < ARRAYSIZE(kTypes); t++) {
		ScriptType type = kTypes[t];
		ScriptContext *scr = movie->getScriptContext(type, id);
		if (!scr)
			continue;
		out += Common::String::format("контекст %s:", scriptType2str(type));
		for (auto &it : scr->_eventHandlers)
			out += Common::String::format(" %s", g_lingo->_eventHandlerTypes[(LEvent)it._key]);
		for (auto &it : scr->_functionHandlers)
			out += Common::String::format(" %s()", it._key.c_str());
		out += "\n";
	}

	return out;
}

Common::String DebugBridge::cmdSound() {
	// Положительный признак вместо отсутствия ошибок: какие каналы реально
	// заняты и чем. Прогон под звуковым драйвером dummy этого не покажет —
	// в нём каналы тоже «играют», но хотя бы видно, что файл найден и поток создан.
	Window *window = g_director->getCurrentWindow();
	DirectorSound *sound = window ? window->getSoundManager() : nullptr;
	if (!sound)
		return "звуковой менеджер недоступен";

	Common::String out = Common::String::format("звук включён: %d, последний файл: '%s'\n",
			sound->getSoundEnabled(), sound->getCurrentSound().c_str());

	for (int ch = 1; ch <= 8; ch++) {
		bool active = sound->isChannelActive(ch);
		SoundChannel *chan = sound->getChannel(ch);
		if (!active && !chan)
			continue;
		out += Common::String::format("  канал %d: играет %d, громкость %d, puppet %d",
				ch, active, sound->getChannelVolume(ch), sound->isChannelPuppet(ch));
		if (chan && chan->lastPlayedSound.type == kSoundCast)
			out += Common::String::format(", последний член каста %d:%d",
					chan->lastPlayedSound.u.cast.castLib, chan->lastPlayedSound.u.cast.member);
		out += "\n";
	}
	return out;
}

Common::String DebugBridge::execute(const Common::String &line) {
	Common::StringTokenizer tok(line);
	Common::String cmd = tok.nextToken();

	if (cmd.equalsIgnoreCase("ping"))
		return "pong";

	if (cmd.equalsIgnoreCase("state"))
		return cmdState();

	if (cmd.equalsIgnoreCase("sprites")) {
		Common::String arg = tok.nextToken();
		return cmdSprites(arg.equalsIgnoreCase("all"));
	}

	if (cmd.equalsIgnoreCase("hit")) {
		int x = atoi(tok.nextToken().c_str());
		int y = atoi(tok.nextToken().c_str());
		return cmdHit(x, y);
	}

	if (cmd.equalsIgnoreCase("behaviors") || cmd.equalsIgnoreCase("beh"))
		return cmdBehaviors(atoi(tok.nextToken().c_str()));

	if (cmd.equalsIgnoreCase("eval") || cmd.equalsIgnoreCase("lingo")) {
		// Остаток строки — код Lingo как есть. `eval` считает его выражением и
		// отдаёт значение, `lingo` — оператором.
		if (!g_debugger)
			return "консоль недоступна";
		Common::String code(line.c_str() + cmd.size());
		code.trim();
		if (code.empty())
			return "нечего выполнять";
		return g_debugger->evalLingo(code, cmd.equalsIgnoreCase("eval"));
	}

	if (cmd.equalsIgnoreCase("sound"))
		return cmdSound();

	if (cmd.equalsIgnoreCase("castinfo"))
		return cmdCastInfo(tok.nextToken());

	if (cmd.equalsIgnoreCase("shot")) {
		// Путь берём остатком строки, а не токеном: в нём бывают пробелы.
		Common::String path(line.c_str() + 4);
		path.trim();
		return cmdShot(path.empty() ? Common::String("bridge-shot.png") : path);
	}

	if (cmd.equalsIgnoreCase("event")) {
		int ch = atoi(tok.nextToken().c_str());
		return cmdEvent(ch, tok.nextToken());
	}

	if (cmd.equalsIgnoreCase("move")) {
		int x = atoi(tok.nextToken().c_str());
		int y = atoi(tok.nextToken().c_str());
		pushMouse(Common::EVENT_MOUSEMOVE, x, y);
		return Common::String::format("мышь в %d,%d", x, y);
	}

	if (cmd.equalsIgnoreCase("down") || cmd.equalsIgnoreCase("up")) {
		int x = atoi(tok.nextToken().c_str());
		int y = atoi(tok.nextToken().c_str());
		pushMouse(Common::EVENT_MOUSEMOVE, x, y);
		pushMouse(cmd.equalsIgnoreCase("down") ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP, x, y);
		return Common::String::format("%s в %d,%d", cmd.c_str(), x, y);
	}

	if (cmd.equalsIgnoreCase("click")) {
		int x = atoi(tok.nextToken().c_str());
		int y = atoi(tok.nextToken().c_str());
		Common::String holdStr = tok.nextToken();
		int hold = holdStr.empty() ? 250 : atoi(holdStr.c_str());

		pushMouse(Common::EVENT_MOUSEMOVE, x, y);
		pushMouse(Common::EVENT_LBUTTONDOWN, x, y);
		schedule(g_system->getMillis() + hold, Common::EVENT_LBUTTONUP, x, y);
		return Common::String::format("клик в %d,%d, отпускание через %d мс", x, y, hold);
	}

	if (cmd.equalsIgnoreCase("drag")) {
		// Перетаскивание: нажать, провести мышь по прямой, отпустить. Игра ловит
		// перетаскивание циклом `repeat while the stilldown`, который читает
		// положение мыши, — одним нажатием и отпусканием его не пройти.
		int x1 = atoi(tok.nextToken().c_str());
		int y1 = atoi(tok.nextToken().c_str());
		int x2 = atoi(tok.nextToken().c_str());
		int y2 = atoi(tok.nextToken().c_str());
		Common::String durStr = tok.nextToken();
		int dur = durStr.empty() ? 1200 : atoi(durStr.c_str());
		const int steps = 12;

		pushMouse(Common::EVENT_MOUSEMOVE, x1, y1);
		pushMouse(Common::EVENT_LBUTTONDOWN, x1, y1);

		uint32 now = g_system->getMillis();
		for (int i = 1; i <= steps; i++) {
			schedule(now + dur * i / (steps + 1), Common::EVENT_MOUSEMOVE,
					x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
		}
		schedule(now + dur, Common::EVENT_MOUSEMOVE, x2, y2);
		schedule(now + dur + 150, Common::EVENT_LBUTTONUP, x2, y2);
		return Common::String::format("перетаскивание (%d,%d) -> (%d,%d) за %d мс", x1, y1, x2, y2, dur);
	}

	if (cmd.equalsIgnoreCase("key")) {
		int code = atoi(tok.nextToken().c_str());
		Common::Event ev;
		ev.type = Common::EVENT_KEYDOWN;
		ev.kbd = Common::KeyState((Common::KeyCode)code, code);
		g_system->getEventManager()->pushEvent(ev);
		ev.type = Common::EVENT_KEYUP;
		g_system->getEventManager()->pushEvent(ev);
		return Common::String::format("клавиша %d", code);
	}

	if (cmd.equalsIgnoreCase("help")) {
		return "Команды моста:\n"
			   "  ping | state | sprites [all] | hit X Y | behaviors CH | castinfo [LIB:]MEM | sound\n"
			   "  shot ПУТЬ | event CH ИМЯ | move X Y | down X Y | up X Y | click X Y [мс]\n"
			   "  drag X1 Y1 X2 Y2 [мс] | key КОД\n"
			   "  eval ВЫРАЖЕНИЕ | lingo ОПЕРАТОР — код Lingo как есть, без разбора командной строки\n"
			   "Всё прочее уходит в консоль ScummVM: help, channels, cast, print ВЫРАЖЕНИЕ,\n"
			   "funcs, var, bt, disasm, bpset, markers, debugflag_enable ...\n";
	}

	// Всё остальное — команда консоли ScummVM. Там уже есть print (вычисление
	// Lingo-выражения), channels, cast, funcs, breakpoints и прочее; выдумывать
	// им замену незачем.
	if (!g_debugger)
		return "консоль недоступна";

	Common::String out;
	g_debugger->runCapturedCommand(line, out);
	if (out.empty())
		out = "(пусто)";
	return out;
}

} // End of namespace Director
