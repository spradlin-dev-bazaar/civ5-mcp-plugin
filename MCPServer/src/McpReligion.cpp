#include "CvGameCoreDLLPCH.h"
#include <algorithm>
#include "McpToolSupport.h"
#include "McpToolHandlers.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvCity.h"
#include "CvCityCitizens.h"
#include "CvUnit.h"
#include "CvReligionClasses.h"
#include "CvBeliefClasses.h"
#include "CvTraitClasses.h"
#include "CvGlobals.h"
#include "CvNotifications.h"
#include "CvPolicyClasses.h"

using namespace McpTool;

// Religion and the great-person choices the game otherwise asks for on screen.

namespace McpTool
{
	JVal BeliefJson(BeliefTypes e)
	{
		CvBeliefEntry* b = GC.GetGameBeliefs()->GetEntry(e);
		JObj o;
		o["type"] = J(b ? b->GetType() : "");
		o["name"] = J(b ? Localized(b->getShortDescription()) : std::string());
		o["effect"] = J(b ? StripMarkup(b->GetDescription()) : "");
		return JVal(o);
	}

	BeliefTypes FindBelief(const std::string& key)
	{
		if(key.empty()) return NO_BELIEF;
		for(int i = 0; i < GC.GetGameBeliefs()->GetNumBeliefs(); ++i)
		{
			CvBeliefEntry* b = GC.GetGameBeliefs()->GetEntry(i);
			if(b && MatchesKey(b->GetType(), Localized(b->getShortDescription()).c_str(), key)) return (BeliefTypes)i;
		}
		return NO_BELIEF;
	}
}

namespace
{
	JArr BeliefsJson(const std::vector<BeliefTypes>& v)
	{
		JArr a;
		for(size_t i = 0; i < v.size(); ++i) a.push_back(BeliefJson(v[i]));
		return a;
	}

	ReligionTypes FindReligion(const std::string& key)
	{
		if(key.empty()) return NO_RELIGION;
		for(int i = 0; i < GC.getNumReligionInfos(); ++i)
		{
			CvReligionEntry* r = GC.getReligionInfo((ReligionTypes)i);
			if(r && MatchesKey(r->GetType(), r->GetDescription(), key)) return (ReligionTypes)i;
		}
		return NO_RELIGION;
	}

	const char* FoundingResult(CvGameReligions::FOUNDING_RESULT r)
	{
		switch(r)
		{
		case CvGameReligions::FOUNDING_OK: return "ok";
		case CvGameReligions::FOUNDING_BELIEF_IN_USE: return "a belief is already taken by another religion";
		case CvGameReligions::FOUNDING_RELIGION_IN_USE: return "that religion was already founded";
		case CvGameReligions::FOUNDING_NOT_ENOUGH_FAITH: return "not enough faith";
		case CvGameReligions::FOUNDING_NO_RELIGIONS_AVAILABLE: return "no religions left to found";
		case CvGameReligions::FOUNDING_INVALID_PLAYER: return "you cannot do this";
		case CvGameReligions::FOUNDING_PLAYER_ALREADY_CREATED_RELIGION: return "you already founded a religion";
		case CvGameReligions::FOUNDING_PLAYER_ALREADY_CREATED_PANTHEON: return "you already have a pantheon";
		case CvGameReligions::FOUNDING_NAME_IN_USE: return "that name is taken";
		case CvGameReligions::FOUNDING_RELIGION_ENHANCED: return "already enhanced";
		default: return "not possible";
		}
	}

