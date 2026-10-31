#include "CvGameCoreDLLPCH.h"
#include "McpToolSupport.h"
#include "McpToolHandlers.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvCity.h"
#include "CvUnit.h"
#include "CvPolicyClasses.h"
#include "CvCultureClasses.h"
#include "CvBuildingClasses.h"
#include "CvTradeClasses.h"
#include "CvGlobals.h"
#include "CvGameCoreUtils.h"
#include "CvTypes.h"

using namespace McpTool;

// Ideology, archaeology, great works, trade routes and the spaceship.

namespace McpTool
{
	const char* SlotTypeName(GreatWorkSlotType e)
	{
		if(e == CvTypes::getGREAT_WORK_SLOT_ART_ARTIFACT()) return "art or artifact";
		if(e == CvTypes::getGREAT_WORK_SLOT_LITERATURE()) return "writing";
		if(e == CvTypes::getGREAT_WORK_SLOT_MUSIC()) return "music";
		return "unknown";
	}
}

namespace
{
	bool IsIdeology(PolicyBranchTypes e)
	{
		CvPolicyBranchEntry* info = GC.getPolicyBranchInfo(e);
		return info && info->IsPurchaseByLevel();
	}

	PolicyBranchTypes FindIdeology(const std::string& key)
	{
		for(int i = 0; i < GC.getNumPolicyBranchInfos(); ++i)
		{
			PolicyBranchTypes e = (PolicyBranchTypes)i;
			CvPolicyBranchEntry* info = GC.getPolicyBranchInfo(e);
			if(!info || !IsIdeology(e)) continue;
			if(MatchesKey(info->GetType(), info->GetDescription(), key)) return e;
		}
		return NO_POLICY_BRANCH_TYPE;
	}

	const char* ArchaeologyName(ArchaeologyChoiceType e)
	{
		switch(e)
		{
		case ARCHAEOLOGY_DO_NOTHING: return "do_nothing";
		case ARCHAEOLOGY_LANDMARK: return "landmark";
		case ARCHAEOLOGY_ARTIFACT_PLAYER1: return "artifact";
		case ARCHAEOLOGY_ARTIFACT_PLAYER2: return "artifact_for_other_civ";
		case ARCHAEOLOGY_CULTURE_BOOST: return "culture_boost";
		case ARCHAEOLOGY_ARTIFACT_WRITING: return "written_artifact";
		default: return "unknown";
		}
	}

	bool FindArchaeologyChoice(const std::string& key, ArchaeologyChoiceType& out)
	{
		for(int i = 0; i < (ARCHAEOLOGY_ARTIFACT_WRITING + 1); ++i)
			if(key == ArchaeologyName((ArchaeologyChoiceType)i)) { out = (ArchaeologyChoiceType)i; return true; }
		return false;
	}

	struct ConnectionName { const char* name; TradeConnectionType type; };
	const ConnectionName CONNECTIONS[] = { { "international", TRADE_CONNECTION_INTERNATIONAL }, { "food", TRADE_CONNECTION_FOOD }, { "production", TRADE_CONNECTION_PRODUCTION } };
	const size_t NUM_CONNECTIONS = sizeof(CONNECTIONS) / sizeof(CONNECTIONS[0]);

	bool FindConnectionType(const std::string& key, TradeConnectionType& out)
	{
		for(size_t i = 0; i < NUM_CONNECTIONS; ++i)
			if(key == CONNECTIONS[i].name) { out = CONNECTIONS[i].type; return true; }
		return false;
	}

	// The checks the game's own archaeology prompt makes before offering a choice.
	bool ArchaeologyChoiceOpen(CvPlayer* me, CvPlot* pPlot, ArchaeologyChoiceType e, std::string& why)
	{
		switch(e)
		{
		case ARCHAEOLOGY_ARTIFACT_PLAYER1:
		case ARCHAEOLOGY_ARTIFACT_PLAYER2:
			if(e == ARCHAEOLOGY_ARTIFACT_PLAYER2 && pPlot->GetArchaeologicalRecord().m_ePlayer2 == NO_PLAYER) { why = "this dig has no second civilization"; return false; }
			if(!me->GetCulture()->HasAvailableGreatWorkSlot(CvTypes::getGREAT_WORK_SLOT_ART_ARTIFACT())) { why = "no free artifact slot in any of your cities"; return false; }
			return true;
		case ARCHAEOLOGY_ARTIFACT_WRITING:
			if(!pPlot->HasWrittenArtifact()) { why = "this dig holds no written artifact"; return false; }
			if(!me->GetCulture()->HasAvailableGreatWorkSlot(CvTypes::getGREAT_WORK_SLOT_LITERATURE())) { why = "no free writing slot in any of your cities"; return false; }
			return true;
		default:
			return true;
		}
	}

