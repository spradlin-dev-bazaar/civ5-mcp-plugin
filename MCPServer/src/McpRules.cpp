#include "CvGameCoreDLLPCH.h"
#include <math.h>
#include "McpRules.h"
#include "McpToolHandlers.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvGlobals.h"
#include "CvInfos.h"
#include "CvTechClasses.h"
#include "CvUnitClasses.h"
#include "CvBuildingClasses.h"
#include "CvImprovementClasses.h"
#include "CvPromotionClasses.h"
#include "CvPolicyClasses.h"
#include "CvProjectClasses.h"
#include "CvBeliefClasses.h"
#include "CvTraitClasses.h"

using namespace McpTool;

namespace
{
	bool StartsWith(const std::string& s, const char* prefix)
	{
		return _strnicmp(s.c_str(), prefix, strlen(prefix)) == 0;
	}

	std::string Named(const char* szName, const char* szType)
	{
		return std::string(szName) + " (" + szType + ")";
	}

	std::string Ref(CvBaseInfo* info)
	{
		return info ? Named(info->GetDescription(), info->GetType()) : std::string();
	}

	CvBaseInfo* TechInfo(int e) { return (e != NO_TECH) ? GC.getTechInfo((TechTypes)e) : NULL; }
	CvBaseInfo* UnitInfo(int e) { return (e != NO_UNIT) ? GC.getUnitInfo((UnitTypes)e) : NULL; }
	CvBaseInfo* BuildingInfo(int e) { return (e != NO_BUILDING) ? GC.getBuildingInfo((BuildingTypes)e) : NULL; }

	void PutRef(JObj& o, const char* k, CvBaseInfo* info) { if(info) o[k] = J(Ref(info)); }
	void AddRef(JArr& a, CvBaseInfo* info) { if(info) a.push_back(J(Ref(info))); }

	std::string YieldKey(int iYield)
	{
		CvYieldInfo* info = GC.getYieldInfo((YieldTypes)iYield);
		std::string key = info ? info->GetType() : "";
		if(StartsWith(key, "YIELD_")) key.erase(0, 6);
		for(size_t i = 0; i < key.size(); ++i) key[i] = (char)tolower((unsigned char)key[i]);
		return key;
	}

	void AddYield(JObj& o, int iYield, int iValue)
	{
		if(iValue != 0) o[YieldKey(iYield)] = J(iValue);
	}

	void PutIfAny(JObj& o, const char* k, const JObj& v) { if(!v.empty()) o[k] = JVal(v); }
	void PutIfAny(JObj& o, const char* k, const JArr& v) { if(!v.empty()) o[k] = JVal(v); }
	void PutIfSet(JObj& o, const char* k, int v) { if(v != 0) o[k] = J(v); }
	void PutHelp(JObj& o, const char* szHelpKey)
	{
		std::string help = Localized(szHelpKey);
		if(!help.empty()) o["help"] = J(help);
	}

	CvPlayer* ModelPlayer()
	{
		std::string ignored;
		return Me(ignored);
	}

	void PutCostsNote(JObj& o, const CvPlayer* me)
	{
		o["costs"] = J(me ? "this game's, for your civilization" : "the base values in the game's tables: you hold no seat yet");
	}

	void PutProductionCost(JObj& o, int iCost, const CvPlayer* me)
	{
		if(iCost <= 0) return;
		o["production_cost"] = J(iCost);
		PutCostsNote(o, me);
	}

	std::string CivilizationName(CivilizationTypes e)
	{
		CvCivilizationInfo* info = (e != NO_CIVILIZATION) ? GC.getCivilizationInfo(e) : NULL;
		return info ? Localized(info->getShortDescriptionKey()) : "";
	}

	std::string CivilizationRef(CivilizationTypes e)
	{
		CvCivilizationInfo* info = (e != NO_CIVILIZATION) ? GC.getCivilizationInfo(e) : NULL;
		return info ? Named(CivilizationName(e).c_str(), info->GetType()) : "";
	}

	bool IsMajorCiv(CvCivilizationInfo* civ)
	{
		return civ && (civ->isPlayable() || civ->isAIPlayable());
	}

	int DefaultUnit(int iClass)
	{
		CvUnitClassInfo* cls = (iClass != NO_UNITCLASS) ? GC.getUnitClassInfo((UnitClassTypes)iClass) : NULL;
		return cls ? cls->getDefaultUnitIndex() : NO_UNIT;
	}

	int DefaultBuilding(int iClass)
	{
		CvBuildingClassInfo* cls = (iClass != NO_BUILDINGCLASS) ? GC.getBuildingClassInfo((BuildingClassTypes)iClass) : NULL;
		return cls ? cls->getDefaultBuildingIndex() : NO_BUILDING;
	}

	CvCivilizationInfo* CivOf(const CvPlayer* me)
	{
		return me ? &me->getCivilizationInfo() : NULL;
	}

	int CivUnit(CvCivilizationInfo* civ, int iClass)
	{
		if(iClass == NO_UNITCLASS) return NO_UNIT;
		int eOwn = civ ? civ->getCivilizationUnits(iClass) : NO_UNIT;
		return (eOwn != NO_UNIT) ? eOwn : DefaultUnit(iClass);
	}

