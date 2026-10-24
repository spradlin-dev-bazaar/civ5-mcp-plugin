#include "CvGameCoreDLLPCH.h"
#include "McpToolSupport.h"
#include "McpToolHandlers.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvCity.h"
#include "CvCityCitizens.h"
#include "CvPlot.h"
#include "CvMap.h"
#include "CvGlobals.h"
#include "CvUnitClasses.h"
#include "CvBuildingClasses.h"
#include "CvTreasury.h"
#include "CvInfos.h"

using namespace McpTool;

// ------------------------------------------------------------------ cities

namespace
{
	// Why the city refused a purchase, checked in the order CvCity::IsCanPurchase checks.
	std::string PurchaseRefusal(CvPlayer* me, CvCity* c, UnitTypes eUnit, BuildingTypes eBuilding, bool bFaith)
	{
		if(c->IsPuppet() && !me->GetPlayerTraits()->IsNoAnnexing()) return "puppet cities cannot buy anything";
		if(c->IsResistance() || c->IsRazing()) return "the city is in resistance or being razed";
		if(eUnit != NO_UNIT && !c->CanPlaceUnitHere(eUnit))
			return "another unit of the same kind already stands on the city tile; move it out first (a city tile holds one military and one civilian unit)";
		std::string why = (eUnit != NO_UNIT) ? TrainRefusal(c, eUnit, true) : BuildRefusal(c, eBuilding, true);
		if(!why.empty()) return (eUnit != NO_UNIT ? "the city cannot train it: " : "the city cannot build it: ") + why;
		if(bFaith)
		{
			int iCost = (eUnit != NO_UNIT) ? c->GetFaithPurchaseCost(eUnit, true) : c->GetFaithPurchaseCost(eBuilding);
			if(iCost < 1) return "it cannot be bought with faith here (it needs the right belief, or the city's majority religion)";
			return McpFormat("it costs %d faith and you have %d, or it needs the city to follow your religion", iCost, me->GetFaith());
		}
		int iCost = (eUnit != NO_UNIT) ? c->GetPurchaseCost(eUnit) : c->GetPurchaseCost(eBuilding);
		if(iCost < 0) return "it cannot be bought with gold";
		return McpFormat("it costs %d gold and you have %d", iCost, me->GetTreasury()->GetGold());
	}
}

MCP_TOOL(ToolBuy)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvCity* c = RequireCity(me, args, err);
	if(!c) return JVal();
	std::string item = ArgStr(args, "item");
	CityItem found = FindCityItem(me, item);
	UnitTypes eUnit = found.eUnit;
	BuildingTypes eBuilding = found.eBuilding;
	if(eUnit == NO_UNIT && eBuilding == NO_BUILDING) { err = "unknown item '" + item + "'; use a UNIT_ or BUILDING_ type from city_options"; return JVal(); }
	bool bFaith = ArgBool(args, "with_faith", false);
	YieldTypes eYield = bFaith ? YIELD_FAITH : YIELD_GOLD;
	int iGoldBefore = me->GetTreasury()->GetGold();
	int iFaithBefore = me->GetFaith();
	if(!c->IsCanPurchase(true, true, eUnit, eBuilding, NO_PROJECT, eYield))
	{
		err = "cannot buy that here now: " + PurchaseRefusal(me, c, eUnit, eBuilding, bFaith);
		if(!found.substituted.empty()) err += "; " + found.substituted;
		return JVal();
	}
	c->Purchase(eUnit, eBuilding, NO_PROJECT, eYield);
	JObj o;
	o["ok"] = J(true);
	if(!found.substituted.empty()) o["substituted"] = J(found.substituted);
	if(bFaith) { o["faith_spent"] = J(iFaithBefore - me->GetFaith()); o["faith"] = J(me->GetFaith()); }
	else { o["gold_spent"] = J(iGoldBefore - me->GetTreasury()->GetGold()); o["gold"] = J(me->GetTreasury()->GetGold()); }
	return JVal(o);
}

MCP_TOOL(ToolBuyTile)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvCity* c = RequireCity(me, args, err);
	int x = 0, y = 0;
	if(!c || !RequireInt(args, "x", x, err) || !RequireInt(args, "y", y, err)) return JVal();
	if(!c->CanBuyPlot(x, y)) { err = "that tile cannot be bought by this city now (too far, owned, or not enough gold)"; return JVal(); }
	int iGoldBefore = me->GetTreasury()->GetGold();
	c->BuyPlot(x, y);
	JObj o;
	o["ok"] = J(true);
	o["gold_spent"] = J(iGoldBefore - me->GetTreasury()->GetGold());
	o["gold"] = J(me->GetTreasury()->GetGold());
	return JVal(o);
}