	JVal ReligionJson(const CvReligion* r)
	{
		JObj o;
		if(!r) return JVal(o);
		o["name"] = J(Localized(r->GetName().c_str()));
		std::string ignored;
		CvPlayer* me = Me(ignored);
		TeamTypes eTeam = me ? me->getTeam() : NO_TEAM;
		bool bMetFounder = me && r->m_eFounder != NO_PLAYER && (r->m_eFounder == me->GetID() || GET_TEAM(eTeam).isHasMet(GET_PLAYER(r->m_eFounder).getTeam()));
		o["founder"] = J(bMetFounder ? CivName(r->m_eFounder) : "a civilization you have not met");
		o["enhanced"] = J(r->m_bEnhanced);
		CvPlot* p = GC.getMap().plot(r->m_iHolyCityX, r->m_iHolyCityY);
		if(p && p->getPlotCity() && eTeam != NO_TEAM && p->getPlotCity()->isRevealed(eTeam, false)) o["holy_city"] = J(p->getPlotCity()->getName().c_str());
		JArr beliefs;
		for(int i = 0; i < r->m_Beliefs.GetNumBeliefs(); ++i) beliefs.push_back(BeliefJson(r->m_Beliefs.GetBelief(i)));
		o["beliefs"] = JVal(beliefs);
		o["followers"] = J(GC.getGame().GetGameReligions()->GetNumFollowers(r->m_eReligion));
		return JVal(o);
	}

	JVal CityReligionJson(CvCity* c)
	{
		JObj o;
		o["city"] = J(c->getName().c_str());
		CvCityReligions* r = c->GetCityReligions();
		ReligionTypes eMajority = r->GetReligiousMajority();
		if(eMajority != NO_RELIGION)
		{
			const CvReligion* pR = GC.getGame().GetGameReligions()->GetReligion(eMajority, NO_PLAYER);
			o["majority_religion"] = J(pR ? Localized(pR->GetName().c_str()) : "pantheon");
			o["followers"] = J(r->GetNumFollowers(eMajority));
		}
		else o["majority_religion"] = J("none");
		o["population"] = J(c->getPopulation());
		if(r->IsHolyCityAnyReligion()) o["holy_city"] = J(true);
		return JVal(o);
	}

	// A religious choice is open only while the game's own prompt for it is still up.
	bool HasPendingNotification(CvPlayer* me, NotificationTypes eType, int& iCityId)
	{
		CvNotifications* pN = me->GetNotifications();
		if(!pN) return false;
		for(int i = 0; i < pN->GetNumNotifications(); ++i)
			if(pN->GetNotificationType(i) == eType && !pN->IsNotificationDismissed(i)) { iCityId = pN->GetNotificationExtraGameData(i); return true; }
		return false;
	}
	bool CanFoundNow(CvPlayer* me, int& iCityId)
	{
		iCityId = -1;
		return HasPendingNotification(me, NOTIFICATION_FOUND_RELIGION, iCityId) || me->GetReligions()->IsFoundingReligion();
	}
	bool CanEnhanceNow(CvPlayer* me) { int iCity; return HasPendingNotification(me, NOTIFICATION_ENHANCE_RELIGION, iCity); }
	bool CanReformNow(CvPlayer* me)
	{
		CvGameReligions* pR = GC.getGame().GetGameReligions();
		return pR->GetReligionCreatedByPlayer(me->GetID()) != NO_RELIGION && me->GetPlayerPolicies()->HasPolicyGrantingReformationBelief() && !pR->HasAddedReformationBelief(me->GetID());
	}
	bool InList(const std::vector<BeliefTypes>& v, BeliefTypes b) { return std::find(v.begin(), v.end(), b) != v.end(); }

	bool IsGreatPerson(UnitTypes e)
	{
		CvUnitEntry* u = GC.getUnitInfo(e);
		return u && u->GetSpecialUnitType() == GC.getInfoTypeForString("SPECIALUNIT_PEOPLE");
	}

	// The same tests as the game's own choice windows: another civilization's unique great
	// person is out; a prophet before a pantheon is out too, except in the Maya long count
	// window, which does not ask.
	bool CanChooseGreatPerson(CvPlayer* me, UnitTypes e, bool bMaya)
	{
		if(!IsGreatPerson(e) || !me->canTrain(e, true, true, true, false)) return false;
		return bMaya || !GC.getUnitInfo(e)->IsFoundReligion() || me->GetReligions()->HasCreatedPantheon();
	}