	int CivBuilding(CvCivilizationInfo* civ, int iClass)
	{
		int eOwn = civ ? civ->getCivilizationBuildings(iClass) : NO_BUILDING;
		return (eOwn != NO_BUILDING) ? eOwn : DefaultBuilding(iClass);
	}

	CivilizationTypes UniqueUnitOwner(UnitTypes e)
	{
		int iClass = GC.getUnitInfo(e)->GetUnitClassType();
		if(DefaultUnit(iClass) == (int)e) return NO_CIVILIZATION;
		for(int i = 0; i < GC.getNumCivilizationInfos(); ++i)
		{
			CvCivilizationInfo* civ = GC.getCivilizationInfo((CivilizationTypes)i);
			if(IsMajorCiv(civ) && civ->getCivilizationUnits(iClass) == (int)e) return (CivilizationTypes)i;
		}
		return NO_CIVILIZATION;
	}

	CivilizationTypes UniqueBuildingOwner(BuildingTypes e)
	{
		int iClass = GC.getBuildingInfo(e)->GetBuildingClassType();
		if(DefaultBuilding(iClass) == (int)e) return NO_CIVILIZATION;
		for(int i = 0; i < GC.getNumCivilizationInfos(); ++i)
		{
			CvCivilizationInfo* civ = GC.getCivilizationInfo((CivilizationTypes)i);
			if(IsMajorCiv(civ) && civ->getCivilizationBuildings(iClass) == (int)e) return (CivilizationTypes)i;
		}
		return NO_CIVILIZATION;
	}

	// Another civilization's unique unit or building answers with that civilization's versions, not the player's.
	CvCivilizationInfo* CivFor(CivilizationTypes eOwner, const CvPlayer* me)
	{
		return (eOwner != NO_CIVILIZATION) ? GC.getCivilizationInfo(eOwner) : CivOf(me);
	}

	void PutUnique(JObj& o, CivilizationTypes eOwner, CvBaseInfo* replaced)
	{
		if(eOwner == NO_CIVILIZATION) return;
		o["unique_to"] = J(CivilizationRef(eOwner));
		PutRef(o, "replaces", replaced);
	}

	const char* UnitCombatName(int e)
	{
		CvBaseInfo* info = (e != NO_UNITCOMBAT) ? GC.getUnitCombatClassInfo((UnitCombatTypes)e) : NULL;
		return info ? info->GetDescription() : "";
	}

	int ProductionCost(UnitTypes e, const CvPlayer* me)
	{
		int iCost = GC.getUnitInfo(e)->GetProductionCost();
		return (me && iCost > 0) ? me->getProductionNeeded(e) : iCost;
	}

	int ProductionCost(BuildingTypes e, const CvPlayer* me)
	{
		int iCost = GC.getBuildingInfo(e)->GetProductionCost();
		return (me && iCost > 0) ? me->getProductionNeeded(e) : iCost;
	}

	int ProductionCost(ProjectTypes e, const CvPlayer* me)
	{
		int iCost = GC.getProjectInfo(e)->GetProductionCost();
		return (me && iCost > 0) ? me->getProductionNeeded(e) : iCost;
	}

	JVal LookupTech(TechTypes e, const CvPlayer* me)
	{
		CvTechEntry* info = GC.getTechInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		o["era"] = J(EraName((EraTypes)info->GetEra()));
		o["cost"] = J(me ? me->GetPlayerTechs()->GetResearchCost(e) : info->GetResearchCost());
		PutCostsNote(o, me);
		JArr requires, requiresOne;
		for(int i = 0; i < GC.getNUM_AND_TECH_PREREQS(); ++i) AddRef(requires, TechInfo(info->GetPrereqAndTechs(i)));
		for(int i = 0; i < GC.getNUM_OR_TECH_PREREQS(); ++i) AddRef(requiresOne, TechInfo(info->GetPrereqOrTechs(i)));
		PutIfAny(o, "requires", requires);
		PutIfAny(o, "requires_one_of", requiresOne);
		PutIfAny(o, "leads_to", TechLeadsTo(e));
		JArr unlocks;
		for(int i = 0; i < GC.getNumUnitInfos(); ++i)
		{
			CvUnitEntry* u = GC.getUnitInfo((UnitTypes)i);
			if(u && u->GetPrereqAndTech() == (int)e) unlocks.push_back(J("unit: " + Ref(u)));
		}
		for(int i = 0; i < GC.getNumBuildingInfos(); ++i)
		{
			CvBuildingEntry* b = GC.getBuildingInfo((BuildingTypes)i);
			if(b && b->GetPrereqAndTech() == (int)e) unlocks.push_back(J("building: " + Ref(b)));
		}
		for(int i = 0; i < GC.getNumBuildInfos(); ++i)
		{
			CvBuildInfo* b = GC.getBuildInfo((BuildTypes)i);
			if(b && b->getTechPrereq() == (int)e) unlocks.push_back(J("worker build: " + Named(BuildName((BuildTypes)i).c_str(), b->GetType())));
		}
		for(int i = 0; i < GC.getNumProjectInfos(); ++i)
		{
			CvProjectEntry* p = GC.getProjectInfo((ProjectTypes)i);
			if(p && p->GetTechPrereq() == (int)e) unlocks.push_back(J("project: " + Ref(p)));
		}
		o["unlocks"] = JVal(unlocks);
		JArr reveals;
		for(int i = 0; i < GC.getNumResourceInfos(); ++i)
		{
			CvResourceInfo* r = GC.getResourceInfo((ResourceTypes)i);
			if(r && r->getTechReveal() == (int)e) AddRef(reveals, r);
		}
		PutIfAny(o, "reveals", reveals);
		PutHelp(o, info->GetHelp());
		return JVal(o);
	}

