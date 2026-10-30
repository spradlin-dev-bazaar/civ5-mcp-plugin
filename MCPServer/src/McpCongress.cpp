#include "CvGameCoreDLLPCH.h"
#include "McpToolSupport.h"
#include "McpToolHandlers.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvVotingClasses.h"
#include "CvGlobals.h"

using namespace McpTool;

// The World Congress without its screen.

namespace
{
	CvLeague* League(std::string& err)
	{
		CvGameLeagues* pLeagues = GC.getGame().GetGameLeagues();
		if(!pLeagues || pLeagues->GetNumActiveLeagues() == 0) { err = "the World Congress has not been founded yet"; return NULL; }
		return pLeagues->GetActiveLeague();
	}

	const char* DecisionName(ResolutionDecisionTypes e)
	{
		switch(e)
		{
		case RESOLUTION_DECISION_NONE: return "none";
		case RESOLUTION_DECISION_YES_OR_NO: return "yes_or_no";
		case RESOLUTION_DECISION_ANY_MEMBER: return "a member civilization";
		case RESOLUTION_DECISION_CITY: return "a city";
		case RESOLUTION_DECISION_ANY_LUXURY_RESOURCE: return "a luxury resource";
		case RESOLUTION_DECISION_REPEAL: return "repeal";
		case RESOLUTION_DECISION_MAJOR_CIV_MEMBER: return "a major civilization";
		case RESOLUTION_DECISION_OTHER_MAJOR_CIV_MEMBER: return "another major civilization";
		case RESOLUTION_DECISION_RELIGION: return "a religion";
		case RESOLUTION_DECISION_IDEOLOGY: return "an ideology";
		default: return "other";
		}
	}

	std::string ChoiceText(CvLeague* pLeague, ResolutionDecisionTypes eDecision, int iChoice)
	{
		return StripMarkup(pLeague->GetTextForChoice(eDecision, iChoice).c_str());
	}

	JArr ChoicesJson(CvLeague* pLeague, ResolutionDecisionTypes eDecision, PlayerTypes eDecider)
	{
		JArr list;
		std::vector<int> choices = pLeague->GetChoicesForDecision(eDecision, eDecider);
		for(size_t i = 0; i < choices.size(); ++i)
		{
			JObj c;
			c["choice"] = J(choices[i]);
			c["text"] = J(ChoiceText(pLeague, eDecision, choices[i]));
			list.push_back(JVal(c));
		}
		return list;
	}

	JVal ProposalJson(CvLeague* pLeague, CvProposal& p, CvVoterDecision* v, bool bRepeal, PlayerTypes me)
	{
		JObj o;
		o["id"] = J(p.GetID());
		o["name"] = J(StripMarkup(pLeague->GetResolutionName(p.GetType(), p.GetID(), p.GetProposerDecision() ? p.GetProposerDecision()->GetDecision() : LeagueHelpers::CHOICE_NONE, true).c_str()));
		o["proposed_by"] = J(CivName(p.GetProposalPlayer()));
		o["repeal"] = J(bRepeal);
		CvResolutionEntry* info = GC.getResolutionInfo(p.GetType());
		if(info) o["effect"] = J(Localized(info->GetHelp()));
		if(v)
		{
			o["votes_cast"] = J(v->GetVotesCast());
			o["my_votes_cast"] = J(v->GetVotesCastByPlayer(me));
			o["vote_choices"] = JVal(ChoicesJson(pLeague, v->GetType(), me));
		}
		return JVal(o);
	}

	bool ResolveChoice(CvLeague* pLeague, ResolutionDecisionTypes eDecision, PlayerTypes me, const JObj& args, int& out, std::string& err)
	{
		std::vector<int> choices = pLeague->GetChoicesForDecision(eDecision, me);
		JObj::const_iterator it = args.find("choice");
		if(!HasArg(args, "choice"))
		{
			if(eDecision == RESOLUTION_DECISION_NONE) { out = LeagueHelpers::CHOICE_NONE; return true; }
			err = std::string("this needs a choice: ") + DecisionName(eDecision);
			return false;
		}
		int n = 0;
		if(IsInt(it->second, n))
		{
			if(std::find(choices.begin(), choices.end(), n) == choices.end()) { err = "that choice is not available here"; return false; }
			out = n;
			return true;
		}
		if(it->second.is<std::string>())
		{
			std::string key = it->second.get<std::string>();
			for(size_t i = 0; i < choices.size(); ++i)
				if(_stricmp(ChoiceText(pLeague, eDecision, choices[i]).c_str(), key.c_str()) == 0) { out = choices[i]; return true; }
			err = "no choice named '" + key + "'";
			return false;
		}
		err = "choice must be a number or the choice text";
		return false;
	}

