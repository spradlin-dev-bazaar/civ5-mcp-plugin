#include "CvGameCoreDLLPCH.h"
#include "McpTools.h"
#include "McpToolSupport.h"
#include "McpRules.h"
#include "McpSeat.h"
#include "McpLogText.h"
#include "McpToolHandlers.h"
#include "McpMapImage.h"
#include "McpBridge.h"
#include "McpDiplomacy.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvCity.h"
#include "CvUnit.h"
#include "CvPlot.h"
#include "CvMap.h"
#include "CvGlobals.h"
#include "CvGameCoreUtils.h"
#include "CvTypes.h"
#include "CvTechClasses.h"
#include "CvUnitClasses.h"
#include "CvBuildingClasses.h"
#include "CvTreasury.h"
#include "CvInfos.h"
#include "CvPreGame.h"

using namespace McpTool;

// ------------------------------------------------------------------ queries

namespace
{
	// Computer-run major civilizations the seat could take: on the on-screen human's team, or against it.
	std::vector<PlayerTypes> ClaimableCivs(bool bTeammates)
	{
		std::vector<PlayerTypes> out;
		for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
		{
			CvPlayer& p = GET_PLAYER((PlayerTypes)i);
			if(p.isAlive() && !p.isHuman() && McpOnScreenTeam((PlayerTypes)i) == bTeammates) out.push_back((PlayerTypes)i);
		}
		return out;
	}
}

MCP_TOOL(ToolGameSettings)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	JObj o;
	CvEraInfo* pEra = GC.getEraInfo(CvPreGame::era());
	if(pEra) o["start_era"] = J(pEra->GetDescription());
	std::string script = CvPreGame::mapScriptName().c_str();
	size_t slash = script.find_last_of("\\/");
	if(slash != std::string::npos) script = script.substr(slash + 1);
	size_t dot = script.find_last_of('.');
	o["map_type"] = J(dot == std::string::npos ? script : script.substr(0, dot));
	o["map_size"] = J(GC.getMap().getWorldInfo().GetDescription());
	PlayerTypes eHuman = GC.getGame().getActivePlayer();
	CvHandicapInfo* pHandicap = GC.getHandicapInfo(CvPreGame::handicap(eHuman == NO_PLAYER ? me->GetID() : eHuman));
	if(pHandicap) o["difficulty"] = J(pHandicap->GetDescription());
	CvGameSpeedInfo* pSpeed = GC.getGameSpeedInfo(CvPreGame::gameSpeed());
	if(pSpeed) o["game_pace"] = J(pSpeed->GetDescription());
	o["max_turns"] = J(GC.getGame().getMaxTurns());

	JArr victories;
	for(int i = 0; i < GC.getNumVictoryInfos(); ++i)
	{
		CvVictoryInfo* v = GC.getVictoryInfo((VictoryTypes)i);
		if(v && CvPreGame::isVictory((VictoryTypes)i)) victories.push_back(J(v->GetDescription()));
	}
	o["victory_types"] = JVal(victories);

	// Every option switched on, hidden ones included: setup mods such as Really Advanced Setup
	// turn on options the in-game Details panel does not list (no happiness, no science...).
	JArr options;
	const std::vector<CvPreGame::CustomOption>& set = CvPreGame::GetGameOptions();
	for(size_t i = 0; i < set.size(); ++i)
	{
		if(set[i].GetValue() == 0) continue;
		std::string key = set[i].GetName();
		JObj opt;
		opt["option"] = J(key);
		for(int k = 0; k < GC.getNumGameOptionInfos(); ++k)
		{
			CvGameOptionInfo* info = GC.getGameOptionInfo((GameOptionTypes)k);
			if(!info || key != info->GetType()) continue;
			opt["name"] = J(info->GetDescription());
			if(info->GetHelp() && *info->GetHelp()) opt["effect"] = J(Localized(info->GetHelp()));
			break;
		}
		if(set[i].GetValue() != 1) opt["value"] = J(set[i].GetValue());
		options.push_back(JVal(opt));
	}
	o["options"] = JVal(options);

	JObj mapOptions;
	const std::vector<CvPreGame::CustomOption>& mapSet = CvPreGame::GetMapOptions();
	for(size_t i = 0; i < mapSet.size(); ++i) mapOptions[mapSet[i].GetName()] = J(mapSet[i].GetValue());
	o["map_script_options"] = JVal(mapOptions);
	return JVal(o);
}

MCP_TOOL(ToolMods)
{
	JArr mods;
	std::vector<McpModInfo> active = McpBridge::Get().ActiveMods();
	for(size_t i = 0; i < active.size(); ++i)
	{
		JObj m;
		m["name"] = J(active[i].name);
		m["description"] = J(active[i].description);
		mods.push_back(JVal(m));
	}
	JObj o;
	o["mods"] = JVal(mods);
	if(active.empty()) o["note"] = J("no mods reported yet: the game's interface reports them a moment after the map loads; try again shortly");
	return JVal(o);
}

namespace
{
	JArr SeatsWaiting()
	{
		JArr list;
		std::string label = McpCurrentLabel();
		std::vector<PlayerTypes> waiting = McpUnboundSeats();
		for(size_t i = 0; i < waiting.size(); ++i) list.push_back(J(McpSeatWaitingText(waiting[i], label)));
		return list;
	}

	// The other connected models, by name and relation only: which civilization each plays is
	// not something a player would know.
	JArr OtherAiPlayers(CvPlayer* me)
	{
		JArr list;
		McpSession mine = McpBridge::Get().Current();
		std::vector<McpSession> all = McpBridge::Get().Sessions();
		for(size_t i = 0; i < all.size(); ++i)
		{
			if(all[i].token == mine.token || all[i].idle) continue;
			std::string label = McpIdentityLabel(all[i].identity);
			if(label.empty()) continue;
			std::string relation;
			if(all[i].seat == MCP_SEAT_SCREEN) relation = "plays the human's own civilization";
			else if(all[i].seat < 0) relation = "between seats";
			else if(me) relation = (GET_PLAYER((PlayerTypes)all[i].seat).getTeam() == me->getTeam()) ? "your teammate" : "your opponent";
			else relation = McpOnScreenTeam((PlayerTypes)all[i].seat) ? "the human's teammate" : "the human's opponent";
			list.push_back(J(label + " (" + relation + ")"));
		}
		return list;
	}
}

MCP_TOOL(ToolStatus)
{
	CvGame& g = GC.getGame();
	JObj o;
	o["game_id"] = J(McpGameId());
	int iSeatState = McpBridge::Get().Current().seat;
	if(iSeatState == MCP_SEAT_LOST || iSeatState == MCP_SEAT_UNDECIDED)
	{
		o["seat"] = J(iSeatState == MCP_SEAT_LOST
		              ? "lost: the game was reloaded or your civilization was released; take_seat claims one again (the civilization you remember for this game_id, or one from seats_waiting_for_a_player)"
		              : "none yet: this game has AI seats, so choose with take_seat: a civilization name, teammate, opponent, or human to play the civilization on screen");
		o["seats_waiting_for_a_player"] = JVal(SeatsWaiting());
		JArr others = OtherAiPlayers(NULL);
		if(!others.empty()) o["other_ai_players"] = JVal(others);
		std::string label = McpCurrentLabel();
		o["playing_as"] = J(label.empty() ? std::string("nobody yet: call set_identity before acting") : label);
		return JVal(o);
	}
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	JArr mods;
	std::vector<McpModInfo> active = McpBridge::Get().ActiveMods();
	for(size_t i = 0; i < active.size(); ++i) mods.push_back(J(active[i].name));
	o["mods"] = JVal(mods);
	o["turn"] = J(g.getGameTurn());
	o["year"] = J(g.getGameTurnYear());
	o["max_turns"] = J(g.getMaxTurns());
	o["player_id"] = J((int)me->GetID());
	o["player_name"] = J(me->getName());
	o["alive"] = J(me->isAlive());
	o["civilization"] = J(me->getCivilizationShortDescription());
	CvLeaderHeadInfo* pLeader = GC.getLeaderHeadInfo(me->getLeaderType());
	o["leader"] = J(pLeader ? pLeader->GetDescription() : "");
	o["is_my_turn"] = J(McpIsPlayerTurn(*me));
	std::string waiting = McpDiploWaitingNote();
	if(!waiting.empty()) o["diplomacy_waiting"] = J(waiting);
	o["ai_processing"] = J(AiProcessing());
	o["end_turn_blocking"] = J(EndTurnBlockingText(me));
	o["units_needing_orders"] = J(me->GetCountReadyUnits());
	std::vector<std::string> chat = McpBridge::Get().TakeChat((int)me->GetID());
	if(!chat.empty())
	{
		JArr messages;
		for(size_t i = 0; i < chat.size(); ++i) messages.push_back(J(chat[i]));
		o["messages_from_your_teammate"] = JVal(messages);
	}
	if(McpSeatOnScreen())
	{
		o["seat"] = J("none claimed: you act for the human's own civilization");
		JArr mates;
		std::vector<PlayerTypes> teammates = ClaimableCivs(true);
		for(size_t i = 0; i < teammates.size(); ++i) mates.push_back(J(CivName(teammates[i])));
		o["teammate_civs_you_could_play"] = JVal(mates);
		o["opponent_civs_you_could_play"] = J((int)ClaimableCivs(false).size());
	}
	else
	{
		o["seat"] = J("claimed: off screen, the human on screen plays another civilization");
		o["role"] = J(!McpOnScreenTeam(me->GetID()) ? "opponent: never tell the human which civilization you play" : "teammate of the human");
	}
	JArr seatsWaiting = SeatsWaiting();
	if(!seatsWaiting.empty()) o["seats_waiting_for_a_player"] = JVal(seatsWaiting);
	JArr others = OtherAiPlayers(me);
	if(!others.empty()) o["other_ai_players"] = JVal(others);
	std::string label = McpCurrentLabel();
	o["playing_as"] = J(label.empty() ? std::string("nobody yet: call set_identity before acting") : label);
	// Mods arrive from the UI a moment after the map loads; hold the briefing until they have.
	if(!McpBridge::Get().ActiveMods().empty() && McpBridge::Get().TakeGameBriefingDue())
	{
		std::string ignored;
		o["game_settings"] = ToolGameSettings(JObj(), ignored);
		JVal modsDetail = ToolMods(JObj(), ignored);
		o["mods"] = modsDetail.get<JObj>()["mods"];
		o["your_civilization"] = CivilizationRules(me->getCivilizationType(), me->getLeaderType());
		o["remember"] = J("store game_settings, mods and your_civilization with your memories for this game_id; they come once per connection and per loaded game");
	}
	return JVal(o);
}

