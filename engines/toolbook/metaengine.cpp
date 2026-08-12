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

#include "toolbook/toolbook.h"

namespace ToolBook {

const char *ToolBookEngine::getGameId() const { return _desc->gameId; }

} // End of namespace ToolBook

class ToolBookMetaEngine : public AdvancedMetaEngine<ADGameDescription> {
	const char *getName() const override {
		return "toolbook";
	}

	Common::Error createInstance(OSystem *syst, Engine **engine, const ADGameDescription *desc) const override {
		*engine = new ToolBook::ToolBookEngine(syst, desc);
		return Common::kNoError;
	}

	bool hasFeature(MetaEngineFeature f) const override {
		return false;
	}
};

#if PLUGIN_ENABLED_DYNAMIC(TOOLBOOK)
	REGISTER_PLUGIN_DYNAMIC(TOOLBOOK, PLUGIN_TYPE_ENGINE, ToolBookMetaEngine);
#else
	REGISTER_PLUGIN_STATIC(TOOLBOOK, PLUGIN_TYPE_ENGINE, ToolBookMetaEngine);
#endif