	BuildingTypes BuildingInCity(CvCity* c, const std::string& key)
	{
		BuildingTypes e = FindBuildingType(key);
		if(e == NO_BUILDING || c->GetCityBuildings()->GetNumBuilding(e) <= 0) return NO_BUILDING;
		return e;
	}
}

MCP_TOOL(ToolChooseIdeology)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvPlayerPolicies* pp = me->GetPlayerPolicies();
	std::string key = ArgStr(args, "ideology");
	JObj o;
	if(key.empty())
	{
		JArr list;
		for(int i = 0; i < GC.getNumPolicyBranchInfos(); ++i)
		{
			PolicyBranchTypes e = (PolicyBranchTypes)i;
			if(!IsIdeology(e)) continue;
			JObj b;
			b["type"] = J(GC.getPolicyBranchInfo(e)->GetType());
			b["name"] = J(GC.getPolicyBranchInfo(e)->GetDescription());
			b["help"] = J(Localized(GC.getPolicyBranchInfo(e)->GetHelp()));
			list.push_back(JVal(b));
		}
		o["ideologies"] = JVal(list);
		o["can_choose_now"] = J(pp->IsTimeToChooseIdeology() && pp->GetLateGamePolicyTree() == NO_POLICY_BRANCH_TYPE);
		if(pp->GetLateGamePolicyTree() != NO_POLICY_BRANCH_TYPE) o["current"] = J(GC.getPolicyBranchInfo(pp->GetLateGamePolicyTree())->GetDescription());
		PolicyBranchTypes ePreferred = me->GetCulture()->GetPublicOpinionPreferredIdeology();
		if(ePreferred != NO_POLICY_BRANCH_TYPE) o["public_opinion_prefers"] = J(GC.getPolicyBranchInfo(ePreferred)->GetDescription());
		return JVal(o);
	}
	me = MeActing(err);
	if(!me) return JVal();
	PolicyBranchTypes e = FindIdeology(key);
	if(e == NO_POLICY_BRANCH_TYPE) { err = "unknown ideology '" + key + "'; call choose_ideology without arguments to list them"; return JVal(); }
	if(pp->GetLateGamePolicyTree() != NO_POLICY_BRANCH_TYPE) { err = "you already follow an ideology; a change comes only from public opinion (switch_ideology)"; return JVal(); }
	if(!pp->IsTimeToChooseIdeology()) { err = "it is not time yet: ideologies open in the Modern era or with three factories"; return JVal(); }
	pp->SetPolicyBranchUnlocked(e, true, false);
	o["ok"] = J(true);
	o["ideology"] = J(GC.getPolicyBranchInfo(e)->GetDescription());
	o["next"] = J("adopt tenets with adopt_policy; policy_options lists them");
	return JVal(o);
}

MCP_TOOL(ToolSwitchIdeology)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvPlayerPolicies* pp = me->GetPlayerPolicies();
	PolicyBranchTypes eCurrent = pp->GetLateGamePolicyTree();
	PolicyBranchTypes ePreferred = me->GetCulture()->GetPublicOpinionPreferredIdeology();
	if(eCurrent == NO_POLICY_BRANCH_TYPE) { err = "you have no ideology yet (choose_ideology)"; return JVal(); }
	if(ePreferred == NO_POLICY_BRANCH_TYPE || ePreferred == eCurrent) { err = "public opinion is not pushing you toward another ideology"; return JVal(); }
	me->SetAnarchyNumTurns(GC.getSWITCH_POLICY_BRANCHES_ANARCHY_TURNS());
	pp->DoSwitchIdeologies(ePreferred);
	JObj o;
	o["ok"] = J(true);
	o["ideology"] = J(GC.getPolicyBranchInfo(ePreferred)->GetDescription());
	o["anarchy_turns"] = J(GC.getSWITCH_POLICY_BRANCHES_ANARCHY_TURNS());
	return JVal(o);
}

