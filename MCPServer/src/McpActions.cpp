#include "CvGameCoreDLLPCH.h"
#include "McpToolSupport.h"
#include "McpToolHandlers.h"
#include "McpBridge.h"
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
#include "CvUnitClasses.h"
#include "CvBuildingClasses.h"
#include "CvPromotionClasses.h"
#include "CvPolicyClasses.h"
#include "CvTreasury.h"
#include "CvInfos.h"

using namespace McpTool;

// ------------------------------------------------------------------ policies

MCP_TOOL(ToolPolicyOptions)
{
	CvPlayer* me = Me(err);
	if(!me) return JVal();
	CvPlayerPolicies* pPolicies = me->GetPlayerPolicies();
	JObj o;
	o["culture"] = J(me->getJONSCulture());
	o["next_policy_cost"] = J(me->getNextPolicyCost());
	o["free_policies"] = J(me->GetNumFreePolicies());
	o["policies_owned"] = J(pPolicies->GetNumPoliciesOwned(MOD_BUGFIX_DUMMY_POLICIES));
	o["can_adopt_now"] = J(PolicyChoiceAvailable(me));

	JArr branches;
	for(int i = 0; i < GC.getNumPolicyBranchInfos(); ++i)
	{
		PolicyBranchTypes e = (PolicyBranchTypes)i;
		CvPolicyBranchEntry* info = GC.getPolicyBranchInfo(e);
		if(!info) continue;
		JObj b;
		b["type"] = J(info->GetType());
		b["name"] = J(info->GetDescription());
		b["unlocked"] = J(pPolicies->IsPolicyBranchUnlocked(e));
		if(!pPolicies->IsPolicyBranchUnlocked(e)) b["can_unlock_now"] = J(pPolicies->CanUnlockPolicyBranch(e));
		if(pPolicies->IsPolicyBranchBlocked(e)) b["blocked_by_another_branch"] = J(true);
		if(pPolicies->IsPolicyBranchFinished(e)) b["finished"] = J(true);
		if(info->GetEraPrereq() != NO_ERA) b["requires_era"] = J(EraName((EraTypes)info->GetEraPrereq()));
		if(!pPolicies->IsPolicyBranchUnlocked(e) && !pPolicies->CanUnlockPolicyBranch(e))
		{
			branches.push_back(JVal(b));
			continue;
		}
		b["help"] = J(Localized(info->GetHelp()));

		JArr policies;
		for(int k = 0; k < GC.getNumPolicyInfos(); ++k)
		{
			PolicyTypes ep = (PolicyTypes)k;
			CvPolicyEntry* pinfo = GC.getPolicyInfo(ep);
			if(!pinfo || pinfo->GetPolicyBranchType() != (int)e) continue;
			JObj p;
			p["type"] = J(pinfo->GetType());
			p["name"] = J(pinfo->GetDescription());
			p["owned"] = J(pPolicies->HasPolicy(ep));
			if(!pPolicies->HasPolicy(ep)) p["can_adopt_now"] = J(me->canAdoptPolicy(ep));
			p["help"] = J(Localized(pinfo->GetHelp()));
			policies.push_back(JVal(p));
		}
		b["policies"] = JVal(policies);
		branches.push_back(JVal(b));
	}
	o["branches"] = JVal(branches);
	return JVal(o);
}