MCP_TOOL(ToolOverview)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	JObj o;
	o["turn"] = J(GC.getGame().getGameTurn());
	o["era"] = J(EraName(me->GetCurrentEra()));
	o["score"] = J(me->GetScore());
	o["gold"] = J(me->GetTreasury()->GetGold());
	o["gold_per_turn"] = J(me->calculateGoldRate());
	o["science_per_turn"] = J(me->GetScience());
	o["culture"] = J(me->getJONSCulture());
	o["culture_per_turn"] = J(me->GetTotalJONSCulturePerTurn());
	o["next_policy_cost"] = J(me->getNextPolicyCost());
	o["faith"] = J(me->GetFaith());
	o["faith_per_turn"] = J(me->GetTotalFaithPerTurn());
	o["happiness"] = J(me->GetExcessHappiness());
	o["techs_known"] = J(GET_TEAM(me->getTeam()).GetTeamTechs()->GetNumTechsKnown());
	o["research"] = ResearchJson(me);
	o["num_cities"] = J(me->getNumCities());
	o["num_units"] = J(me->getNumUnits());
	o["military_units"] = J(me->getNumMilitaryUnits());
	o["units_supported_free"] = J(me->GetNumUnitsSupplied());
	o["end_turn_blocking"] = J(EndTurnBlockingText(me));
	o["cities"] = JVal(CitiesJson(me));
	o["units"] = JVal(UnitsJson(me));
	return JVal(o);
}

MCP_TOOL(ToolUnits)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	JObj o;
	o["units"] = JVal(UnitsJson(me));
	return JVal(o);
}

namespace
{
	JArr AttackTargets(CvPlayer* me, CvUnit* u)
	{
		JArr targets;
		if(!u->IsCanAttack()) return targets;
		int range = u->isRanged() ? u->GetRange() : 1;
		for(int dy = -range; dy <= range; ++dy)
		{
			for(int dx = -range; dx <= range; ++dx)
			{
				CvPlot* p = plotXYWithRangeCheck(u->getX(), u->getY(), dx, dy, range);
				if(!p) continue;
				AttackTarget t;
				std::string why;
				if(!ResolveAttackTarget(me, u, p, t, why)) continue;
				JObj o;
				o["x"] = J(p->getX());
				o["y"] = J(p->getY());
				if(t.city)
				{
					o["city"] = J(t.city->getName().c_str());
					o["owner"] = J(CivName(t.city->getOwner()));
					o["hp"] = J(t.city->GetMaxHitPoints() - t.city->getDamage());
				}
				else
				{
					o["unit"] = J(UnitTypeName(t.unit->getUnitType()));
					o["owner"] = J(CivName(t.unit->getOwner()));
					o["hp"] = J(t.unit->GetCurrHitPoints());
					o["strength"] = J(t.unit->GetBaseCombatStrength());
				}
				targets.push_back(JVal(o));
			}
		}
		return targets;
	}

	// The steps of CvLuaPlayer::lGetRecommendedFoundCityPlots, which needs the unit selected on screen. Kept identical,
	// odd loop bounds and all, so models get the sites the game marks for a human.
	JArr RecommendedSites(CvPlayer* me, CvUnit* u)
	{
		const int iEvalDistance = GC.getSETTLER_EVALUATION_DISTANCE();
		const int iDropoffMod = GC.getSETTLER_DISTANCE_DROPOFF_MODIFIER();
		CvWeightedVector<CvPlot*, 800, true> candidates;
		for(int x = u->getX() - iEvalDistance; x != u->getX() + iEvalDistance; ++x)
		{
			for(int y = u->getY() - iEvalDistance; y != u->getY() + iEvalDistance; ++y)
			{
				CvPlot* p = GC.getMap().plot(x, y);
				if(!p || !p->isRevealed(u->getTeam()) || !me->canFound(x, y) || p->isVisibleEnemyUnit(me->GetID())) continue;
				int iValue = me->AI_foundValue(x, y, -1, false);
				iValue = iValue * (100 - iDropoffMod * plotDistance(x, y, u->getX(), u->getY()) / iEvalDistance) / 100;
				const int iDanger = me->GetPlotDanger(*p);
				if(iDanger < 1000) candidates.push_back(p, (1000 - iDanger) * iValue / 1000);
			}
		}
		candidates.SortItems();
		JArr sites;
		int iFailLimit = 20;
		for(int i = 0; i < candidates.size() && sites.size() < 3; ++i)
		{
			CvPlot* p = candidates.GetElement(i);
			int iTurns = 0;
			if(!u->GeneratePath(p, MOVE_TERRITORY_NO_UNEXPLORED, true, &iTurns))
			{
				if(iFailLimit-- == 0) break;
				continue;
			}
			JObj s;
			s["x"] = J(p->getX());
			s["y"] = J(p->getY());
			s["turns"] = J(iTurns);
			sites.push_back(JVal(s));
		}
		return sites;
	}
}

MCP_TOOL(ToolUnitOptions)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvUnit* u = RequireUnit(me, args, err);
	if(!u) return JVal();
	CvPlot* p = u->plot();
	JObj o;
	o["unit"] = UnitJson(u);
	JArr actions, waiting;
	size_t count = 0;
	const UnitActionDef* defs = UnitActions(count);
	for(size_t i = 0; i < count; ++i)
	{
		if(!defs[i].can(u, p)) continue;
		if(defs[i].needsMoves && !u->canMove()) waiting.push_back(J(defs[i].name));
		else actions.push_back(J(defs[i].name));
	}
	if(u->CanAutomate(AUTOMATE_EXPLORE)) actions.push_back(J("explore"));
	if(u->CanAutomate(AUTOMATE_BUILD)) actions.push_back(J("automate_work"));
	if(u->IsAutomated()) actions.push_back(J("stop_automation"));
	o["actions"] = JVal(actions);
	if(!waiting.empty())
	{
		o["actions_needing_moves"] = JVal(waiting);
		o["moves_note"] = J("the unit has no moves left this turn, so the actions_needing_moves can be used next turn");
	}

	JArr builds;
	for(int i = 0; i < GC.getNumBuildInfos(); ++i)
	{
		BuildTypes eBuild = (BuildTypes)i;
		if(u->canBuild(p, eBuild))
		{
			JObj b;
			b["type"] = J(GC.getBuildInfo(eBuild)->GetType());
			b["name"] = J(BuildName(eBuild));
			builds.push_back(JVal(b));
		}
	}
	o["builds_here"] = JVal(builds);

	JArr promotions = PromotionsAvailable(u);
	if(!promotions.empty()) o["promotions_available"] = JVal(promotions);
	JArr targets = AttackTargets(me, u);
	if(!targets.empty()) o["attack_targets"] = JVal(targets);
	// The game's screen (InGame.lua) marks no sites before the player's first city.
	if(u->isFound() && me->getNumCities() > 0) o["recommended_sites"] = JVal(RecommendedSites(me, u));

	JArr adjacent;
	for(int d = 0; d < NUM_DIRECTION_TYPES; ++d)
	{
		CvPlot* n = plotDirection(u->getX(), u->getY(), (DirectionTypes)d);
		if(!n || !n->isRevealed(me->getTeam())) continue;
		JObj a;
		a["x"] = J(n->getX());
		a["y"] = J(n->getY());
		a["terrain"] = J(TerrainName(n->getTerrainType()));
		if(n->isHills()) a["hills"] = J(true);
		if(n->getFeatureType() != NO_FEATURE) a["feature"] = J(FeatureName(n->getFeatureType()));
		a["can_move_there"] = J(u->canMoveInto(*n));
		adjacent.push_back(JVal(a));
	}
	o["adjacent_tiles"] = JVal(adjacent);
	return JVal(o);
}

MCP_TOOL(ToolCities)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	JObj o;
	o["cities"] = JVal(CitiesJson(me));
	return JVal(o);
}

MCP_TOOL(ToolCityOptions)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvCity* c = RequireCity(me, args, err);
	if(!c) return JVal();
	JObj o;
	o["city"] = CityJson(c);
	o["gold"] = J(me->GetTreasury()->GetGold());
	JArr units;
	for(int i = 0; i < GC.getNumUnitInfos(); ++i)
	{
		UnitTypes e = (UnitTypes)i;
		bool bTrain = c->canTrain(e);
		bool bBuy = c->IsCanPurchase(false, true, e, NO_BUILDING, NO_PROJECT, YIELD_GOLD);
		bool bBuyFaith = c->IsCanPurchase(false, true, e, NO_BUILDING, NO_PROJECT, YIELD_FAITH);
		if(!bTrain && !bBuy && !bBuyFaith) continue;
		CvUnitEntry* info = GC.getUnitInfo(e);
		JObj u;
		u["type"] = J(info->GetType());
		u["name"] = J(info->GetDescription());
		if(bTrain)
		{
			u["cost"] = J(c->getProductionNeeded(e));
			u["turns"] = J(c->getProductionTurnsLeft(e, 0));
		}
		if(bBuy) u["buy_gold"] = J(c->GetPurchaseCost(e));
		if(bBuyFaith) u["buy_faith"] = J(c->GetFaithPurchaseCost(e, true));
		if(info->GetCombat() > 0) u["strength"] = J(info->GetCombat());
		if(info->GetRangedCombat() > 0) u["ranged_strength"] = J(info->GetRangedCombat());
		u["moves"] = J(info->GetMoves());
		units.push_back(JVal(u));
	}
	o["units"] = JVal(units);
	JArr buildings;
	for(int i = 0; i < GC.getNumBuildingInfos(); ++i)
	{
		BuildingTypes e = (BuildingTypes)i;
		bool bBuild = c->canConstruct(e);
		bool bBuy = c->IsCanPurchase(false, true, NO_UNIT, e, NO_PROJECT, YIELD_GOLD);
		bool bBuyFaith = c->IsCanPurchase(false, true, NO_UNIT, e, NO_PROJECT, YIELD_FAITH);
		if(!bBuild && !bBuy && !bBuyFaith) continue;
		CvBuildingEntry* info = GC.getBuildingInfo(e);
		JObj b;
		b["type"] = J(info->GetType());
		b["name"] = J(info->GetDescription());
		if(bBuild)
		{
			b["cost"] = J(c->getProductionNeeded(e));
			b["turns"] = J(c->getProductionTurnsLeft(e, 0));
		}
		if(bBuy) b["buy_gold"] = J(c->GetPurchaseCost(e));
		if(bBuyFaith) b["buy_faith"] = J(c->GetFaithPurchaseCost(e));
		PutBuildingRules(b, e, me);
		buildings.push_back(JVal(b));
	}
	o["buildings"] = JVal(buildings);
	JArr projects;
	for(int i = 0; i < GC.getNumProjectInfos(); ++i)
	{
		ProjectTypes e = (ProjectTypes)i;
		if(!c->canCreate(e)) continue;
		CvProjectEntry* info = GC.getProjectInfo(e);
		JObj pj;
		pj["type"] = J(info->GetType());
		pj["name"] = J(info->GetDescription());
		pj["cost"] = J(c->getProductionNeeded(e));
		projects.push_back(JVal(pj));
	}
	if(!projects.empty()) o["projects"] = JVal(projects);
	JArr processes;
	for(int i = 0; i < GC.getNumProcessInfos(); ++i)
	{
		ProcessTypes e = (ProcessTypes)i;
		if(!c->canMaintain(e)) continue;
		CvProcessInfo* info = GC.getProcessInfo(e);
		JObj pr;
		pr["type"] = J(info->GetType());
		pr["name"] = J(info->GetDescription());
		processes.push_back(JVal(pr));
	}
	if(!processes.empty()) o["processes"] = JVal(processes);
	o["faith"] = J(me->GetFaith());

	JArr tiles;
	int iBuyRange = c->getBuyPlotDistance();
	for(int dy = -iBuyRange; dy <= iBuyRange; ++dy)
	{
		for(int dx = -iBuyRange; dx <= iBuyRange; ++dx)
		{
			CvPlot* p = plotXYWithRangeCheck(c->getX(), c->getY(), dx, dy, iBuyRange);
			if(!p || !c->CanBuyPlot(p->getX(), p->getY())) continue;
			JObj t;
			t["x"] = J(p->getX());
			t["y"] = J(p->getY());
			t["gold"] = J(c->GetBuyPlotCost(p->getX(), p->getY()));
			t["terrain"] = J(TerrainName(p->getTerrainType()));
			if(p->getFeatureType() != NO_FEATURE) t["feature"] = J(FeatureName(p->getFeatureType()));
			ResourceTypes eRes = p->getResourceType(me->getTeam());
			if(eRes != NO_RESOURCE) t["resource"] = J(ResourceName(eRes));
			tiles.push_back(JVal(t));
		}
	}
	o["buyable_tiles"] = JVal(tiles);
	return JVal(o);
}

