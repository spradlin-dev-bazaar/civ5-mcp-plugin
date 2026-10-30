#include "CvGameCoreDLLPCH.h"
#include <map>
#include "McpToolSupport.h"
#include "McpToolHandlers.h"
#include "McpBridge.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvCity.h"
#include "CvUnit.h"
#include "CvPlot.h"
#include "CvMap.h"
#include "CvGlobals.h"
#include "CvGameCoreUtils.h"
#include "CvNotifications.h"
#include "McpDiplomacy.h"
#include "CvDiplomacyAI.h"
#include "CvMinorCivAI.h"
#include "CvTechClasses.h"
#include "CvUnitClasses.h"
#include "CvBuildingClasses.h"
#include "CvImprovementClasses.h"
#include "CvPromotionClasses.h"
#include "CvPolicyClasses.h"
#include "CvTreasury.h"
#include "CvInfos.h"

using namespace McpTool;

namespace
{
	const int THREAT_RADIUS = 5;

	const char* ApproachName(MajorCivApproachTypes e)
	{
		switch(e)
		{
		case MAJOR_CIV_APPROACH_WAR: return "war";
		case MAJOR_CIV_APPROACH_HOSTILE: return "hostile";
		case MAJOR_CIV_APPROACH_DECEPTIVE: return "deceptive";
		case MAJOR_CIV_APPROACH_GUARDED: return "guarded";
		case MAJOR_CIV_APPROACH_AFRAID: return "afraid";
		case MAJOR_CIV_APPROACH_FRIENDLY: return "friendly";
		case MAJOR_CIV_APPROACH_NEUTRAL: return "neutral";
		default: return "unknown";
		}
	}

	const char* OpinionName(MajorCivOpinionTypes e)
	{
		switch(e)
		{
		case MAJOR_CIV_OPINION_UNFORGIVABLE: return "unforgivable";
		case MAJOR_CIV_OPINION_ENEMY: return "enemy";
		case MAJOR_CIV_OPINION_COMPETITOR: return "competitor";
		case MAJOR_CIV_OPINION_NEUTRAL: return "neutral";
		case MAJOR_CIV_OPINION_FAVORABLE: return "favorable";
		case MAJOR_CIV_OPINION_FRIEND: return "friend";
		case MAJOR_CIV_OPINION_ALLY: return "ally";
		default: return "unknown";
		}
	}

	const char* MinorTraitName(MinorCivTraitTypes e)
	{
		switch(e)
		{
		case MINOR_CIV_TRAIT_CULTURED: return "cultured";
		case MINOR_CIV_TRAIT_MILITARISTIC: return "militaristic";
		case MINOR_CIV_TRAIT_MARITIME: return "maritime";
		case MINOR_CIV_TRAIT_MERCANTILE: return "mercantile";
		case MINOR_CIV_TRAIT_RELIGIOUS: return "religious";
		default: return "unknown";
		}
	}

	bool HasMet(CvPlayer* me, CvPlayer& other)
	{
		return GET_TEAM(me->getTeam()).isHasMet(other.getTeam());
	}

