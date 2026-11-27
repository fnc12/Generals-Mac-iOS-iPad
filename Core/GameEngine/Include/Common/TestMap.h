/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "Common/AsciiString.h"

// GeneralsX @feature fnc12 27/11/2026 Start a map straight from the command line, like the
// World Editor's "Test Map" (used by the END Editor's Play button):
//   -testMap <map>                 a .map file (absolute, or relative like Maps\X\X.map), or the
//                                  name of a map in the user Maps folder (<name>\<name>.map)
//   -testMapFaction <name|Random>  the local player's PlayerTemplate (default FactionAmerica)
//   -testMapAI <count>             AI opponents (default 1, at most the map's start positions - 1)
//   -testMapAIDifficulty <easy|medium|hard>  (default medium)
//   -testMapStart <n>              the local player's start position, 1-based; 0 = random (default 1)
//   -testMapFrames <n>             with -headless: run n logic frames, then exit (default 300)
//   -testMapSeed <n>               the game's random seed (default: the clock), for repeatable runs
// A map with two or more start positions is started as a skirmish (the local player and the AI
// opponents at start positions, the default starting cash); any other map as a single player
// mission, like -file does. No intro, shell map or main menu is shown before it. In -headless mode
// the result is printed as one "TEST_MAP ..." line and the exit code is 0 only when the map
// loaded and ran all frames, and in a skirmish the local player owns objects.
class TestMap
{
public:
	struct Settings
	{
		Settings();

		AsciiString map;
		AsciiString faction;
		Int aiCount;
		Int aiDifficulty; // 0 easy, 1 medium, 2 hard
		Int startPosition; // 1-based, 0 random
		Int frames;
		Int seed; // 0: the clock
	};

	static Settings s_settings;

	static Bool isRequested() { return s_settings.map.isNotEmpty(); }

	// Queues the new game: through TheMessageStream when the client runs, straight to
	// TheCommandList in -headless mode (the message stream is not processed there).
	// Returns FALSE (and prints why) when the map cannot be started.
	static Bool start(Bool toCommandList);

	// -headless: starts the map, runs the frames and returns the exit code.
	static Int runHeadless();

	// Called by the client once the intro is over (like -loadsave and -loadreplay).
	static void startQueued();
};