MCP_TOOL(ToolMap)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	int cx = 0, cy = 0;
	if(!ResolveMapCenter(me, args, cx, cy, err)) return JVal();
	int radius = ArgInt(args, "radius", 3);
	if(radius < 1) radius = 1;
	if(radius > 8) radius = 8;
	CvMap& map = GC.getMap();

	JObj o;
	o["center_x"] = J(cx);
	o["center_y"] = J(cy);
	o["radius"] = J(radius);
	o["map_width"] = J(map.getGridWidth());
	o["map_height"] = J(map.getGridHeight());
	o["wraps_east_west"] = J(map.isWrapX());
	JArr tiles;
	int unknown = 0;
	for(int dy = -radius; dy <= radius; ++dy)
	{
		for(int dx = -radius; dx <= radius; ++dx)
		{
			CvPlot* p = plotXYWithRangeCheck(cx, cy, dx, dy, radius);
			if(!p) continue;
			if(!p->isRevealed(me->getTeam())) { ++unknown; continue; }
			tiles.push_back(PlotJson(p, me->getTeam(), me->GetID(), cx, cy));
		}
	}
	o["tiles"] = JVal(tiles);
	o["unexplored_tiles_in_range"] = J(unknown);
	return JVal(o);
}

MCP_TOOL(ToolResearchOptions)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	JObj o;
	o["research"] = ResearchJson(me);
	JArr techs;
	for(int i = 0; i < GC.getNumTechInfos(); ++i)
	{
		TechTypes e = (TechTypes)i;
		if(!me->GetPlayerTechs()->CanResearch(e)) continue;
		CvTechEntry* info = GC.getTechInfo(e);
		JObj t;
		t["type"] = J(info->GetType());
		t["name"] = J(info->GetDescription());
		t["era"] = J(EraName((EraTypes)info->GetEra()));
		t["cost"] = J(me->GetPlayerTechs()->GetResearchCost(e));
		t["turns"] = J(me->GetPlayerTechs()->GetResearchTurnsLeft(e, true));
		t["help"] = J(Localized(info->GetHelp()));
		JArr leadsTo = TechLeadsTo(e);
		if(!leadsTo.empty()) t["leads_to"] = JVal(leadsTo);
		techs.push_back(JVal(t));
	}
	o["available"] = JVal(techs);
	return JVal(o);
}

// ------------------------------------------------------------------ actions

MCP_TOOL(ToolMoveUnit)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	int x = 0, y = 0;
	CvUnit* u = RequireUnit(me, args, err);
	if(!u || !RequireInt(args, "x", x, err) || !RequireInt(args, "y", y, err)) return JVal();
	CvMap& map = GC.getMap();
	if(x < 0 || y < 0 || x >= map.getGridWidth() || y >= map.getGridHeight()) { err = "destination is off the map"; return JVal(); }
	CvPlot* dest = map.plot(x, y);
	if(!dest->isRevealed(me->getTeam())) { err = "destination tile is unexplored"; return JVal(); }
	if(dest->isMountain()) { err = "destination is a mountain: impassable"; return JVal(); }
	if(dest == u->plot()) { err = "the unit is already on that tile"; return JVal(); }
	if(dest->isVisible(me->getTeam()))
	{
		CvCity* c = dest->getPlotCity();
		if((c && IsEnemyOf(me, c->getOwner())) || dest->isVisibleEnemyDefender(u))
		{
			err = "a defending enemy is on that tile; use the attack tool instead of move_unit (a lone enemy civilian is captured by moving onto it)";
			return JVal();
		}
	}
	int iTurns = 0;
	if(!u->GeneratePath(dest, 0, false, &iTurns))
	{
		err = "cannot move there: " + MoveRefusal(u, dest);
		return JVal();
	}
	if(ArgBool(args, "preview_only", false))
	{
		// The path, from the tile after the unit's to the destination, with the turn each is reached.
		JArr path;
		for(CvAStarNode* n = GC.getPathFinder().GetLastNode(); n; n = n->m_pParent)
		{
			if(n->m_iX == u->getX() && n->m_iY == u->getY()) continue;
			JObj step;
			step["x"] = J(n->m_iX);
			step["y"] = J(n->m_iY);
			step["turn"] = J(n->m_iData2);
			path.insert(path.begin(), JVal(step));
		}
		JObj o;
		o["previewed_only"] = J(true);
		if(!u->canMove()) o["note"] = J("this unit has no moves left this turn; give the order next turn");
		o["turns"] = J(iTurns);
		o["path"] = JVal(path);
		CvPlot* pEnd = u->GetPathEndTurnPlot();
		if(pEnd) o["this_turn_ends_at"] = J(McpFormat("(%d,%d)", pEnd->getX(), pEnd->getY()));
		return JVal(o);
	}
	if(!RequireMoves(u, err)) return JVal();
	int fromX = u->getX(), fromY = u->getY();
	{
		NetMessageScope scope;
		u->PushMission(CvTypes::getMISSION_MOVE_TO(), x, y, 0, false, true);
	}
	JObj o;
	o["unit_id"] = J(u->GetID());
	o["from"] = J(McpFormat("(%d,%d)", fromX, fromY));
	o["now_at"] = J(McpFormat("(%d,%d)", u->getX(), u->getY()));
	o["destination"] = J(McpFormat("(%d,%d)", x, y));
	bool bArrived = (u->getX() == x && u->getY() == y);
	o["arrived"] = J(bArrived);
	if(!bArrived) o["note"] = J("the unit continues toward the destination when you end the turn, so it is still here when your next turn opens; do not re-issue the order");
	o["estimated_turns_total"] = J(iTurns);
	o["moves_left"] = J(Moves(u->movesLeft()));
	return JVal(o);
}

namespace
{
	// An order can use the unit up or get it killed (a missionary's last spread, a great person's improvement, a nuke,
	// a fighter shot down on a sweep), so the unit is looked up again by id, never read through u afterwards.
	void PushOrderAndReport(JObj& o, CvPlayer* me, CvUnit* u, MissionTypes eMission, int iData1, int iData2)
	{
		int iUnit = u->GetID();
		std::string name = UnitTypeName(u->getUnitType());
		{
			NetMessageScope scope;
			u->PushMission(eMission, iData1, iData2, 0, false, true);
		}
		o["ok"] = J(true);
		CvUnit* after = me->getUnit(iUnit);
		if(!after || after->isDelayedDeath() || !after->plot()) o["unit_gone"] = J("the " + name + " is gone");
		else o["unit"] = UnitJson(after);
	}
}

