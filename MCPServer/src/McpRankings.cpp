#include "CvGameCoreDLLPCH.h"
#include "McpToolSupport.h"
#include "McpToolHandlers.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvGlobals.h"
#include "CvGameCoreUtils.h"
#include "CvInternalGameCoreUtils.h"
#include "CvTreasury.h"
#include "CvCultureClasses.h"
#include "CvTradeClasses.h"
#include "CvPolicyClasses.h"
#include "CvTechClasses.h"
#include "CvBuildingClasses.h"
#include "CvInfos.h"

using namespace McpTool;

// Where the player's civilization stands: the game's Demographics screen, scores of met
// civilizations, and (with the InfoAddict mod loaded) the figures InfoAddict shows per met
// civilization, plus the game's own per-turn history. Everything is from the point of view
// of the player's civilization: unmet civilizations are never named.

namespace
{
	const char* INFOADDICT_MOD_ID = "aec5d10d-f00f-4fc7-b330-c3a1e86c91c3";
	const int MAX_HISTORY_TURNS = 100;

	int Score(CvPlayer& p) { return p.GetScore(); }
	int RealPopulation(CvPlayer& p) { return p.getRealPopulation(); }
	int Population(CvPlayer& p) { return p.getTotalPopulation(); }
	int Cities(CvPlayer& p) { return p.getNumCities(); }
	int Land(CvPlayer& p) { return p.GetNumPlots(); }
	int LandRecorded(CvPlayer& p) { return p.getTotalLand(); }
	int Food(CvPlayer& p) { return p.calculateTotalYield(YIELD_FOOD); }
	int Production(CvPlayer& p) { return p.calculateTotalYield(YIELD_PRODUCTION); }
	int Gold(CvPlayer& p) { return p.GetTreasury()->GetGold(); }
	int GoldRate(CvPlayer& p) { return p.calculateGoldRate(); }
	int GrossGold(CvPlayer& p) { return p.GetTreasury()->CalculateGrossGold(); }
	int GoldYield(CvPlayer& p) { return p.calculateTotalYield(YIELD_GOLD); }
	int Science(CvPlayer& p) { return p.GetScience(); }
	int ScienceYield(CvPlayer& p) { return p.calculateTotalYield(YIELD_SCIENCE); }
	int Techs(CvPlayer& p) { return GET_TEAM(p.getTeam()).GetTeamTechs()->GetNumTechsKnown(); }
	int CulturePerTurn(CvPlayer& p) { return p.GetTotalJONSCulturePerTurn(); }
	int Culture(CvPlayer& p) { return p.getJONSCulture(); }
	int Policies(CvPlayer& p) { return p.GetPlayerPolicies()->GetNumPoliciesOwned(MOD_BUGFIX_DUMMY_POLICIES); }
	int Faith(CvPlayer& p) { return p.GetFaith(); }
	int FaithPerTurn(CvPlayer& p) { return p.GetFaithPerTurnFromCities() + p.GetFaithPerTurnFromMinorCivs() + p.GetFaithPerTurnFromReligion(); }
	int ExcessHappiness(CvPlayer& p) { return p.GetExcessHappiness(); }
	int MilitaryMight(CvPlayer& p) { return p.GetMilitaryMight(); }
	int GreatWorks(CvPlayer& p) { return p.GetCulture()->GetNumGreatWorks(); }
	int Tourism(CvPlayer& p) { return p.GetCulture()->GetTourism(); }
	int CivsInfluenced(CvPlayer& p) { return p.GetCulture()->GetNumCivsInfluentialOn(); }
	int TradeRoutesUsed(CvPlayer& p) { return p.GetTrade()->GetNumTradeRoutesUsed(true); }
	int Wonders(CvPlayer& p)
	{
		int iCount = 0;
		for(int i = 0; i < GC.getNumBuildingInfos(); ++i)
		{
			CvBuildingEntry* b = GC.getBuildingInfo((BuildingTypes)i);
			if(b && ::isWorldWonderClass(b->GetBuildingClassInfo())) iCount += p.countNumBuildings((BuildingTypes)i);
		}
		return iCount;
	}