	// Once every type has been taken once, the long count allows repeats.
	bool MayaRepeatBlocked(CvPlayer* me, UnitTypes e)
	{
		CvPlayerTraits* t = me->GetPlayerTraits();
		return t->GetUnitBaktun(e) > 0 && !t->IsFreeMayaGreatPersonChoice();
	}

	JArr GreatPersonChoices(CvPlayer* me, bool bMaya)
	{
		JArr list;
		for(int i = 0; i < GC.getNumUnitInfos(); ++i)
		{
			UnitTypes e = (UnitTypes)i;
			if(!CanChooseGreatPerson(me, e, bMaya)) continue;
			if(bMaya && MayaRepeatBlocked(me, e)) continue;
			CvCity* c = me->GetGreatPersonSpawnCity(e);
			if(!c) continue;
			JObj o;
			o["type"] = J(GC.getUnitInfo(e)->GetType());
			o["name"] = J(UnitTypeName(e));
			o["appears_in"] = J(c->getName().c_str());
			list.push_back(JVal(o));
		}
		return list;
	}

	bool ParseBeliefs(const JObj& args, std::vector<BeliefTypes>& out, std::string& err)
	{
		JObj::const_iterator it = args.find("beliefs");
		if(it == args.end() || !it->second.is<JArr>()) { err = "beliefs must be a list of belief names or types (religion_options lists them)"; return false; }
		const JArr& a = it->second.get<JArr>();
		for(size_t i = 0; i < a.size(); ++i)
		{
			if(!a[i].is<std::string>()) { err = "each belief must be a string"; return false; }
			BeliefTypes b = FindBelief(a[i].get<std::string>());
			if(b == NO_BELIEF) { err = "unknown belief '" + a[i].get<std::string>() + "'"; return false; }
			out.push_back(b);
		}
		return true;
	}
}

MCP_TOOL(ToolReligionOptions)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvGameReligions* pR = GC.getGame().GetGameReligions();
	JObj o;
	o["faith"] = J(me->GetFaith());
	o["faith_per_turn"] = J(me->GetTotalFaithPerTurn());

	JObj pantheon;
	pantheon["have"] = J(pR->HasCreatedPantheon(me->GetID()) || pR->HasCreatedReligion(me->GetID()));
	if(pR->HasCreatedPantheon(me->GetID())) pantheon["belief"] = BeliefJson(pR->GetBeliefInPantheon(me->GetID()));
	else if(pR->HasCreatedReligion(me->GetID())) pantheon["note"] = J("with a religion founded, a pantheon can no longer be founded");
	else
	{
		pantheon["faith_needed"] = J(pR->GetMinimumFaithNextPantheon());
		CvGameReligions::FOUNDING_RESULT r = pR->CanCreatePantheon(me->GetID(), true);
		pantheon["can_found_now"] = J(r == CvGameReligions::FOUNDING_OK);
		if(r != CvGameReligions::FOUNDING_OK) pantheon["why_not"] = J(FoundingResult(r));
		if(r == CvGameReligions::FOUNDING_OK) pantheon["beliefs_available"] = JVal(BeliefsJson(pR->GetAvailablePantheonBeliefs(me->GetID())));
	}
	o["pantheon"] = JVal(pantheon);

	ReligionTypes eMine = pR->GetReligionCreatedByPlayer(me->GetID());
	if(eMine != NO_RELIGION) o["my_religion"] = ReligionJson(pR->GetReligion(eMine, me->GetID()));
	o["religions_left_to_found"] = J(pR->GetNumReligionsStillToFound());

	int iProphetCity = -1;
	if(CanFoundNow(me, iProphetCity))
	{
		JObj f;
		JArr names;
		for(int i = 0; i < GC.getNumReligionInfos(); ++i)
		{
			CvReligionEntry* r = GC.getReligionInfo((ReligionTypes)i);
			if(!r || _stricmp(r->GetType(), "RELIGION_PANTHEON") == 0) continue;
			if(pR->GetReligion((ReligionTypes)i, NO_PLAYER) != NULL) continue;
			JObj n;
			n["type"] = J(r->GetType());
			n["name"] = J(r->GetDescription());
			names.push_back(JVal(n));
		}
		f["religions"] = JVal(names);
		f["founder_beliefs"] = JVal(BeliefsJson(pR->GetAvailableFounderBeliefs()));
		f["follower_beliefs"] = JVal(BeliefsJson(pR->GetAvailableFollowerBeliefs()));
		if(iProphetCity >= 0) f["prophet_city_id"] = J(iProphetCity);
		f["how"] = J("found_religion with religion, beliefs [one founder, one follower] and city_id for the holy city (the prophet's city by default)");
		o["found_religion"] = JVal(f);
	}
	if(CanEnhanceNow(me))
	{
		JObj e;
		e["follower_beliefs"] = JVal(BeliefsJson(pR->GetAvailableFollowerBeliefs()));
		e["enhancer_beliefs"] = JVal(BeliefsJson(pR->GetAvailableEnhancerBeliefs()));
		e["how"] = J("enhance_religion with beliefs [one follower, one enhancer]");
		o["enhance_religion"] = JVal(e);
	}
	if(CanReformNow(me))
	{
		JObj e;
		e["reformation_beliefs"] = JVal(BeliefsJson(pR->GetAvailableReformationBeliefs()));
		e["how"] = J("add_reformation_belief with belief");
		o["reformation"] = JVal(e);
	}
	if(me->GetNumFaithGreatPeople() > 0)
	{
		JObj g; g["count"] = J(me->GetNumFaithGreatPeople()); g["choices"] = JVal(GreatPersonChoices(me, false)); g["how"] = J("choose_great_person with unit and source=faith");
		o["faith_great_people_to_claim"] = JVal(g);
	}
	if(me->GetNumFreeGreatPeople() > 0)
	{
		JObj g; g["count"] = J(me->GetNumFreeGreatPeople()); g["choices"] = JVal(GreatPersonChoices(me, false)); g["how"] = J("choose_great_person with unit and source=free");
		o["free_great_people_to_claim"] = JVal(g);
	}
	if(me->GetNumMayaBoosts() > 0)
	{
		JObj g; g["count"] = J(me->GetNumMayaBoosts()); g["choices"] = JVal(GreatPersonChoices(me, true)); g["how"] = J("choose_great_person with unit and source=maya");
		o["maya_great_people_to_claim"] = JVal(g);
	}
	return JVal(o);
}