	JVal LookupUnit(UnitTypes e, const CvPlayer* me)
	{
		CvUnitEntry* info = GC.getUnitInfo(e);
		CivilizationTypes eOwner = UniqueUnitOwner(e);
		CvCivilizationInfo* civ = CivFor(eOwner, me);
		JObj o;
		o["name"] = J(info->GetDescription());
		PutProductionCost(o, ProductionCost(e, me), me);
		o["strength"] = J(info->GetCombat());
		if(info->GetRangedCombat() > 0)
		{
			o["ranged_strength"] = J(info->GetRangedCombat());
			o["range"] = J(info->GetRange());
		}
		o["moves"] = J(info->GetMoves());
		const char* szCombat = UnitCombatName(info->GetUnitCombatType());
		if(*szCombat) o["unit_type"] = J(szCombat);
		PutRef(o, "requires_tech", TechInfo(info->GetPrereqAndTech()));
		PutRef(o, "obsolete_with", TechInfo(info->GetObsoleteTech()));
		JArr upgrades;
		for(int i = 0; i < GC.getNumUnitClassInfos(); ++i)
			if(info->GetUpgradeUnitClass(i)) AddRef(upgrades, UnitInfo(CivUnit(civ, i)));
		PutIfAny(o, "upgrades_to", upgrades);
		JObj resources;
		for(int i = 0; i < GC.getNumResourceInfos(); ++i)
		{
			int iAmount = info->GetResourceQuantityRequirement(i);
			if(iAmount > 0) resources[Ref(GC.getResourceInfo((ResourceTypes)i))] = J(iAmount);
		}
		PutIfAny(o, "needs_resources", resources);
		JArr promotions;
		for(int i = 0; i < GC.getNumPromotionInfos(); ++i)
			if(info->GetFreePromotions(i)) AddRef(promotions, GC.getPromotionInfo((PromotionTypes)i));
		PutIfAny(o, "free_promotions", promotions);
		PutUnique(o, eOwner, UnitInfo(DefaultUnit(info->GetUnitClassType())));
		PutHelp(o, info->GetHelp());
		return JVal(o);
	}

	JVal LookupBuilding(BuildingTypes e, const CvPlayer* me)
	{
		CvBuildingEntry* info = GC.getBuildingInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		PutProductionCost(o, ProductionCost(e, me), me);
		PutRef(o, "requires_tech", TechInfo(info->GetPrereqAndTech()));
		PutBuildingRules(o, e, me);
		return JVal(o);
	}

	JVal LookupPolicyBranch(PolicyBranchTypes e)
	{
		CvPolicyBranchEntry* info = GC.getPolicyBranchInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		if(info->GetEraPrereq() != NO_ERA) o["requires_era"] = J(EraName((EraTypes)info->GetEraPrereq()));
		PutHelp(o, info->GetHelp());
		return JVal(o);
	}

	JVal LookupPolicy(PolicyTypes e)
	{
		CvPolicyEntry* info = GC.getPolicyInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		if(info->GetPolicyBranchType() != NO_POLICY_BRANCH_TYPE)
			PutRef(o, "branch", GC.getPolicyBranchInfo((PolicyBranchTypes)info->GetPolicyBranchType()));
		PutIfSet(o, "tenet_level", info->GetLevel());
		JArr requires, requiresOne;
		for(int i = 0; i < GC.getNumPolicyInfos(); ++i)
		{
			if(info->GetPrereqAndPolicies(i) != NO_POLICY) AddRef(requires, GC.getPolicyInfo((PolicyTypes)info->GetPrereqAndPolicies(i)));
			if(info->GetPrereqOrPolicies(i) != NO_POLICY) AddRef(requiresOne, GC.getPolicyInfo((PolicyTypes)info->GetPrereqOrPolicies(i)));
		}
		PutIfAny(o, "requires", requires);
		PutIfAny(o, "requires_one_of", requiresOne);
		PutHelp(o, info->GetHelp());
		return JVal(o);
	}

