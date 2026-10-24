#include "CvGameCoreDLLPCH.h"
#include "McpToolSupport.h"
#include "McpToolHandlers.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvUnit.h"
#include "CvMinorCivAI.h"
#include "CvGlobals.h"

using namespace McpTool;

// City-states: standing, quests and the actions the diplomacy screen offers.

namespace
{
	const char* QuestName(MinorCivQuestTypes e)
	{
		switch(e)
		{
		case MINOR_CIV_QUEST_ROUTE: return "build a road to them";
		case MINOR_CIV_QUEST_KILL_CAMP: return "destroy a barbarian camp";
		case MINOR_CIV_QUEST_CONNECT_RESOURCE: return "connect a resource";
		case MINOR_CIV_QUEST_CONSTRUCT_WONDER: return "build a wonder";
		case MINOR_CIV_QUEST_GREAT_PERSON: return "earn a great person";
		case MINOR_CIV_QUEST_KILL_CITY_STATE: return "destroy a city-state";
		case MINOR_CIV_QUEST_FIND_PLAYER: return "find a civilization";
		case MINOR_CIV_QUEST_FIND_NATURAL_WONDER: return "find a natural wonder";
		case MINOR_CIV_QUEST_GIVE_GOLD: return "give gold";
		case MINOR_CIV_QUEST_PLEDGE_TO_PROTECT: return "pledge to protect them";
		case MINOR_CIV_QUEST_CONTEST_CULTURE: return "culture contest";
		case MINOR_CIV_QUEST_CONTEST_FAITH: return "faith contest";
		case MINOR_CIV_QUEST_CONTEST_TECHS: return "technology contest";
		case MINOR_CIV_QUEST_INVEST: return "invest gold";
		case MINOR_CIV_QUEST_BULLY_CITY_STATE: return "bully a city-state";
		case MINOR_CIV_QUEST_DENOUNCE_MAJOR: return "denounce a civilization";
		case MINOR_CIV_QUEST_SPREAD_RELIGION: return "spread your religion to them";
		case MINOR_CIV_QUEST_TRADE_ROUTE: return "send a trade route";
		default: return "quest";
		}
	}

	CvPlayer* MinorFromArgs(CvPlayer* me, const JObj& args, std::string& err)
	{
		std::string key = ArgStr(args, "civ");
		PlayerTypes e = FindPlayer(key);
		if(e == NO_PLAYER || !GET_PLAYER(e).isMinorCiv()) { err = "civ must name a city-state (see civs)"; return NULL; }
		CvPlayer& minor = GET_PLAYER(e);
		if(!minor.isAlive()) { err = CivName(e) + " no longer exists"; return NULL; }
		if(!GET_TEAM(me->getTeam()).isHasMet(minor.getTeam())) { err = "you have not met " + CivName(e); return NULL; }
		return &minor;
	}

	bool AtWar(CvPlayer* me, CvPlayer& minor) { return GET_TEAM(me->getTeam()).isAtWar(minor.getTeam()); }
	bool AtPeace(CvPlayer* me, CvPlayer& minor) { return !AtWar(me, minor); }
	CvMinorCivAI* MinorAI(CvPlayer& minor) { return minor.GetMinorCivAI(); }

	typedef bool (*MinorCan)(CvPlayer* me, CvPlayer& minor);
	typedef bool (*MinorRun)(CvPlayer* me, CvPlayer& minor, const JObj& args, JObj& out, std::string& err);
	struct MinorAction { const char* name; MinorCan can; MinorRun run; const char* refusal; };