	struct Figure
	{
		const char* name;
		int (*live)(CvPlayer&);   // NULL: the game keeps only a history of it
		const char* replaySet;    // the game's per-turn record of the same getter; NULL when it keeps none
		bool infoAddict;          // InfoAddict shows it for every met civilization
	};

	// The InfoAddict figures come from InfoAddictDataManager.lua; the replay sets from
	// CvPlayer::GatherPerTurnReplayStats, paired only where the recorded getter is the same.
	const Figure FIGURES[] =
	{
		{ "score", Score, NULL, true },
		{ "real_population", RealPopulation, NULL, true },
		{ "population", Population, "REPLAYDATASET_POPULATION", false },
		{ "cities", Cities, "REPLAYDATASET_CITYCOUNT", true },
		{ "land", Land, NULL, true },
		{ "land_recorded", LandRecorded, "REPLAYDATASET_TOTALLAND", false },
		{ "food_per_turn", Food, "REPLAYDATASET_FOODPERTURN", true },
		{ "production_per_turn", Production, "REPLAYDATASET_PRODUCTIONPERTURN", true },
		{ "gold", Gold, "REPLAYDATASET_TOTALGOLD", true },
		{ "gold_per_turn", GoldRate, NULL, true },
		{ "gross_gold_per_turn", GrossGold, NULL, true },
		{ "gold_yield_per_turn", GoldYield, "REPLAYDATASET_GOLDPERTURN", false },
		{ "science_per_turn", Science, NULL, true },
		{ "science_yield_per_turn", ScienceYield, "REPLAYDATASET_SCIENCEPERTURN", false },
		{ "techs", Techs, "REPLAYDATASET_TECHSKNOWN", true },
		{ "culture_per_turn", CulturePerTurn, "REPLAYDATASET_CULTUREPERTURN", true },
		{ "culture", Culture, "REPLAYDATASET_TOTALCULTURE", false },
		{ "policies", Policies, "REPLAYDATASET_NUMBEROFPOLICIES", true },
		{ "faith", Faith, NULL, true },
		{ "faith_per_turn", FaithPerTurn, NULL, true },
		{ "excess_happiness", ExcessHappiness, "REPLAYDATASET_EXCESSHAPINESS", true },
		{ "military_might", MilitaryMight, "REPLAYDATASET_MILITARYMIGHT", true },
		{ "wonders", Wonders, NULL, true },
		{ "great_works", GreatWorks, NULL, true },
		{ "tourism", Tourism, NULL, true },
		{ "civs_influenced", CivsInfluenced, NULL, true },
		{ "trade_routes_used", TradeRoutesUsed, NULL, true },
		{ "workers", NULL, "REPLAYDATASET_NUMBEROFWORKERS", false },
		{ "improved_tiles", NULL, "REPLAYDATASET_IMPROVEDTILES", false },
		{ "worked_tiles", NULL, "REPLAYDATASET_WORKEDTILES", false },
	};
	const size_t NUM_FIGURES = sizeof(FIGURES) / sizeof(FIGURES[0]);

	const Figure* FindFigure(const std::string& name)
	{
		for(size_t i = 0; i < NUM_FIGURES; ++i)
			if(name == FIGURES[i].name) return &FIGURES[i];
		return NULL;
	}

	std::string HistoryFigureNames()
	{
		std::string s;
		for(size_t i = 0; i < NUM_FIGURES; ++i)
		{
			if(!FIGURES[i].replaySet) continue;
			if(!s.empty()) s += ", ";
			s += FIGURES[i].name;
		}
		return s;
	}

	// bInfoAddictOnly: a met civilization, whose figures a player sees only through InfoAddict.
	JVal StatsJson(CvPlayer& p, bool bInfoAddictOnly)
	{
		JObj o;
		for(size_t i = 0; i < NUM_FIGURES; ++i)
		{
			const Figure& f = FIGURES[i];
			if(!f.live || (bInfoAddictOnly && !f.infoAddict)) continue;
			o[f.name] = J(f.live(p));
		}
		return JVal(o);
	}

