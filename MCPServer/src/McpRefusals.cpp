#include "CvGameCoreDLLPCH.h"
#include "McpToolSupport.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvCity.h"
#include "CvUnit.h"
#include "CvPlot.h"
#include "CvGlobals.h"
#include "CvGameCoreUtils.h"
#include "CvInternalGameCoreUtils.h"
#include "CvBuildingClasses.h"
#include "CvUnitClasses.h"
#include "CvTechClasses.h"
#include "CvPolicyClasses.h"
#include "CvTreasury.h"

// Why the game refuses something, in words. The engine's own checks only carry a
// reason for a few cases, and those that do only collect it: with a text sink the
// check does not fail. So every helper here asks for the verdict first, then for the
// engine's text, and names the silent rules itself in the engine's order.

namespace McpTool
{
	// The unit on dest (or sharing u's own tile) that keeps u from being there, by
	// CvPlot::getNumFriendlyUnitsOfType's rules: a non-enemy of the same kind (military or civilian), or
	// another civilization's unit of any kind; trade units stack freely. The first match is the blocker
	// because the game allows one unit of a kind per tile. A unit the player cannot see is skipped, so a
	// hidden blocker falls through to the pathing reasons unnamed.
	const CvUnit* BlockingUnit(const CvUnit* u, const CvPlot* dest)
	{
		if(u->isTrade() || !dest->isVisible(u->getTeam())) return NULL;
		bool bWouldEmbark = dest != u->plot() && dest->isWater() && u->canEmbarkOnto(*u->plot(), *dest);
		for(const IDInfo* node = dest->headUnitNode(); node != NULL; node = dest->nextUnitNode(node))
		{
			const CvUnit* other = ::getUnit(*node);
			if(!other || other == u || other->isCargo() || other->isInvisible(u->getTeam(), false) || GET_TEAM(u->getTeam()).isAtWar(other->getTeam())) continue;
			if(other->getOwner() != u->getOwner() || other->AreUnitsOfSameType(*u, bWouldEmbark)) return other;
		}
		return NULL;
	}

	namespace
	{
		// Who the unit keeping u off a tile is, from u's owner's side: a teammate's unit is named as
		// such, because a model that only sees "another civilization" assumes it is not an ally's.
		std::string Sharer(const CvUnit* u, const CvUnit* other)
		{
			if(other->getOwner() == u->getOwner()) return "one of your units of the same kind";
			std::string owner;
			if(other->getTeam() == u->getTeam()) owner = "your teammate " + CivName(other->getOwner()) + "'s";
			else if(!GET_TEAM(u->getTeam()).isHasMet(other->getTeam())) owner = "another civilization's";
			else owner = CivName(other->getOwner()) + "'s";
			return owner + " " + UnitTypeName(other->getUnitType());
		}

		std::string SharingRule(const CvUnit* u, const CvUnit* other)
		{
			if(other->getOwner() == u->getOwner()) return "a tile holds one military and one civilian unit";
			if(other->getTeam() == u->getTeam()) return "units of different civilizations cannot share a tile, teammates included";
			return "units of civilizations you are not at war with cannot share a tile, and only an attack moves an enemy";
		}

		std::string Stacked(const CvUnit* u, const CvPlot* p)
		{
			const CvUnit* other = BlockingUnit(u, p);
			if(!other) return "another unit shares this tile; a tile holds one military and one civilian unit";
			return Sharer(u, other) + " shares this tile; " + SharingRule(u, other);
		}

		std::string TechNeeded(TechTypes e)
		{
			return std::string("needs the technology ") + TechName(e);
		}

		bool HasTech(const CvPlayer& p, int iTech)
		{
			return iTech == NO_TECH || GET_TEAM(p.getTeam()).GetTeamTechs()->HasTech((TechTypes)iTech);
		}
	}