MCP_TOOL(ToolAdoptPolicy)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	std::string key = ArgStr(args, "policy");
	CvPlayerPolicies* pPolicies = me->GetPlayerPolicies();
	JObj o;

	PolicyBranchTypes eBranch = FindPolicyBranchType(key);
	if(eBranch != NO_POLICY_BRANCH_TYPE)
	{
		if(pPolicies->IsPolicyBranchBlocked(eBranch) && pPolicies->IsPolicyBranchUnlocked(eBranch))
		{
			if(!ArgBool(args, "accept_anarchy", false))
			{
				err = "switching to that branch blocks its rival branch and causes a turn of anarchy; pass accept_anarchy=true to do it";
				return JVal();
			}
			pPolicies->DoSwitchToPolicyBranch(eBranch);
			o["ok"] = J(true);
			o["result"] = J("switched to that branch; anarchy follows for a turn");
			return JVal(o);
		}
		if(pPolicies->IsPolicyBranchUnlocked(eBranch)) { err = "that branch is already open; adopt one of its policies instead"; return JVal(); }
		if(!pPolicies->CanUnlockPolicyBranch(eBranch)) { err = "cannot open that branch now (era requirement, blocked by another branch, or not enough culture)"; return JVal(); }
		pPolicies->DoUnlockPolicyBranch(eBranch);
		o["ok"] = J(true);
		o["result"] = J("branch opened");
	}
	else
	{
		PolicyTypes ePolicy = FindPolicyType(key);
		if(ePolicy == NO_POLICY) { err = "unknown policy '" + key + "'; use a POLICY_ or POLICY_BRANCH_ type from policy_options"; return JVal(); }
		if(pPolicies->HasPolicy(ePolicy)) { err = "you already have that policy"; return JVal(); }
		if(!me->canAdoptPolicy(ePolicy)) { err = "cannot adopt that policy now (branch not open, prerequisite policy missing, or not enough culture)"; return JVal(); }
		me->doAdoptPolicy(ePolicy);
		o["ok"] = J(true);
		o["result"] = J("policy adopted");
	}
	o["culture"] = J(me->getJONSCulture());
	o["next_policy_cost"] = J(me->getNextPolicyCost());
	return JVal(o);
}

// ------------------------------------------------------------------ units

MCP_TOOL(ToolPromoteUnit)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvUnit* u = RequireUnit(me, args, err);
	if(!u) return JVal();
	std::string key = ArgStr(args, "promotion");
	PromotionTypes e = FindPromotionType(key);
	if(e == NO_PROMOTION) { err = "unknown promotion '" + key + "'; see unit_options.promotions_available"; return JVal(); }
	if(!u->canPromote(e, -1)) { err = "that unit cannot take that promotion now"; return JVal(); }
	u->doCommand(COMMAND_PROMOTION, e, -1);
	JObj o;
	o["ok"] = J(true);
	o["unit"] = UnitJson(u);
	return JVal(o);
}

MCP_TOOL(ToolUpgradeUnit)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvUnit* u = RequireUnit(me, args, err);
	if(!u) return JVal();
	std::string why = UpgradeRefusal(u);
	if(!why.empty()) { err = "that unit cannot upgrade now: " + why; return JVal(); }
	int iGoldBefore = me->GetTreasury()->GetGold();
	CvUnit* pNew = u->DoUpgrade();
	if(!pNew) { err = "the game refused the upgrade"; return JVal(); }
	JObj o;
	o["ok"] = J(true);
	o["gold_spent"] = J(iGoldBefore - me->GetTreasury()->GetGold());
	o["unit"] = UnitJson(pNew);
	return JVal(o);
}

// ------------------------------------------------------------------ combat

namespace
{
	// The game's own preview numbers: strengths on the player-facing scale, and
	// damage from the same calls CvUnitCombat makes.
	JVal CombatEstimate(CvUnit* u, CvPlot* p, const AttackTarget& t)
	{
		JObj est;
		if(t.unit && !t.unit->IsCanDefend())
		{
			est["mode"] = J(t.ranged ? "ranged" : "melee");
			est["target"] = J(UnitTypeName(t.unit->getUnitType()));
			est["outcome"] = J("the civilian is captured");
			return JVal(est);
		}
		int att = 0, def = 0, dmgToTarget = 0, dmgToMe = 0;
		if(t.ranged)
		{
			att = u->GetMaxRangedCombatStrength(t.unit, t.city, true, true);
			if(!t.unit) def = t.city->getStrengthValue();
			else if(t.unit->isRanged()) def = t.unit->GetMaxRangedCombatStrength(u, NULL, false, false);
			else def = t.unit->GetMaxDefenseStrength(p, u, true);
			dmgToTarget = u->GetRangeCombatDamage(t.unit, t.city, false);
		}
		else
		{
			att = u->GetMaxAttackStrength(u->plot(), p, t.unit);
			def = t.unit ? t.unit->GetMaxDefenseStrength(p, u) : t.city->getStrengthValue();
			dmgToTarget = u->getCombatDamage(att, def, u->getDamage(), false, false, t.unit == NULL);
			dmgToMe = t.unit ? t.unit->getCombatDamage(def, att, t.unit->getDamage(), false, false, false)
			                 : u->getCombatDamage(def, att, t.city->getDamage(), false, true, false);
		}
		est["mode"] = J(t.ranged ? "ranged" : "melee");
		est["your_strength"] = J(att / 100.0);
		est["their_strength"] = J(def / 100.0);
		est["target"] = J(t.unit ? UnitTypeName(t.unit->getUnitType()) : t.city->getName().c_str());
		est["target_hp"] = J(t.unit ? t.unit->GetCurrHitPoints() : t.city->GetMaxHitPoints() - t.city->getDamage());
		est["expected_damage_to_target"] = J(dmgToTarget);
		if(u->getDomainType() == DOMAIN_AIR) est["damage_to_you"] = J("not estimated: interceptors and anti-air can still hit the aircraft");
		else est["expected_damage_to_you"] = J(dmgToMe);
		return JVal(est);
	}
}