	std::string CannotVote(CvLeague* pLeague, PlayerTypes e)
	{
		if(!pLeague->IsMember(e)) return "you cannot vote: you are not a member of the World Congress";
		if(!pLeague->IsInSession())
		{
			int iTurns = pLeague->GetTurnsUntilSession();
			return McpFormat("you cannot vote now: the World Congress votes when it meets, in %d turn%s", iTurns, iTurns == 1 ? "" : "s");
		}
		return "you cannot vote now: you have used all your votes this session";
	}
}

MCP_TOOL(ToolCongress)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvLeague* pLeague = League(err);
	if(!pLeague) return JVal();
	PlayerTypes e = me->GetID();
	JObj o;
	o["name"] = J(pLeague->GetName().toUTF8());
	o["host"] = J(CivName(pLeague->GetHostMember()));
	o["members"] = J(pLeague->GetNumMembers());
	o["in_session"] = J(pLeague->IsInSession());
	o["turns_until_session"] = J(pLeague->GetTurnsUntilSession());
	o["member"] = J(pLeague->IsMember(e));
	o["my_votes_remaining"] = J(pLeague->GetRemainingVotesForMember(e));
	// As the game's own World Congress screen counts them. The core votes leave out the host's and other extra delegates;
	// in session, the starting votes would follow alliances made since the votes were dealt.
	o["my_votes_total"] = J(pLeague->IsInSession() ? pLeague->GetRemainingVotesForMember(e) + pLeague->GetSpentVotesForMember(e)
	                                                  : pLeague->CalculateStartingVotesForMember(e));
	o["my_proposals_remaining"] = J(pLeague->GetRemainingProposalsForMember(e));

	JArr active;
	ActiveResolutionList act = pLeague->GetActiveResolutions();
	for(ActiveResolutionList::iterator it = act.begin(); it != act.end(); ++it)
	{
		JObj a;
		a["id"] = J(it->GetID());
		// No prefix: the game names an active resolution "REPEAL: ..." while a repeal of it is proposed.
		a["name"] = J(StripMarkup(pLeague->GetResolutionName(it->GetType(), it->GetID(), it->GetProposerDecision() ? it->GetProposerDecision()->GetDecision() : LeagueHelpers::CHOICE_NONE, false).c_str()));
		a["enacted_turn"] = J(it->GetTurnEnacted());
		if(pLeague->CanPropose(e)) a["can_propose_repeal"] = J(pLeague->CanProposeRepeal(it->GetID(), e));
		active.push_back(JVal(a));
	}
	o["active_resolutions"] = JVal(active);

	JArr pending;
	EnactProposalList enact = pLeague->GetEnactProposals();
	for(EnactProposalList::iterator it = enact.begin(); it != enact.end(); ++it) pending.push_back(ProposalJson(pLeague, *it, it->GetVoterDecision(), false, e));
	RepealProposalList repeal = pLeague->GetRepealProposals();
	for(RepealProposalList::iterator it = repeal.begin(); it != repeal.end(); ++it) pending.push_back(ProposalJson(pLeague, *it, it->GetRepealDecision(), true, e));
	o["proposals_awaiting_votes"] = JVal(pending);

	if(pLeague->CanPropose(e))
	{
		JArr can;
		std::vector<ResolutionTypes> inactive = pLeague->GetInactiveResolutions();
		for(size_t i = 0; i < inactive.size(); ++i)
		{
			if(!pLeague->CanProposeEnactAnyChoice(inactive[i], e)) continue;
			CvResolutionEntry* info = GC.getResolutionInfo(inactive[i]);
			if(!info) continue;
			JObj r;
			r["type"] = J(info->GetType());
			r["name"] = J(info->GetDescription());
			r["effect"] = J(Localized(info->GetHelp()));
			r["decision"] = J(DecisionName(info->GetProposerDecision()));
			if(info->GetProposerDecision() != RESOLUTION_DECISION_NONE) r["choices"] = JVal(ChoicesJson(pLeague, info->GetProposerDecision(), e));
			can.push_back(JVal(r));
		}
		o["can_propose"] = JVal(can);
	}
	return JVal(o);
}

