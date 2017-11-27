/*
 * CGameStateTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "mock/mock_MapService.h"
#include "mock/mock_IGameCallback.h"

#include "../../lib/VCMIDirs.h"
#include "../../lib/CGameState.h"
#include "../../lib/NetPacks.h"
#include "../../lib/StartInfo.h"

#include "../../lib/battle/BattleInfo.h"

#include "../../lib/filesystem/ResourceID.h"

#include "../../lib/mapping/CMap.h"

#include "../../lib/spells/CSpellHandler.h"
#include "../../lib/spells/ISpellMechanics.h"

class CGameStateTest : public ::testing::Test, public SpellCastEnvironment, public MapListener
{
public:
	CGameStateTest()
		: gameCallback(new GameCallbackMock(this)),
		mapService("test/MiniTest/", this),
		map(nullptr)
	{
		IObjectInterface::cb = gameCallback.get();
	}

	virtual ~CGameStateTest()
	{
		IObjectInterface::cb = nullptr;
	}

	void sendAndApply(CPackForClient * info) const override
	{
		gameState->apply(info);
	}

	void complain(const std::string & problem) const
	{
		FAIL() << "Server-side assertion:" << problem;
	};

	CRandomGenerator & getRandomGenerator() const override
	{
		return gameState->getRandomGenerator();//todo: mock this
	}

	const CMap * getMap() const override
	{
		return map;
	}
	const CGameInfoCallback * getCb() const override
	{
		return gameState.get();
	}

	bool moveHero(ObjectInstanceID hid, int3 dst, bool teleporting) const override
	{
		return false;
	}

	void genericQuery(Query * request, PlayerColor color, std::function<void(const JsonNode &)> callback) const
	{
		//todo:
	}

	void mapLoaded(CMap * map) override
	{
		EXPECT_EQ(this->map, nullptr);
		this->map = map;
	}

	std::shared_ptr<CGameState> gameState;

	std::shared_ptr<GameCallbackMock> gameCallback;

	MapServiceMock mapService;

	CMap * map;
};

//Issue #2765, Ghost Dragons can cast Age on Catapults
TEST_F(CGameStateTest, issue2765)
{
	StartInfo si;
	si.mapname = "anything";//does not matter, map service mocked
	si.difficulty = 0;
	si.mapfileChecksum = 0;
	si.mode = StartInfo::NEW_GAME;
	si.seedToBeUsed = 42;

	std::unique_ptr<CMapHeader> header = mapService.loadMapHeader(ResourceID(si.mapname));

	ASSERT_NE(header.get(), nullptr);

	//FIXME: this has been copied from CPreGame, but should be part of StartInfo
	for(int i = 0; i < header->players.size(); i++)
	{
		const PlayerInfo & pinfo = header->players[i];

		//neither computer nor human can play - no player
		if (!(pinfo.canHumanPlay || pinfo.canComputerPlay))
			continue;

		PlayerSettings & pset = si.playerInfos[PlayerColor(i)];
		pset.color = PlayerColor(i);
		pset.playerID = i;
		pset.name = "Player";

		pset.castle = pinfo.defaultCastle();
		pset.hero = pinfo.defaultHero();

		if(pset.hero != PlayerSettings::RANDOM && pinfo.hasCustomMainHero())
		{
			pset.hero = pinfo.mainCustomHeroId;
			pset.heroName = pinfo.mainCustomHeroName;
			pset.heroPortrait = pinfo.mainCustomHeroPortrait;
		}

		pset.handicap = PlayerSettings::NO_HANDICAP;
	}

	gameState = std::make_shared<CGameState>();
	gameCallback->setGameState(gameState.get());
	gameState->init(&mapService, &si, false);

	ASSERT_NE(map, nullptr);
	ASSERT_EQ(map->heroesOnMap.size(), 2);

	CGHeroInstance * attacker = map->heroesOnMap[0];
	CGHeroInstance * defender = map->heroesOnMap[1];

	ASSERT_NE(attacker->tempOwner, defender->tempOwner);

	{
		CArtifactInstance * a = new CArtifactInstance();
		a->artType = const_cast<CArtifact *>(ArtifactID(ArtifactID::BALLISTA).toArtifact());

		NewArtifact na;
		na.art = a;
		gameCallback->sendAndApply(&na);

		PutArtifact pack;
		pack.al = ArtifactLocation(defender, ArtifactPosition::MACH1);
		pack.art = a;
		gameCallback->sendAndApply(&pack);
	}


	const CGHeroInstance * heroes[2] = {attacker, defender};
    const CArmedInstance * armedInstancies[2] = {attacker, defender};

	int3 tile(4,4,0);

	const auto t = gameCallback->getTile(tile);

	ETerrainType terrain = t->terType;
	BFieldType terType = BFieldType::GRASS_HILLS;

	//send info about battles

	BattleInfo * battle = BattleInfo::setupBattle(tile, terrain, terType, armedInstancies, heroes, false, nullptr);

	BattleStart bs;
	bs.info = battle;
	ASSERT_EQ(gameState->curB, nullptr);
	gameCallback->sendAndApply(&bs);
	ASSERT_EQ(gameState->curB, battle);
	{
		BattleStackAdded bsa;
		bsa.newStackID = battle->battleNextUnitId();
		bsa.creID = CreatureID(69);
		bsa.side = BattleSide::ATTACKER;
		bsa.summoned = false;
		bsa.pos = battle->getAvaliableHex(bsa.creID, bsa.side);
		bsa.amount = 1;
		gameCallback->sendAndApply(&bsa);
	}

	const CStack * att = nullptr;
	const CStack * def = nullptr;

	for(const CStack * s : battle->stacks)
	{
		if(s->type->idNumber == CreatureID::BALLISTA)
			def = s;
		else if(s->type->idNumber == CreatureID(69))
			att = s;
	}
	ASSERT_NE(att, nullptr);
	ASSERT_NE(def, nullptr);

	ASSERT_EQ(att->getMyHero(), attacker);
	ASSERT_EQ(def->getMyHero(), defender);

	{
		const CSpell * age = SpellID(SpellID::AGE).toSpell();
		ASSERT_NE(age, nullptr);
		//here tested ballista, but this applied to all war machines
		spells::BattleCast cast(battle, att, spells::Mode::AFTER_ATTACK, age);
		cast.aimToUnit(def);
		cast.setSpellLevel(3);

		EXPECT_FALSE(age->canBeCastAt(battle, spells::Mode::AFTER_ATTACK, att, def->getPosition()));

		EXPECT_TRUE(cast.castIfPossible(this));//should be possible, but with no effect (change to aimed cast check?)

		EXPECT_TRUE(def->activeSpells().empty());
	}

}