MCP_TOOL(ToolFoundPantheon)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvGameReligions* pR = GC.getGame().GetGameReligions();
	CvGameReligions::FOUNDING_RESULT r = pR->CanCreatePantheon(me->GetID(), true);
	if(r != CvGameReligions::FOUNDING_OK) { err = std::string("cannot found a pantheon: ") + FoundingResult(r); return JVal(); }
	BeliefTypes b = FindBelief(ArgStr(args, "belief"));
	if(b == NO_BELIEF) { err = "unknown belief; religion_options lists the available ones"; return JVal(); }
	if(!pR->IsPantheonBeliefAvailable(b, me->GetID())) { err = "that belief is not available as a pantheon belief"; return JVal(); }
	pR->FoundPantheon(me->GetID(), b);
	JObj o;
	o["ok"] = J(true);
	o["pantheon"] = BeliefJson(b);
	return JVal(o);
}

MCP_TOOL(ToolFoundReligion)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	int iProphetCity = -1;
	if(!CanFoundNow(me, iProphetCity)) { err = "no religion founding is waiting: a Great Prophet must first be used to found one"; return JVal(); }
	ReligionTypes eRel = FindReligion(ArgStr(args, "religion"));
	if(eRel == NO_RELIGION) { err = "unknown religion; religion_options lists the ones left"; return JVal(); }
	std::vector<BeliefTypes> beliefs;
	if(!ParseBeliefs(args, beliefs, err)) return JVal();
	if(beliefs.size() < 2 || beliefs.size() > 4) { err = "a new religion takes two beliefs (one founder, one follower), up to four with bonus beliefs"; return JVal(); }
	CvGameReligions* pR = GC.getGame().GetGameReligions();
	if(!InList(pR->GetAvailableFounderBeliefs(), beliefs[0])) { err = "the first belief must be an available founder belief"; return JVal(); }
	if(!InList(pR->GetAvailableFollowerBeliefs(), beliefs[1])) { err = "the second belief must be an available follower belief"; return JVal(); }
	int iCity = ArgInt(args, "city_id", iProphetCity);
	CvCity* pHoly = (iCity >= 0) ? me->getCity(iCity) : me->getCapitalCity();
	if(!pHoly) { err = "no city for the holy city"; return JVal(); }
	std::string name = ArgStr(args, "name");
	if(name.size() >= 127) { err = "name must be shorter than 127 characters"; return JVal(); }
	BeliefTypes b3 = beliefs.size() > 2 ? beliefs[2] : NO_BELIEF;
	BeliefTypes b4 = beliefs.size() > 3 ? beliefs[3] : NO_BELIEF;
	CvGameReligions::FOUNDING_RESULT r = pR->CanFoundReligion(me->GetID(), eRel, name.c_str(), beliefs[0], beliefs[1], b3, b4, pHoly);
	if(r != CvGameReligions::FOUNDING_OK) { err = std::string("cannot found this religion: ") + FoundingResult(r); return JVal(); }
	pR->FoundReligion(me->GetID(), eRel, name.c_str(), beliefs[0], beliefs[1], b3, b4, pHoly);
	JObj o;
	o["ok"] = J(true);
	o["religion"] = ReligionJson(pR->GetReligion(eRel, me->GetID()));
	return JVal(o);
}