	int ReplaySetIndex(CvPlayer& p, const char* szName)
	{
		for(unsigned int i = 0; i < p.getNumReplayDataSets(); ++i)
			if(strcmp(p.getReplayDataSetName(i), szName) == 0) return (int)i;
		return -1;
	}

	JVal HistoryJson(CvPlayer& p, const std::vector<const Figure*>& figures, int iTurns, bool bInfoAddictOnly)
	{
		int iNow = GC.getGame().getGameTurn();
		JObj o;
		for(size_t m = 0; m < figures.size(); ++m)
		{
			if(bInfoAddictOnly && !figures[m]->infoAddict) continue;
			int iSet = ReplaySetIndex(p, figures[m]->replaySet);
			if(iSet < 0) continue;
			CvPlayer::TurnData record = p.getReplayDataHistory((unsigned int)iSet);
			JObj byTurn;
			for(int t = iNow - iTurns + 1; t <= iNow; ++t)
			{
				if(t < 0) continue;
				CvPlayer::TurnData::const_iterator it = record.find((unsigned int)t);
				if(it != record.end()) byTurn[McpFormat("%d", t)] = J(it->second);
			}
			o[figures[m]->name] = JVal(byTurn);
		}
		return JVal(o);
	}

	// The game's Demographics screen (Demographics.lua), in the units it displays.
	struct Demographic
	{
		const char* name;
		double (*value)(CvPlayer&);
	};
	double DemoPopulation(CvPlayer& p) { return RealPopulation(p); }
	double DemoFood(CvPlayer& p) { return Food(p); }
	double DemoProduction(CvPlayer& p) { return Production(p); }
	double DemoGold(CvPlayer& p) { return GrossGold(p); }
	double DemoLand(CvPlayer& p) { return Land(p) * 10000.0; }
	double DemoArmy(CvPlayer& p) { return sqrt((double)MilitaryMight(p)) * 2000.0; }
	double DemoApproval(CvPlayer& p)
	{
		double v = 60 + ExcessHappiness(p) * 3;
		return v < 0 ? 0 : (v > 100 ? 100 : v);
	}
	double DemoLiteracy(CvPlayer& p)
	{
		CvTeamTechs* t = GET_TEAM(p.getTeam()).GetTeamTechs();
		TechTypes eWriting = FindTechType("TECH_WRITING");
		if(eWriting != NO_TECH && !t->HasTech(eWriting)) return 0;
		return 100.0 * t->GetNumTechsKnown() / GC.getNumTechInfos();
	}
	const Demographic DEMOGRAPHICS[] =
	{
		{ "population", DemoPopulation },
		{ "food", DemoFood },
		{ "production", DemoProduction },
		{ "gross_national_product", DemoGold },
		{ "land", DemoLand },
		{ "military_manpower", DemoArmy },
		{ "approval", DemoApproval },
		{ "literacy", DemoLiteracy },
	};

	bool Met(CvPlayer* me, PlayerTypes e)
	{
		return GET_TEAM(me->getTeam()).isHasMet(GET_PLAYER(e).getTeam());
	}

	JVal CivLabel(CvPlayer* me, PlayerTypes e)
	{
		return J(Met(me, e) ? CivName(e) : std::string("unmet"));
	}

