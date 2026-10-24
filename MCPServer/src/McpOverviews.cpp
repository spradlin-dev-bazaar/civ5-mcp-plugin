#include "CvGameCoreDLLPCH.h"
#include "McpToolSupport.h"
#include "McpToolHandlers.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvCity.h"
#include "CvUnit.h"
#include "CvPlot.h"
#include "CvMap.h"
#include "CvGlobals.h"
#include "CvTreasury.h"
#include "CvTradeClasses.h"
#include "CvCultureClasses.h"
#include "CvVotingClasses.h"
#include "CvProjectClasses.h"
#include "CvInfos.h"

// The game's overview screens as tools: what a player reads off the Economic, Trade
// Route, Military and Victory Progress screens, and the city list a player sees on
// the map. Everything here is limited to what the player's own screens show.

using namespace McpTool;

namespace
{
	const char* UsageWord(DomainTypes e) { return e == DOMAIN_SEA ? "sea" : "land"; }

	const char* RouteKind(TradeConnectionType e)
	{
		switch(e)
		{
		case TRADE_CONNECTION_FOOD: return "food";
		case TRADE_CONNECTION_PRODUCTION: return "production";
		default: return "international";
		}
	}

	CvCity* CityAt(int x, int y)
	{
		CvPlot* p = GC.getMap().plot(x, y);
		return p ? p->getPlotCity() : NULL;
	}

	JVal RouteJson(CvPlayer* me, const TradeConnection& k)
	{
		JObj o;
		CvCity* origin = CityAt(k.m_iOriginX, k.m_iOriginY);
		CvCity* dest = CityAt(k.m_iDestX, k.m_iDestY);
		bool bMine = (k.m_eOriginOwner == me->GetID());
		o["from"] = J(origin ? origin->getName().c_str() : "?");
		if(!bMine) o["from_civilization"] = J(CivName(k.m_eOriginOwner));
		o["to"] = J(dest ? dest->getName().c_str() : "?");
		if(k.m_eDestOwner != me->GetID()) o["to_civilization"] = J(CivName(k.m_eDestOwner));
		o["domain"] = J(UsageWord(k.m_eDomain));
		o["kind"] = J(RouteKind(k.m_eConnectionType));
		o["turns_left"] = J(k.m_iTurnRouteComplete - GC.getGame().getGameTurn());
		if(bMine) o["unit_id"] = J(k.m_unitID);
		CvPlayerTrade* mine = me->GetTrade();
		JObj you;
		if(k.m_eConnectionType == TRADE_CONNECTION_INTERNATIONAL)
		{
			you["gold"] = J(mine->GetTradeConnectionValueTimes100(k, YIELD_GOLD, bMine) / 100.0);
			you["science"] = J(mine->GetTradeConnectionValueTimes100(k, YIELD_SCIENCE, bMine) / 100.0);
		}
		else
		{
			you["food"] = J(mine->GetTradeConnectionValueTimes100(k, YIELD_FOOD, false) / 100.0);
			you["production"] = J(mine->GetTradeConnectionValueTimes100(k, YIELD_PRODUCTION, false) / 100.0);
		}
		o["for_you"] = JVal(you);
		return JVal(o);
	}

	JVal ResourceJson(CvPlayer* me, ResourceTypes e)
	{
		JObj o;
		o["resource"] = J(ResourceName(e));
		o["total"] = J(me->getNumResourceTotal(e));
		o["used"] = J(me->getNumResourceUsed(e));
		o["available"] = J(me->getNumResourceAvailable(e));
		int iImport = me->getResourceImport(e), iExport = me->getResourceExport(e);
		if(iImport > 0) o["imported"] = J(iImport);
		if(iExport > 0) o["exported"] = J(iExport);
		return JVal(o);
	}

	const char* InfluenceWord(InfluenceLevelTypes e)
	{
		switch(e)
		{
		case INFLUENCE_LEVEL_EXOTIC: return "exotic";
		case INFLUENCE_LEVEL_FAMILIAR: return "familiar";
		case INFLUENCE_LEVEL_POPULAR: return "popular";
		case INFLUENCE_LEVEL_INFLUENTIAL: return "influential";
		case INFLUENCE_LEVEL_DOMINANT: return "dominant";
		default: return "unknown";
		}
	}

