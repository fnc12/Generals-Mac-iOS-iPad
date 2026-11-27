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

// GeneralsX @feature fnc12 27/11/2026 Map objects owned by a start position (END Editor).
//
// A multiplayer map's SidesList has no players of the game: they are made when the game starts,
// one per lobby slot. The END Editor lets a map object be owned by a start position instead: its
// originalOwner is "teamStart<N>" (N = 1..8, the Player_<N>_Start waypoint). When the map loads,
// such an object goes to the default team of the player who starts at start position N, and is
// not created at all when nobody starts there (GameLogic::startNewGame). A team of that name the
// map has itself wins, so maps without these owners load exactly as before.
// Standalone (no engine types): Tests/Catch2/start_position_owner_tests.cpp.

#pragma once

namespace StartPositionOwner
{
	// The 0-based start position of an originalOwner "teamStart<N>" (N 1..8), or -1 for any other
	// owner.
	inline int parse(const char *owner)
	{
		if (owner == nullptr)
			return -1;
		static const char prefix[] = "teamStart";
		for (int i = 0; prefix[i] != '\0'; ++i)
		{
			if (owner[i] != prefix[i])
				return -1;
		}
		const char *number = owner + sizeof(prefix) - 1;
		if (number[0] < '1' || number[0] > '8' || number[1] != '\0')
			return -1;
		return number[0] - '1';
	}
}