	JVal LookupPromotion(PromotionTypes e)
	{
		CvPromotionEntry* info = GC.getPromotionInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		JArr requires;
		if(info->GetPrereqPromotion() != NO_PROMOTION) AddRef(requires, GC.getPromotionInfo((PromotionTypes)info->GetPrereqPromotion()));
		PutIfAny(o, "requires", requires);
		const int ors[] = { info->GetPrereqOrPromotion1(), info->GetPrereqOrPromotion2(), info->GetPrereqOrPromotion3(),
		                    info->GetPrereqOrPromotion4(), info->GetPrereqOrPromotion5(), info->GetPrereqOrPromotion6(),
		                    info->GetPrereqOrPromotion7(), info->GetPrereqOrPromotion8(), info->GetPrereqOrPromotion9() };
		JArr requiresOne;
		for(size_t i = 0; i < sizeof(ors) / sizeof(ors[0]); ++i)
			if(ors[i] != NO_PROMOTION) AddRef(requiresOne, GC.getPromotionInfo((PromotionTypes)ors[i]));
		PutIfAny(o, "requires_one_of", requiresOne);
		JArr unitTypes;
		for(int i = 0; i < GC.getNumUnitCombatClassInfos(); ++i)
			if(info->GetUnitCombatClass(i)) unitTypes.push_back(J(UnitCombatName(i)));
		PutIfAny(o, "unit_types", unitTypes);
		if(info->IsCannotBeChosen()) o["cannot_be_chosen"] = J(true);
		PutHelp(o, info->GetHelp());
		return JVal(o);
	}

	void PutTechYields(JObj& o, const char* k, CvImprovementEntry* info, int (CvImprovementEntry::*get)(int, int) const)
	{
		JObj byTech;
		for(int t = 0; t < GC.getNumTechInfos(); ++t)
		{
			JObj yields;
			for(int y = 0; y < NUM_YIELD_TYPES; ++y) AddYield(yields, y, (info->*get)(t, y));
			if(!yields.empty()) byTech[Ref(GC.getTechInfo((TechTypes)t))] = JVal(yields);
		}
		PutIfAny(o, k, byTech);
	}

	JVal LookupImprovement(ImprovementTypes e)
	{
		CvImprovementEntry* info = GC.getImprovementInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		JObj yields;
		for(int y = 0; y < NUM_YIELD_TYPES; ++y) AddYield(yields, y, info->GetYieldChange(y));
		PutIfAny(o, "yields", yields);
		for(int i = 0; i < GC.getNumBuildInfos(); ++i)
		{
			CvBuildInfo* b = GC.getBuildInfo((BuildTypes)i);
			if(b && b->getImprovement() == (int)e) { PutRef(o, "requires_tech", TechInfo(b->getTechPrereq())); break; }
		}
		JArr connects;
		for(int i = 0; i < GC.getNumResourceInfos(); ++i)
			if(info->IsImprovementResourceTrade(i)) AddRef(connects, GC.getResourceInfo((ResourceTypes)i));
		PutIfAny(o, "connects_resources", connects);
		PutTechYields(o, "tech_bonuses", info, &CvImprovementEntry::GetTechYieldChanges);
		PutTechYields(o, "tech_bonuses_with_fresh_water", info, &CvImprovementEntry::GetTechFreshWaterYieldChanges);
		PutTechYields(o, "tech_bonuses_without_fresh_water", info, &CvImprovementEntry::GetTechNoFreshWaterYieldChanges);
		PutIfSet(o, "defense_percent", info->GetDefenseModifier());
		if(info->IsSpecificCivRequired()) o["unique_to"] = J(CivilizationRef(info->GetRequiredCivilization()));
		PutHelp(o, info->GetHelp());
		return JVal(o);
	}

	const char* ResourceKind(ResourceUsageTypes e)
	{
		switch(e)
		{
		case RESOURCEUSAGE_BONUS: return "bonus";
		case RESOURCEUSAGE_STRATEGIC: return "strategic";
		case RESOURCEUSAGE_LUXURY: return "luxury";
		default: return "other";
		}
	}

	JVal LookupResource(ResourceTypes e)
	{
		CvResourceInfo* info = GC.getResourceInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		o["kind"] = J(ResourceKind(info->getResourceUsage()));
		JObj yields;
		for(int y = 0; y < NUM_YIELD_TYPES; ++y) AddYield(yields, y, info->getYieldChange(y));
		PutIfAny(o, "yields", yields);
		PutRef(o, "revealed_by", TechInfo(info->getTechReveal()));
		JArr connectedBy;
		for(int i = 0; i < GC.getNumImprovementInfos(); ++i)
		{
			CvImprovementEntry* imp = GC.getImprovementInfo((ImprovementTypes)i);
			if(imp && imp->IsImprovementResourceTrade(e)) AddRef(connectedBy, imp);
		}
		PutIfAny(o, "connected_by", connectedBy);
		PutIfSet(o, "happiness", info->getHappiness());
		PutHelp(o, info->GetHelp());
		return JVal(o);
	}

	const char* BeliefKind(CvBeliefEntry* b)
	{
		if(b->IsPantheonBelief()) return "pantheon";
		if(b->IsFounderBelief()) return "founder";
		if(b->IsFollowerBelief()) return "follower";
		if(b->IsEnhancerBelief()) return "enhancer";
		if(b->IsReformationBelief()) return "reformation";
		return "other";
	}