MCP_TOOL(ToolUnitAction)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvUnit* u = RequireUnit(me, args, err);
	if(!u) return JVal();
	CvPlot* p = u->plot();
	std::string action = ArgStr(args, "action");
	JObj o;

	if(action == "build")
	{
		std::string key = ArgStr(args, "build");
		BuildTypes eBuild = FindBuildType(key);
		if(eBuild == NO_BUILD) { err = "unknown build type '" + key + "' (use unit_options to list builds_here)"; return JVal(); }
		if(!u->canBuild(p, eBuild)) { err = "cannot do that build on this tile (wrong unit, tech missing, or terrain)"; return JVal(); }
		PushOrderAndReport(o, me, u, CvTypes::getMISSION_BUILD(), eBuild, -1);
		return JVal(o);
	}

	if(action == "explore" || action == "automate_work")
	{
		bool bExplore = action == "explore";
		if(!u->CanAutomate(bExplore ? AUTOMATE_EXPLORE : AUTOMATE_BUILD))
		{
			err = bExplore ? "this unit cannot explore on its own (only land and sea combat units can, and only while unexplored land it can reach remains)"
			               : "only a worker or work boat can be automated";
			return JVal();
		}
		u->Automate(bExplore ? AUTOMATE_EXPLORE : AUTOMATE_BUILD);
		o["ok"] = J(true);
		o["warning"] = J(bExplore
		    ? "The game's AI now steers this unit. It picks its own route toward unexplored land, avoids danger poorly and can walk into barbarians or hostile units and die, and it hands the unit back to you when nothing it can reach is left to explore. Any direct order or stop_automation takes it back sooner."
		    : "The game's AI now decides what this worker builds. It knows nothing of your plans: it may chop forests or jungles you wanted kept, ignore a strategic resource you need next, build roads you do not need and improve tiles you would not have chosen. Any direct order or stop_automation takes it back.");
		o["unit"] = UnitJson(u);
		return JVal(o);
	}
	if(action == "stop_automation")
	{
		if(!u->IsAutomated()) { err = "this unit is not automated"; return JVal(); }
		u->SetAutomateType(NO_AUTOMATE);
		o["ok"] = J(true);
		o["unit"] = UnitJson(u);
		return JVal(o);
	}

	if(action == "rebase")
	{
		int x = 0, y = 0;
		if(!RequireMoves(u, err) || !RequireInt(args, "x", x, err) || !RequireInt(args, "y", y, err)) return JVal();
		if(!u->canRebaseAt(p, x, y)) { err = "this unit cannot rebase there (not an aircraft, out of range, or no city or carrier of yours at the tile)"; return JVal(); }
		PushOrderAndReport(o, me, u, CvTypes::getMISSION_REBASE(), x, y);
		return JVal(o);
	}
	if(action == "air_sweep")
	{
		int x = 0, y = 0;
		if(!RequireMoves(u, err) || !RequireInt(args, "x", x, err) || !RequireInt(args, "y", y, err)) return JVal();
		if(!u->canAirSweepAt(x, y)) { err = "this unit cannot sweep that tile (not a fighter, out of range, or it already attacked this turn)"; return JVal(); }
		PushOrderAndReport(o, me, u, CvTypes::getMISSION_AIR_SWEEP(), x, y);
		return JVal(o);
	}
	if(action == "paradrop")
	{
		int x = 0, y = 0;
		if(!RequireMoves(u, err) || !RequireInt(args, "x", x, err) || !RequireInt(args, "y", y, err)) return JVal();
		if(!u->canParadropAt(p, x, y)) { err = "this unit cannot drop there (not a paratrooper, must start in your territory, target within range and not enemy-held)"; return JVal(); }
		PushOrderAndReport(o, me, u, CvTypes::getMISSION_PARADROP(), x, y);
		return JVal(o);
	}
	if(action == "nuke")
	{
		int x = 0, y = 0;
		if(!RequireMoves(u, err) || !RequireInt(args, "x", x, err) || !RequireInt(args, "y", y, err)) return JVal();
		if(!u->canNukeAt(p, x, y)) { err = "this unit cannot strike that tile (not a nuclear weapon, out of range, or the blast would hit someone you cannot declare war on)"; return JVal(); }
		CvPlot* pTarget = GC.getMap().plot(x, y);
		if(!pTarget) { err = "that tile is off the map"; return JVal(); }
		CvTeam& myTeam = GET_TEAM(me->getTeam());
		for(int i = 0; i < MAX_TEAMS; ++i)
		{
			TeamTypes eTeam = (TeamTypes)i;
			if(u->isNukeVictim(pTarget, eTeam) && !myTeam.isAtWar(eTeam))
			{
				err = "the blast would hit " + CivName(GET_TEAM(eTeam).getLeaderID()) + ", who you are not at war with; declare war first if you mean to";
				return JVal();
			}
		}
		PushOrderAndReport(o, me, u, CvTypes::getMISSION_NUKE(), x, y);
		return JVal(o);
	}
	if(action == "disband")
	{
		if(!u->canScrap()) { err = "this unit cannot be disbanded right now"; return JVal(); }
		std::string name = UnitTypeName(u->getUnitType());
		u->scrap();
		o["ok"] = J(true);
		o["disbanded"] = J(name);
		return JVal(o);
	}

	size_t count = 0;
	const UnitActionDef* defs = UnitActions(count);
	for(size_t i = 0; i < count; ++i)
	{
		const UnitActionDef& def = defs[i];
		if(action != def.name) continue;
		if(!def.can(u, p))
		{
			err = (def.needsMoves || action == "heal") ? "" : RestRefusal(u, p, action);
			if(err.empty()) err = def.refusal;
			return JVal();
		}
		if(def.needsMoves && !RequireMoves(u, err)) return JVal();
		PushOrderAndReport(o, me, u, def.mission(), -1, -1);
		CvCity* c = (action == "found_city") ? p->getPlotCity() : NULL;
		if(c) o["city_founded"] = J(c->getName().c_str());
		return JVal(o);
	}

	err = "unknown action '" + action + "'; use one of " + UnitActionNames() + ", build, explore, automate_work, stop_automation, rebase, air_sweep, paradrop, nuke or disband";
	return JVal();
}

MCP_TOOL(ToolSetProduction)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvCity* c = RequireCity(me, args, err);
	if(!c) return JVal();
	std::string item = ArgStr(args, "item");
	bool bAppend = ArgBool(args, "append", false);

	CityItem found = FindCityItem(me, item);
	UnitTypes eUnit = found.eUnit;
	BuildingTypes eBuilding = found.eBuilding;
	ProjectTypes eProject = (eUnit == NO_UNIT && eBuilding == NO_BUILDING) ? FindProjectType(item) : NO_PROJECT;
	ProcessTypes eProcess = (eUnit == NO_UNIT && eBuilding == NO_BUILDING && eProject == NO_PROJECT) ? FindProcessType(item) : NO_PROCESS;
	if(eUnit != NO_UNIT)
	{
		std::string why = TrainRefusal(c, eUnit, false);
		if(!why.empty()) { err = "this city cannot train " + std::string(GC.getUnitInfo(eUnit)->GetType()) + ": " + why; return JVal(); }
		c->pushOrder(ORDER_TRAIN, eUnit, -1, false, !bAppend, bAppend);
	}
	else if(eBuilding != NO_BUILDING)
	{
		std::string why = BuildRefusal(c, eBuilding);
		if(!why.empty()) { err = "this city cannot build " + std::string(GC.getBuildingInfo(eBuilding)->GetType()) + ": " + why; return JVal(); }
		c->pushOrder(ORDER_CONSTRUCT, eBuilding, -1, false, !bAppend, bAppend);
	}
	else if(eProject != NO_PROJECT)
	{
		if(!c->canCreate(eProject)) { err = "this city cannot work on " + item + " right now"; return JVal(); }
		c->pushOrder(ORDER_CREATE, eProject, -1, false, !bAppend, bAppend);
	}
	else if(eProcess != NO_PROCESS)
	{
		if(!c->canMaintain(eProcess)) { err = "this city cannot run " + item + " yet (its technology is missing)"; return JVal(); }
		c->pushOrder(ORDER_MAINTAIN, eProcess, -1, false, !bAppend, bAppend);
	}
	else
	{
		err = "unknown item '" + item + "'; use a type from city_options such as UNIT_WARRIOR, BUILDING_MONUMENT, PROJECT_APOLLO_PROGRAM or PROCESS_WEALTH";
		return JVal();
	}
	JObj o;
	o["ok"] = J(true);
	if(!found.substituted.empty()) o["substituted"] = J(found.substituted);
	o["city"] = CityJson(c);
	return JVal(o);
}

MCP_TOOL(ToolSetResearch)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	std::string key = ArgStr(args, "tech");
	TechTypes eTech = FindTechType(key);
	if(eTech == NO_TECH) { err = "unknown tech '" + key + "'; use a name or type from research_options"; return JVal(); }
	bool bAdd = ArgBool(args, "add_to_queue", false);
	bool bPreview = ArgBool(args, "preview_only", false);
	JObj o;
	if(me->GetNumFreeTechs() > 0)
	{
		if(bAdd || bPreview) { err = "claim your free technology first: set_research without add_to_queue or preview_only grants it"; return JVal(); }
		if(!me->GetPlayerTechs()->CanResearchForFree(eTech)) { err = "that technology cannot be taken as the free one (prerequisites missing or already known)"; return JVal(); }
		GET_TEAM(me->getTeam()).setHasTech(eTech, true, me->GetID(), true, true);
		me->SetNumFreeTechs(me->GetNumFreeTechs() - 1);
		o["free_tech_claimed"] = J(TechName(eTech));
	}
	else
	{
		if(GET_TEAM(me->getTeam()).GetTeamTechs()->HasTech(eTech)) { err = std::string("you already know ") + TechName(eTech); return JVal(); }
		if(!me->GetPlayerTechs()->CanEverResearch(eTech)) { err = std::string("your civilization can never research ") + TechName(eTech); return JVal(); }
		if(bAdd && me->GetPlayerTechs()->IsResearchingTech(eTech)) { err = std::string(TechName(eTech)) + " is already in your research queue"; return JVal(); }
		std::vector<TechTypes> previous;
		for(const CLLNode<TechTypes>* n = me->headResearchQueueNode(); n; n = me->nextResearchQueueNode(n)) previous.push_back(n->m_data);
		// pushResearch returns early, queue untouched, for a tech already queued.
		if(!bAdd) me->clearResearchQueue();
		bool bQueued = me->pushResearch(eTech, false);
		if(bQueued && bPreview) o["research_would_be"] = ResearchJson(me);
		if(!bQueued || bPreview)
		{
			me->clearResearchQueue();
			for(size_t i = 0; i < previous.size(); ++i) me->pushResearch(previous[i]);
		}
		if(!bQueued) { err = std::string("cannot research ") + TechName(eTech) + ": a technology on the way to it can never be researched"; return JVal(); }
		if(bPreview)
		{
			o["previewed_only"] = J(true);
			return JVal(o);
		}
	}
	o["ok"] = J(true);
	o["research"] = ResearchJson(me);
	return JVal(o);
}

MCP_TOOL(ToolEndTurn)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvGame& g = GC.getGame();
	bool bForce = ArgBool(args, "force", false);
	std::string capture = PendingCaptureDecision(me);
	if(!capture.empty()) { err = "end of turn blocked - " + capture; return JVal(); }
	EndTurnBlockingTypes eBlock = EndTurnBlocking(me);
	bool bIdleUnits = (eBlock == ENDTURN_BLOCKING_UNITS);
	if(eBlock != NO_ENDTURN_BLOCKING_TYPE && !(bIdleUnits && bForce))
	{
		err = "end of turn blocked - " + BlockingName(eBlock);
		return JVal();
	}
	if(McpSeatOnScreen() && AiProcessing())
	{
		err = "the game is still processing other civilizations; wait a moment and try again";
		return JVal();
	}
	if(bIdleUnits) me->EndTurnsForReadyUnits();
	if(McpSeatOnScreen())
	{
		if(!g.isNetworkMultiPlayer() && GC.GetPostTurnAutosaves())
			gDLL->AutoSave(false, true);
		gDLL->sendTurnComplete();
	}
	else
	{
		me->setEndTurn(true);
		if(!me->isEndTurn())
		{
			if(me->hasReadyUnit()) { err = "a unit still needs orders; see units"; return JVal(); }
			err = "a unit or city stayed busy for several seconds; call end_turn again";
			McpBridge::RetryLater();
			return JVal();
		}
	}
	JObj o;
	o["ok"] = J(true);
	o["ended_turn"] = J(g.getGameTurn());
	o["next"] = J("call wait_for_my_turn, then briefing; the turn comes back to you early if a unit finishes a queued move with moves to spare");
	return JVal(o);
}

MCP_TOOL(ToolAcknowledgeMessages)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	std::vector<int> ids;
	JObj::const_iterator it = args.find("ids");
	if(HasArg(args, "ids"))
	{
		if(!it->second.is<JArr>() || it->second.get<JArr>().empty()) { err = "ids must be a non-empty array of message ids; omit it to acknowledge every unread message"; return JVal(); }
		const JArr& list = it->second.get<JArr>();
		for(size_t i = 0; i < list.size(); ++i)
		{
			int id = 0;
			if(!IsInt(list[i], id)) { err = McpFormat("ids[%d] is not an integer message id", (int)i); return JVal(); }
			ids.push_back(id);
		}
	}
	int n = McpBridge::Get().Acknowledge(me->GetID(), ids.empty() ? NULL : &ids);
	JObj o;
	o["acknowledged"] = J(n);
	o["still_unread"] = J(McpBridge::Get().CountUnread(me->GetID()));
	return JVal(o);
}