	// bPurchase: the question is whether it can be bought, so production cost does not count.
	std::string TrainRefusal(CvCity* c, UnitTypes eUnit, bool bPurchase)
	{
		if(c->canTrain(eUnit, false, false, bPurchase, bPurchase)) return "";
		CvString why;
		c->canTrain(eUnit, false, false, bPurchase, bPurchase, &why);
		std::string s = StripMarkup(why.c_str());
		if(!s.empty()) return s;
		CvUnitEntry* info = GC.getUnitInfo(eUnit);
		if(!info) return "no such unit";
		CvPlayer& p = GET_PLAYER(c->getOwner());
		UnitClassTypes eClass = (UnitClassTypes)info->GetUnitClassType();
		if(p.GetPlayerTraits()->NoTrain(eClass)) return "your civilization cannot train this kind of unit";
		if(p.getCivilizationInfo().getCivilizationUnits(eClass) != eUnit) return "your civilization has its own unit in place of this one";
		if(!bPurchase && info->GetProductionCost() == -1) return info->IsRequiresFaithPurchaseEnabled() ? "it cannot be built with production; buy it with faith" : "it cannot be built with production; buy it";
		if((info->IsFound() || info->IsFoundAbroad()) && GC.getGame().isOption(GAMEOPTION_ONE_CITY_CHALLENGE)) return "the One City Challenge forbids settlers";
		PolicyTypes ePolicy = (PolicyTypes)info->GetPolicyType();
		if(ePolicy != NO_POLICY && !p.GetPlayerPolicies()->HasPolicy(ePolicy)) return std::string("needs the policy ") + GC.getPolicyInfo(ePolicy)->GetDescription();
		if(GC.getGame().isOption(GAMEOPTION_NO_RELIGION) && (info->IsFoundReligion() || info->IsSpreadReligion() || info->IsRemoveHeresy())) return "religion is disabled in this game";
		if(!HasTech(p, info->GetPrereqAndTech())) return TechNeeded((TechTypes)info->GetPrereqAndTech());
		for(int i = 0; i < GC.getNUM_UNIT_AND_TECH_PREREQS(); ++i)
			if(!HasTech(p, info->GetPrereqAndTechs(i))) return TechNeeded((TechTypes)info->GetPrereqAndTechs(i));
		if(info->GetObsoleteTech() != NO_TECH && HasTech(p, info->GetObsoleteTech())) return "it is obsolete; build its replacement";
		if(GC.getGame().isUnitClassMaxedOut(eClass) || GET_TEAM(p.getTeam()).isUnitClassMaxedOut(eClass) || p.isUnitClassMaxedOut(eClass)) return "the limit for this kind of unit has been reached";
		if(info->GetWorkRate() > 0 && info->GetDomainType() == DOMAIN_LAND && p.GetMaxNumBuilders() > -1 && p.GetNumBuilders() >= p.GetMaxNumBuilders()) return "you already have as many workers as allowed";
		if(info->GetNukeDamageLevel() > 0 && !GC.getGame().isNukesValid()) return "nuclear weapons are not allowed in this game";
		if(!bPurchase && info->IsPurchaseOnly()) return "it can only be bought, not built";
		if((info->IsFound() || info->IsFoundAbroad()) && c->getPopulation() < GC.getCITY_MIN_SIZE_FOR_SETTLERS()) return McpFormat("the city must reach size %d to train a settler", GC.getCITY_MIN_SIZE_FOR_SETTLERS());
		for(int i = 0; i < GC.getNumBuildingClassInfos(); ++i)
		{
			if(!info->GetBuildingClassRequireds(i)) continue;
			BuildingTypes ePrereq = (BuildingTypes)p.getCivilizationInfo().getCivilizationBuildings(i);
			if(ePrereq != NO_BUILDING && c->GetCityBuildings()->GetNumBuilding(ePrereq) == 0) return std::string("needs ") + GC.getBuildingInfo(ePrereq)->GetDescription() + " in this city first";
		}
		if(info->GetDomainType() == DOMAIN_SEA && !c->isCoastal(info->GetMinAreaSize())) return "needs a coastal city on a large enough sea";
		return "the game's rules do not allow it here; it is not listed in city_options";
	}