MCP_TOOL(ToolSetCityFocus)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvCity* c = RequireCity(me, args, err);
	if(!c) return JVal();
	std::string focus = ArgStr(args, "focus");
	CityAIFocusTypes e;
	if(!FindFocus(focus, e)) { err = "unknown focus '" + focus + "'; use one of " + FocusNames(); return JVal(); }
	CvCityCitizens* pCitizens = c->GetCityCitizens();
	pCitizens->SetFocusType(e);
	if(HasArg(args, "avoid_growth")) pCitizens->SetForcedAvoidGrowth(ArgBool(args, "avoid_growth", false));
	JObj o;
	o["ok"] = J(true);
	o["city"] = CityJson(c);
	return JVal(o);
}

namespace
{
	// What every citizen of a city does, as the city screen shows it.
	JVal CitizensJson(CvPlayer* me, CvCity* c)
	{
		CvCityCitizens* pCitizens = c->GetCityCitizens();
		TeamTypes eTeam = me->getTeam();
		JObj o;
		o["city"] = J(c->getName().c_str());
		o["population"] = J(c->getPopulation());
		o["unemployed"] = J(pCitizens->GetNumDefaultSpecialists());
		o["focus"] = J(FocusName(pCitizens->GetFocusType()));
		o["avoid_growth"] = J(pCitizens->IsForcedAvoidGrowth());
		JArr worked, available;
		for(int i = 0; i < NUM_CITY_PLOTS; ++i)
		{
			if(i == CITY_HOME_PLOT) continue;
			CvPlot* p = pCitizens->GetCityPlotFromIndex(i);
			if(!p) continue;
			if(pCitizens->IsWorkingPlot(p))
			{
				JVal v = PlotJson(p, eTeam, me->GetID(), c->getX(), c->getY());
				v.get<JObj>()["locked"] = J(pCitizens->IsForcedWorkingPlot(p));
				worked.push_back(v);
			}
			else if(pCitizens->IsCanWork(p)) available.push_back(PlotJson(p, eTeam, me->GetID(), c->getX(), c->getY()));
		}
		o["worked_tiles"] = JVal(worked);
		o["workable_tiles"] = JVal(available);
		JArr specialists;
		for(int i = 0; i < GC.getNumBuildingInfos(); ++i)
		{
			BuildingTypes e = (BuildingTypes)i;
			CvBuildingEntry* b = GC.getBuildingInfo(e);
			if(!b || c->GetCityBuildings()->GetNumBuilding(e) <= 0) continue;
			int iMax = std::min(pCitizens->GetNumSpecialistsAllowedByBuilding(*b), GC.getMAX_SPECIALISTS_FROM_BUILDING());
			if(iMax <= 0) continue;
			CvSpecialistInfo* s = GC.getSpecialistInfo((SpecialistTypes)b->GetSpecialistType());
			JObj so;
			so["building"] = J(b->GetDescription());
			so["type"] = J(b->GetType());
			so["specialist"] = J(s ? s->GetDescription() : "");
			so["count"] = J(pCitizens->GetNumSpecialistsInBuilding(e));
			so["max"] = J(iMax);
			so["can_add"] = J(pCitizens->IsCanAddSpecialistToBuilding(e));
			specialists.push_back(JVal(so));
		}
		o["specialists"] = JVal(specialists);
		o["how"] = J("city_work_tile locks or frees a tile; city_specialist adds or removes a specialist; unemployed citizens yield nothing until reassigned");
		return JVal(o);
	}
}

MCP_TOOL(ToolCityCitizens)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvCity* c = RequireCity(me, args, err);
	if(!c) return JVal();
	return CitizensJson(me, c);
}

