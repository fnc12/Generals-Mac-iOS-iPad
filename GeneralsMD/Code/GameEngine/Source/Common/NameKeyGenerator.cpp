/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: NameKeyGenerator.cpp /////////////////////////////////////////////////////////////////////
// Created:   Michael Booth, May 2001
//						Colin Day, May 2001
// Desc:      Name key system to translate between names and unique key ids
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <algorithm>
#include <vector>

// Public Data ////////////////////////////////////////////////////////////////////////////////////
NameKeyGenerator *TheNameKeyGenerator = nullptr;  ///< name key gen. singleton

//-------------------------------------------------------------------------------------------------
NameKeyGenerator::NameKeyGenerator()
{

	m_nextID = (UnsignedInt)NAMEKEY_INVALID;  // uninitialized system

	for (Int i = 0; i < SOCKET_COUNT; ++i)
		m_sockets[i] = nullptr;

}

//-------------------------------------------------------------------------------------------------
NameKeyGenerator::~NameKeyGenerator()
{

	// free all system data
	freeSockets();

}

//-------------------------------------------------------------------------------------------------
void NameKeyGenerator::init()
{
	DEBUG_ASSERTCRASH(m_nextID == (UnsignedInt)NAMEKEY_INVALID, ("NameKeyGen already inited"));

	// start keys at the beginning again
	freeSockets();
	m_nextID = 1;

}

//-------------------------------------------------------------------------------------------------
void NameKeyGenerator::reset()
{
	freeSockets();
	m_nextID = 1;

}

//-------------------------------------------------------------------------------------------------
void NameKeyGenerator::freeSockets()
{
	for (Int i = 0; i < SOCKET_COUNT; ++i)
	{
		Bucket *next;
		for (Bucket *b = m_sockets[i]; b; b = next)
		{
			next = b->m_nextInSocket;
			deleteInstance(b);
		}
		m_sockets[i] = nullptr;
	}

}

//-------------------------------------------------------------------------------------------------
inline UnsignedInt calcHashForString(const char* p)
{
	UnsignedInt result = 0;
	Byte *pp = (Byte*)p;
	while (*pp)
		result = (result << 5) + result + *pp++;
	return result;
}

//-------------------------------------------------------------------------------------------------
inline UnsignedInt calcHashForLowercaseString(const char* p)
{
	UnsignedInt result = 0;
	Byte *pp = (Byte*)p;
	while (*pp)
		result = (result << 5) + result + tolower(*pp++);
	return result;
}

//-------------------------------------------------------------------------------------------------
AsciiString NameKeyGenerator::keyToName(NameKeyType key)
{
	for (Int i = 0; i < SOCKET_COUNT; ++i)
	{
		for (Bucket *b = m_sockets[i]; b; b = b->m_nextInSocket)
		{
			if (key == b->m_key)
				return b->m_nameString;
		}
	}
	return AsciiString::TheEmptyString;
}

//-------------------------------------------------------------------------------------------------
#if RETAIL_COMPATIBLE_CRC
#if RTS_ZEROHOUR
// TheSuperHackers @bugfix Caball009 24/02/2026 Originally the game would hash three files
// for the file exist cache of the file system in Zero Hour.
// TheScienceStore and TheUpgradeCenter rely on having the exact same name key IDs across all clients.
// That means that we still need to hash 3 dummy strings to keep the name key IDs synchronized with retail.
void NameKeyGenerator::syncNameKeyID()
{
	nameToLowercaseKey("Data\\English\\Language9x.ini");
	nameToLowercaseKey("Data\\Audio\\Tracks\\English\\GLA_02.mp3");
	nameToLowercaseKey("Data\\Audio\\Tracks\\GLA_02.mp3");
}
#endif
void NameKeyGenerator::verifyNameKeyID(UnsignedInt expectedNextID) const
{
	// this should only be called before the initialization of TheScienceStore and TheUpgradeCenter in GameEngine::init
	DEBUG_ASSERTCRASH(expectedNextID == m_nextID,
		("Retail client expects items to start with name key ID %d for unmodded files, but starts with %d", expectedNextID, m_nextID));
}
#endif

//-------------------------------------------------------------------------------------------------
NameKeyType NameKeyGenerator::nameToKey(const AsciiString& name)
{
	const UnsignedInt hash = calcHashForString(name.str()) % SOCKET_COUNT;

	// do we have it already?
	const Bucket *b;
	for (b = m_sockets[hash]; b; b = b->m_nextInSocket)
	{
		if (name.compare(b->m_nameString) == 0)
			return b->m_key;
	}

	// nope, guess not. let's allocate it.
	return createNameKey(hash, name);
}

//-------------------------------------------------------------------------------------------------
NameKeyType NameKeyGenerator::nameToLowercaseKey(const AsciiString& name)
{
	const UnsignedInt hash = calcHashForLowercaseString(name.str()) % SOCKET_COUNT;

	// do we have it already?
	const Bucket *b;
	for (b = m_sockets[hash]; b; b = b->m_nextInSocket)
	{
		if (name.compareNoCase(b->m_nameString) == 0)
			return b->m_key;
	}

	// nope, guess not. let's allocate it.
	// TheSuperHackers @fix CryoTheRenegade 08/08/2026 Store the canonical lowercase name
	// so a regular lowercase lookup reuses this key.
	AsciiString lowercaseName = name;
	lowercaseName.toLower();
	return createNameKey(hash, lowercaseName);
}