	JVal MajorCivJson(CvPlayer* me, CvPlayer& other, bool bBrief)
	{
		JObj o;
		o["civilization"] = J(other.getCivilizationShortDescription());
		o["player_id"] = J((int)other.GetID());
		CvLeaderHeadInfo* pLeader = GC.getLeaderHeadInfo(other.getLeaderType());
		o["leader"] = J(pLeader ? pLeader->GetDescription() : "");
		o["at_war"] = J(GET_TEAM(me->getTeam()).isAtWar(other.getTeam()));
		if(other.getTeam() == me->getTeam()) o["same_team"] = J(true);
		if(other.isHuman()) o["human"] = J(true);
		CvDiplomacyAI* pDiplo = other.GetDiplomacyAI();
		if(!other.isHuman())
		{
			o["their_attitude"] = J(ApproachName(pDiplo->GetMajorCivApproach(me->GetID(), true)));
			o["their_opinion_of_you"] = J(OpinionName(pDiplo->GetMajorCivOpinion(me->GetID())));
		}
		if(pDiplo->IsDenouncedPlayer(me->GetID())) o["denounced_you"] = J(true);
		if(me->GetDiplomacyAI()->IsDenouncedPlayer(other.GetID())) o["you_denounced_them"] = J(true);
		if(pDiplo->IsDoFAccepted(me->GetID())) o["declared_friendship"] = J(true);
		if(bBrief) return JVal(o);
		o["score"] = J(other.GetScore());
		o["num_cities"] = J(other.getNumCities());
		int iMine = me->GetMilitaryMight();
		if(iMine > 0) o["military_strength_vs_yours"] = J((double)other.GetMilitaryMight() / (double)iMine);
		o["can_declare_war"] = J(GET_TEAM(me->getTeam()).canDeclareWar(other.getTeam(), me->GetID()));
		CvCity* cap = other.getCapitalCity();
		if(cap && cap->plot()->isRevealed(me->getTeam()))
			o["capital"] = J(McpFormat("%s at (%d,%d)", cap->getName().c_str(), cap->getX(), cap->getY()));
		return JVal(o);
	}

	JVal MinorCivJson(CvPlayer* me, CvPlayer& minor)
	{
		JObj o;
		o["name"] = J(minor.getCivilizationShortDescription());
		o["player_id"] = J((int)minor.GetID());
		CvMinorCivAI* pAI = minor.GetMinorCivAI();
		if(pAI)
		{
			o["type"] = J(MinorTraitName(pAI->GetTrait()));
			o["friendship"] = J(pAI->GetEffectiveFriendshipWithMajor(me->GetID()));
			o["friends"] = J(pAI->IsFriends(me->GetID()));
			o["allies"] = J(pAI->IsAllies(me->GetID()));
			PlayerTypes eAlly = pAI->GetAlly();
			if(eAlly != NO_PLAYER) o["ally"] = J(CivName(eAlly));
		}
		o["at_war"] = J(GET_TEAM(me->getTeam()).isAtWar(minor.getTeam()));
		CvCity* cap = minor.getCapitalCity();
		if(cap && cap->plot()->isRevealed(me->getTeam()))
			o["city_at"] = J(McpFormat("(%d,%d)", cap->getX(), cap->getY()));
		return JVal(o);
	}

	// ------------------------------------------------------------ briefing sections

	// Other players' notifications carry the turn this player ended, so the default window starts there and
	// skips, by id (ids only grow), what last turn's briefings already reported.
	struct NoticesReported
	{
		int turn, newest, newestLastTurn;
		NoticesReported() : turn(-1), newest(-1), newestLastTurn(-1) {}
		void BeginTurn(int iTurn)
		{
			if(turn == iTurn) return;
			newestLastTurn = (turn == iTurn - 1) ? newest : -1;
			newest = -1;
			turn = iTurn;
		}
	};
	std::map<int, NoticesReported> s_noticesReported;

	JArr BriefingEvents(CvPlayer* me, int iSince, bool bSkipReported)
	{
		JArr events;
		CvNotifications* pNotes = me->GetNotifications();
		if(!pNotes) return events;
		int iTurn = GC.getGame().getGameTurn();
		NoticesReported& reported = s_noticesReported[(int)me->GetID()];
		reported.BeginTurn(iTurn);
		for(int i = 0; i < pNotes->GetNumNotifications(); ++i)
		{
			int iNoteTurn = pNotes->GetNotificationTurn(i);
			if(iNoteTurn < iSince) continue;
			int iId = pNotes->GetNotificationID(i);
			if(iId > reported.newest) reported.newest = iId;
			if(bSkipReported && iNoteTurn < iTurn && iId <= reported.newestLastTurn) continue;
			JObj ev;
			ev["turn"] = J(iNoteTurn);
			ev["summary"] = J(StripMarkup(pNotes->GetNotificationSummary(i).c_str()));
			ev["text"] = J(StripMarkup(pNotes->GetNotificationStr(i).c_str()));
			int x = pNotes->GetNotificationX(i), y = pNotes->GetNotificationY(i);
			if(x >= 0 && y >= 0) ev["at"] = J(McpFormat("(%d,%d)", x, y));
			events.push_back(JVal(ev));
		}
		return events;
	}

