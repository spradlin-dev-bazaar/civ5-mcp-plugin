#include "CvGameCoreDLLPCH.h"
#include "McpToolSupport.h"
#include "McpToolHandlers.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvCity.h"
#include "CvEspionageClasses.h"
#include "CvTechClasses.h"
#include "CvGlobals.h"

using namespace McpTool;

// Spies without the espionage screen.

namespace
{
	const char* SpyStateName(CvSpyState e)
	{
		switch(e)
		{
		case SPY_STATE_UNASSIGNED: return "unassigned";
		case SPY_STATE_TRAVELLING: return "travelling";
		case SPY_STATE_SURVEILLANCE: return "establishing surveillance";
		case SPY_STATE_GATHERING_INTEL: return "gathering intel";
		case SPY_STATE_RIG_ELECTION: return "rigging elections";
		case SPY_STATE_COUNTER_INTEL: return "counter-intelligence";
		case SPY_STATE_MAKING_INTRODUCTIONS: return "making introductions";
		case SPY_STATE_SCHMOOZE: return "schmoozing as a diplomat";
		case SPY_STATE_DEAD: return "dead";
		default: return "unknown";
		}
	}

	// A city the model's civilization has seen; one it has not is reported as absent, so the
	// lookup itself reveals nothing.
	CvCity* FindCityAnywhere(TeamTypes eTeam, const JObj& args, std::string& err)
	{
		int x = ArgInt(args, "x", -1), y = ArgInt(args, "y", -1);
		if((x >= 0) != (y >= 0)) { err = "give both x and y"; return NULL; }
		if(x >= 0 && y >= 0)
		{
			CvPlot* p = GC.getMap().plot(x, y);
			CvCity* c = p ? p->getPlotCity() : NULL;
			if(c && !c->isRevealed(eTeam, false)) c = NULL;
			if(!c) err = "no city you have seen at that tile";
			return c;
		}
		PlayerTypes e = FindPlayer(ArgStr(args, "civ"));
		if(e == NO_PLAYER) { err = "name the city with x and y, or with civ and city"; return NULL; }
		std::string name = ArgStr(args, "city");
		if(name.empty()) { err = "name the city with x and y, or with civ and city"; return NULL; }
		int iLoop = 0;
		CvPlayer& owner = GET_PLAYER(e);
		for(CvCity* c = owner.firstCity(&iLoop); c; c = owner.nextCity(&iLoop))
			if(c->isRevealed(eTeam, false) && _stricmp(c->getName().c_str(), name.c_str()) == 0) return c;
		err = "no city of " + CivName(e) + " named '" + name + "' that you have seen";
		return NULL;
	}

	bool SpyIndex(CvPlayer* me, const JObj& args, int& out, std::string& err)
	{
		if(!RequireInt(args, "spy_index", out, err)) return false;
		if(out < 0 || out >= me->GetEspionage()->GetNumSpies()) { err = "no spy with that index; see spies"; return false; }
		return true;
	}
}