//-------------------------------------------------------------------------------------------------
NameKeyType NameKeyGenerator::nameToKey(const char* name)
{
	const UnsignedInt hash = calcHashForString(name) % SOCKET_COUNT;

	// do we have it already?
	const Bucket *b;
	for (b = m_sockets[hash]; b; b = b->m_nextInSocket)
	{
		if (strcmp(name, b->m_nameString.str()) == 0)
			return b->m_key;
	}

	// nope, guess not. let's allocate it.
	return createNameKey(hash, name);
}

//-------------------------------------------------------------------------------------------------
NameKeyType NameKeyGenerator::nameToLowercaseKey(const char *name)
{
	const UnsignedInt hash = calcHashForLowercaseString(name) % SOCKET_COUNT;

	// do we have it already?
	const Bucket *b;
	for (b = m_sockets[hash]; b; b = b->m_nextInSocket)
	{
		if (_stricmp(name, b->m_nameString.str()) == 0)
			return b->m_key;
	}

	// nope, guess not. let's allocate it.
	// TheSuperHackers @fix CryoTheRenegade 08/08/2026 Store the canonical lowercase name
	// so a regular lowercase lookup reuses this key.
	AsciiString lowercaseName(name);
	lowercaseName.toLower();
	return createNameKey(hash, lowercaseName);
}

//-------------------------------------------------------------------------------------------------
NameKeyType NameKeyGenerator::createNameKey(UnsignedInt hash, const AsciiString& name)
{
	Bucket *b = newInstance(Bucket);
	b->m_key = (NameKeyType)m_nextID++;
	b->m_nameString = name;
	b->m_nextInSocket = m_sockets[hash];
	m_sockets[hash] = b;

	NameKeyType result = b->m_key;

#if defined(RTS_DEBUG)
	// reality-check to be sure our hasher isn't going bad.
	const Int maxThresh = 3;
	Int numOverThresh = 0;
	for (Int i = 0; i < SOCKET_COUNT; ++i)
	{
		Int numInThisSocket = 0;
		for (b = m_sockets[i]; b; b = b->m_nextInSocket)
			++numInThisSocket;

		if (numInThisSocket > maxThresh)
			++numOverThresh;
	}

	// if more than a small percent of the sockets are getting deep, probably want to increase the socket count.
	if (numOverThresh > SOCKET_COUNT/20)
	{
		DEBUG_CRASH(("hmm, might need to increase the number of bucket-sockets for NameKeyGenerator (numOverThresh %d = %f%%)",numOverThresh,(Real)numOverThresh/(Real)(SOCKET_COUNT/20)));
	}
#endif

	return result;
}

//-------------------------------------------------------------------------------------------------
void NameKeyGenerator::reportNameKeys(const char* stage) const
{
	const char* dumpDir = getenv("GENERALSX_NAMEKEY_DUMP");
	if (dumpDir == nullptr || dumpDir[0] == '\0')
		return;

	std::vector<const Bucket*> buckets;
	for (Int i = 0; i < SOCKET_COUNT; ++i)
	{
		for (const Bucket *b = m_sockets[i]; b; b = b->m_nextInSocket)
			buckets.push_back(b);
	}
	std::sort(buckets.begin(), buckets.end(), [](const Bucket* a, const Bucket* b) { return a->m_key < b->m_key; });

	AsciiString fileName;
	FILE* file = nullptr;
	if (strcmp(dumpDir, "1") != 0)
	{
		fileName.format("%s/namekeys-%s.txt", dumpDir, stage);
		file = fopen(fileName.str(), "w");
	}

	// FNV-1a over "key name\n" lines in key order.
	UnsignedInt hash = 2166136261u;
	Int player0 = NAMEKEY_INVALID;
	char line[1024];
	for (const Bucket* b : buckets)
	{
		const Int len = snprintf(line, sizeof(line), "%d %s\n", (Int)b->m_key, b->m_nameString.str());
		for (Int i = 0; i < len && i < (Int)sizeof(line) - 1; ++i)
			hash = (hash ^ (UnsignedByte)line[i]) * 16777619u;
		if (file)
			fputs(line, file);
		if (b->m_nameString.compare("player0") == 0)
			player0 = b->m_key;
	}
	if (file)
		fclose(file);

	fprintf(stderr, "[GeneralsX] NAMEKEY_TABLE stage=%s count=%d next=%u hash=0x%08X player0=%d\n",
		stage, (Int)buckets.size(), m_nextID, hash, player0);
	fflush(stderr);
}

//-------------------------------------------------------------------------------------------------
// Get a string out of the INI. Store it into a NameKeyType
//-------------------------------------------------------------------------------------------------
void NameKeyGenerator::parseStringAsNameKeyType( INI *ini, void *instance, void *store, const void* userData )
{
  *(NameKeyType *)store = TheNameKeyGenerator->nameToKey( ini->getNextToken() );
}


//-------------------------------------------------------------------------------------------------
NameKeyType StaticNameKey::key() const
{
	if (m_key == NAMEKEY_INVALID)
	{
		DEBUG_ASSERTCRASH(TheNameKeyGenerator, ("no TheNameKeyGenerator yet"));
		if (TheNameKeyGenerator)
			m_key = TheNameKeyGenerator->nameToKey(m_name);
	}
	return m_key;
}