MCP_TOOL(ToolAttack)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	int x = 0, y = 0;
	CvUnit* u = RequireUnit(me, args, err);
	if(!u || !RequireInt(args, "x", x, err) || !RequireInt(args, "y", y, err)) return JVal();
	int iUnit = u->GetID();
	bool bPreview = ArgBool(args, "preview_only", false);
	CvMap& map = GC.getMap();
	if(x < 0 || y < 0 || x >= map.getGridWidth() || y >= map.getGridHeight()) { err = "target is off the map"; return JVal(); }
	CvPlot* p = map.plot(x, y);

	AttackTarget target;
	if(!ResolveAttackTarget(me, u, p, target, err)) return JVal();

	JObj o;
	o["estimate"] = CombatEstimate(u, p, target);
	if(bPreview)
	{
		o["previewed_only"] = J(true);
		if(!u->canMove()) o["note"] = J("this unit has no moves left this turn, so it can attack next turn");
		return JVal(o);
	}

	// The game queues an attack from a unit with no moves and never runs it.
	if(!RequireMoves(u, err)) return JVal();
	// A unit still animating its last order (a set-up, a move) queues the attack behind it, and
	// the result would be read before the attack ran.
	if(u->IsBusy()) { err = "the unit is still finishing its last order; try again in a moment"; McpBridge::RetryLater(); return JVal(); }
	PlayerTypes eDefOwner = target.unit ? target.unit->getOwner() : NO_PLAYER;
	int iDefId = target.unit ? target.unit->GetID() : -1;
	int hpBefore = u->GetCurrHitPoints();
	int movesBefore = u->movesLeft();
	int targetHpBefore = target.unit ? target.unit->GetCurrHitPoints() : target.city->GetMaxHitPoints() - target.city->getDamage();
	{
		NetMessageScope scope;
		// Aircraft strike through a move onto the target; the engine rejects them in the range-attack mission.
		if(target.ranged && u->getDomainType() != DOMAIN_AIR) u->PushMission(CvTypes::getMISSION_RANGE_ATTACK(), x, y, 0, false, true);
		else u->PushMission(CvTypes::getMISSION_MOVE_TO(), x, y, 0, false, true);
	}

	int targetHpAfter = -1;
	CvUnit* uAfter = me->getUnit(iUnit);
	if(uAfter && !uAfter->isDelayedDeath())
	{
		o["your_hp"] = J(uAfter->GetCurrHitPoints());
		o["damage_taken"] = J(hpBefore - uAfter->GetCurrHitPoints());
		o["moves_left"] = J(Moves(uAfter->movesLeft()));
		o["now_at"] = J(McpFormat("(%d,%d)", uAfter->getX(), uAfter->getY()));
	}
	else
	{
		o["your_unit_destroyed"] = J(true);
	}

	if(target.unit)
	{
		CvUnit* dAfter = GET_PLAYER(eDefOwner).getUnit(iDefId);
		if(dAfter && !dAfter->isDelayedDeath()) { targetHpAfter = dAfter->GetCurrHitPoints(); o["target_hp"] = J(targetHpAfter); }
		else o["target_destroyed"] = J(true);
	}
	else
	{
		CvCity* cityAfter = p->getPlotCity();
		if(cityAfter && cityAfter->getOwner() == me->GetID())
		{
			o["city_captured"] = J(true);
			if(CaptureUndecided(cityAfter)) o["note"] = J("decide its fate with city_task: " + CaptureChoices(me, cityAfter));
		}
		else if(cityAfter) { targetHpAfter = cityAfter->GetMaxHitPoints() - cityAfter->getDamage(); o["target_hp"] = J(targetHpAfter); }
	}
	// Every attack that ran changed something: the target's health, or the attacker's health or moves.
	bool bNothingChanged = uAfter && !uAfter->isDelayedDeath() && targetHpAfter == targetHpBefore
	                       && uAfter->GetCurrHitPoints() == hpBefore && uAfter->movesLeft() == movesBefore;
	o["attacked"] = J(!bNothingChanged);
	if(bNothingChanged) o["note"] = J("the game did not carry out the attack; check attack with preview_only for the reason, or try again in a moment");
	return JVal(o);
}