	JVal LookupBelief(BeliefTypes e)
	{
		JObj o = BeliefJson(e).get<JObj>();
		o["kind"] = J(BeliefKind(GC.GetGameBeliefs()->GetEntry(e)));
		return JVal(o);
	}

	JVal LookupTerrain(TerrainTypes e)
	{
		CvTerrainInfo* info = GC.getTerrainInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		JObj yields;
		for(int y = 0; y < NUM_YIELD_TYPES; ++y) AddYield(yields, y, info->getYield(y));
		PutIfAny(o, "yields", yields);
		o["movement_cost"] = J(info->getMovementCost());
		PutIfSet(o, "defense_percent", info->getDefenseModifier());
		if(info->isImpassable()) o["impassable"] = J(true);
		PutHelp(o, info->GetHelp());
		return JVal(o);
	}

	JVal LookupFeature(FeatureTypes e)
	{
		CvFeatureInfo* info = GC.getFeatureInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		if(info->IsNaturalWonder()) o["natural_wonder"] = J(true);
		JObj yields;
		for(int y = 0; y < NUM_YIELD_TYPES; ++y) AddYield(yields, y, info->getYieldChange(y));
		PutIfAny(o, "yields", yields);
		if(info->isYieldNotAdditive()) o["replaces_terrain_yields"] = J(true);
		o["movement_cost"] = J(info->getMovementCost());
		PutIfSet(o, "defense_percent", info->getDefenseModifier());
		if(info->isImpassable()) o["impassable"] = J(true);
		PutIfSet(o, "happiness_inside_your_borders", info->getInBorderHappiness());
		PutIfSet(o, "damage_per_turn", info->getTurnDamage());
		PutHelp(o, info->GetHelp());
		return JVal(o);
	}

	JVal LookupSpecialist(SpecialistTypes e, const CvPlayer* me)
	{
		CvSpecialistInfo* info = GC.getSpecialistInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		JObj yields;
		for(int y = 0; y < NUM_YIELD_TYPES; ++y) AddYield(yields, y, info->getYieldChange(y));
		PutIfAny(o, "yields", yields);
		PutIfSet(o, "culture_per_turn", info->getCulturePerTurn());
		PutIfSet(o, "great_person_points", info->getGreatPeopleRateChange());
		PutRef(o, "great_person", UnitInfo(CivUnit(CivOf(me), info->getGreatPeopleUnitClass())));
		PutHelp(o, info->GetHelp());
		return JVal(o);
	}

	JVal LookupProject(ProjectTypes e, const CvPlayer* me)
	{
		CvProjectEntry* info = GC.getProjectInfo(e);
		JObj o;
		o["name"] = J(info->GetDescription());
		PutProductionCost(o, ProductionCost(e, me), me);
		PutRef(o, "requires_tech", TechInfo(info->GetTechPrereq()));
		PutHelp(o, info->GetHelp());
		return JVal(o);
	}

	struct GameNumber
	{
		const char* key;
		double value;
	};

	JVal LookupGameNumbers()
	{
		const GameNumber NUMBERS[] = {
			{ "no_new_city_within_tiles", GC.getMIN_CITY_RANGE() },
			{ "food_eaten_per_citizen", GC.getFOOD_CONSUMPTION_PER_POPULATION() },
			{ "science_per_citizen", GC.getSCIENCE_PER_POPULATION() },
			{ "city_size_needed_for_settlers", GC.getCITY_MIN_SIZE_FOR_SETTLERS() },
			{ "tile_buy_distance", GC.getMAXIMUM_BUY_PLOT_DISTANCE() },
			{ "unhappiness_per_city", GC.getUNHAPPINESS_PER_CITY() },
			{ "unhappiness_per_citizen", GC.getUNHAPPINESS_PER_POPULATION() },
			{ "unhappiness_per_occupied_citizen", GC.getUNHAPPINESS_PER_OCCUPIED_POPULATION() },
			{ "unhappiness_per_captured_city", GC.getUNHAPPINESS_PER_CAPTURED_CITY() },
			{ "happiness_per_natural_wonder_discovered", GC.getHAPPINESS_PER_NATURAL_WONDER() },
			{ "golden_age_first_happiness_needed", GC.getGOLDEN_AGE_BASE_THRESHOLD_HAPPINESS() },
			{ "golden_age_happiness_needed_rise_each_time", GC.getGOLDEN_AGE_EACH_GA_ADDITIONAL_HAPPINESS() },
			{ "golden_age_turns", GC.getGOLDEN_AGE_LENGTH() },
			{ "unit_max_hit_points", GC.getMAX_HIT_POINTS() },
			{ "unit_heal_per_turn_in_enemy_land", GC.getENEMY_HEAL_RATE() },
			{ "unit_heal_per_turn_in_neutral_land", GC.getNEUTRAL_HEAL_RATE() },
			{ "unit_heal_per_turn_in_your_land", GC.getFRIENDLY_HEAL_RATE() },
			{ "unit_heal_per_turn_in_a_city", GC.getCITY_HEAL_RATE() },
			{ "unit_heal_from_a_healing_promotion", GC.getINSTA_HEAL_RATE() },
			{ "city_hit_points_healed_per_turn", GC.getCITY_HIT_POINTS_HEALED_PER_TURN() },
			{ "city_attack_range", GC.getCITY_ATTACK_RANGE() },
			{ "flanking_percent_per_adjacent_unit", GC.getBONUS_PER_ADJACENT_FRIEND() },
			{ "attack_across_river_percent", GC.getRIVER_ATTACK_MODIFIER() },
			{ "attack_from_water_percent", GC.getAMPHIB_ATTACK_MODIFIER() },
			{ "great_general_percent", GC.getGREAT_GENERAL_STRENGTH_MOD() },
			{ "great_general_range", GC.getGREAT_GENERAL_RANGE() },
			{ "fortify_percent_per_turn", GC.getFORTIFY_MODIFIER_PER_TURN() },
			{ "fortify_max_turns", GC.getMAX_FORTIFY_TURNS() },
			{ "embarked_moves", GC.getEMBARKED_UNIT_MOVEMENT() },
			{ "city_state_friends_at_influence", GC.getFRIENDSHIP_THRESHOLD_FRIENDS() },
			{ "city_state_allies_at_influence", GC.getFRIENDSHIP_THRESHOLD_ALLIES() },
			{ "city_state_influence_change_per_turn_above_resting_point", GC.getMINOR_FRIENDSHIP_DROP_PER_TURN() / 100.0 },
		};
		JObj numbers;
		for(size_t i = 0; i < sizeof(NUMBERS) / sizeof(NUMBERS[0]); ++i)
			numbers[NUMBERS[i].key] = J(floor(NUMBERS[i].value * 100 + 0.5) / 100);
		JObj o;
		o["rules"] = JVal(numbers);
		o["note"] = J("the game's base values; difficulty, map size, game pace and your number of cities scale some of them");
		return JVal(o);
	}