	// bPurchase: the question is whether it can be bought, so production cost does not count, and a
	// building the city is already working on may be bought, judged as CvCity::IsCanPurchase judges it.
	std::string BuildRefusal(CvCity* c, BuildingTypes eBuilding, bool bPurchase)
	{
		if(bPurchase && c->getFirstBuildingOrder(eBuilding) != -1 && c->canConstruct(eBuilding, true, false, true)) return "";
		if(c->canConstruct(eBuilding, false, false, bPurchase, bPurchase)) return "";
		CvString why;
		c->canConstruct(eBuilding, false, false, bPurchase, bPurchase, &why);
		std::string s = StripMarkup(why.c_str());
		if(!s.empty()) return s;
		CvBuildingEntry* info = GC.getBuildingInfo(eBuilding);
		if(!info) return "no such building";
		CvPlayer& p = GET_PLAYER(c->getOwner());
		CvTeam& t = GET_TEAM(p.getTeam());
		BuildingClassTypes eClass = (BuildingClassTypes)info->GetBuildingClassType();
		if(GC.getGame().isOption(GAMEOPTION_NO_ESPIONAGE) && info->IsEspionage()) return "espionage is disabled in this game";
		if(p.getCivilizationInfo().getCivilizationBuildings(eClass) != eBuilding) return "your civilization has its own building in place of this one";
		if(c->GetCityBuildings()->GetNumBuilding(eBuilding) > 0) return "the city already has it";
		if(!bPurchase && c->getFirstBuildingOrder(eBuilding) != -1) return "it is already in this city's build queue";
		if(!bPurchase && info->GetProductionCost() == -1) return "it cannot be built with production";
		PolicyBranchTypes eBranch = (PolicyBranchTypes)info->GetPolicyBranchType();
		if(eBranch != NO_POLICY_BRANCH_TYPE && !p.GetPlayerPolicies()->IsPolicyBranchUnlocked(eBranch)) return std::string("needs the ") + GC.getPolicyBranchInfo(eBranch)->GetDescription() + " policy branch";
		if(!HasTech(p, info->GetPrereqAndTech())) return TechNeeded((TechTypes)info->GetPrereqAndTech());
		for(int i = 0; i < GC.getNUM_BUILDING_AND_TECH_PREREQS(); ++i)
			if(!HasTech(p, info->GetPrereqAndTechs(i))) return TechNeeded((TechTypes)info->GetPrereqAndTechs(i));
		if(t.isObsoleteBuilding(eBuilding)) return "it is obsolete";
		BuildingClassTypes eReplacementClass = (BuildingClassTypes)info->GetReplacementBuildingClass();
		if(eReplacementClass != NO_BUILDINGCLASS)
		{
			BuildingTypes eReplacement = (BuildingTypes)p.getCivilizationInfo().getCivilizationBuildings(eReplacementClass);
			if(eReplacement != NO_BUILDING && p.canConstruct(eReplacement)) return std::string("its replacement, ") + GC.getBuildingInfo(eReplacement)->GetDescription() + ", can be built instead";
		}
		if(info->GetVictoryPrereq() != NO_VICTORY && !GC.getGame().isVictoryValid((VictoryTypes)info->GetVictoryPrereq())) return "the victory it serves is disabled in this game";
		if(info->GetMaxStartEra() != NO_ERA && GC.getGame().getStartEra() > info->GetMaxStartEra()) return "not available with this game's starting era";
		if(GC.getGame().isBuildingClassMaxedOut(eClass)) return "it has already been built somewhere in the world";
		if(t.isBuildingClassMaxedOut(eClass)) return "your team already has it";
		if(p.isBuildingClassMaxedOut(eClass)) return "you already have one; only one is allowed";
		if(p.getNumCities() < info->GetNumCitiesPrereq()) return McpFormat("needs %d cities", info->GetNumCitiesPrereq());
		if(p.getHighestUnitLevel() < info->GetUnitLevelPrereq()) return McpFormat("needs a unit of level %d", info->GetUnitLevelPrereq());
		if(info->IsWater() && !c->isCoastal(info->GetMinAreaSize())) return "needs a coastal city";
		if(info->IsRiver() && !c->plot()->isRiver()) return "needs a city on a river";
		if(info->IsFreshWater() && !c->plot()->isFreshWater()) return "needs a city next to fresh water";
		if(info->IsHill() && !c->plot()->isHills()) return "needs a city on a hill";
		if(info->IsMountain() || info->IsNearbyMountainRequired())
		{
			bool bMountain = false;
			for(int i = 0; i < NUM_DIRECTION_TYPES && !bMountain; ++i)
			{
				CvPlot* adj = plotDirection(c->getX(), c->getY(), (DirectionTypes)i);
				bMountain = adj && adj->isMountain();
			}
			if(!bMountain) return "needs a mountain next to the city";
		}
		if(!c->isValidBuildingLocation(eBuilding)) return "the city's location does not allow it";
		if(info->IsRequiresHolyCity() && !c->GetCityReligions()->IsHolyCityAnyReligion()) return "only a holy city can build it";
		if(info->IsNoOccupiedUnhappiness() && !c->IsOccupied()) return "only an occupied city (conquered and not annexed into contentment) needs it";
		for(int i = 0; i < GC.getNumBuildingClassInfos(); ++i)
		{
			if(!info->IsBuildingClassNeededInCity(i)) continue;
			BuildingTypes ePrereq = (BuildingTypes)p.getCivilizationInfo().getCivilizationBuildings(i);
			if(ePrereq != NO_BUILDING && c->GetCityBuildings()->GetNumBuilding(ePrereq) == 0) return std::string("needs ") + GC.getBuildingInfo(ePrereq)->GetDescription() + " in this city first";
		}
		const CvBuildingClassInfo& kClass = info->GetBuildingClassInfo();
		if(!kClass.isNoLimit())
		{
			if(isWorldWonderClass(kClass) && c->isWorldWondersMaxed()) return "the city cannot take on another world wonder";
			if(isTeamWonderClass(kClass) && c->isTeamWondersMaxed()) return "the city cannot take on another team wonder";
			if(isNationalWonderClass(kClass) && c->isNationalWondersMaxed()) return "the city cannot take on another national wonder";
			if(!isWorldWonderClass(kClass) && !isTeamWonderClass(kClass) && !isNationalWonderClass(kClass) && c->isBuildingsMaxed()) return "the city has reached its building limit";
		}
		for(int i = 0; i < GC.getNumBuildingClassInfos(); ++i)
		{
			int iLocked = info->GetLockedBuildingClasses(i);
			if(iLocked == NO_BUILDINGCLASS) continue;
			BuildingTypes eLocked = (BuildingTypes)p.getCivilizationInfo().getCivilizationBuildings(iLocked);
			if(eLocked != NO_BUILDING && c->GetCityBuildings()->GetNumBuilding(eLocked) > 0) return std::string("a building the city already has, ") + GC.getBuildingInfo(eLocked)->GetDescription() + ", rules it out";
		}
		if(info->GetMutuallyExclusiveGroup() != -1)
			for(int i = 0; i < GC.getNumBuildingInfos(); ++i)
			{
				CvBuildingEntry* other = GC.getBuildingInfo((BuildingTypes)i);
				if(other && other->GetMutuallyExclusiveGroup() == info->GetMutuallyExclusiveGroup() && c->GetCityBuildings()->GetNumBuilding((BuildingTypes)i) > 0)
					return std::string("a building the city already has, ") + other->GetDescription() + ", rules it out";
			}
		return "the game's rules do not allow it here; it is not listed in city_options";
	}