	JArr BriefingGameMessages(CvPlayer* me, int iSince)
	{
		JArr list;
		std::vector<McpPopup> seen = McpBridge::Get().GetPopupsSince(me->GetID(), iSince);
		for(size_t i = 0; i < seen.size(); ++i)
		{
			JObj p;
			p["id"] = J(seen[i].id);
			p["turn"] = J(seen[i].turn);
			p["text"] = J(seen[i].text);
			if(!seen[i].read) p["unread"] = J(true);
			list.push_back(JVal(p));
		}
		return list;
	}

	// One snapshot per player, advanced once per turn, so every briefing in a turn
	// reports the same changes and a new game starts clean.
	struct UnitSnapshot
	{
		int turn;
		std::map<int, std::string> units;
		std::map<int, std::string> previous;
		UnitSnapshot() : turn(-1) {}
	};
	std::map<int, UnitSnapshot> s_snapshots;

	void BriefingUnitChanges(CvPlayer* me, int iTurn, JObj& o)
	{
		UnitSnapshot& snap = s_snapshots[(int)me->GetID()];
		if(snap.turn >= 0 && iTurn > snap.turn + 1) snap = UnitSnapshot();
		if(snap.turn != iTurn)
		{
			snap.previous.swap(snap.units);
			snap.units.clear();
			int iLoop = 0;
			for(CvUnit* u = me->firstUnit(&iLoop); u; u = me->nextUnit(&iLoop))
				snap.units[u->GetID()] = McpFormat("%s (id %d) at (%d,%d)", UnitTypeName(u->getUnitType()), u->GetID(), u->getX(), u->getY());
			bool bFirst = (snap.turn == -1);
			snap.turn = iTurn;
			if(bFirst) return;
		}
		JArr appeared, gone;
		for(std::map<int, std::string>::const_iterator it = snap.units.begin(); it != snap.units.end(); ++it)
			if(snap.previous.find(it->first) == snap.previous.end()) appeared.push_back(J(it->second));
		for(std::map<int, std::string>::const_iterator it = snap.previous.begin(); it != snap.previous.end(); ++it)
			if(snap.units.find(it->first) == snap.units.end()) gone.push_back(J(it->second));
		if(!appeared.empty()) o["units_appeared_since_last_turn"] = JVal(appeared);
		if(!gone.empty()) o["units_gone_since_last_turn"] = JVal(gone);
	}