	void PutAbilities(JObj& o, LeaderHeadTypes eLeader)
	{
		CvLeaderHeadInfo* leader = GC.getLeaderHeadInfo(eLeader);
		o["leader"] = J(leader->GetDescription());
		JArr abilities;
		for(int t = 0; t < GC.getNumTraitInfos(); ++t)
		{
			CvTraitEntry* trait = leader->hasTrait(t) ? GC.getTraitInfo((TraitTypes)t) : NULL;
			if(!trait) continue;
			JObj a;
			a["name"] = J(Localized(trait->getShortDescription()));
			a["effect"] = J(StripMarkup(trait->GetDescription()));
			abilities.push_back(JVal(a));
		}
		PutIfAny(o, "unique_ability", abilities);
	}

	JArr UniqueUnits(CvCivilizationInfo* civ)
	{
		JArr units;
		for(int i = 0; i < GC.getNumUnitClassInfos(); ++i)
		{
			int eOwn = civ->getCivilizationUnits(i);
			if(eOwn == NO_UNIT || eOwn == DefaultUnit(i)) continue;
			JObj u;
			u["unit"] = J(Ref(UnitInfo(eOwn)));
			PutRef(u, "replaces", UnitInfo(DefaultUnit(i)));
			units.push_back(JVal(u));
		}
		return units;
	}

	JArr UniqueBuildings(CvCivilizationInfo* civ)
	{
		JArr buildings;
		for(int i = 0; i < GC.getNumBuildingClassInfos(); ++i)
		{
			int eOwn = civ->getCivilizationBuildings(i);
			if(eOwn == NO_BUILDING || eOwn == DefaultBuilding(i)) continue;
			JObj b;
			b["building"] = J(Ref(BuildingInfo(eOwn)));
			PutRef(b, "replaces", BuildingInfo(DefaultBuilding(i)));
			buildings.push_back(JVal(b));
		}
		return buildings;
	}

	JArr UniqueImprovements(CivilizationTypes e)
	{
		JArr improvements;
		for(int i = 0; i < GC.getNumImprovementInfos(); ++i)
		{
			CvImprovementEntry* imp = GC.getImprovementInfo((ImprovementTypes)i);
			if(imp && imp->IsSpecificCivRequired() && imp->GetRequiredCivilization() == e) AddRef(improvements, imp);
		}
		return improvements;
	}
}

namespace McpTool
{
	JArr TechLeadsTo(TechTypes e)
	{
		JArr leadsTo;
		for(int i = 0; i < GC.getNumTechInfos(); ++i)
		{
			CvTechEntry* t = GC.getTechInfo((TechTypes)i);
			if(!t) continue;
			bool bNeeds = false;
			for(int j = 0; j < GC.getNUM_AND_TECH_PREREQS() && !bNeeds; ++j) bNeeds = (t->GetPrereqAndTechs(j) == (int)e);
			for(int j = 0; j < GC.getNUM_OR_TECH_PREREQS() && !bNeeds; ++j) bNeeds = (t->GetPrereqOrTechs(j) == (int)e);
			if(bNeeds) AddRef(leadsTo, t);
		}
		return leadsTo;
	}

