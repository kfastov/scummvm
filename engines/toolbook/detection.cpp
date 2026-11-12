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

#include "base/plugins.h"

#include "engines/advancedDetector.h"
#include "engines/game.h"

static const PlainGameDescriptor toolbookGames[] = {
	{ "knowledgetower", "\xd0\x91\xd0\xb0\xd1\x88\xd0\xbd\xd1\x8f \xd0\xb7\xd0\xbd\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb9" }, // Башня знаний
	{ nullptr, nullptr }
};

static const DebugChannelDef debugFlagList[] = {
	{ 1, "general", "General debug level" },
	{ 2, "book", "Book parsing" },
	DEBUG_CHANNEL_END
};

namespace ToolBook {

static const ADGameDescription gameDescriptions[] = {
	// «Башня знаний», New Media Generation, 1996.
	// Книга ToolBook зашита в 16-битный KNTOWER.EXE со смещения 0x29e0.
	{
		"knowledgetower",
		nullptr,
		AD_ENTRY1s("KNTOWER.EXE", "bfddff378e8c7877c5e0c5b0d5567a41", 38831653),
		Common::RU_RUS,
		Common::kPlatformWindows,
		ADGF_UNSTABLE,
		GUIO1(GUIO_NOMIDI)
	},

	AD_TABLE_END_MARKER
};

} // End of namespace ToolBook

class ToolBookMetaEngineDetection : public AdvancedMetaEngineDetection<ADGameDescription> {
public:
	ToolBookMetaEngineDetection() : AdvancedMetaEngineDetection(ToolBook::gameDescriptions, toolbookGames) {
	}

	const char *getName() const override {
		return "toolbook";
	}

	const char *getEngineName() const override {
		return "Asymetrix ToolBook";
	}

	const char *getOriginalCopyright() const override {
		return "Multimedia ToolBook (C) Asymetrix Corporation";
	}

	const DebugChannelDef *getDebugChannels() const override {
		return debugFlagList;
	}
};

REGISTER_PLUGIN_STATIC(TOOLBOOK_DETECTION, PLUGIN_TYPE_ENGINE_DETECTION, ToolBookMetaEngineDetection);