	bool RunGiftGold(CvPlayer* me, CvPlayer& minor, const JObj& args, JObj&, std::string& err)
	{
		int iGold = ArgInt(args, "amount", 0);
		if(iGold <= 0 || iGold > me->GetTreasury()->GetGold()) { err = McpFormat("amount must be between 1 and your gold (%d)", me->GetTreasury()->GetGold()); return false; }
		MinorAI(minor)->DoGoldGiftFromMajor(me->GetID(), iGold);
		return true;
	}
	bool RunGiftUnit(CvPlayer* me, CvPlayer& minor, const JObj& args, JObj&, std::string& err)
	{
		CvUnit* u = RequireUnit(me, args, err);
		if(!u) return false;
		if(u->plot()->getOwner() == minor.GetID() && u->canGift()) u->gift();
		else if(u->CanDistanceGift(minor.GetID())) minor.DoDistanceGift(me->GetID(), u);
		else { err = "that unit cannot be gifted to them (only military units, not already pledged, and they must be willing to receive it)"; return false; }
		return true;
	}
	bool CanBullyGold(CvPlayer* me, CvPlayer& minor) { return AtPeace(me, minor) && MinorAI(minor)->CanMajorBullyGold(me->GetID()); }
	bool RunBullyGold(CvPlayer* me, CvPlayer& minor, const JObj&, JObj& out, std::string&)
	{
		int iGold = MinorAI(minor)->GetBullyGoldAmount(me->GetID());
		MinorAI(minor)->DoMajorBullyGold(me->GetID(), iGold);
		out["gold_taken"] = J(iGold);
		return true;
	}
	bool CanBullyWorker(CvPlayer* me, CvPlayer& minor) { return AtPeace(me, minor) && MinorAI(minor)->CanMajorBullyUnit(me->GetID()); }
	bool RunBullyWorker(CvPlayer* me, CvPlayer& minor, const JObj&, JObj&, std::string&)
	{
		MinorAI(minor)->DoMajorBullyUnit(me->GetID(), (UnitTypes)GC.getInfoTypeForString("UNIT_WORKER"));
		return true;
	}
	bool CanPledge(CvPlayer* me, CvPlayer& minor) { return AtPeace(me, minor) && MinorAI(minor)->CanMajorStartProtection(me->GetID()); }
	bool RunPledge(CvPlayer* me, CvPlayer& minor, const JObj&, JObj&, std::string&)
	{
		MinorAI(minor)->DoChangeProtectionFromMajor(me->GetID(), true);
		return true;
	}
	bool CanRevoke(CvPlayer* me, CvPlayer& minor) { return MinorAI(minor)->CanMajorWithdrawProtection(me->GetID()); }
	bool RunRevoke(CvPlayer* me, CvPlayer& minor, const JObj&, JObj&, std::string&)
	{
		MinorAI(minor)->DoChangeProtectionFromMajor(me->GetID(), false);
		return true;
	}
	bool CanBuyout(CvPlayer* me, CvPlayer& minor) { return AtPeace(me, minor) && MinorAI(minor)->CanMajorBuyout(me->GetID()); }
	bool RunBuyout(CvPlayer* me, CvPlayer& minor, const JObj&, JObj&, std::string&)
	{
		MinorAI(minor)->DoBuyout(me->GetID(), MOD_GLOBAL_CS_MARRIAGE_KEEPS_RESOURCES);
		return true;
	}
	bool CanMakePeace(CvPlayer* me, CvPlayer& minor)
	{
		return AtWar(me, minor) && GET_TEAM(me->getTeam()).canChangeWarPeace(minor.getTeam()) && !MinorAI(minor)->IsPeaceBlocked(me->getTeam());
	}
	bool RunMakePeace(CvPlayer* me, CvPlayer& minor, const JObj&, JObj&, std::string&)
	{
		GET_TEAM(me->getTeam()).makePeace(minor.getTeam(), true, false, me->GetID());
		return true;
	}

	const MinorAction* MinorActions(size_t& count)
	{
		static const MinorAction TABLE[] =
		{
			{ "gift_gold", AtPeace, RunGiftGold, "you are at war with them" },
			{ "gift_unit", AtPeace, RunGiftUnit, "you are at war with them" },
			{ "bully_gold", CanBullyGold, RunBullyGold, "they cannot be bullied for gold now (at war, not enough military presence, or recently bullied)" },
			{ "bully_worker", CanBullyWorker, RunBullyWorker, "they cannot be bullied for a worker now" },
			{ "pledge_protection", CanPledge, RunPledge, "you cannot pledge to protect them now" },
			{ "revoke_protection", CanRevoke, RunRevoke, "you are not protecting them, or the pledge is too recent to withdraw" },
			{ "buyout", CanBuyout, RunBuyout, "you cannot buy them out now" },
			{ "make_peace", CanMakePeace, RunMakePeace, "you are not at war with them, or peace is not possible yet" },
		};
		count = sizeof(TABLE) / sizeof(TABLE[0]);
		return TABLE;
	}

	std::string MinorActionNames()
	{
		size_t n = 0;
		const MinorAction* a = MinorActions(n);
		std::string s;
		for(size_t i = 0; i < n; ++i) s += (i ? ", " : "") + std::string(a[i].name);
		return s;
	}