MCP_TOOL(ToolEnhanceReligion)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvGameReligions* pR = GC.getGame().GetGameReligions();
	ReligionTypes eRel = pR->GetReligionCreatedByPlayer(me->GetID());
	if(eRel == NO_RELIGION) { err = "you have not founded a religion"; return JVal(); }
	if(!CanEnhanceNow(me)) { err = "no enhancement is waiting: a Great Prophet must first be used to enhance your religion"; return JVal(); }
	std::vector<BeliefTypes> beliefs;
	if(!ParseBeliefs(args, beliefs, err)) return JVal();
	if(beliefs.size() != 2) { err = "enhancing takes exactly two beliefs (one follower, one enhancer)"; return JVal(); }
	if(!InList(pR->GetAvailableFollowerBeliefs(), beliefs[0])) { err = "the first belief must be an available follower belief"; return JVal(); }
	if(!InList(pR->GetAvailableEnhancerBeliefs(), beliefs[1])) { err = "the second belief must be an available enhancer belief"; return JVal(); }
	CvGameReligions::FOUNDING_RESULT r = pR->CanEnhanceReligion(me->GetID(), eRel, beliefs[0], beliefs[1]);
	if(r != CvGameReligions::FOUNDING_OK) { err = std::string("cannot enhance: ") + FoundingResult(r); return JVal(); }
	pR->EnhanceReligion(me->GetID(), eRel, beliefs[0], beliefs[1]);
	JObj o;
	o["ok"] = J(true);
	o["religion"] = ReligionJson(pR->GetReligion(eRel, me->GetID()));
	return JVal(o);
}

MCP_TOOL(ToolAddReformationBelief)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvGameReligions* pR = GC.getGame().GetGameReligions();
	ReligionTypes eRel = pR->GetReligionCreatedByPlayer(me->GetID());
	if(eRel == NO_RELIGION) { err = "you have not founded a religion"; return JVal(); }
	if(pR->HasAddedReformationBelief(me->GetID())) { err = "you already added a reformation belief"; return JVal(); }
	if(!CanReformNow(me)) { err = "no policy of yours grants a reformation belief"; return JVal(); }
	BeliefTypes b = FindBelief(ArgStr(args, "belief"));
	if(b == NO_BELIEF) { err = "unknown belief"; return JVal(); }
	std::vector<BeliefTypes> avail = pR->GetAvailableReformationBeliefs();
	if(std::find(avail.begin(), avail.end(), b) == avail.end()) { err = "that belief is not an available reformation belief"; return JVal(); }
	pR->AddReformationBelief(me->GetID(), eRel, b);
	JObj o;
	o["ok"] = J(true);
	o["religion"] = ReligionJson(pR->GetReligion(eRel, me->GetID()));
	return JVal(o);
}

