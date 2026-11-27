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

// GeneralsX @feature fnc12 27/11/2026 -testMap: start a map straight from the command line (see TestMap.h).

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/TestMap.h"

#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/RandomValue.h"
#include "GameClient/GameClient.h"
#include "GameClient/MapUtil.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameNetwork/GameInfo.h"

TestMap::Settings TestMap::s_settings;

TestMap::Settings::Settings()
	: faction("FactionAmerica")
	, aiCount(1)
	, aiDifficulty(1)
	, startPosition(1)
	, frames(300)
	, seed(0)
{
}

namespace
{
AsciiString withSeparator(const AsciiString &path, char separator)
{
	AsciiString result;
	for (const char *c = path.str(); *c; ++c)
		result.concat((*c == '/' || *c == '\\') ? separator : *c);
	return result;
}

// The map's cache entry, read again from its file: a map saved over with the same size (an editor
// that moved a start waypoint) would otherwise keep its old start positions and CRC, because the
// cache compares only the file size.
const MapMetaData *findTestMap(AsciiString name)
{
	if (TheMapCache == nullptr)
		return nullptr;

	AsciiString candidates[3];
	Int count = 0;
	if (name.find('/') == nullptr && name.find('\\') == nullptr && !name.endsWithNoCase(".map"))
	{
		// A map name: <user Maps>/<name>/<name>.map
		AsciiString path = TheMapCache->getUserMapDir();
		path.concat(getNativePathSeparator());
		path.concat(name);
		path.concat(getNativePathSeparator());
		path.concat(name);
		path.concat(".map");
		candidates[count++] = path;
	}
	else
	{
		candidates[count++] = name;
		candidates[count++] = withSeparator(name, '/');
		candidates[count++] = withSeparator(name, '\\');
	}

	for (Int i = 0; i < count; ++i)
	{
		AsciiString lower = candidates[i];
		lower.toLower();
		MapCache::iterator it = TheMapCache->find(lower);
		if (it != TheMapCache->end() && !it->second.m_isOfficial)
		{
			TheMapCache->erase(it);
			TheMapCache->updateCache();
		}
		const MapMetaData *md = TheMapCache->findMap(candidates[i]);
		if (md != nullptr)
			return md;
	}
	return nullptr;
}

SlotState aiSlotState(Int difficulty)
{
	switch (difficulty)
	{
	case 0: return SLOT_EASY_AI;
	case 2: return SLOT_BRUTAL_AI;
	default: return SLOT_MED_AI;
	}
}

// The playable faction's PlayerTemplate index, PLAYERTEMPLATE_RANDOM for "Random" or an unknown name.
Int factionIndex(const AsciiString &name)
{
	if (name.isEmpty() || name.compareNoCase("Random") == 0)
		return PLAYERTEMPLATE_RANDOM;
	for (Int i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
	{
		const PlayerTemplate *pt = ThePlayerTemplateStore->getNthPlayerTemplate(i);
		if (pt && pt->isPlayableSide() && pt->getName().compareNoCase(name) == 0)
			return i;
	}
	fprintf(stderr, "TEST_MAP unknown faction '%s', using a random one. Playable factions:", name.str());
	for (Int i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
	{
		const PlayerTemplate *pt = ThePlayerTemplateStore->getNthPlayerTemplate(i);
		if (pt && pt->isPlayableSide())
			fprintf(stderr, " %s", pt->getName().str());
	}
	fprintf(stderr, "\n");
	fflush(stderr);
	return PLAYERTEMPLATE_RANDOM;
}

void appendNewGame(Bool toCommandList, GameMode mode)
{
	GameMessage *msg = toCommandList ? newInstance(GameMessage)(GameMessage::MSG_NEW_GAME)
		: TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
	msg->appendIntegerArgument(mode);
	msg->appendIntegerArgument(DIFFICULTY_NORMAL);
	msg->appendIntegerArgument(0);
	if (toCommandList)
		TheCommandList->appendMessage(msg);
}
} // namespace

Bool TestMap::start(Bool toCommandList)
{
	const Settings &settings = s_settings;
	const MapMetaData *md = findTestMap(settings.map);
	if (md == nullptr)
	{
		fprintf(stderr, "TEST_MAP error: map '%s' not found (user maps: %s)\n", settings.map.str(),
			TheMapCache ? TheMapCache->getUserMapDir().str() : "?");
		fflush(stderr);
		return FALSE;
	}

	if (TheGameLogic->isInGame())
		TheGameLogic->clearGameData(FALSE);

	if (!md->m_isMultiplayer)
	{
		// A mission: the map's own players and scripts, like -file.
		fprintf(stderr, "TEST_MAP start map='%s' mode=single_player\n", md->m_fileName.str());
		fflush(stderr);
		TheWritableGlobalData->m_pendingFile = md->m_fileName;
		InitRandom(settings.seed);
		appendNewGame(toCommandList, GAME_SINGLE_PLAYER);
		return TRUE;
	}

	if (TheSkirmishGameInfo == nullptr)
		TheSkirmishGameInfo = NEW SkirmishGameInfo;
	else if (TheSkirmishGameInfo->isInGame())
		TheSkirmishGameInfo->endGame();
	TheSkirmishGameInfo->init();
	TheSkirmishGameInfo->clearSlotList();
	TheSkirmishGameInfo->reset();
	TheSkirmishGameInfo->setLocalIP(TheSkirmishGameInfo->getSlot(0)->getIP());
	TheSkirmishGameInfo->enterGame();

	const Int positions = md->m_numPlayers;
	Int startPosition = settings.startPosition - 1;
	if (startPosition < 0 || startPosition >= positions)
		startPosition = -1;
	Int aiCount = settings.aiCount;
	if (aiCount < 0)
		aiCount = 0;
	if (aiCount > positions - 1)
		aiCount = positions - 1;
	if (aiCount > MAX_SLOTS - 1)
		aiCount = MAX_SLOTS - 1;

	GameSlot local;
	local.setState(SLOT_PLAYER, UnicodeString(L"Player"));
	local.setPlayerTemplate(factionIndex(settings.faction));
	local.setStartPos(startPosition);
	local.setColor(-1);
	TheSkirmishGameInfo->setSlot(0, local);
	for (Int i = 1; i < MAX_SLOTS; ++i)
	{
		GameSlot slot;
		if (i <= aiCount)
		{
			slot.setState(aiSlotState(settings.aiDifficulty));
			slot.setPlayerTemplate(PLAYERTEMPLATE_RANDOM);
			slot.setStartPos(-1);
			slot.setColor(-1);
		}
		else
		{
			slot.setState(SLOT_CLOSED);
		}
		TheSkirmishGameInfo->setSlot(i, slot);
	}

	TheSkirmishGameInfo->setMap(md->m_fileName);
	TheSkirmishGameInfo->setMapCRC(md->m_CRC);
	TheSkirmishGameInfo->setMapSize(md->m_filesize);
	TheSkirmishGameInfo->setSeed(settings.seed != 0 ? settings.seed : GetTickCount());
	TheSkirmishGameInfo->setSuperweaponRestriction(0);
	TheWritableGlobalData->m_mapName = md->m_fileName;
	TheSkirmishGameInfo->startGame(0);

	fprintf(stderr, "TEST_MAP start map='%s' mode=skirmish positions=%d faction=%s start=%d ai=%d difficulty=%d\n",
		md->m_fileName.str(), positions, settings.faction.str(), startPosition + 1, aiCount, settings.aiDifficulty);
	fflush(stderr);

	InitRandom(TheSkirmishGameInfo->getSeed());
	appendNewGame(toCommandList, GAME_SKIRMISH);
	return TRUE;
}

void TestMap::startQueued()
{
	if (!isRequested())
		return;
	start(FALSE);
	s_settings.map.clear();
}

Int TestMap::runHeadless()
{
	if (!start(TRUE))
		return 1;
	s_settings.map.clear();

	const UnsignedInt frames = s_settings.frames > 0 ? static_cast<UnsignedInt>(s_settings.frames) : 300u;
	// The first updates run the new game message, which loads the map, and its first frame.
	for (Int i = 0; i < 10 && TheGameLogic->getFrame() < 1 && !TheGameEngine->getQuitting(); ++i)
	{
		TheGameClient->updateHeadless();
		TheGameLogic->UPDATE();
	}
	if (!TheGameLogic->isInGame() || TheGameLogic->isInShellGame() || TheGameLogic->getFrame() < 1)
	{
		fprintf(stderr, "TEST_MAP error: the map did not start\n");
		fflush(stderr);
		return 1;
	}
	// The objects of the map itself (and the players' starting buildings and units).
	Int loadedObjects = 0;
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
		++loadedObjects;
	while (TheGameLogic->getFrame() < frames && !TheGameEngine->getQuitting())
	{
		TheGameClient->updateHeadless();
		TheGameLogic->UPDATE();
	}

	Player *localPlayer = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	Int objects = 0;
	Int localObjects = 0;
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		++objects;
		if (localPlayer && obj->getControllingPlayer() == localPlayer)
			++localObjects;
	}
	const Bool skirmish = TheGameLogic->getGameMode() == GAME_SKIRMISH;
	const PlayerTemplate *pt = localPlayer ? localPlayer->getPlayerTemplate() : nullptr;
	// A mission may have no objects at all (a cinematic); a skirmish gives the local player its start.
	const Bool ok = TheGameLogic->getFrame() >= frames && (!skirmish || localObjects > 0);
	fprintf(stderr, "TEST_MAP %s frames=%u mode=%s players=%d loaded_objects=%d objects=%d local_objects=%d local_faction=%s\n",
		ok ? "ok" : "error", TheGameLogic->getFrame(), skirmish ? "skirmish" : "single_player",
		ThePlayerList ? ThePlayerList->getPlayerCount() : 0, loadedObjects, objects, localObjects,
		pt ? pt->getName().str() : "-");
	fflush(stderr);
	return ok ? 0 : 1;
}