namespace
{
	// Letters, digits, spaces, dots and dashes only: the name is shown in the game, where brackets are markup.
	std::string CleanName(const std::string& s)
	{
		std::string out;
		for(size_t i = 0; i < s.size() && out.size() < 24; ++i)
		{
			unsigned char ch = (unsigned char)s[i];
			if(isalnum(ch) || ch == ' ' || ch == '.' || ch == '-') out += (char)ch;
		}
		size_t b = out.find_first_not_of(' ');
		return (b == std::string::npos) ? "" : out.substr(b, out.find_last_not_of(' ') - b + 1);
	}
}

MCP_TOOL(ToolSetIdentity)
{
	McpIdentity id;
	id.provider = CleanName(ArgStr(args, "provider"));
	id.model = CleanName(ArgStr(args, "model"));
	id.version = CleanName(ArgStr(args, "version"));
	bool bHasLetter = false;
	for(size_t i = 0; i < id.model.size(); ++i) bHasLetter = bHasLetter || isalnum((unsigned char)id.model[i]);
	if(!bHasLetter) { err = "model is required (letters, digits, spaces, dots or dashes), e.g. Opus"; return JVal(); }
	McpBridge::Get().SetCurrentIdentity(id);
	std::string label = McpIdentityLabel(id);
	JObj o;
	o["ok"] = J(true);
	o["playing_as"] = J(label);
	McpSession session = McpBridge::Get().Current();
	if(session.seat >= 0)
	{
		McpBindSeatLabel((PlayerTypes)session.seat, label);
		return JVal(o);
	}
	// A seat this same model left behind (a reconnect) is taken back at once.
	std::vector<PlayerTypes> waiting = McpUnboundSeats();
	for(size_t i = 0; i < waiting.size(); ++i)
	{
		if(!McpSeatLabelMatches(waiting[i], label)) continue;
		std::string ignored;
		if(McpClaimSeat(waiting[i], ignored)) o["resumed"] = J("you are back on " + CivName(waiting[i]));
		return JVal(o);
	}
	if(!waiting.empty()) o["seats_waiting_for_a_player"] = JVal(SeatsWaiting());
	return JVal(o);
}

MCP_TOOL(ToolTakeSeat)
{
	if(!RequireIdentity(err)) return JVal();
	std::string key = ArgStr(args, "civ");
	PlayerTypes e = NO_PLAYER;
	if(key == "human" || key == "screen")
	{
		McpSession session = McpBridge::Get().Current();
		if(session.seat >= 0) { err = "you hold a seat; call release_seat first"; return JVal(); }
		McpBridge::Get().SetCurrentSeat(MCP_SEAT_SCREEN);
		JObj o;
		o["ok"] = J(true);
		o["playing"] = J("the civilization on screen, with the human");
		return JVal(o);
	}
	bool bAssignedOpponent = false;
	if(key == "opponent") { e = McpWaitingOpponentSeat(); bAssignedOpponent = (e != NO_PLAYER); } // the one opponent seat left behind, before a fresh pick
	if(e == NO_PLAYER && (key == "teammate" || key == "opponent"))
	{
		std::vector<PlayerTypes> choices = ClaimableCivs(key == "teammate");
		if(choices.empty()) { err = "no computer-run " + key + " civilization is left to play"; return JVal(); }
		e = choices[GC.getGame().getAsyncRandNum((int)choices.size(), "MCP seat pick")];
	}
	else if(e == NO_PLAYER) e = FindPlayer(key);
	if(e == NO_PLAYER) { err = "unknown civilization '" + key + "'; use a name from civs, or teammate, opponent or human"; return JVal(); }
	if(!McpClaimSeat(e, err, bAssignedOpponent)) return JVal();
	JObj o;
	o["ok"] = J(true);
	o["playing"] = J(CivName(e));
	o["your_civilization"] = CivilizationRules(GET_PLAYER(e).getCivilizationType(), GET_PLAYER(e).getLeaderType());
	if(!McpOnScreenTeam(e)) o["secret"] = J("you are the human's opponent: never tell them which civilization you play; they will see you once they meet you");
	o["next"] = J("call wait_for_my_turn; your turn comes after the human's each round");
	return JVal(o);
}

MCP_TOOL(ToolReleaseSeat)
{
	if(!RequireIdentity(err)) return JVal();
	if(!McpReleaseSeat(err)) return JVal();
	JObj o;
	o["ok"] = J(true);
	return JVal(o);
}

MCP_TOOL(ToolSkipHumanTurn)
{
	if(!RequireIdentity(err)) return JVal();
	if(!McpBridge::Get().TestingMode()) { err = "skip_human_turn is a testing tool: call testing_mode with enabled=true first"; return JVal(); }
	PlayerTypes e = GC.getGame().getActivePlayer();
	if(e == NO_PLAYER || !GET_PLAYER(e).isHuman()) { err = "no human player is on screen"; return JVal(); }
	if(McpSeatOnScreen()) { err = "the player on screen is your own seat; use end_turn"; return JVal(); }
	CvPlayer& p = GET_PLAYER(e);
	if(!McpIsPlayerTurn(p)) { err = "it is not the screen player's turn right now"; return JVal(); }
	p.EndTurnsForReadyUnits();
	gDLL->sendTurnComplete();
	JObj o;
	o["ok"] = J(true);
	o["skipped_turn_of"] = J(CivName(e));
	return JVal(o);
}

MCP_TOOL(ToolTestingMode)
{
	bool bOn = ArgBool(args, "enabled", false);
	McpBridge::Get().SetTestingMode(bOn);
	JObj o;
	o["ok"] = J(true);
	o["enabled"] = J(bOn);
	return JVal(o);
}

MCP_TOOL(ToolLog)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	std::string text = ArgStr(args, "text");
	if(text.empty()) { err = "text is required"; return JVal(); }
	McpBridge::Get().LogLine(MCP_LOG_REASONING, text);
	JObj o;
	o["ok"] = J(true);
	return JVal(o);
}

MCP_TOOL(ToolChat)
{
	if(!RequireIdentity(err)) return JVal();
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	std::string text = ArgStr(args, "text");
	if(text.empty()) { err = "text is required"; return JVal(); }
	if(McpSeatOnScreen() || !McpOnScreenTeam(me->GetID())) { err = "chat is for a seat with a human teammate; you have none"; return JVal(); }
	McpBridge::Get().ReplyChat(text);
	JObj o;
	o["ok"] = J(true);
	return JVal(o);
}

MCP_TOOL(ToolMapImage)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	int cx = 0, cy = 0;
	if(!ResolveMapCenter(me, args, cx, cy, err)) return JVal();
	int radius = ArgInt(args, "radius", 5);
	if(radius < 1) radius = 1;
	if(radius > 8) radius = 8;
	McpBridge::Get().LogLine(MCP_LOG_EMPIRE, McpFormat("Looks at a map picture around (%d, %d), radius %d", cx, cy, radius));
	std::string summary;
	JArr cities, units;
	std::string png = McpRenderMapPng(me->GetID(), cx, cy, radius, summary, cities, units);
	if(png.empty()) { err = summary; return JVal(); }
	JObj o;
	o["center_x"] = J(cx);
	o["center_y"] = J(cy);
	o["radius"] = J(radius);
	o["note"] = J(summary);
	o["cities_in_picture"] = JVal(cities);
	o["units_in_picture"] = JVal(units);
	o["image_png_base64"] = J(McpBase64(png));
	return JVal(o);
}

// ------------------------------------------------------------------ registry