	void PutBuildingRules(JObj& o, BuildingTypes e, const CvPlayer* me)
	{
		CvBuildingEntry* info = GC.getBuildingInfo(e);
		CivilizationTypes eOwner = UniqueBuildingOwner(e);
		CvCivilizationInfo* civ = CivFor(eOwner, me);
		PutIfSet(o, "upkeep", info->GetGoldMaintenance());
		JObj yields, percent, perCitizen;
		for(int y = 0; y < NUM_YIELD_TYPES; ++y)
		{
			AddYield(yields, y, info->GetYieldChange(y));
			AddYield(percent, y, info->GetYieldModifier(y));
			if(info->GetYieldChangePerPop(y) != 0) perCitizen[YieldKey(y)] = J(info->GetYieldChangePerPop(y) / 100.0);
		}
		PutIfAny(o, "yields", yields);
		PutIfAny(o, "yield_percent", percent);
		PutIfAny(o, "yields_per_citizen", perCitizen);
		PutIfSet(o, "happiness", info->GetHappiness() + info->GetUnmoddedHappiness());
		if(info->GetDefenseModifier() != 0) o["defense"] = J(info->GetDefenseModifier() / 100.0);
		PutIfSet(o, "hit_points", info->GetExtraCityHitPoints());
		CvSpecialistInfo* specialist = (info->GetSpecialistType() != NO_SPECIALIST) ? GC.getSpecialistInfo((SpecialistTypes)info->GetSpecialistType()) : NULL;
		if(specialist && info->GetSpecialistCount() > 0)
		{
			JObj slots;
			slots["specialist"] = J(Ref(specialist));
			slots["slots"] = J(info->GetSpecialistCount());
			o["specialist_slots"] = JVal(slots);
		}
		if(specialist && info->GetGreatPeopleRateChange() != 0)
		{
			PutRef(o, "great_person", UnitInfo(CivUnit(civ, specialist->getGreatPeopleUnitClass())));
			o["great_person_points"] = J(info->GetGreatPeopleRateChange());
		}
		if(info->GetGreatWorkCount() > 0)
		{
			JObj works;
			works["kind"] = J(SlotTypeName(info->GetGreatWorkSlotType()));
			works["slots"] = J(info->GetGreatWorkCount());
			o["great_work_slots"] = JVal(works);
		}
		JObj resources;
		for(int i = 0; i < GC.getNumResourceInfos(); ++i)
		{
			int iAmount = info->GetResourceQuantityRequirement(i);
			if(iAmount > 0) resources[Ref(GC.getResourceInfo((ResourceTypes)i))] = J(iAmount);
		}
		PutIfAny(o, "needs_resources", resources);
		JArr needs;
		for(int i = 0; i < GC.getNumBuildingClassInfos(); ++i)
			if(info->IsBuildingClassNeededInCity(i)) AddRef(needs, BuildingInfo(CivBuilding(civ, i)));
		PutIfAny(o, "needs_in_city", needs);
		JArr nearby, nearbyOne;
		for(int i = 0; i < GC.getNUM_BUILDING_RESOURCE_PREREQS(); ++i)
		{
			if(info->GetLocalResourceAnd(i) != NO_RESOURCE) AddRef(nearby, GC.getResourceInfo((ResourceTypes)info->GetLocalResourceAnd(i)));
			if(info->GetLocalResourceOr(i) != NO_RESOURCE) AddRef(nearbyOne, GC.getResourceInfo((ResourceTypes)info->GetLocalResourceOr(i)));
		}
		PutIfAny(o, "needs_nearby_resources", nearby);
		PutIfAny(o, "needs_one_nearby_resource_of", nearbyOne);
		PutRef(o, "obsolete_with", TechInfo(info->GetObsoleteTech()));
		// The Civilopedia's own test (CivilopediaScreen.lua): guilds, one per player but with specialists, are not wonders.
		CvBuildingClassInfo* cls = GC.getBuildingClassInfo((BuildingClassTypes)info->GetBuildingClassType());
		if(cls && cls->getMaxGlobalInstances() > 0) o["wonder"] = J("world");
		else if(cls && cls->getMaxTeamInstances() > 0) o["wonder"] = J("team");
		else if(cls && cls->getMaxPlayerInstances() == 1 && info->GetSpecialistCount() == 0) o["wonder"] = J("national");
		if(info->GetPolicyBranchType() != NO_POLICY_BRANCH_TYPE)
			PutRef(o, "needs_policy_branch", GC.getPolicyBranchInfo((PolicyBranchTypes)info->GetPolicyBranchType()));
		PutUnique(o, eOwner, BuildingInfo(DefaultBuilding(info->GetBuildingClassType())));
		PutHelp(o, info->GetHelp());
	}