MCP_TOOL(ToolChooseGreatPerson)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	std::string source = ArgStr(args, "source");
	UnitTypes e = FindUnitType(ArgStr(args, "unit"));
	if(e == NO_UNIT || !IsGreatPerson(e)) { err = "unit must be a great person type from religion_options"; return JVal(); }
	bool bFaith = (source == "faith"), bFree = (source == "free"), bMaya = (source == "maya");
	if(!bFaith && !bFree && !bMaya) { err = "source must be faith, free or maya"; return JVal(); }
	if(!CanChooseGreatPerson(me, e, bMaya)) { err = "you cannot take that great person (for example another civilization's unique great person, or a Great Prophet before you have a pantheon); religion_options lists the ones you can"; return JVal(); }
	if(bFaith && me->GetNumFaithGreatPeople() <= 0) { err = "no faith great person to claim"; return JVal(); }
	if(bFree && me->GetNumFreeGreatPeople() <= 0) { err = "no free great person to claim"; return JVal(); }
	if(bMaya && me->GetNumMayaBoosts() <= 0) { err = "no Maya long count bonus to claim"; return JVal(); }
	if(bMaya && MayaRepeatBlocked(me, e)) { err = "you already took that great person in an earlier long count; each type can be taken once until you have taken them all"; return JVal(); }
	CvCity* c = me->GetGreatPersonSpawnCity(e);
	if(!c) { err = "that great person cannot appear for you right now"; return JVal(); }
	c->GetCityCitizens()->DoSpawnGreatPerson(e, true, bFaith, !bFaith && MOD_GLOBAL_TRULY_FREE_GP);
	if(bFaith) me->ChangeNumFaithGreatPeople(-1);
	else if(bFree) me->ChangeNumFreeGreatPeople(-1);
	else { me->ChangeNumMayaBoosts(-1); me->GetPlayerTraits()->SetUnitBaktun(e); }
	JObj o;
	o["ok"] = J(true);
	o["appeared_in"] = J(c->getName().c_str());
	o["unit"] = J(UnitTypeName(e));
	return JVal(o);
}

MCP_TOOL(ToolReligionOverview)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvGameReligions* pR = GC.getGame().GetGameReligions();
	JObj o;
	o["faith"] = J(me->GetFaith());
	o["faith_per_turn"] = J(me->GetTotalFaithPerTurn());
	o["next_great_prophet_costs"] = J(me->GetReligions()->GetCostNextProphet(true, true, MOD_GLOBAL_TRULY_FREE_GP));
	BeliefTypes ePantheon = pR->GetBeliefInPantheon(me->GetID());
	if(ePantheon != NO_BELIEF) o["pantheon"] = BeliefJson(ePantheon);
	ReligionTypes eMine = pR->GetReligionCreatedByPlayer(me->GetID());
	if(eMine != NO_RELIGION) o["your_religion"] = ReligionJson(pR->GetReligion(eMine, me->GetID()));
	JArr founded;
	for(int i = 0; i < GC.getNumReligionInfos(); ++i)
	{
		const CvReligion* r = pR->GetReligion((ReligionTypes)i, NO_PLAYER);
		if(!r || r->m_bPantheon || (ReligionTypes)i == eMine) continue;
		JVal v = ReligionJson(r);
		v.get<JObj>()["cities_following"] = J(pR->GetNumCitiesFollowing((ReligionTypes)i));
		founded.push_back(v);
	}
	o["other_religions_founded"] = JVal(founded);
	o["religions_left_to_found"] = J(GC.getGame().GetGameReligions()->GetNumReligionsStillToFound());
	JArr cities;
	int iLoop = 0;
	for(CvCity* c = me->firstCity(&iLoop); c; c = me->nextCity(&iLoop)) cities.push_back(CityReligionJson(c));
	o["your_cities"] = JVal(cities);
	return JVal(o);
}
