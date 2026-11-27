// GeneralsX @feature fnc12 27/11/2026 Map objects owned by a start position: which originalOwner
// names one (Core/GameEngine/Include/Common/StartPositionOwner.h).
#include <catch2/catch_all.hpp>

#include "Common/StartPositionOwner.h"

TEST_CASE("StartPositionOwner: teamStart1..teamStart8 are start positions 0..7")
{
	CHECK(StartPositionOwner::parse("teamStart1") == 0);
	CHECK(StartPositionOwner::parse("teamStart2") == 1);
	CHECK(StartPositionOwner::parse("teamStart8") == 7);
}

TEST_CASE("StartPositionOwner: every other owner is no start position")
{
	for (const char *owner : {"", "team", "teamplayer0", "teamStart", "teamStart0", "teamStart9", "teamStart10",
			 "teamStart1 ", "teamstart1", "TeamStart1", "teamStartA", "xteamStart1", "teamCivilian"})
	{
		INFO(owner);
		CHECK(StartPositionOwner::parse(owner) == -1);
	}
	CHECK(StartPositionOwner::parse(nullptr) == -1);
}