MCP_TOOL(ToolSpies)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvPlayerEspionage* pE = me->GetEspionage();
	JObj o;
	JArr spies;
	for(int i = 0; i < pE->GetNumSpies(); ++i)
	{
		CvEspionageSpy& spy = pE->m_aSpyList[i];
		JObj s;
		s["spy_index"] = J(i);
		s["name"] = J(me->getCivilizationInfo().getSpyNames(spy.m_iName));
		s["rank"] = J(Localized(pE->GetSpyRankName(spy.m_eRank)));
		s["state"] = J(SpyStateName(spy.m_eSpyState));
		if(spy.m_bIsDiplomat) s["diplomat"] = J(true);
		CvCity* c = pE->GetCityWithSpy(i);
		if(c)
		{
			s["city"] = J(c->getName().c_str());
			s["city_owner"] = J(CivName(c->getOwner()));
			s["x"] = J(c->getX());
			s["y"] = J(c->getY());
			if(spy.m_eSpyState != SPY_STATE_DEAD && spy.m_eSpyState != SPY_STATE_UNASSIGNED)
			{
				s["turns_until_done"] = J(pE->GetTurnsUntilStateComplete(i));
				s["percent_done"] = J(pE->GetPercentOfStateComplete(i));
			}
			if(pE->CanStageCoup(i)) s["coup_chance_percent"] = J(pE->GetCoupChanceOfSuccess(i));
		}
		spies.push_back(JVal(s));
	}
	o["spies"] = JVal(spies);

	JArr steals;
	for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		PlayerTypes e = (PlayerTypes)i;
		if(e == me->GetID() || !GET_PLAYER(e).isAlive() || pE->GetNumTechsToSteal(e) <= 0) continue;
		JObj st;
		st["from"] = J(CivName(e));
		st["count"] = J(pE->GetNumTechsToSteal(e));
		JArr techs;
		for(int t = 0; t < GC.getNumTechInfos(); ++t)
			if(pE->IsTechStealable(e, (TechTypes)t)) techs.push_back(J(GC.getTechInfo((TechTypes)t)->GetType()));
		st["techs"] = JVal(techs);
		steals.push_back(JVal(st));
	}
	if(!steals.empty()) o["techs_to_steal"] = JVal(steals);
	return JVal(o);
}

MCP_TOOL(ToolMoveSpy)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	int iSpy = 0;
	if(!SpyIndex(me, args, iSpy, err)) return JVal();
	CvPlayerEspionage* pE = me->GetEspionage();
	JObj o;
	if(ArgBool(args, "withdraw", false))
	{
		if(!pE->IsSpyInCity(iSpy)) { err = "that spy is not in a city"; return JVal(); }
		pE->ExtractSpyFromCity(iSpy);
		o["ok"] = J(true);
		o["spy"] = J("withdrawn");
		return JVal(o);
	}
	CvCity* c = FindCityAnywhere(me->getTeam(), args, err);
	if(!c) return JVal();
	bool bDiplomat = ArgBool(args, "as_diplomat", false);
	if(!pE->CanMoveSpyTo(c, iSpy, bDiplomat)) { err = "the spy cannot go there now (unknown city, your own city as diplomat, already a spy there, or the spy is busy)"; return JVal(); }
	if(!pE->MoveSpyTo(c, iSpy, bDiplomat)) { err = "the move was refused"; return JVal(); }
	o["ok"] = J(true);
	o["travelling_to"] = J(c->getName().c_str());
	return JVal(o);
}

MCP_TOOL(ToolStageCoup)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	int iSpy = 0;
	if(!SpyIndex(me, args, iSpy, err)) return JVal();
	CvPlayerEspionage* pE = me->GetEspionage();
	if(!pE->CanStageCoup(iSpy)) { err = "that spy cannot stage a coup now (not in a city-state allied to someone else, or surveillance not established)"; return JVal(); }
	int iChance = pE->GetCoupChanceOfSuccess(iSpy);
	bool bWon = pE->AttemptCoup(iSpy);
	JObj o;
	o["ok"] = J(true);
	o["chance_was_percent"] = J(iChance);
	o["succeeded"] = J(bWon);
	return JVal(o);
}

MCP_TOOL(ToolStealTech)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	PlayerTypes e = FindPlayer(ArgStr(args, "civ"));
	if(e == NO_PLAYER) { err = "civ must name the civilization you stole from (see spies.techs_to_steal)"; return JVal(); }
	CvPlayerEspionage* pE = me->GetEspionage();
	if(pE->GetNumTechsToSteal(e) <= 0) { err = "no stolen technology is waiting from " + CivName(e); return JVal(); }
	TechTypes eTech = FindTechType(ArgStr(args, "tech"));
	if(eTech == NO_TECH || !pE->IsTechStealable(e, eTech)) { err = "that technology cannot be stolen from them; spies lists the choices"; return JVal(); }
	GET_TEAM(me->getTeam()).setHasTech(eTech, true, me->GetID(), true, true);
	pE->m_aiNumTechsToStealList[e]--;
	JObj o;
	o["ok"] = J(true);
	o["learned"] = J(TechName(eTech));
	return JVal(o);
}