	void BriefingDecisions(CvPlayer* me, JObj& o)
	{
		JArr pending, idle, automated;
		if(me->GetNumFreeTechs() > 0)
			pending.push_back(J(McpFormat("free_tech: %d free technology to claim (research_options, set_research)", me->GetNumFreeTechs())));
		else if(me->GetPlayerTechs()->GetCurrentResearch() == NO_TECH)
			pending.push_back(J("research: nothing being researched (research_options, set_research)"));
		int iLoop = 0;
		for(CvCity* c = me->firstCity(&iLoop); c; c = me->nextCity(&iLoop))
		{
			// The game asks for production only once a captured city's fate is chosen.
			if(CaptureUndecided(c))
				pending.push_back(J(CaptureDecision(me, c)));
			else if(c->getOrderQueueLength() == 0)
				pending.push_back(J(McpFormat("production: %s (id %d) is building nothing (city_options, set_production)", c->getName().c_str(), c->GetID())));
			if(c->CanRangeStrikeNow())
				pending.push_back(J(McpFormat("city_attack: %s can bombard an enemy this turn", c->getName().c_str())));
		}
		int iRequests = McpCountDiploRequests();
		if(iRequests > 0)
			pending.push_back(J(McpFormat("diplomacy: %d request(s) from other leaders await your answer (diplomacy; respond_to_request, or answer_deal for a human's deal)", iRequests)));
		if(PolicyChoiceAvailable(me))
			pending.push_back(J("policy: you can adopt a social policy now (policy_options, adopt_policy)"));
		for(CvUnit* u = me->firstUnit(&iLoop); u; u = me->nextUnit(&iLoop))
		{
			if(u->isPromotionReady())
				pending.push_back(J(McpFormat("promotion: %s (id %d) can be promoted (unit_options, promote_unit)", UnitTypeName(u->getUnitType()), u->GetID())));
			if(u->CanUpgradeRightNow(false))
				pending.push_back(J(McpFormat("upgrade: %s (id %d) can upgrade to %s for %d gold (upgrade_unit)", UnitTypeName(u->getUnitType()), u->GetID(), UnitTypeName(u->GetUpgradeUnitType()), u->upgradePrice(u->GetUpgradeUnitType()))));
			if(u->ReadyToMove())
				idle.push_back(J(McpFormat("%s (id %d) at (%d,%d)", UnitTypeName(u->getUnitType()), u->GetID(), u->getX(), u->getY())));
			if(u->IsAutomated())
				automated.push_back(J(McpFormat("%s (id %d) at (%d,%d): %s", UnitTypeName(u->getUnitType()), u->GetID(), u->getX(), u->getY(), u->GetAutomateType() == AUTOMATE_EXPLORE ? "exploring" : "working")));
		}
		EndTurnBlockingTypes eBlock = EndTurnBlocking(me);
		if(eBlock != NO_ENDTURN_BLOCKING_TYPE && eBlock != ENDTURN_BLOCKING_UNITS)
			pending.push_back(J("end_turn is blocked - " + BlockingName(eBlock)));
		o["decisions_pending"] = JVal(pending);
		o["idle_units"] = JVal(idle);
		if(!automated.empty()) o["units_automated_by_the_game_ai"] = JVal(automated);
	}

	int DistanceToNearestOwn(CvPlayer* me, int x, int y, std::string& nearest)
	{
		int best = INT_MAX;
		int iLoop = 0;
		for(CvCity* c = me->firstCity(&iLoop); c; c = me->nextCity(&iLoop))
		{
			int d = plotDistance(x, y, c->getX(), c->getY());
			if(d < best) { best = d; nearest = McpFormat("city %s", c->getName().c_str()); }
		}
		for(CvUnit* u = me->firstUnit(&iLoop); u; u = me->nextUnit(&iLoop))
		{
			int d = plotDistance(x, y, u->getX(), u->getY());
			if(d < best) { best = d; nearest = McpFormat("unit %d (%s)", u->GetID(), UnitTypeName(u->getUnitType())); }
		}
		return best;
	}

	JArr BriefingThreats(CvPlayer* me)
	{
		JArr threats;
		TeamTypes eTeam = me->getTeam();
		for(int i = 0; i < MAX_PLAYERS; ++i)
		{
			CvPlayer& other = GET_PLAYER((PlayerTypes)i);
			if(!other.isAlive() || !IsEnemyOf(me, other.GetID())) continue;
			int iUnitLoop = 0;
			for(CvUnit* u = other.firstUnit(&iUnitLoop); u; u = other.nextUnit(&iUnitLoop))
			{
				CvPlot* p = u->plot();
				if(!p || !p->isVisible(eTeam) || u->isInvisible(eTeam, false)) continue;
				std::string nearest;
				int d = DistanceToNearestOwn(me, u->getX(), u->getY(), nearest);
				if(d > THREAT_RADIUS) continue;
				JObj t;
				t["owner"] = J(CivName(other.GetID()));
				t["unit"] = J(UnitTypeName(u->getUnitType()));
				t["strength"] = J(u->GetBaseCombatStrength());
				if(u->isRanged()) t["ranged_strength"] = J(u->GetBaseRangedCombatStrength());
				t["hp"] = J(u->GetCurrHitPoints());
				t["at"] = J(McpFormat("(%d,%d)", u->getX(), u->getY()));
				t["distance"] = J(d);
				t["nearest_of_yours"] = J(nearest);
				threats.push_back(JVal(t));
			}
		}
		return threats;
	}