	int OriginalCapitalsHeld(PlayerTypes e)
	{
		int n = 0, iLoop = 0;
		CvPlayer& p = GET_PLAYER(e);
		for(CvCity* c = p.firstCity(&iLoop); c; c = p.nextCity(&iLoop))
			if(c->getOriginalOwner() < MAX_MAJOR_CIVS && c->IsOriginalCapital()) ++n;
		return n;
	}

	// The civilization as the player knows it: by name once met, otherwise only that it exists.
	std::string KnownCivName(CvPlayer* me, PlayerTypes e)
	{
		return GET_TEAM(me->getTeam()).isHasMet(GET_PLAYER(e).getTeam()) ? CivName(e) : "an unmet civilization";
	}
}

MCP_TOOL(ToolEconomicOverview)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvTreasury* t = me->GetTreasury();
	JObj o;
	o["treasury"] = J(t->GetGold());
	o["net_per_turn"] = J(me->calculateGoldRate());
	JObj income;
	// The treasury's own trade-route figure is an engine stub; the city total with and without routes gives it.
	income["cities"] = J(t->GetGoldFromCitiesTimes100(true) / 100.0);
	income["trade_routes"] = J((t->GetGoldFromCitiesTimes100(false) - t->GetGoldFromCitiesTimes100(true)) / 100.0);
	income["city_connections"] = J(t->GetCityConnectionGold());
	income["diplomacy"] = J(t->GetGoldPerTurnFromDiplomacy());
	income["religion"] = J(t->GetGoldPerTurnFromReligion());
	income["traits"] = J(t->GetGoldPerTurnFromTraits());
	income["gross"] = J(t->CalculateGrossGold());
	o["income_per_turn"] = JVal(income);
	int iTotalCosts = t->CalculateInflatedCosts(); // refreshes the cached supply cost read below
	int iFree = 0, iPaid = 0, iBase = 0, iExtra = 0;
	int iUnitCost = t->CalculateUnitCost(iFree, iPaid, iBase, iExtra);
	JObj expenses;
	expenses["unit_maintenance"] = J(iUnitCost);
	expenses["units_free_of_maintenance"] = J(iFree);
	expenses["units_paid_for"] = J(iPaid);
	expenses["unit_supply"] = J(t->GetExpensePerTurnUnitSupply());
	expenses["building_maintenance"] = J(t->GetBuildingGoldMaintenance());
	expenses["improvement_maintenance"] = J(t->GetImprovementGoldMaintenance());
	expenses["total"] = J(iTotalCosts);
	o["expenses_per_turn"] = JVal(expenses);
	JArr strategic, luxuries;
	for(int i = 0; i < GC.getNumResourceInfos(); ++i)
	{
		ResourceTypes e = (ResourceTypes)i;
		CvResourceInfo* info = GC.getResourceInfo(e);
		if(!info) continue;
		if(me->getNumResourceTotal(e) == 0 && me->getNumResourceUsed(e) == 0 && me->getResourceExport(e) == 0) continue;
		if(info->getResourceUsage() == RESOURCEUSAGE_STRATEGIC) strategic.push_back(ResourceJson(me, e));
		else if(info->getResourceUsage() == RESOURCEUSAGE_LUXURY) luxuries.push_back(ResourceJson(me, e));
	}
	o["strategic_resources"] = JVal(strategic);
	o["luxury_resources"] = JVal(luxuries);
	o["resources_note"] = J("available is what is left to build or trade with; a strategic deficit stops units that need it from being built and weakens those that exist; a second copy of a luxury adds no happiness and can be traded");
	return JVal(o);
}