namespace
{
	const char* SCHEMA_EMPTY = "{\"type\":\"object\",\"properties\":{}}";
	const char* SCHEMA_UNIT = "{\"type\":\"object\",\"properties\":{\"unit_id\":{\"type\":\"integer\"}},\"required\":[\"unit_id\"]}";
	const char* SCHEMA_CITY = "{\"type\":\"object\",\"properties\":{\"city_id\":{\"type\":\"integer\"}},\"required\":[\"city_id\"]}";
	const char* SCHEMA_UNIT_XY_PREVIEW = "{\"type\":\"object\",\"properties\":{\"unit_id\":{\"type\":\"integer\"},\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"preview_only\":{\"type\":\"boolean\"}},\"required\":[\"unit_id\",\"x\",\"y\"]}";
	const char* SCHEMA_CITY_XY = "{\"type\":\"object\",\"properties\":{\"city_id\":{\"type\":\"integer\"},\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"}},\"required\":[\"city_id\",\"x\",\"y\"]}";

	const McpToolDef TOOLS[] =
	{
		{ MCP_TOOL_STATUS,
		  "Current turn, who you are, whether it is your turn, and what blocks ending it. Cheap; call it freely.",
		  SCHEMA_EMPTY, ToolStatus, LOG_NONE, false },
		{ "wait_for_my_turn",
		  "Blocks until it is your turn, a new request from another leader arrives, or your human teammate messages you (25 seconds by default; timeout_seconds sets another limit, up to 55), then returns status. Call it again if is_my_turn is still false.",
		  "{\"type\":\"object\",\"properties\":{\"timeout_seconds\":{\"type\":\"number\"}}}", ToolStatus, LOG_NONE, true },
		{ "briefing",
		  "Start every turn with this: what happened since your last turn, game messages you must acknowledge, decisions waiting for you, nearby enemies, economy warnings, and a diplomacy snapshot. Events from last turn that a briefing for this civilization showed during that turn, even to a session before yours, are left out; since_turn shows every event from that turn on.",
		  "{\"type\":\"object\",\"properties\":{\"since_turn\":{\"type\":\"integer\"}}}", ToolBriefing, LOG_NONE, false },
		{ "overview",
		  "Your empire at a glance: treasury, yields, happiness, research, every city and every unit with its activity.",
		  SCHEMA_EMPTY, ToolOverview, LOG_NONE, false },
		{ "units",
		  "All your units with position, moves left, health, strength and current activity. needs_orders=true means the unit is idle this turn.",
		  SCHEMA_EMPTY, ToolUnits, LOG_NONE, false },
		{ "unit_options",
		  "What one unit can do right now: actions for unit_action, tile improvements it can build here, promotions available, enemies it can attack, and which adjacent tiles it can enter. For a settler, once you have a city, recommended_sites lists up to three city sites with the turns to reach each: the ones the game marks for a human player who selects that settler. They are the game's suggestions; where to settle is your choice.",
		  SCHEMA_UNIT, ToolUnitOptions, LOG_NONE, false },
		{ "cities",
		  "All your cities: population, growth, production, the buildings each already has, focus and per-turn yields.",
		  SCHEMA_EMPTY, ToolCities, LOG_NONE, false },
		{ "city_options",
		  "Everything one city can build or buy right now: units, buildings and projects with production cost and turns, gold and faith prices, and the tiles it can buy. Buildings also show their upkeep and effects. Use the type strings with set_production or buy.",
		  SCHEMA_CITY, ToolCityOptions, LOG_NONE, false },
		{ "map",
		  "Explored tiles within radius (default 3, max 8) of a point (default: your capital). Hex grid with offset rows: odd rows sit half a tile east of even rows, so use the dist field rather than computing distances. Unexplored tiles are counted, not listed. Only visible_now tiles show units. Compass: y increases NORTHWARD (y=0 is the south edge) and x increases eastward.",
		  "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"radius\":{\"type\":\"integer\"}}}", ToolMap, LOG_NONE, false },
		{ "research_options",
		  "Technologies you can research now, with cost, turns, what they give and what each leads to.",
		  SCHEMA_EMPTY, ToolResearchOptions, LOG_NONE, false },
		{ "policy_options",
		  "Social policy branches and policies: what is unlocked, adoptable now, and what each does; plus your culture and the next policy cost.",
		  SCHEMA_EMPTY, ToolPolicyOptions, LOG_NONE, false },
		{ "game_settings",
		  "How this game was set up, fixed for its whole life: start era, map type and size, difficulty, pace, victory types, every game option switched on (including ones setup mods add, such as no happiness) and the map script's own options.",
		  SCHEMA_EMPTY, ToolGameSettings, LOG_NONE, false },
		{ "rankings",
		  "Where you stand, as the game's Demographics and score screens show it: your rank and the best, average and worst for population, food, production, gold, land, military, approval and literacy (best and worst are named only if you have met them); your score breakdown; and each met civilization's score. With the InfoAddict mod loaded, met civilizations also show InfoAddict's figures. history_turns (up to 100) with history_metrics adds the game's per-turn history, e.g. [\"science_yield_per_turn\", \"military_might\"].",
		  "{\"type\":\"object\",\"properties\":{\"history_turns\":{\"type\":\"integer\"},\"history_metrics\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}", ToolRankings, LOG_NONE, false },
		{ "mods",
		  "The mods loaded in this game, each with its version and the description its author wrote, so you know how the game differs from the standard rules. They can change between loads of the same game.",
		  SCHEMA_EMPTY, ToolMods, LOG_NONE, false },
		{ "civs",
		  "Every civilization and city-state you have met: war or peace, their attitude toward you, friendship levels, relative strength, and known capitals.",
		  SCHEMA_EMPTY, ToolCivs, LOG_NONE, false },
		{ "lookup",
		  "The rules of the game being played, read from its data tables as the Civilopedia shows them, mods' changes included. Give a type key: TECH_..., UNIT_..., BUILDING_... (wonders too), POLICY_..., POLICY_BRANCH_..., PROMOTION_..., IMPROVEMENT_..., RESOURCE_..., BELIEF_..., CIVILIZATION_..., TERRAIN_..., FEATURE_... (natural wonders too), SPECIALIST_... or PROJECT_..., or RULES for game-wide numbers such as unhappiness per city, heal rates and city-state influence levels. It gives costs, requirements, upgrades, upkeep and yields, and for a tech what it leads to. Once you hold a seat, costs are this game's and units and buildings are your civilization's own versions. Every item it names carries its key.",
		  "{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\"}},\"required\":[\"type\"]}", ToolLookup, LOG_NONE, false },
		{ "move_unit",
		  "Order a unit to move to a tile. Multi-turn moves continue when you end each turn (the unit is still in place when your next turn opens); do not re-issue them. Fails with a reason if there is no path. preview_only=true shows the path and turns without moving. To attack, use attack.",
		  SCHEMA_UNIT_XY_PREVIEW, ToolMoveUnit, LOG_MOVE, false },
		{ "unit_action",
		  "Give a unit an order on its current tile. action is one of the names unit_options lists under actions, build (with build=BUILD_... from builds_here), rebase (aircraft, with x and y of one of your cities or carriers), air_sweep (fighters, x and y of the tile to clear of interceptors), paradrop (paratroopers, x and y of the landing tile), nuke (atomic bombs and nuclear missiles, x and y of the target), or disband. Aircraft bomb with the attack tool. explore (land or sea combat units) and automate_work (workers and work boats) hand the unit to the game's own AI, which moves it every turn until you give it any order or call stop_automation; it knows nothing of your plans, so this trades control for speed.",
		  "{\"type\":\"object\",\"properties\":{\"unit_id\":{\"type\":\"integer\"},\"action\":{\"type\":\"string\"},\"build\":{\"type\":\"string\"},\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"}},\"required\":[\"unit_id\",\"action\"]}", ToolUnitAction, LOG_UNIT, false },
		{ "attack",
		  "Attack the enemy unit or city on a tile with one of your units (melee needs to be adjacent; ranged units strike from range). Returns the game's own damage estimate; pass preview_only=true to see the estimate without attacking.",
		  SCHEMA_UNIT_XY_PREVIEW, ToolAttack, LOG_COMBAT, false },
		{ "city_attack",
		  "Have a city bombard an enemy unit in range (cities with can_attack=true).",
		  SCHEMA_CITY_XY, ToolCityAttack, LOG_COMBAT, false },
		{ "promote_unit",
		  "Give a unit a promotion (type from unit_options.promotions_available).",
		  "{\"type\":\"object\",\"properties\":{\"unit_id\":{\"type\":\"integer\"},\"promotion\":{\"type\":\"string\"}},\"required\":[\"unit_id\",\"promotion\"]}", ToolPromoteUnit, LOG_UNIT, false },
		{ "upgrade_unit",
		  "Upgrade a unit for gold (units list upgrade_available_to and upgrade_cost). The unit keeps its position; its id changes.",
		  SCHEMA_UNIT, ToolUpgradeUnit, LOG_UNIT, false },
		{ "set_production",
		  "Set what a city builds. item is a type string from city_options (UNIT_..., BUILDING_..., PROJECT_... or PROCESS_...). Replaces the current item unless append=true, which adds it to the end of the queue.",
		  "{\"type\":\"object\",\"properties\":{\"city_id\":{\"type\":\"integer\"},\"item\":{\"type\":\"string\"},\"append\":{\"type\":\"boolean\"}},\"required\":[\"city_id\",\"item\"]}", ToolSetProduction, LOG_CITY, false },
		{ "buy",
		  "Buy a unit or building in a city with gold (item type from city_options; the price is buy_gold), or with faith when with_faith=true (price buy_faith).",
		  "{\"type\":\"object\",\"properties\":{\"city_id\":{\"type\":\"integer\"},\"item\":{\"type\":\"string\"},\"with_faith\":{\"type\":\"boolean\"}},\"required\":[\"city_id\",\"item\"]}", ToolBuy, LOG_CITY, false },
		{ "buy_tile",
		  "Buy a tile for a city with gold (tiles and prices from city_options.buyable_tiles).",
		  SCHEMA_CITY_XY, ToolBuyTile, LOG_CITY, false },
		{ "set_city_focus",
		  "Set how a city assigns its citizens (focus names as shown in cities.focus; an unknown name lists them). Optional avoid_growth=true/false.",
		  "{\"type\":\"object\",\"properties\":{\"city_id\":{\"type\":\"integer\"},\"focus\":{\"type\":\"string\"},\"avoid_growth\":{\"type\":\"boolean\"}},\"required\":[\"city_id\",\"focus\"]}", ToolSetCityFocus, LOG_CITY, false },
		{ "city_citizens",
		  "What every citizen of a city does: worked tiles (locked ones are your choice), tiles it could work instead, specialists per building, and unemployed citizens.",
		  "{\"type\":\"object\",\"properties\":{\"city_id\":{\"type\":\"integer\"}},\"required\":[\"city_id\"]}", ToolCityCitizens, LOG_NONE, false },
		{ "city_work_tile",
		  "Lock a tile so the city works it, or free a locked or worked tile (the citizen becomes unemployed until reassigned), exactly like clicking the tile in the city screen. A tile another of your cities works is moved to this city first. The city's own tile resets every tile and specialist to automatic.",
		  SCHEMA_CITY_XY, ToolCityWorkTile, LOG_CITY, false },
		{ "city_specialist",
		  "Put a citizen to work as a specialist in a building (type from city_citizens.specialists), or take one out with remove=true.",
		  "{\"type\":\"object\",\"properties\":{\"city_id\":{\"type\":\"integer\"},\"building\":{\"type\":\"string\"},\"remove\":{\"type\":\"boolean\"}},\"required\":[\"city_id\",\"building\"]}", ToolCitySpecialist, LOG_CITY, false },
		{ "set_research",
		  "Choose the technology to research (type string from research_options, e.g. TECH_POTTERY). A technology further down the tree replaces your queue with the path to it, as clicking it on the tech tree does. add_to_queue=true instead adds it, with any technologies it still needs, to the end of the queue, as a Shift-click does; to reorder the queue, set the first technology, then add the others in the order you want. preview_only=true shows the queue a choice would make without changing anything. research.queued_after lists the queue after the current technology, with the turns each one takes on its own at your current science. When you have a free technology to claim, this grants it immediately instead, but free technologies can only be used for technologies that you are currently able to research immediately (not future techs).",
		  "{\"type\":\"object\",\"properties\":{\"tech\":{\"type\":\"string\"},\"add_to_queue\":{\"type\":\"boolean\"},\"preview_only\":{\"type\":\"boolean\"}},\"required\":[\"tech\"]}", ToolSetResearch, LOG_EMPIRE, false },
		{ "adopt_policy",
		  "Adopt a social policy (POLICY_...) or open a policy branch (POLICY_BRANCH_...) using your culture; see policy_options. Switching to a branch blocked by a rival branch costs a turn of anarchy and needs accept_anarchy=true.",
		  "{\"type\":\"object\",\"properties\":{\"policy\":{\"type\":\"string\"},\"accept_anarchy\":{\"type\":\"boolean\"}},\"required\":[\"policy\"]}", ToolAdoptPolicy, LOG_EMPIRE, false },
		{ "declare_war",
		  "Declare war on a civilization or city-state (by name from civs). Irreversible for many turns; be sure.",
		  "{\"type\":\"object\",\"properties\":{\"civ\":{\"type\":\"string\"}},\"required\":[\"civ\"]}", ToolDeclareWar, LOG_DIPLOMACY, false },
		{ MCP_TOOL_END_TURN,
		  "End your turn, after reading the briefing and giving orders. summary is a sentence or two on what you did this turn, for the human reading the log. Refused while a decision is pending (research, production, policy...). If units still await orders it is refused too, unless force=true, which makes them skip this turn.",
		  "{\"type\":\"object\",\"properties\":{\"summary\":{\"type\":\"string\"},\"force\":{\"type\":\"boolean\"}},\"required\":[\"summary\"]}", ToolEndTurn, LOG_TURN, false },
		{ "acknowledge_messages",
		  "Confirm you have read game messages (briefing.game_messages entries with unread=true). Actions are refused while any are unread. ids lists specific message ids; omit it to acknowledge all. Informational dialogs close in the game once acknowledged.",
		  "{\"type\":\"object\",\"properties\":{\"ids\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}}}", ToolAcknowledgeMessages, LOG_MESSAGES, false },
		{ "diplomacy",
		  "Relations with every civilization you have met (war, embassies, open borders, pacts, friendship); each one's teammates and its wars, friendships and denouncements with others you have met (your own relation to each is in its own fields); the deals in force; and requests from other leaders waiting for your answer. Requests are kept in memory only: they do not survive a game reload.",
		  SCHEMA_EMPTY, ToolDiplomacy, LOG_NONE, false },
		{ "respond_to_request",
		  "Answer a request from another leader (id and choices from diplomacy.requests or briefing.diplomacy_requests). Deal offers take accept or decline; warnings take the listed choices; statements take acknowledge.",
		  "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"choice\":{\"type\":\"string\"}},\"required\":[\"id\",\"choice\"]}", ToolRespondToRequest, LOG_DIPLOMACY, false },
		{ "deal_options",
		  "What a civilization could put in a deal with you and what you could put in one with them, as the game's trade screen lists it: gold on hand, gold per turn, luxury and strategic resources to spare (with how many), cities, embassy, open borders, defensive pact, research agreement, and who each could be asked to fight or make peace with. Gold and agreements that cannot be offered right now come with the reason; resources, cities and third parties that cannot be traded are left out. Whether they would accept is a different question: ask propose_deal with preview_only.",
		  "{\"type\":\"object\",\"properties\":{\"civ\":{\"type\":\"string\"}},\"required\":[\"civ\"]}", ToolDealOptions, LOG_NONE, false },
		{ "propose_deal",
		  "Offer a deal to a civilization (deal_options lists what each side has to offer). give and get are lists of items: {item: gold|gold_per_turn|resource|open_borders|embassy|defensive_pact|research_agreement|maps|declaration_of_friendship (human players only; ask a computer player with discuss)|city|third_party_war|third_party_peace, amount, resource, city_id, civ}. With preview_only=true you learn whether the game's AI would accept and what would make it work, without proposing. A human decides on their own trade screen.",
		  "{\"type\":\"object\",\"properties\":{\"civ\":{\"type\":\"string\"},\"give\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"get\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"preview_only\":{\"type\":\"boolean\"}},\"required\":[\"civ\"]}", ToolProposeDeal, LOG_DIPLOMACY, false },
		{ "make_peace",
		  "Negotiate peace with a civilization you are at war with. The game's AI sets the terms it will accept (white peace, or a surrender one way or the other); preview_only=true shows them without agreeing.",
		  "{\"type\":\"object\",\"properties\":{\"civ\":{\"type\":\"string\"},\"preview_only\":{\"type\":\"boolean\"}},\"required\":[\"civ\"]}", ToolMakePeace, LOG_DIPLOMACY, false },
		{ "answer_deal",
		  "Accept or decline a deal a human proposed to you (listed in diplomacy.requests as human_deal_offer): civ names them, accept is true or false. withdraw=true instead takes back an offer of yours they have not answered.",
		  "{\"type\":\"object\",\"properties\":{\"civ\":{\"type\":\"string\"},\"accept\":{\"type\":\"boolean\"},\"withdraw\":{\"type\":\"boolean\"}},\"required\":[\"civ\"]}", ToolAnswerDeal, LOG_DIPLOMACY, false },
		{ "demand",
		  "Demand tribute from an AI civilization: get lists the items (as in propose_deal). They give in only if they fear you; refusals sour relations.",
		  "{\"type\":\"object\",\"properties\":{\"civ\":{\"type\":\"string\"},\"get\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}},\"required\":[\"civ\",\"get\"]}", ToolDemand, LOG_DIPLOMACY, false },
		{ "discuss",
		  "Talk to a leader: topic is ask_friendship, end_friendship, denounce, stop_settling, stop_spying, stop_converting, stop_digging or joint_war (with target: the civilization to fight). For ask_friendship, their_reply is the leader's answer in the leader's own words, as a human player would hear it.",
		  "{\"type\":\"object\",\"properties\":{\"civ\":{\"type\":\"string\"},\"topic\":{\"type\":\"string\"},\"target\":{\"type\":\"string\"}},\"required\":[\"civ\",\"topic\"]}", ToolDiscuss, LOG_DIPLOMACY, false },
		{ "religion_options",
		  "Your faith, pantheon and religion, and every religious choice waiting for you: pantheon beliefs, founding or enhancing a religion, reformation beliefs, and great people to claim (faith, free or Maya).",
		  SCHEMA_EMPTY, ToolReligionOptions, LOG_NONE, false },
		{ "found_pantheon", "Found your pantheon with a belief from religion_options.",
		  "{\"type\":\"object\",\"properties\":{\"belief\":{\"type\":\"string\"}},\"required\":[\"belief\"]}", ToolFoundPantheon, LOG_EMPIRE, false },
		{ "found_religion", "Found a religion once a Great Prophet has started it (unit_action found_religion): religion, beliefs [founder, follower] and optionally name and city_id for the holy city.",
		  "{\"type\":\"object\",\"properties\":{\"religion\":{\"type\":\"string\"},\"beliefs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"name\":{\"type\":\"string\"},\"city_id\":{\"type\":\"integer\"}},\"required\":[\"religion\",\"beliefs\"]}", ToolFoundReligion, LOG_EMPIRE, false },
		{ "enhance_religion", "Enhance your religion with beliefs [follower, enhancer] after a Great Prophet started it (unit_action enhance_religion).",
		  "{\"type\":\"object\",\"properties\":{\"beliefs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"beliefs\"]}", ToolEnhanceReligion, LOG_EMPIRE, false },
		{ "add_reformation_belief", "Add the reformation belief your religion has earned.",
		  "{\"type\":\"object\",\"properties\":{\"belief\":{\"type\":\"string\"}},\"required\":[\"belief\"]}", ToolAddReformationBelief, LOG_EMPIRE, false },
		{ "choose_great_person", "Claim a great person you are owed: unit is a great person type from religion_options, source is faith, free or maya.",
		  "{\"type\":\"object\",\"properties\":{\"unit\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"}},\"required\":[\"unit\",\"source\"]}", ToolChooseGreatPerson, LOG_EMPIRE, false },
		{ "congress", "The World Congress: session timing, your votes and proposals, resolutions in force, proposals awaiting votes with their choices, and what you can propose.",
		  SCHEMA_EMPTY, ToolCongress, LOG_NONE, false },
		{ "propose_resolution", "Propose a resolution (resolution type from congress with a choice where one is needed) or a repeal (repeal_id).",
		  "{\"type\":\"object\",\"properties\":{\"resolution\":{\"type\":\"string\"},\"choice\":{},\"repeal_id\":{\"type\":\"integer\"}}}", ToolProposeResolution, LOG_EMPIRE, false },
		{ "vote_resolution", "Cast votes on a proposal awaiting votes: resolution_id, choice (number or text from congress) and votes (default: all remaining).",
		  "{\"type\":\"object\",\"properties\":{\"resolution_id\":{\"type\":\"integer\"},\"choice\":{},\"votes\":{\"type\":\"integer\"}},\"required\":[\"resolution_id\"]}", ToolVoteResolution, LOG_EMPIRE, false },
		{ "abstain", "Abstain with some or all of your remaining votes.",
		  "{\"type\":\"object\",\"properties\":{\"votes\":{\"type\":\"integer\"}}}", ToolAbstain, LOG_EMPIRE, false },
		{ "spies", "Your spies: where they are, what they are doing, coup chances, and technologies waiting to be stolen.",
		  SCHEMA_EMPTY, ToolSpies, LOG_NONE, false },
		{ "move_spy", "Send a spy (spy_index) to a city named by x,y or by civ and city, optionally as_diplomat; or withdraw=true to pull it out.",
		  "{\"type\":\"object\",\"properties\":{\"spy_index\":{\"type\":\"integer\"},\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"civ\":{\"type\":\"string\"},\"city\":{\"type\":\"string\"},\"as_diplomat\":{\"type\":\"boolean\"},\"withdraw\":{\"type\":\"boolean\"}},\"required\":[\"spy_index\"]}", ToolMoveSpy, LOG_EMPIRE, false },
		{ "stage_coup", "Attempt a coup in the city-state where the spy is; the spy may die.",
		  "{\"type\":\"object\",\"properties\":{\"spy_index\":{\"type\":\"integer\"}},\"required\":[\"spy_index\"]}", ToolStageCoup, LOG_EMPIRE, false },
		{ "steal_tech", "Take a stolen technology: civ it was stolen from and tech from spies.techs_to_steal.",
		  "{\"type\":\"object\",\"properties\":{\"civ\":{\"type\":\"string\"},\"tech\":{\"type\":\"string\"}},\"required\":[\"civ\",\"tech\"]}", ToolStealTech, LOG_EMPIRE, false },
		{ "city_state", "City-states you have met (or one, with civ): influence, status, quests for you, and the actions open to you.",
		  "{\"type\":\"object\",\"properties\":{\"civ\":{\"type\":\"string\"}}}", ToolCityState, LOG_NONE, false },
		{ "city_state_action", "Act toward a city-state: gift_gold (amount), gift_unit (unit_id), bully_gold, bully_worker, pledge_protection, revoke_protection, buyout, make_peace.",
		  "{\"type\":\"object\",\"properties\":{\"civ\":{\"type\":\"string\"},\"action\":{\"type\":\"string\"},\"amount\":{\"type\":\"integer\"},\"unit_id\":{\"type\":\"integer\"}},\"required\":[\"civ\",\"action\"]}", ToolCityStateAction, LOG_DIPLOMACY, false },
		{ "choose_ideology", "Pick your ideology (Freedom, Order or Autocracy) when the game allows it; without ideology it lists them and whether you can choose now.",
		  "{\"type\":\"object\",\"properties\":{\"ideology\":{\"type\":\"string\"}}}", ToolChooseIdeology, LOG_EMPIRE, false },
		{ "switch_ideology", "Give in to public opinion and switch to the ideology your people prefer (costs turns of anarchy).",
		  SCHEMA_EMPTY, ToolSwitchIdeology, LOG_EMPIRE, false },
		{ "archaeology", "Completed digs: without choice it shows the dig and the options; with choice it decides (artifact, artifact_for_other_civ, landmark, culture_boost, written_artifact, do_nothing).",
		  "{\"type\":\"object\",\"properties\":{\"choice\":{\"type\":\"string\"}}}", ToolArchaeology, LOG_EMPIRE, false },
		{ "great_works", "Your great works by city and building slot, and your tourism.",
		  SCHEMA_EMPTY, ToolGreatWorks, LOG_NONE, false },
		{ "move_great_work", "Move a great work between slots (swapping if the target is full): from_city_id, from_building, from_slot, to_city_id, to_building, to_slot.",
		  "{\"type\":\"object\",\"properties\":{\"from_city_id\":{\"type\":\"integer\"},\"from_building\":{\"type\":\"string\"},\"from_slot\":{\"type\":\"integer\"},\"to_city_id\":{\"type\":\"integer\"},\"to_building\":{\"type\":\"string\"},\"to_slot\":{\"type\":\"integer\"}},\"required\":[\"from_city_id\",\"from_building\",\"from_slot\",\"to_city_id\",\"to_building\",\"to_slot\"]}", ToolMoveGreatWork, LOG_EMPIRE, false },
		{ "trade_route_options", "Where a caravan or cargo ship (unit_id, inside one of your cities) can open a trade route, and of which type.",
		  SCHEMA_UNIT, ToolTradeRouteOptions, LOG_NONE, false },
		{ "establish_trade_route", "Send a caravan or cargo ship (unit_id) to the city at x,y with type international, food or production.",
		  "{\"type\":\"object\",\"properties\":{\"unit_id\":{\"type\":\"integer\"},\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"type\":{\"type\":\"string\"}},\"required\":[\"unit_id\",\"x\",\"y\",\"type\"]}", ToolEstablishTradeRoute, LOG_UNIT, false },
		{ "launch_spaceship", "Launch the completed spaceship for a science victory.",
		  SCHEMA_EMPTY, ToolLaunchSpaceship, LOG_EMPIRE, false },
		{ MCP_TOOL_ADVANCE_ROUND,
		  "TESTING ONLY (needs testing_mode enabled). Ends your turn, ends the on-screen human's turn when it opens, and waits for your next turn (about a minute at most). Returns status plus round_advanced / turn_reopened.",
		  "{\"type\":\"object\",\"properties\":{\"summary\":{\"type\":\"string\"},\"force\":{\"type\":\"boolean\"}}}", NULL, LOG_TURN, false },
		{ "city_task",
		  "Decide a city's fate or status: annex, puppet, raze, stop_razing or liberate (right after a capture, give the city back to the civilization that founded it, if you took it from someone else).",
		  "{\"type\":\"object\",\"properties\":{\"city_id\":{\"type\":\"integer\"},\"task\":{\"type\":\"string\"}},\"required\":[\"city_id\",\"task\"]}", ToolCityTask, LOG_CITY, false },
		{ "set_identity",
		  "Name yourself; call it first on every connection. provider is your maker's product (Claude, ChatGPT, Gemini), model your model family (Opus, Fable, GPT) and version its version (5.5). The game shows it after your leader's name, e.g. Dido (Claude Opus 5.5), and, while you are the only model in the game, on the log button.",
		  "{\"type\":\"object\",\"properties\":{\"provider\":{\"type\":\"string\"},\"model\":{\"type\":\"string\"},\"version\":{\"type\":\"string\"}},\"required\":[\"provider\",\"model\",\"version\"]}", ToolSetIdentity, LOG_TURN, false },
		{ "take_seat",
		  "Play a computer-controlled civilization in a shared game: it becomes a human seat for you at the human's difficulty, and the game's AI stops acting for it. The human on screen tells you which civilization, or civ=teammate or civ=opponent to have the game pick one; claim it during their turn. Also takes back a seat from status.seats_waiting_for_a_player (civ=opponent takes the one opponent seat waiting, without naming it). civ=human plays the civilization on screen instead.",
		  "{\"type\":\"object\",\"properties\":{\"civ\":{\"type\":\"string\"}},\"required\":[\"civ\"]}", ToolTakeSeat, LOG_TURN, false },
		{ "release_seat",
		  "Hand the claimed civilization back to the game's AI.",
		  SCHEMA_EMPTY, ToolReleaseSeat, LOG_TURN, false },
		{ MCP_TOOL_SKIP_HUMAN_TURN,
		  "TESTING ONLY. Ends the turn of the human on screen as if they clicked End Turn, so a test game can run with nobody at the keyboard. Never call it in a real shared game: it ends the human's turn for them.",
		  SCHEMA_EMPTY, ToolSkipHumanTurn, LOG_TURN, false },
		{ "testing_mode",
		  "TESTING ONLY. enabled=true unlocks skip_human_turn and advance_round and closes the idle human's own popups and leader screens unread, so an unattended test game does not stall. Off by default, and off again whenever a game is started or loaded. Never enable it in a real shared game.",
		  "{\"type\":\"object\",\"properties\":{\"enabled\":{\"type\":\"boolean\"}}}", ToolTestingMode, LOG_TURN, false },
		{ "log",
		  "Write a line to the in-game log the human can read. Use it for your reasoning and plans each turn: what you see, what you decided and why.",
		  "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}", ToolLog, LOG_NONE, false },
		{ "chat",
		  "Send a message to your human teammate. It appears in their in-game log window, where they also write to you. Only a seat on the human's team can use it.",
		  "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}", ToolChat, LOG_NONE, false },
		{ "economic_overview",
		  "The Economic Overview screen: where gold comes from and where it goes each turn (cities, trade routes, diplomacy; unit upkeep and supply, building and improvement upkeep), and your strategic and luxury resources with totals, amounts in use and amounts available.",
		  SCHEMA_EMPTY, ToolEconomicOverview, LOG_NONE, false },
		{ "trade_routes",
		  "The Trade Route Overview screen: every route of yours (origin, destination, kind, what it earns you per turn, turns left, the unit running it) and every route other civilizations run into your cities, with what it earns you.",
		  SCHEMA_EMPTY, ToolTradeRoutes, LOG_NONE, false },
		{ "military_overview",
		  "The Military Overview screen: unit supply and where it comes from, units over supply and the production penalty, unit upkeep in gold, and every unit with its promotion choices when one is ready.",
		  SCHEMA_EMPTY, ToolMilitaryOverview, LOG_NONE, false },
		{ "religion_overview",
		  "The Religion screen: your faith, pantheon and religion, every founded religion with its beliefs, founder, holy city and followers, and the religion followed in each of your cities.",
		  SCHEMA_EMPTY, ToolReligionOverview, LOG_NONE, false },
		{ "victory_progress",
		  "The Victory Progress screen: which victories are enabled; original capitals held by each civilization (Domination); your spaceship projects (Science); civilizations you are influential over (Culture); votes held and needed (Diplomatic).",
		  SCHEMA_EMPTY, ToolVictoryProgress, LOG_NONE, false },
		{ "map_image",
		  "A picture of the explored tiles within radius (default 5, max 8) of a point (default: your capital), drawn like the game's strategic view. Hexes in rows, odd rows half a hex east, y growing northward; each row's y is written at the left edge and the top row's x values along the top. Terrain by color: greens grass, brown-tan plains, pale yellow desert, grey-blue tundra, white snow, light blue coast, dark blue ocean; darker shade = hills; dark grey hex with MTN in the middle = mountain. Hexes inside someone's borders are covered in a grid of dots in the owner's color, and the territory's outline is a thick line in that color just inside its edge. Inside each hex, three centered rows: its coordinates x,y in bold on top (followed by +N when more units are stacked there than drawn); the city, units or MTN in the middle; and along the bottom what is on the ground: a short resource name (Iron, Oil, Coal, Horses...), a feature letter (F forest, J jungle, M marsh, O oasis, fp flood plains, ice, X fallout, atl atoll, NW natural wonder) with h added for hills (Fh = forest on hills, h alone = bare hills), and an improvement (the first letters of its name: farm, mine, tradi, plant...; ancient ruins show as ruins). Bright cyan edges = rivers; dark brown lines with a dot = roads. Cities are labeled boxes in the owner's color (units inside a city are not drawn). Units are circles in the owner's color with a short name; the ring says whose: white yours, green a teammate's, red an enemy's, grey anyone else's; +N when more are stacked than shown. Hatched hexes are explored but out of sight now (no units shown); a dark hex with ? was never explored. The legend names each civilization's color and relation. The text beside the picture lists the cities and units in it with exact coordinates. Use it to see borders, fronts and where armies stand; use map for exact tile details.",
		  "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"radius\":{\"type\":\"integer\"}}}", ToolMapImage, LOG_NONE, false },
		{ "known_cities",
		  "Every city of other civilizations and city-states your civilization has seen, with position, the owner as you last saw it, whether you are at war with that owner, and, while the tile is in sight, population, hit points and strength.",
		  SCHEMA_EMPTY, ToolKnownCities, LOG_NONE, false },
	};
}