	JArr BriefingWarnings(CvPlayer* me)
	{
		JArr warnings;
		if(me->calculateGoldRate() < 0)
			warnings.push_back(J(McpFormat("gold: losing %d per turn with %d in the treasury", -me->calculateGoldRate(), me->GetTreasury()->GetGold())));
		if(me->GetExcessHappiness() < 0)
			warnings.push_back(J(McpFormat("unhappiness: %d (growth and production suffer)", -me->GetExcessHappiness())));
		int iLoop = 0;
		for(CvCity* c = me->firstCity(&iLoop); c; c = me->nextCity(&iLoop))
		{
			int iFoodTimes100 = c->foodDifferenceTimes100();
			if(iFoodTimes100 < 0)
				warnings.push_back(J(McpFormat("starvation: %s loses %g food per turn", c->getName().c_str(), -iFoodTimes100 / 100.0)));
			if(c->getDamage() > 0)
				warnings.push_back(J(McpFormat("city damaged: %s at %d/%d hp", c->getName().c_str(), c->GetMaxHitPoints() - c->getDamage(), c->GetMaxHitPoints())));
		}
		return warnings;
	}

	JVal BriefingDiplomacy(CvPlayer* me)
	{
		JArr majors;
		int minorsMet = 0, minorsAtWar = 0;
		for(int i = 0; i < MAX_CIV_PLAYERS; ++i)
		{
			CvPlayer& other = GET_PLAYER((PlayerTypes)i);
			if(!other.isAlive() || other.GetID() == me->GetID() || other.isBarbarian() || !HasMet(me, other)) continue;
			if(other.isMinorCiv())
			{
				++minorsMet;
				if(GET_TEAM(me->getTeam()).isAtWar(other.getTeam())) ++minorsAtWar;
				continue;
			}
			majors.push_back(MajorCivJson(me, other, true));
		}
		JObj diplo;
		diplo["civilizations_met"] = JVal(majors);
		diplo["city_states_met"] = J(minorsMet);
		if(minorsAtWar) diplo["city_states_at_war_with_you"] = J(minorsAtWar);
		return JVal(diplo);
	}

	JVal BriefingEconomy(CvPlayer* me)
	{
		JObj econ;
		econ["gold"] = J(me->GetTreasury()->GetGold());
		econ["gold_per_turn"] = J(me->calculateGoldRate());
		econ["happiness"] = J(me->GetExcessHappiness());
		econ["culture"] = J(me->getJONSCulture());
		econ["next_policy_cost"] = J(me->getNextPolicyCost());
		econ["science_per_turn"] = J(me->GetScience());
		if(me->isGoldenAge()) econ["golden_age_turns_left"] = J(me->getGoldenAgeTurns());
		return JVal(econ);
	}
}

void McpResetBriefing()
{
	s_snapshots.clear();
	s_noticesReported.clear();
}

MCP_TOOL(ToolBriefing)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvGame& g = GC.getGame();
	int iTurn = g.getGameTurn();
	bool bDefaultWindow = !HasArg(args, "since_turn");
	int iSince = ArgInt(args, "since_turn", iTurn - 1);

	JObj o;
	o["turn"] = J(iTurn);
	o["year"] = J(g.getGameTurnYear());
	o["is_my_turn"] = J(McpIsPlayerTurn(*me));
	o["end_turn_blocking"] = J(EndTurnBlockingText(me));
	o["events_since_turn"] = J(iSince);
	o["events"] = JVal(BriefingEvents(me, iSince, bDefaultWindow));
	o["game_messages"] = JVal(BriefingGameMessages(me, iSince));
	int iUnread = McpBridge::Get().CountUnread(me->GetID());
	if(iUnread > 0) o["unread_messages"] = J(McpFormat("%d - read them above and call acknowledge_messages before acting", iUnread));
	BriefingUnitChanges(me, iTurn, o);
	BriefingDecisions(me, o);
	o["threats"] = JVal(BriefingThreats(me));
	o["warnings"] = JVal(BriefingWarnings(me));
	o["diplomacy"] = BriefingDiplomacy(me);
	o["diplomacy_requests"] = McpDiploRequestsJson();
	o["economy"] = BriefingEconomy(me);
	o["research"] = ResearchJson(me);
	return JVal(o);
}