	JVal MinorJson(CvPlayer* me, CvPlayer& minor)
	{
		CvMinorCivAI* ai = minor.GetMinorCivAI();
		PlayerTypes e = me->GetID();
		JObj o;
		o["civilization"] = J(CivName(minor.GetID()));
		o["influence"] = J(ai->GetEffectiveFriendshipWithMajor(e));
		o["friends_at"] = J(ai->GetFriendsThreshold());
		o["allies_at"] = J(ai->GetAlliesThreshold());
		o["status"] = J(GET_TEAM(me->getTeam()).isAtWar(minor.getTeam()) ? "at war" : ai->IsAllies(e) ? "allies" : ai->IsFriends(e) ? "friends" : "neutral");
		if(ai->GetAlly() != NO_PLAYER) o["their_ally"] = J(CivName(ai->GetAlly()));
		o["protected_by_you"] = J(ai->IsProtectedByMajor(e));
		CvCity* cap = minor.getCapitalCity();
		if(cap) o["city"] = J(McpFormat("%s at (%d,%d)", cap->getName().c_str(), cap->getX(), cap->getY()));
		JArr quests;
		for(int q = 0; q < NUM_MINOR_CIV_QUEST_TYPES; ++q)
		{
			MinorCivQuestTypes eQ = (MinorCivQuestTypes)q;
			if(!ai->IsActiveQuestForPlayer(e, eQ)) continue;
			JObj qj;
			qj["quest"] = J(QuestName(eQ));
			int iTurns = ai->GetQuestTurnsRemaining(e, eQ, GC.getGame().getGameTurn());
			if(iTurns >= 0) qj["turns_left"] = J(iTurns);
			int iData = ai->GetQuestData1(e, eQ);
			if(eQ == MINOR_CIV_QUEST_CONNECT_RESOURCE) qj["resource"] = J(ResourceName((ResourceTypes)iData));
			else if(eQ == MINOR_CIV_QUEST_CONSTRUCT_WONDER) qj["wonder"] = J(BuildingName((BuildingTypes)iData));
			else if(eQ == MINOR_CIV_QUEST_GREAT_PERSON) qj["great_person"] = J(UnitTypeName((UnitTypes)iData));
			else if(eQ == MINOR_CIV_QUEST_KILL_CITY_STATE || eQ == MINOR_CIV_QUEST_FIND_PLAYER || eQ == MINOR_CIV_QUEST_BULLY_CITY_STATE || eQ == MINOR_CIV_QUEST_DENOUNCE_MAJOR)
				qj["target"] = J(CivName((PlayerTypes)iData));
			else if(eQ == MINOR_CIV_QUEST_KILL_CAMP) qj["camp_at"] = J(McpFormat("(%d,%d)", iData, ai->GetQuestData2(e, eQ)));
			quests.push_back(JVal(qj));
		}
		o["quests_for_you"] = JVal(quests);
		JObj can;
		size_t nActions = 0;
		const MinorAction* actions = MinorActions(nActions);
		for(size_t i = 0; i < nActions; ++i)
		{
			if(!actions[i].can(me, minor)) continue;
			std::string name = actions[i].name;
			can[name] = J(true);
			if(name == "gift_gold")
			{
				can["gift_gold_influence_for_250"] = J(ai->GetFriendshipFromGoldGift(e, 250));
				can["gift_gold_influence_for_500"] = J(ai->GetFriendshipFromGoldGift(e, 500));
				can["gift_gold_influence_for_1000"] = J(ai->GetFriendshipFromGoldGift(e, 1000));
			}
			else if(name == "bully_gold") can["bully_gold_amount"] = J(ai->GetBullyGoldAmount(e));
			else if(name == "buyout") can["buyout_cost"] = J(ai->GetBuyoutCost(e));
		}
		o["actions"] = JVal(can);
		return JVal(o);
	}
}

MCP_TOOL(ToolCityState)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	std::string key = ArgStr(args, "civ");
	if(!key.empty())
	{
		CvPlayer* minor = MinorFromArgs(me, args, err);
		if(!minor) return JVal();
		return MinorJson(me, *minor);
	}
	JArr all;
	for(int i = MAX_MAJOR_CIVS; i < MAX_CIV_PLAYERS; ++i)
	{
		CvPlayer& minor = GET_PLAYER((PlayerTypes)i);
		if(!minor.isAlive() || !minor.isMinorCiv() || !GET_TEAM(me->getTeam()).isHasMet(minor.getTeam())) continue;
		all.push_back(MinorJson(me, minor));
	}
	JObj o;
	o["city_states"] = JVal(all);
	return JVal(o);
}

MCP_TOOL(ToolCityStateAction)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvPlayer* minor = MinorFromArgs(me, args, err);
	if(!minor) return JVal();
	std::string action = ArgStr(args, "action");
	size_t n = 0;
	const MinorAction* table = MinorActions(n);
	const MinorAction* a = NULL;
	for(size_t i = 0; i < n; ++i) if(action == table[i].name) a = &table[i];
	if(!a) { err = "unknown action '" + action + "'; use " + MinorActionNames(); return JVal(); }
	if(!a->can(me, *minor)) { err = a->refusal; return JVal(); }
	JObj o;
	if(!a->run(me, *minor, args, o, err)) return JVal();
	o["ok"] = J(true);
	o["city_state"] = MinorJson(me, *minor);
	return JVal(o);
}