	std::string RestRefusal(CvUnit* u, CvPlot* p, const std::string& action)
	{
		if(u->isWaiting()) return "the unit is already resting (fortified, asleep or on alert)";
		if(p->getNumFriendlyUnitsOfType(u) > GC.getPLOT_UNIT_LIMIT()) return Stacked(u, p) + "; one must move away before this unit can rest";
		if(action == "fortify")
		{
			if(!u->IsCombatUnit()) return "non-combat units sleep instead of fortifying";
			if(u->noDefensiveBonus()) return "this unit gets no defensive bonus, so it cannot fortify; use sleep instead";
		}
		if(action == "sleep" && u->IsCombatUnit() && !u->noDefensiveBonus()) return "combat units fortify instead of sleeping";
		return "";
	}

	std::string MoveRefusal(CvUnit* u, CvPlot* dest)
	{
		const CvUnit* blocker = BlockingUnit(u, dest);
		if(blocker) return Sharer(u, blocker) + " stands on that tile; " + SharingRule(u, blocker);
		if(dest->isOwned() && dest->getTeam() != u->getTeam() && !u->canEnterTerritory(dest->getTeam(), false, dest->isCity()))
			return "that tile belongs to " + CivName(dest->getOwner()) + ", whose borders are closed to you (no open borders and not at war)";
		if(!dest->isValidDomainForLocation(*u)) return dest->isWater() ? "that is water: this unit cannot embark yet, or no sea route exists" : "that is land: a ship cannot go there";
		if(!u->canEnterTerrain(*dest)) return "this unit cannot enter that terrain";
		std::string why = "no path: units in the way, or land you may not enter, block every route";
		if(u->IsAutomated()) why += "; the unit is also automated: unit_action stop_automation, then try again";
		return why;
	}