MCP_TOOL(ToolArchaeology)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvPlayerCulture* pc = me->GetCulture();
	std::string choice = ArgStr(args, "choice");
	CvPlot* pPlot = pc->GetNextDigCompletePlot();
	JObj o;
	if(choice.empty())
	{
		o["digs_waiting"] = J(me->GetNumArchaeologyChoices());
		if(pPlot)
		{
			o["dig_at"] = J(McpFormat("(%d,%d)", pPlot->getX(), pPlot->getY()));
			o["suggested"] = J(ArchaeologyName(pc->GetArchaeologyChoice(pPlot)));
			if(pPlot->getOwner() != NO_PLAYER) o["tile_owner"] = J(CivName(pPlot->getOwner()));
		}
		JArr choices;
		std::string why;
		for(int i = 0; i < (ARCHAEOLOGY_ARTIFACT_WRITING + 1); ++i)
			if(!pPlot || ArchaeologyChoiceOpen(me, pPlot, (ArchaeologyChoiceType)i, why)) choices.push_back(J(ArchaeologyName((ArchaeologyChoiceType)i)));
		o["choices"] = JVal(choices);
		o["notes"] = J("artifact places a work in a free Great Work slot (an art slot); landmark builds an improvement here; culture_boost cashes it in; written_artifact needs a writing slot");
		return JVal(o);
	}
	me = MeActing(err);
	if(!me) return JVal();
	if(!pPlot || me->GetNumArchaeologyChoices() <= 0) { err = "no completed dig is waiting"; return JVal(); }
	ArchaeologyChoiceType e;
	if(!FindArchaeologyChoice(choice, e)) { err = "unknown choice '" + choice + "'"; return JVal(); }
	std::string why;
	if(!ArchaeologyChoiceOpen(me, pPlot, e, why)) { err = choice + " is not possible: " + why; return JVal(); }
	pc->DoArchaeologyChoice(e);
	o["ok"] = J(true);
	o["chose"] = J(choice);
	o["digs_waiting"] = J(me->GetNumArchaeologyChoices());
	return JVal(o);
}

MCP_TOOL(ToolGreatWorks)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvGameCulture* pg = GC.getGame().GetGameCulture();
	JArr cities;
	int iLoop = 0;
	for(CvCity* c = me->firstCity(&iLoop); c; c = me->nextCity(&iLoop))
	{
		JArr buildings;
		for(int i = 0; i < GC.getNumBuildingInfos(); ++i)
		{
			BuildingTypes e = (BuildingTypes)i;
			CvBuildingEntry* info = GC.getBuildingInfo(e);
			if(!info || info->GetGreatWorkCount() <= 0 || c->GetCityBuildings()->GetNumBuilding(e) <= 0) continue;
			JObj b;
			b["building"] = J(info->GetType());
			b["name"] = J(info->GetDescription());
			b["slot_type"] = J(SlotTypeName(info->GetGreatWorkSlotType()));
			JArr slots;
			for(int s = 0; s < info->GetGreatWorkCount(); ++s)
			{
				int idx = c->GetCityBuildings()->GetBuildingGreatWork((BuildingClassTypes)info->GetBuildingClassType(), s);
				JObj sl;
				sl["slot"] = J(s);
				if(idx >= 0)
				{
					sl["work"] = J(Localized(pg->GetGreatWorkName(idx).c_str()));
					sl["era"] = J(pg->GetGreatWorkEra(idx).c_str());
					sl["creator"] = J(CivName(pg->GetGreatWorkCreator(idx)));
				}
				else sl["work"] = J("empty");
				slots.push_back(JVal(sl));
			}
			b["slots"] = JVal(slots);
			buildings.push_back(JVal(b));
		}
		if(buildings.empty()) continue;
		JObj cj;
		cj["city"] = J(c->getName().c_str());
		cj["city_id"] = J(c->GetID());
		cj["buildings"] = JVal(buildings);
		cities.push_back(JVal(cj));
	}
	JObj o;
	o["cities"] = JVal(cities);
	o["tourism_per_turn"] = J(me->GetCulture()->GetTourism());
	return JVal(o);
}