MCP_TOOL(ToolCivs)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	JArr majors, minors;
	for(int i = 0; i < MAX_CIV_PLAYERS; ++i)
	{
		CvPlayer& other = GET_PLAYER((PlayerTypes)i);
		if(!other.isAlive() || other.GetID() == me->GetID() || other.isBarbarian() || !HasMet(me, other)) continue;
		if(other.isMinorCiv()) minors.push_back(MinorCivJson(me, other));
		else majors.push_back(MajorCivJson(me, other, false));
	}
	JObj o;
	o["your_score"] = J(me->GetScore());
	o["your_military_strength"] = J(me->GetMilitaryMight());
	o["civilizations"] = JVal(majors);
	o["city_states"] = JVal(minors);
	return JVal(o);
}

// ------------------------------------------------------------------ lookup

namespace
{
	bool StartsWith(const std::string& s, const char* prefix)
	{
		return _strnicmp(s.c_str(), prefix, strlen(prefix)) == 0;
	}

	JVal LookupTech(TechTypes e)
	{
		CvTechEntry* info = GC.getTechInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		o["era"] = J(EraName((EraTypes)info->GetEra()));
		o["cost"] = J(info->GetResearchCost());
		o["help"] = J(Localized(info->GetHelp()));
		JArr unlocks;
		for(int i = 0; i < GC.getNumUnitInfos(); ++i)
		{
			CvUnitEntry* u = GC.getUnitInfo((UnitTypes)i);
			if(u && u->GetPrereqAndTech() == (int)e) unlocks.push_back(J(McpFormat("unit: %s (%s)", u->GetDescription(), u->GetType())));
		}
		for(int i = 0; i < GC.getNumBuildingInfos(); ++i)
		{
			CvBuildingEntry* b = GC.getBuildingInfo((BuildingTypes)i);
			if(b && b->GetPrereqAndTech() == (int)e) unlocks.push_back(J(McpFormat("building: %s (%s)", b->GetDescription(), b->GetType())));
		}
		for(int i = 0; i < GC.getNumBuildInfos(); ++i)
		{
			CvBuildInfo* b = GC.getBuildInfo((BuildTypes)i);
			if(b && b->getTechPrereq() == (int)e) unlocks.push_back(J(McpFormat("worker build: %s (%s)", BuildName((BuildTypes)i).c_str(), b->GetType())));
		}
		o["unlocks"] = JVal(unlocks);
		return JVal(o);
	}

	JVal LookupUnit(UnitTypes e)
	{
		CvUnitEntry* info = GC.getUnitInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		o["help"] = J(Localized(info->GetHelp()));
		o["production_cost"] = J(info->GetProductionCost());
		o["strength"] = J(info->GetCombat());
		if(info->GetRangedCombat() > 0)
		{
			o["ranged_strength"] = J(info->GetRangedCombat());
			o["range"] = J(info->GetRange());
		}
		o["moves"] = J(info->GetMoves());
		if(info->GetPrereqAndTech() != NO_TECH) o["requires_tech"] = J(TechName((TechTypes)info->GetPrereqAndTech()));
		return JVal(o);
	}

	JVal LookupBuilding(BuildingTypes e)
	{
		CvBuildingEntry* info = GC.getBuildingInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		o["help"] = J(Localized(info->GetHelp()));
		o["production_cost"] = J(info->GetProductionCost());
		if(info->GetPrereqAndTech() != NO_TECH) o["requires_tech"] = J(TechName((TechTypes)info->GetPrereqAndTech()));
		return JVal(o);
	}