	JVal CivilizationRules(CivilizationTypes e, LeaderHeadTypes eLeader)
	{
		CvCivilizationInfo* civ = GC.getCivilizationInfo(e);
		JObj o;
		o["name"] = J(CivilizationName(e));
		o["type"] = J(civ->GetType());
		for(int i = 0; eLeader == NO_LEADER && i < GC.getNumLeaderHeadInfos(); ++i)
			if(civ->isLeaders(i)) eLeader = (LeaderHeadTypes)i;
		if(eLeader != NO_LEADER) PutAbilities(o, eLeader);
		PutIfAny(o, "unique_units", UniqueUnits(civ));
		PutIfAny(o, "unique_buildings", UniqueBuildings(civ));
		PutIfAny(o, "unique_improvements", UniqueImprovements(e));
		return JVal(o);
	}
}

MCP_TOOL(ToolLookup)
{
	std::string key = ArgStr(args, "type");
	if(key.empty()) { err = "type is required"; return JVal(); }
	const CvPlayer* me = ModelPlayer();
	JObj o;
	o["type"] = J(key);
	// POLICY_BRANCH_ must be tested before POLICY_, which it also matches.
	if(StartsWith(key, "TECH_"))
	{
		TechTypes e = FindTechType(key);
		if(e == NO_TECH) { err = "unknown tech " + key; return JVal(); }
		o["tech"] = LookupTech(e, me);
	}
	else if(StartsWith(key, "UNIT_"))
	{
		UnitTypes e = FindUnitType(key);
		if(e == NO_UNIT) { err = "unknown unit " + key; return JVal(); }
		o["unit"] = LookupUnit(e, me);
	}
	else if(StartsWith(key, "BUILDING_"))
	{
		BuildingTypes e = FindBuildingType(key);
		if(e == NO_BUILDING) { err = "unknown building " + key; return JVal(); }
		o["building"] = LookupBuilding(e, me);
	}
	else if(StartsWith(key, "POLICY_BRANCH_"))
	{
		PolicyBranchTypes e = FindPolicyBranchType(key);
		if(e == NO_POLICY_BRANCH_TYPE) { err = "unknown policy branch " + key; return JVal(); }
		o["policy_branch"] = LookupPolicyBranch(e);
	}
	else if(StartsWith(key, "POLICY_"))
	{
		PolicyTypes e = FindPolicyType(key);
		if(e == NO_POLICY) { err = "unknown policy " + key; return JVal(); }
		o["policy"] = LookupPolicy(e);
	}
	else if(StartsWith(key, "PROMOTION_"))
	{
		PromotionTypes e = FindPromotionType(key);
		if(e == NO_PROMOTION) { err = "unknown promotion " + key; return JVal(); }
		o["promotion"] = LookupPromotion(e);
	}
	else if(StartsWith(key, "IMPROVEMENT_"))
	{
		ImprovementTypes e = FindImprovementType(key);
		if(e == NO_IMPROVEMENT) { err = "unknown improvement " + key; return JVal(); }
		o["improvement"] = LookupImprovement(e);
	}
	else if(StartsWith(key, "RESOURCE_"))
	{
		ResourceTypes e = FindResourceType(key);
		if(e == NO_RESOURCE) { err = "unknown resource " + key; return JVal(); }
		o["resource"] = LookupResource(e);
	}
	else if(StartsWith(key, "BELIEF_"))
	{
		BeliefTypes e = FindBelief(key);
		if(e == NO_BELIEF) { err = "unknown belief " + key; return JVal(); }
		o["belief"] = LookupBelief(e);
	}
	else if(StartsWith(key, "CIVILIZATION_"))
	{
		CivilizationTypes e = FindCivilizationType(key);
		if(e == NO_CIVILIZATION) { err = "unknown civilization " + key; return JVal(); }
		o["civilization"] = CivilizationRules(e, NO_LEADER);
	}
	else if(StartsWith(key, "TERRAIN_"))
	{
		TerrainTypes e = FindTerrainType(key);
		if(e == NO_TERRAIN) { err = "unknown terrain " + key; return JVal(); }
		o["terrain"] = LookupTerrain(e);
	}
	else if(StartsWith(key, "FEATURE_"))
	{
		FeatureTypes e = FindFeatureType(key);
		if(e == NO_FEATURE) { err = "unknown feature " + key; return JVal(); }
		o["feature"] = LookupFeature(e);
	}
	else if(StartsWith(key, "SPECIALIST_"))
	{
		SpecialistTypes e = FindSpecialistType(key);
		if(e == NO_SPECIALIST) { err = "unknown specialist " + key; return JVal(); }
		o["specialist"] = LookupSpecialist(e, me);
	}
	else if(StartsWith(key, "PROJECT_"))
	{
		ProjectTypes e = FindProjectType(key);
		if(e == NO_PROJECT) { err = "unknown project " + key; return JVal(); }
		o["project"] = LookupProject(e, me);
	}
	else if(_stricmp(key.c_str(), "RULES") == 0)
	{
		o["game_rules"] = LookupGameNumbers();
	}
	else
	{
		err = "unsupported key; use a TECH_, UNIT_, BUILDING_, POLICY_, POLICY_BRANCH_, PROMOTION_, IMPROVEMENT_, RESOURCE_, BELIEF_, "
		      "CIVILIZATION_, TERRAIN_, FEATURE_, SPECIALIST_ or PROJECT_ type, or RULES";
		return JVal();
	}
	return JVal(o);
}