MCP_TOOL(ToolCityWorkTile)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvCity* c = RequireCity(me, args, err);
	if(!c) return JVal();
	int x = 0, y = 0;
	if(!RequireInt(args, "x", x, err) || !RequireInt(args, "y", y, err)) return JVal();
	CvPlot* p = GC.getMap().plot(x, y);
	if(!p) { err = "that tile is off the map"; return JVal(); }
	CvCityCitizens* pCitizens = c->GetCityCitizens();
	int iIndex = pCitizens->GetCityIndexFromPlot(p);
	if(iIndex < 0) { err = "that tile is not within this city's reach"; return JVal(); }
	JObj o;
	if(iIndex == CITY_HOME_PLOT)
	{
		pCitizens->DoAlterWorkingPlot(iIndex);
		o["reset"] = J("all tiles and specialists back to automatic assignment");
	}
	else
	{
		if(p->isCity()) { err = "that tile holds a city"; return JVal(); }
		CvCity* pOther = p->getWorkingCity();
		bool bTakeFromOther = !pCitizens->IsCanWork(p) && p->getOwner() == me->GetID() && pOther != c;
		if(!pCitizens->IsCanWork(p) && !bTakeFromOther) { err = "this city cannot work that tile (not your territory, an enemy unit stands on it, a sea tile before you can work the sea, no yield, or blockaded)"; return JVal(); }
		bool bWas = pCitizens->IsWorkingPlot(p);
		pCitizens->DoAlterWorkingPlot(iIndex);
		if(bTakeFromOther)
		{
			if(p->getWorkingCity() != c) { err = "that tile is locked to a puppet city and cannot be taken from it"; return JVal(); }
			if(pOther) o["taken_from"] = J(pOther->getName().c_str());
			o["note"] = J("the tile now belongs to this city; call city_work_tile again to work it");
		}
		o["tile"] = J(McpFormat("(%d,%d)", x, y));
		o["now_worked"] = J(pCitizens->IsWorkingPlot(p));
		o["locked"] = J(pCitizens->IsForcedWorkingPlot(p));
		if(bWas) o["note"] = J("the citizen taken off that tile is unemployed until you assign it elsewhere");
	}
	o["ok"] = J(true);
	o["citizens"] = CitizensJson(me, c);
	return JVal(o);
}

MCP_TOOL(ToolCitySpecialist)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvCity* c = RequireCity(me, args, err);
	if(!c) return JVal();
	std::string key = ArgStr(args, "building");
	BuildingTypes e = FindBuildingType(key);
	if(e == NO_BUILDING) { err = "unknown building '" + key + "'"; return JVal(); }
	if(c->GetCityBuildings()->GetNumBuilding(e) <= 0) { err = "the city has no " + std::string(BuildingName(e)); return JVal(); }
	CvCityCitizens* pCitizens = c->GetCityCitizens();
	bool bRemove = ArgBool(args, "remove", false);
	if(bRemove)
	{
		if(pCitizens->GetNumSpecialistsInBuilding(e) <= 0) { err = "no specialist works there"; return JVal(); }
		pCitizens->DoRemoveSpecialistFromBuilding(e, true);
		pCitizens->DoAddBestCitizenFromUnassigned();
	}
	else
	{
		if(!pCitizens->IsCanAddSpecialistToBuilding(e)) { err = "no free specialist slot there (every slot is filled, or the city has too few citizens)"; return JVal(); }
		int iBefore = pCitizens->GetNumSpecialistsInBuilding(e);
		pCitizens->DoAddSpecialistToBuilding(e, true);
		if(pCitizens->GetNumSpecialistsInBuilding(e) == iBefore) { err = "the game could not free a citizen to work there"; return JVal(); }
	}
	JObj o;
	o["ok"] = J(true);
	o["citizens"] = CitizensJson(me, c);
	return JVal(o);
}

MCP_TOOL(ToolCityTask)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvCity* c = RequireCity(me, args, err);
	if(!c) return JVal();
	std::string task = ArgStr(args, "task");
	if(task == "annex")
	{
		if(!c->IsPuppet() && !c->IsIgnoreCityForHappiness()) { err = "the city is already yours outright"; return JVal(); }
		if(me->GetPlayerTraits()->IsNoAnnexing()) { err = "your civilization cannot annex cities"; return JVal(); }
		c->DoAnnex();
	}
	else if(task == "puppet")
	{
		if(c->IsPuppet()) { err = "already a puppet"; return JVal(); }
		if(!c->IsIgnoreCityForHappiness()) { err = "only a city whose fate is still undecided after capture can be made a puppet"; return JVal(); }
		c->DoCreatePuppet();
	}
	else if(task == "raze")
	{
		if(!me->canRaze(c)) { err = "this city cannot be razed (capitals and some others never can)"; return JVal(); }
		me->raze(c);
	}
	else if(task == "stop_razing")
	{
		if(!c->IsRazing()) { err = "the city is not being razed"; return JVal(); }
		me->unraze(c);
	}
	else if(task == "liberate")
	{
		PlayerTypes eOrig = c->getOriginalOwner();
		if(eOrig == NO_PLAYER || eOrig == me->GetID() || !me->CanLiberatePlayerCity(eOrig)) { err = "this city cannot be liberated (its original owner cannot take it back)"; return JVal(); }
		me->DoLiberatePlayer(eOrig, c->GetID());
		JObj done;
		done["ok"] = J(true);
		done["liberated_to"] = J(CivName(eOrig));
		return JVal(done);
	}
	else { err = "unknown task '" + task + "'; use annex, puppet, raze, stop_razing or liberate"; return JVal(); }
	JObj o;
	o["ok"] = J(true);
	o["city"] = CityJson(c);
	return JVal(o);
}