const McpToolDef* McpGetTools(size_t& count)
{
	count = sizeof(TOOLS) / sizeof(TOOLS[0]);
	return TOOLS;
}

namespace
{
	const char* TypeMismatch(const JVal& property, const JVal& value)
	{
		if(!property.is<JObj>() || value.is<picojson::null>()) return NULL;
		JObj::const_iterator t = property.get<JObj>().find("type");
		if(t == property.get<JObj>().end() || !t->second.is<std::string>()) return NULL;
		const std::string& type = t->second.get<std::string>();
		int i = 0;
		if(type == "boolean") return value.is<bool>() ? NULL : "true or false";
		if(type == "integer") return IsInt(value, i) ? NULL : "a whole number";
		if(type == "number") return value.is<double>() ? NULL : "a number";
		if(type == "string") return value.is<std::string>() ? NULL : "text";
		if(type == "array") return value.is<JArr>() ? NULL : "a list";
		if(type == "object") return value.is<JObj>() ? NULL : "an object";
		return NULL;
	}
}

std::string McpCheckArgs(const McpToolDef& tool, const picojson::object& args)
{
	picojson::value schema;
	if(!picojson::parse(schema, std::string(tool.inputSchema)).empty() || !schema.is<JObj>())
		return McpFormat("the argument schema for %s is broken; this is a bug in the mod", tool.name);
	const JObj& s = schema.get<JObj>();
	JObj props;
	JObj::const_iterator pit = s.find("properties");
	if(pit != s.end() && pit->second.is<JObj>()) props = pit->second.get<JObj>();
	for(JObj::const_iterator it = args.begin(); it != args.end(); ++it)
	{
		JObj::const_iterator prop = props.find(it->first);
		if(prop != props.end())
		{
			const char* kind = TypeMismatch(prop->second, it->second);
			if(kind) return McpFormat("argument '%s' for %s must be %s", it->first.c_str(), tool.name, kind);
			continue;
		}
		std::string known;
		for(JObj::const_iterator k = props.begin(); k != props.end(); ++k) known += (known.empty() ? "" : ", ") + k->first;
		return McpFormat("unknown argument '%s' for %s; it accepts %s", it->first.c_str(), tool.name, known.empty() ? "no arguments" : known.c_str());
	}
	JObj::const_iterator rit = s.find("required");
	if(rit != s.end() && rit->second.is<JArr>())
	{
		const JArr& req = rit->second.get<JArr>();
		for(size_t i = 0; i < req.size(); ++i)
			if(req[i].is<std::string>() && !HasArg(args, req[i].get<std::string>().c_str()))
				return McpFormat("missing required argument '%s' for %s", req[i].get<std::string>().c_str(), tool.name);
	}
	return "";
}

const McpToolDef* McpFindTool(const std::string& name)
{
	for(size_t i = 0; i < sizeof(TOOLS) / sizeof(TOOLS[0]); ++i)
		if(name == TOOLS[i].name) return &TOOLS[i];
	return NULL;
}