	JVal NameAndHelp(const char* szName, const char* szHelp)
	{
		JObj p;
		p["name"] = J(szName);
		p["help"] = J(Localized(szHelp));
		return JVal(p);
	}
}

MCP_TOOL(ToolLookup)
{
	std::string key = ArgStr(args, "type");
	if(key.empty()) { err = "type is required"; return JVal(); }
	JObj o;
	o["type"] = J(key);
	// POLICY_BRANCH_ must be tested before POLICY_, which it also matches.
	if(StartsWith(key, "TECH_"))
	{
		TechTypes e = FindTechType(key);
		if(e == NO_TECH) { err = "unknown tech " + key; return JVal(); }
		o["tech"] = LookupTech(e);
	}
	else if(StartsWith(key, "UNIT_"))
	{
		UnitTypes e = FindUnitType(key);
		if(e == NO_UNIT) { err = "unknown unit " + key; return JVal(); }
		o["unit"] = LookupUnit(e);
	}
	else if(StartsWith(key, "BUILDING_"))
	{
		BuildingTypes e = FindBuildingType(key);
		if(e == NO_BUILDING) { err = "unknown building " + key; return JVal(); }
		o["building"] = LookupBuilding(e);
	}
	else if(StartsWith(key, "POLICY_BRANCH_"))
	{
		PolicyBranchTypes e = FindPolicyBranchType(key);
		if(e == NO_POLICY_BRANCH_TYPE) { err = "unknown policy branch " + key; return JVal(); }
		CvPolicyBranchEntry* info = GC.getPolicyBranchInfo(e);
		JVal v = NameAndHelp(info->GetDescription(), info->GetHelp());
		if(info->GetEraPrereq() != NO_ERA) v.get<JObj>()["requires_era"] = J(EraName((EraTypes)info->GetEraPrereq()));
		o["policy_branch"] = v;
	}
	else if(StartsWith(key, "POLICY_"))
	{
		PolicyTypes e = FindPolicyType(key);
		if(e == NO_POLICY) { err = "unknown policy " + key; return JVal(); }
		CvPolicyEntry* info = GC.getPolicyInfo(e);
		JVal v = NameAndHelp(info->GetDescription(), info->GetHelp());
		if(info->GetPolicyBranchType() != NO_POLICY_BRANCH_TYPE)
			v.get<JObj>()["branch"] = J(GC.getPolicyBranchInfo((PolicyBranchTypes)info->GetPolicyBranchType())->GetDescription());
		o["policy"] = v;
	}
	else if(StartsWith(key, "PROMOTION_"))
	{
		PromotionTypes e = FindPromotionType(key);
		if(e == NO_PROMOTION) { err = "unknown promotion " + key; return JVal(); }
		CvPromotionEntry* info = GC.getPromotionInfo(e);
		o["promotion"] = NameAndHelp(info->GetDescription(), info->GetHelp());
	}
	else if(StartsWith(key, "IMPROVEMENT_"))
	{
		ImprovementTypes e = FindImprovementType(key);
		if(e == NO_IMPROVEMENT) { err = "unknown improvement " + key; return JVal(); }
		CvImprovementEntry* info = GC.getImprovementInfo(e);
		o["improvement"] = NameAndHelp(info->GetDescription(), info->GetHelp());
	}
	else if(StartsWith(key, "RESOURCE_"))
	{
		ResourceTypes e = FindResourceType(key);
		if(e == NO_RESOURCE) { err = "unknown resource " + key; return JVal(); }
		CvResourceInfo* info = GC.getResourceInfo(e);
		o["resource"] = NameAndHelp(info->GetDescription(), info->GetHelp());
	}
	else
	{
		err = "unsupported key; use a TECH_, UNIT_, BUILDING_, POLICY_, POLICY_BRANCH_, PROMOTION_, IMPROVEMENT_ or RESOURCE_ type";
		return JVal();
	}
	return JVal(o);
}