MCP_TOOL(ToolCityAttack)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	CvCity* c = RequireCity(me, args, err);
	int x = 0, y = 0;
	if(!c || !RequireInt(args, "x", x, err) || !RequireInt(args, "y", y, err)) return JVal();
	if(!c->CanRangeStrikeNow())
	{
		if(c->IsResistance() || c->IsRazing()) err = "this city cannot attack while in resistance or being razed";
		else if(c->isMadeAttack()) err = "this city has already attacked this turn";
		else err = "this city cannot attack now: no enemy is within its range";
		return JVal();
	}
	if(!c->canRangeStrikeAt(x, y)) { err = "no attackable enemy on that tile within the city's range"; return JVal(); }
	CvPlot* p = GC.getMap().plot(x, y);
	UnitHandle hDefender = p->getBestDefender(NO_PLAYER, me->GetID(), NULL, true, false, false, true);
	CvUnit* d = hDefender.pointer();
	std::string targetName = d ? UnitTypeName(d->getUnitType()) : "";
	int hpBefore = d ? d->GetCurrHitPoints() : -1;
	PlayerTypes eDefOwner = d ? d->getOwner() : NO_PLAYER;
	int iDefId = d ? d->GetID() : -1;

	if(c->doTask(TASK_RANGED_ATTACK, x, y) == TASK_ABORTED) { err = "the city could not carry out the attack"; return JVal(); }

	JObj o;
	o["ok"] = J(true);
	if(d)
	{
		CvUnit* dAfter = GET_PLAYER(eDefOwner).getUnit(iDefId);
		o["target"] = J(targetName);
		o["target_hp_before"] = J(hpBefore);
		if(dAfter && !dAfter->isDelayedDeath()) o["target_hp"] = J(dAfter->GetCurrHitPoints());
		else o["target_destroyed"] = J(true);
	}
	return JVal(o);
}

// ------------------------------------------------------------------ diplomacy

MCP_TOOL(ToolDeclareWar)
{
	CvPlayer* me = MeActing(err);
	if(!me) return JVal();
	std::string key = ArgStr(args, "civ");
	PlayerTypes e = FindPlayer(key);
	if(e == NO_PLAYER || e == me->GetID()) { err = "unknown civilization '" + key + "'; use a name from civs"; return JVal(); }
	CvPlayer& other = GET_PLAYER(e);
	CvTeam& myTeam = GET_TEAM(me->getTeam());
	if(!myTeam.isHasMet(other.getTeam())) { err = "you have not met them"; return JVal(); }
	if(myTeam.isAtWar(other.getTeam())) { err = "you are already at war with them"; return JVal(); }
	if(!myTeam.canDeclareWar(other.getTeam(), me->GetID())) { err = "you cannot declare war on them right now (peace treaty or other restriction)"; return JVal(); }
	myTeam.declareWar(other.getTeam(), false, me->GetID());
	JObj o;
	o["ok"] = J(true);
	o["at_war_with"] = J(CivName(e));
	return JVal(o);
}