	std::string UpgradeRefusal(CvUnit* u)
	{
		if(u->CanUpgradeRightNow(false)) return "";
		if(u->movesLeft() <= 0) return "no moves left; a unit upgrades before it moves";
		UnitTypes eUp = u->GetUpgradeUnitType();
		if(eUp == NO_UNIT) return "no upgrade exists for this unit";
		CvUnitEntry* info = GC.getUnitInfo(eUp);
		CvPlayer& p = GET_PLAYER(u->getOwner());
		if(info && !HasTech(p, info->GetPrereqAndTech())) return TechNeeded((TechTypes)info->GetPrereqAndTech());
		if(u->plot()->getNumFriendlyUnitsOfType(u) > GC.getPLOT_UNIT_LIMIT()) return Stacked(u, u->plot());
		if(u->plot()->getOwner() != u->getOwner()) return "the unit must stand inside your own borders";
		int iCost = u->upgradePrice(eUp);
		if(p.GetTreasury()->GetGold() < iCost) return McpFormat("it costs %d gold and you have %d", iCost, p.GetTreasury()->GetGold());
		if(info)
			for(int i = 0; i < GC.getNumResourceInfos(); ++i)
			{
				int iNeeded = info->GetResourceQuantityRequirement((ResourceTypes)i);
				if(iNeeded <= 0) continue;
				int iAvailable = p.getNumResourceAvailable((ResourceTypes)i) + u->getUnitInfo().GetResourceQuantityRequirement((ResourceTypes)i);
				if(iAvailable < iNeeded) return McpFormat("needs %d %s and you have %d to spare", iNeeded, ResourceName((ResourceTypes)i), iAvailable);
			}
		return "the game refused the upgrade";
	}

	ProcessTypes FindProcessType(const std::string& key)
	{
		if(key.empty()) return NO_PROCESS;
		for(int i = 0; i < GC.getNumProcessInfos(); ++i)
		{
			CvProcessInfo* info = GC.getProcessInfo((ProcessTypes)i);
			if(info && MatchesKey(info->GetType(), info->GetDescription(), key)) return (ProcessTypes)i;
		}
		return NO_PROCESS;
	}
}