	// Ties break as the screen's GetBest (first wins) and GetWorst (last wins) do.
	JVal DemographicsJson(CvPlayer* me)
	{
		JObj o;
		for(size_t d = 0; d < sizeof(DEMOGRAPHICS) / sizeof(DEMOGRAPHICS[0]); ++d)
		{
			double mine = DEMOGRAPHICS[d].value(*me);
			double best = 0, worst = 0, sum = 0;
			PlayerTypes eBest = NO_PLAYER, eWorst = NO_PLAYER;
			int iCivs = 0, iAbove = 0;
			for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
			{
				CvPlayer& p = GET_PLAYER((PlayerTypes)i);
				if(!p.isAlive() || p.isMinorCiv()) continue;
				double v = DEMOGRAPHICS[d].value(p);
				++iCivs;
				sum += v;
				if(v > mine) ++iAbove;
				if(eBest == NO_PLAYER || v > best) { best = v; eBest = (PlayerTypes)i; }
				if(eWorst == NO_PLAYER || v <= worst) { worst = v; eWorst = (PlayerTypes)i; }
			}
			JObj e;
			e["you"] = J(mine);
			e["rank"] = J(iAbove + 1);
			e["of"] = J(iCivs);
			e["best"] = J(best);
			e["best_civ"] = CivLabel(me, eBest);
			e["average"] = J(iCivs ? sum / iCivs : 0.0);
			e["worst"] = J(worst);
			e["worst_civ"] = CivLabel(me, eWorst);
			o[DEMOGRAPHICS[d].name] = JVal(e);
		}
		return JVal(o);
	}
}

MCP_TOOL(ToolRankings)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	int iTurns = ArgInt(args, "history_turns", 0);
	if(iTurns < 0 || iTurns > MAX_HISTORY_TURNS) { err = McpFormat("history_turns must be between 0 and %d", MAX_HISTORY_TURNS); return JVal(); }
	std::vector<const Figure*> history;
	JObj::const_iterator it = args.find("history_metrics");
	if(HasArg(args, "history_metrics"))
	{
		if(!it->second.is<JArr>()) { err = "history_metrics must be an array of names from: " + HistoryFigureNames(); return JVal(); }
		const JArr& names = it->second.get<JArr>();
		for(size_t i = 0; i < names.size(); ++i)
		{
			const Figure* f = names[i].is<std::string>() ? FindFigure(names[i].get<std::string>()) : NULL;
			if(!f || !f->replaySet) { err = "history_metrics must be from: " + HistoryFigureNames(); return JVal(); }
			history.push_back(f);
		}
	}
	if(iTurns > 0 && history.empty()) { err = "history_turns needs history_metrics, from: " + HistoryFigureNames(); return JVal(); }

	bool bInfoAddict = gDLL->IsModActivated(INFOADDICT_MOD_ID);
	JObj o;
	o["turn"] = J(GC.getGame().getGameTurn());
	o["demographics"] = DemographicsJson(me);
	o["you"] = StatsJson(*me, false);
	JObj score;
	score["cities"] = J(me->GetScoreFromCities());
	score["population"] = J(me->GetScoreFromPopulation());
	score["land"] = J(me->GetScoreFromLand());
	score["wonders"] = J(me->GetScoreFromWonders());
	score["policies"] = J(me->GetScoreFromPolicies());
	score["great_works"] = J(me->GetScoreFromGreatWorks());
	score["religion"] = J(me->GetScoreFromReligion());
	score["techs"] = J(me->GetScoreFromTechs());
	score["future_tech"] = J(me->GetScoreFromFutureTech());
	o["your_score_from"] = JVal(score);
	if(iTurns > 0) o["your_history"] = HistoryJson(*me, history, iTurns, false);

	JArr civs;
	for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		PlayerTypes e = (PlayerTypes)i;
		CvPlayer& p = GET_PLAYER(e);
		if(e == me->GetID() || !p.isAlive() || p.isMinorCiv() || !Met(me, e)) continue;
		JObj c;
		c["civ"] = J(CivName(e));
		c["score"] = J(p.GetScore());
		c["at_war_with_you"] = J(GET_TEAM(me->getTeam()).isAtWar(p.getTeam()));
		if(bInfoAddict)
		{
			c["stats"] = StatsJson(p, true);
			if(iTurns > 0) c["history"] = HistoryJson(p, history, iTurns, true);
		}
		civs.push_back(JVal(c));
	}
	o["civs_met"] = JVal(civs);
	o["detail"] = J(bInfoAddict ? "InfoAddict is loaded, so met civilizations show the figures it shows: stats and history"
	                            : "without the InfoAddict mod a player sees only scores for other civilizations, so stats and history cover only you");
	return JVal(o);
}