MCP_TOOL(ToolMoveGreatWork)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	int c1 = 0, c2 = 0, s1 = 0, s2 = 0;
	if(!RequireInt(args, "from_city_id", c1, err) || !RequireInt(args, "from_slot", s1, err) || !RequireInt(args, "to_city_id", c2, err) || !RequireInt(args, "to_slot", s2, err)) return JVal();
	CvCity* pc1 = me->getCity(c1);
	CvCity* pc2 = me->getCity(c2);
	if(!pc1 || !pc2) { err = "no city with that id"; return JVal(); }
	BuildingTypes b1 = BuildingInCity(pc1, ArgStr(args, "from_building"));
	BuildingTypes b2 = BuildingInCity(pc2, ArgStr(args, "to_building"));
	if(b1 == NO_BUILDING || b2 == NO_BUILDING) { err = "from_building / to_building must be building types the cities have (see great_works)"; return JVal(); }
	int cls1 = GC.getBuildingInfo(b1)->GetBuildingClassType(), cls2 = GC.getBuildingInfo(b2)->GetBuildingClassType();
	if(s1 < 0 || s1 >= GC.getBuildingInfo(b1)->GetGreatWorkCount() || s2 < 0 || s2 >= GC.getBuildingInfo(b2)->GetGreatWorkCount()) { err = "slot out of range"; return JVal(); }
	if(pc1->GetCityBuildings()->GetBuildingGreatWork((BuildingClassTypes)cls1, s1) < 0) { err = "the source slot is empty"; return JVal(); }
	if(GC.getBuildingInfo(b1)->GetGreatWorkSlotType() != GC.getBuildingInfo(b2)->GetGreatWorkSlotType()) { err = "the two buildings hold different kinds of great work (see slot_type in great_works)"; return JVal(); }
	GC.getGame().GetGameCulture()->MoveGreatWorks(me->GetID(), c1, cls1, s1, c2, cls2, s2);
	JObj o;
	o["ok"] = J(true);
	return JVal(o);
}

namespace
{
	// The same numbers the game's route chooser shows: what each end earns per turn.
	JVal RoutePreview(CvPlayer* me, CvCity* origin, CvCity* dest, DomainTypes eDomain, TradeConnectionType eType)
	{
		TradeConnection k;
		k.m_iID = -1;
		k.m_iOriginX = origin->getX();
		k.m_iOriginY = origin->getY();
		k.m_iDestX = dest->getX();
		k.m_iDestY = dest->getY();
		k.m_eOriginOwner = origin->getOwner();
		k.m_eDestOwner = dest->getOwner();
		k.m_eDomain = eDomain;
		k.m_eConnectionType = eType;
		k.m_iTradeUnitLocationIndex = -1;
		k.m_bTradeUnitMovingForward = true;
		k.m_unitID = -1;
		k.m_iCircuitsCompleted = 0;
		k.m_iCircuitsToComplete = 0;
		k.m_iTurnRouteComplete = 0;
		for(int i = 0; i < NUM_YIELD_TYPES; ++i) k.m_aiOriginYields[i] = k.m_aiDestYields[i] = 0;
		CvPlayerTrade* mine = me->GetTrade();
		CvPlayerTrade* theirs = GET_PLAYER(dest->getOwner()).GetTrade();
		JObj o;
		if(eType == TRADE_CONNECTION_INTERNATIONAL)
		{
			JObj you, them;
			you["gold"] = J(mine->GetTradeConnectionValueTimes100(k, YIELD_GOLD, true) / 100.0);
			you["science"] = J(mine->GetTradeConnectionValueTimes100(k, YIELD_SCIENCE, true) / 100.0);
			them["gold"] = J(theirs->GetTradeConnectionValueTimes100(k, YIELD_GOLD, false) / 100.0);
			them["science"] = J(theirs->GetTradeConnectionValueTimes100(k, YIELD_SCIENCE, false) / 100.0);
			o["for_you"] = JVal(you);
			o["for_them"] = JVal(them);
		}
		else
		{
			JObj city;
			city["food"] = J(theirs->GetTradeConnectionValueTimes100(k, YIELD_FOOD, false) / 100.0);
			city["production"] = J(theirs->GetTradeConnectionValueTimes100(k, YIELD_PRODUCTION, false) / 100.0);
			o["for_destination_city"] = JVal(city);
		}
		ReligionTypes eToReligion = NO_RELIGION, eFromReligion = NO_RELIGION;
		int iTo = 0, iFrom = 0;
		origin->GetCityReligions()->WouldExertTradeRoutePressureToward(dest, eToReligion, iTo);
		dest->GetCityReligions()->WouldExertTradeRoutePressureToward(origin, eFromReligion, iFrom);
		if(eToReligion != NO_RELIGION) o["spreads_your_religion_pressure"] = J(iTo / GC.getRELIGION_MISSIONARY_PRESSURE_MULTIPLIER());
		if(eFromReligion != NO_RELIGION) o["brings_their_religion_pressure"] = J(iFrom / GC.getRELIGION_MISSIONARY_PRESSURE_MULTIPLIER());
		return JVal(o);
	}
}