MCP_TOOL(ToolTradeRoutes)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	JObj o;
	JArr yours, toYou;
	const TradeConnectionList& all = GC.getGame().GetGameTrade()->m_aTradeConnections;
	for(size_t i = 0; i < all.size(); ++i)
	{
		const TradeConnection& k = all[i];
		if(k.m_eOriginOwner == NO_PLAYER) continue;
		if(k.m_eOriginOwner == me->GetID()) yours.push_back(RouteJson(me, k));
		else if(k.m_eDestOwner == me->GetID()) toYou.push_back(RouteJson(me, k));
	}
	o["your_routes"] = JVal(yours);
	o["routes_to_your_cities"] = JVal(toYou);
	o["routes_possible"] = J((int)me->GetTrade()->GetNumTradeRoutesPossible());
	o["routes_remaining"] = J(me->GetTrade()->GetNumTradeRoutesRemaining(false));
	CvTreasury* t = me->GetTreasury();
	o["gold_per_turn_from_routes"] = J((t->GetGoldFromCitiesTimes100(false) - t->GetGoldFromCitiesTimes100(true)) / 100.0);
	return JVal(o);
}

MCP_TOOL(ToolMilitaryOverview)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	JObj o;
	JObj supply;
	supply["units_supplied"] = J(me->GetNumUnitsSupplied());
	supply["from_handicap"] = J(me->GetNumUnitsSuppliedByHandicap());
	supply["from_cities"] = J(me->GetNumUnitsSuppliedByCities());
	supply["from_population"] = J(me->GetNumUnitsSuppliedByPopulation());
	supply["units_over_supply"] = J(me->GetNumUnitsOutOfSupply());
	if(me->GetNumUnitsOutOfSupply() > 0) supply["production_penalty_percent"] = J(-me->GetUnitProductionMaintenanceMod());
	o["supply"] = JVal(supply);
	int iFree = 0, iPaid = 0, iBase = 0, iExtra = 0;
	int iUnitCost = me->GetTreasury()->CalculateUnitCost(iFree, iPaid, iBase, iExtra);
	JObj upkeep;
	upkeep["gold_per_turn"] = J(iUnitCost);
	upkeep["units_free"] = J(iFree);
	upkeep["units_paid_for"] = J(iPaid);
	o["upkeep"] = JVal(upkeep);
	o["military_units"] = J(me->getNumMilitaryUnits());
	o["all_units"] = J(me->getNumUnits());
	o["units"] = JVal(UnitsJson(me));
	return JVal(o);
}

MCP_TOOL(ToolVictoryProgress)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvGame& g = GC.getGame();
	CvTeam& myTeam = GET_TEAM(me->getTeam());
	JObj o;
	JArr enabled;
	for(int i = 0; i < GC.getNumVictoryInfos(); ++i)
		if(g.isVictoryValid((VictoryTypes)i)) enabled.push_back(J(GC.getVictoryInfo((VictoryTypes)i)->GetDescription()));
	o["victories_enabled"] = JVal(enabled);

	JObj dom;
	int iTotal = 0, iUnmetHeld = 0;
	JArr holders;
	for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		PlayerTypes e = (PlayerTypes)i;
		if(!GET_PLAYER(e).isAlive()) continue;
		int n = OriginalCapitalsHeld(e);
		iTotal += n;
		if(n == 0) continue;
		if(!myTeam.isHasMet(GET_PLAYER(e).getTeam())) { iUnmetHeld += n; continue; }
		JObj h;
		h["civilization"] = J(CivName(e));
		h["original_capitals_held"] = J(n);
		holders.push_back(JVal(h));
	}
	if(iUnmetHeld > 0) dom["held_by_civilizations_you_have_not_met"] = J(iUnmetHeld);
	int iTeamHeld = 0;
	for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
		if(GET_PLAYER((PlayerTypes)i).isAlive() && GET_PLAYER((PlayerTypes)i).getTeam() == me->getTeam()) iTeamHeld += OriginalCapitalsHeld((PlayerTypes)i);
	dom["original_capitals_in_play"] = J(iTotal);
	dom["held_by_your_team"] = J(iTeamHeld);
	dom["holders"] = JVal(holders);
	dom["rule"] = J("the team holding every original capital still standing wins");
	o["domination"] = JVal(dom);

	JObj sci;
	JArr parts;
	for(int i = 0; i < GC.getNumProjectInfos(); ++i)
	{
		CvProjectEntry* info = GC.getProjectInfo((ProjectTypes)i);
		if(!info || (!info->IsSpaceship() && info->GetVictoryPrereq() == NO_VICTORY)) continue;
		JObj p;
		p["project"] = J(info->GetDescription());
		p["built"] = J(myTeam.getProjectCount((ProjectTypes)i));
		if(info->GetMaxTeamInstances() > 0) p["needed"] = J(info->GetMaxTeamInstances());
		parts.push_back(JVal(p));
	}
	sci["your_projects"] = JVal(parts);
	o["science"] = JVal(sci);

	JObj cul;
	cul["civilizations_you_are_influential_over"] = J(me->GetCulture()->GetNumCivsInfluentialOn());
	cul["needed"] = J(me->GetCulture()->GetNumCivsToBeInfluentialOn());
	JArr influence;
	for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		PlayerTypes e = (PlayerTypes)i;
		if(e == me->GetID() || !GET_PLAYER(e).isAlive() || !myTeam.isHasMet(GET_PLAYER(e).getTeam())) continue;
		JObj c;
		c["civilization"] = J(CivName(e));
		c["your_influence"] = J(InfluenceWord(me->GetCulture()->GetInfluenceLevel(e)));
		influence.push_back(JVal(c));
	}
	cul["influence"] = JVal(influence);
	o["culture"] = JVal(cul);

	JObj dip;
	CvLeague* pLeague = g.GetGameLeagues()->GetActiveLeague();
	if(pLeague)
	{
		dip["votes_needed_to_win"] = J(g.GetVotesNeededForDiploVictory());
		dip["your_votes"] = J(pLeague->CalculateStartingVotesForMember(me->GetID()));
	}
	else dip["note"] = J("no World Congress yet");
	o["diplomatic"] = JVal(dip);
	return JVal(o);
}