MCP_TOOL(ToolProposeResolution)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvLeague* pLeague = League(err);
	if(!pLeague) return JVal();
	PlayerTypes e = me->GetID();
	if(!pLeague->CanPropose(e)) { err = "you cannot propose now (not your turn to propose, or no proposals left)"; return JVal(); }
	JObj o;
	int iRepeal = ArgInt(args, "repeal_id", -1);
	if(iRepeal >= 0)
	{
		if(!pLeague->CanProposeRepeal(iRepeal, e)) { err = "that resolution cannot be repealed by you now"; return JVal(); }
		pLeague->DoProposeRepeal(iRepeal, e);
		o["ok"] = J(true);
		o["proposed_repeal_of"] = J(iRepeal);
		return JVal(o);
	}
	std::string key = ArgStr(args, "resolution");
	ResolutionTypes eRes = NO_RESOLUTION;
	for(int i = 0; i < GC.getNumResolutionInfos(); ++i)
	{
		CvResolutionEntry* info = GC.getResolutionInfo((ResolutionTypes)i);
		if(info && MatchesKey(info->GetType(), info->GetDescription(), key)) { eRes = (ResolutionTypes)i; break; }
	}
	if(eRes == NO_RESOLUTION) { err = "unknown resolution '" + key + "'; congress lists what you can propose"; return JVal(); }
	CvResolutionEntry* info = GC.getResolutionInfo(eRes);
	int iChoice = LeagueHelpers::CHOICE_NONE;
	if(!ResolveChoice(pLeague, info->GetProposerDecision(), e, args, iChoice, err)) return JVal();
	CvString why;
	if(!pLeague->CanProposeEnact(eRes, e, iChoice, &why)) { err = "cannot propose that: " + std::string(Localized(why.c_str())); return JVal(); }
	pLeague->DoProposeEnact(eRes, e, iChoice);
	o["ok"] = J(true);
	o["proposed"] = J(info->GetDescription());
	return JVal(o);
}

MCP_TOOL(ToolVoteResolution)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvLeague* pLeague = League(err);
	if(!pLeague) return JVal();
	PlayerTypes e = me->GetID();
	if(!pLeague->CanVote(e)) { err = CannotVote(pLeague, e); return JVal(); }
	int iId = 0;
	if(!RequireInt(args, "resolution_id", iId, err)) return JVal();
	int iVotes = ArgInt(args, "votes", pLeague->GetRemainingVotesForMember(e));
	if(iVotes <= 0 || iVotes > pLeague->GetRemainingVotesForMember(e)) { err = McpFormat("votes must be between 1 and %d", pLeague->GetRemainingVotesForMember(e)); return JVal(); }
	CvEnactProposal* pEnact = pLeague->GetEnactProposal(iId);
	CvRepealProposal* pRepeal = pEnact ? NULL : pLeague->GetRepealProposal(iId);
	if(!pEnact && !pRepeal) { err = "no proposal with that id is awaiting votes"; return JVal(); }
	CvVoterDecision* v = pEnact ? pEnact->GetVoterDecision() : pRepeal->GetRepealDecision();
	int iChoice = LeagueHelpers::CHOICE_NONE;
	if(!ResolveChoice(pLeague, v->GetType(), e, args, iChoice, err)) return JVal();
	if(pEnact) pLeague->DoVoteEnact(iId, e, iVotes, iChoice);
	else pLeague->DoVoteRepeal(iId, e, iVotes, iChoice);
	JObj o;
	o["ok"] = J(true);
	o["votes_cast"] = J(iVotes);
	o["votes_remaining"] = J(pLeague->GetRemainingVotesForMember(e));
	return JVal(o);
}

MCP_TOOL(ToolAbstain)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvLeague* pLeague = League(err);
	if(!pLeague) return JVal();
	PlayerTypes e = me->GetID();
	if(!pLeague->CanVote(e)) { err = CannotVote(pLeague, e); return JVal(); }
	int iVotes = ArgInt(args, "votes", pLeague->GetRemainingVotesForMember(e));
	if(iVotes <= 0 || iVotes > pLeague->GetRemainingVotesForMember(e)) { err = "votes out of range"; return JVal(); }
	pLeague->DoVoteAbstain(e, iVotes);
	JObj o;
	o["ok"] = J(true);
	o["votes_remaining"] = J(pLeague->GetRemainingVotesForMember(e));
	return JVal(o);
}