MCP_TOOL(ToolTradeRouteOptions)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvUnit* u = RequireUnit(me, args, err);
	if(!u) return JVal();
	CvPlot* p = u->plot();
	if(!u->canMakeTradeRoute(p)) { err = "this unit cannot start a trade route here (it must be a caravan or cargo ship inside one of your cities with routes left)"; return JVal(); }
	CvCity* origin = p->getPlotCity();
	JObj o;
	o["from"] = J(origin ? origin->getName().c_str() : "?");
	o["routes_remaining"] = J(me->GetTrade()->GetNumTradeRoutesRemaining(false));
	o["routes_possible"] = J((int)me->GetTrade()->GetNumTradeRoutesPossible());
	int iTradeUnits = 0, iLoop = 0;
	for(CvUnit* t = me->firstUnit(&iLoop); t; t = me->nextUnit(&iLoop))
		if(t->isTrade()) ++iTradeUnits;
	o["trade_units_owned"] = J(iTradeUnits);
	o["routes_note"] = J("every caravan and cargo ship you own takes one route slot, idle or not, as does each one being built; disband an idle one to free a slot");
	int iRange = origin ? me->GetTrade()->GetTradeRouteRange(u->getDomainType(), origin) : 0;
	if(origin) o["range"] = J(iRange);
	JArr dests;
	for(int i = 0; i < MAX_CIV_PLAYERS; ++i)
	{
		CvPlayer& owner = GET_PLAYER((PlayerTypes)i);
		if(!owner.isAlive()) continue;
		int iLoop = 0;
		for(CvCity* c = owner.firstCity(&iLoop); c; c = owner.nextCity(&iLoop))
		{
			if(c == origin || !c->plot()->isRevealed(me->getTeam())) continue;
			if(origin && plotDistance(p->getX(), p->getY(), c->getX(), c->getY()) > iRange) continue;
			for(size_t t = 0; t < NUM_CONNECTIONS; ++t)
			{
				if(!u->canMakeTradeRouteAt(p, c->getX(), c->getY(), CONNECTIONS[t].type)) continue;
				JObj d;
				d["city"] = J(c->getName().c_str());
				d["owner"] = J(CivName(c->getOwner()));
				d["x"] = J(c->getX());
				d["y"] = J(c->getY());
				d["type"] = J(CONNECTIONS[t].name);
				d["distance"] = J(plotDistance(p->getX(), p->getY(), c->getX(), c->getY()));
				d["per_turn"] = RoutePreview(me, origin, c, u->getDomainType(), CONNECTIONS[t].type);
				dests.push_back(JVal(d));
			}
		}
	}
	o["destinations"] = JVal(dests);
	return JVal(o);
}

MCP_TOOL(ToolEstablishTradeRoute)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	int x = 0, y = 0;
	CvUnit* u = RequireUnit(me, args, err);
	if(!u || !RequireInt(args, "x", x, err) || !RequireInt(args, "y", y, err)) return JVal();
	TradeConnectionType eType;
	if(!FindConnectionType(ArgStr(args, "type"), eType)) { err = "type must be international, food or production"; return JVal(); }
	CvPlot* dest = GC.getMap().plot(x, y);
	if(!dest || !dest->getPlotCity()) { err = "no city at that tile"; return JVal(); }
	if(!u->canMakeTradeRouteAt(u->plot(), x, y, eType)) { err = "that route is not possible (see trade_route_options)"; return JVal(); }
	{
		NetMessageScope scope;
		u->PushMission(CvTypes::getMISSION_ESTABLISH_TRADE_ROUTE(), GC.getMap().plotNum(x, y), eType, 0, false, true);
	}
	JObj o;
	o["ok"] = J(true);
	o["route_to"] = J(dest->getPlotCity()->getName().c_str());
	return JVal(o);
}

MCP_TOOL(ToolLaunchSpaceship)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	VictoryTypes eSpace = (VictoryTypes)GC.getInfoTypeForString("VICTORY_SPACE_RACE");
	if(eSpace == NO_VICTORY || !GET_TEAM(me->getTeam()).canLaunch(eSpace)) { err = "the spaceship is not ready: build the Apollo Program and all parts, and assemble them in your capital"; return JVal(); }
	me->launch(eSpace);
	JObj o;
	o["ok"] = J(true);
	o["launched"] = J(true);
	return JVal(o);
}