MCP_TOOL(ToolKnownCities)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	TeamTypes eTeam = me->getTeam();
	JArr list;
	for(int i = 0; i < MAX_CIV_PLAYERS; ++i)
	{
		PlayerTypes e = (PlayerTypes)i;
		CvPlayer& p = GET_PLAYER(e);
		if(e == me->GetID() || !p.isAlive()) continue;
		int iLoop = 0;
		for(CvCity* c = p.firstCity(&iLoop); c; c = p.nextCity(&iLoop))
		{
			if(!c->isRevealed(eTeam, false)) continue;
			CvPlot* plot = c->plot();
			bool bVisible = plot->isVisible(eTeam);
			JObj o;
			o["name"] = J(c->getName().c_str());
			if(p.getTeam() == eTeam) o["teammate"] = J(true);
			o["x"] = J(c->getX());
			o["y"] = J(c->getY());
			PlayerTypes eKnownOwner = bVisible ? c->getOwner() : plot->getRevealedOwner(eTeam);
			o["owner"] = J(eKnownOwner != NO_PLAYER ? CivName(eKnownOwner) : "unknown: it changed hands while out of sight");
			if(eKnownOwner != NO_PLAYER)
			{
				CvTeam& ownerTeam = GET_TEAM(GET_PLAYER(eKnownOwner).getTeam());
				o["at_war_with_you"] = J(ownerTeam.isAtWar(eTeam));
				if(GET_PLAYER(eKnownOwner).isMinorCiv()) o["city_state"] = J(true);
			}
			if(c->getOriginalOwner() < MAX_MAJOR_CIVS && c->IsOriginalCapital()) o["original_capital_of"] = J(KnownCivName(me, c->getOriginalOwner()));
			o["visible_now"] = J(bVisible);
			if(bVisible)
			{
				o["population"] = J(c->getPopulation());
				o["hp"] = J(c->GetMaxHitPoints() - c->getDamage());
				o["max_hp"] = J(c->GetMaxHitPoints());
				o["defense_strength"] = J(c->getStrengthValue() / 100);
			}
			list.push_back(JVal(o));
		}
	}
	JObj o;
	o["cities"] = JVal(list);
	o["note"] = J("only cities your civilization has seen; the owner is the last one you saw, and hit points, population and strength appear only while the tile is in sight");
	return JVal(o);
}
