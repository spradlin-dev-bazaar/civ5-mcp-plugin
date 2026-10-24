#include "CvGameCoreDLLPCH.h"
#include "McpLogText.h"
#include "McpTools.h"
#include "McpToolSupport.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvCity.h"
#include "CvUnit.h"
#include "CvPlot.h"
#include "CvMap.h"
#include "CvGlobals.h"
#include "CvInfos.h"
#include "CvPolicyClasses.h"

using namespace McpTool;

namespace
{
	// "acknowledge_messages" -> "Acknowledge messages"
	std::string Words(const std::string& s)
	{
		std::string out;
		for(size_t i = 0; i < s.size(); ++i)
		{
			char ch = (s[i] == '_') ? ' ' : (char)tolower((unsigned char)s[i]);
			out += (i == 0) ? (char)toupper((unsigned char)ch) : ch;
		}
		return out;
	}

	bool StartsWith(const std::string& s, const char* prefix)
	{
		return s.compare(0, strlen(prefix), prefix) == 0;
	}

	// "BELIEF_GOD_OF_WAR" -> "God of war", for keys with no name lookup below.
	std::string KeyWords(const std::string& key)
	{
		size_t cut = key.find('_');
		return Words(cut == std::string::npos ? key : key.substr(cut + 1));
	}

	std::string TypeName(const std::string& key)
	{
		if(StartsWith(key, "POLICY_BRANCH_")) { PolicyBranchTypes e = FindPolicyBranchType(key); if(e != NO_POLICY_BRANCH_TYPE) return GC.getPolicyBranchInfo(e)->GetDescription(); }
		if(StartsWith(key, "RESOURCE_")) { ResourceTypes e = FindResourceType(key); if(e != NO_RESOURCE) return ResourceName(e); }
		if(StartsWith(key, "IMPROVEMENT_")) { ImprovementTypes e = FindImprovementType(key); if(e != NO_IMPROVEMENT) return ImprovementName(e); }
		if(StartsWith(key, "UNIT_")) { UnitTypes e = FindUnitType(key); if(e != NO_UNIT) return UnitTypeName(e); }
		if(StartsWith(key, "BUILDING_")) { BuildingTypes e = FindBuildingType(key); if(e != NO_BUILDING) return BuildingName(e); }
		if(StartsWith(key, "TECH_")) { TechTypes e = FindTechType(key); if(e != NO_TECH) return TechName(e); }
		if(StartsWith(key, "BUILD_")) { BuildTypes e = FindBuildType(key); if(e != NO_BUILD) return BuildName(e); }
		if(StartsWith(key, "PROMOTION_")) { PromotionTypes e = FindPromotionType(key); if(e != NO_PROMOTION) return PromotionName(e); }
		if(StartsWith(key, "POLICY_")) { PolicyTypes e = FindPolicyType(key); if(e != NO_POLICY) return GC.getPolicyInfo(e)->GetDescription(); }
		if(StartsWith(key, "PROJECT_")) { ProjectTypes e = FindProjectType(key); if(e != NO_PROJECT) return GC.getProjectInfo(e)->GetDescription(); }
		bool bTypeKey = key.find('_') != std::string::npos;
		for(size_t i = 0; bTypeKey && i < key.size(); ++i) bTypeKey = isupper((unsigned char)key[i]) || isdigit((unsigned char)key[i]) || key[i] == '_';
		return bTypeKey ? KeyWords(key) : key;
	}

	CvUnit* ArgUnit(const JObj& args)
	{
		std::string ignored;
		CvPlayer* me = Me(ignored);
		return (me && HasArg(args, "unit_id")) ? me->getUnit(ArgInt(args, "unit_id", -1)) : NULL;
	}

	std::string UnitText(const JObj& args)
	{
		CvUnit* u = ArgUnit(args);
		if(!u) return McpFormat("unit #%d", ArgInt(args, "unit_id", -1));
		return McpFormat("%s at (%d, %d)", UnitTypeName(u->getUnitType()), u->getX(), u->getY());
	}

	std::string CityText(const JObj& args, const char* key)
	{
		std::string ignored;
		CvPlayer* me = Me(ignored);
		CvCity* c = me ? me->getCity(ArgInt(args, key, -1)) : NULL;
		return c ? std::string(c->getName().c_str()) : McpFormat("city #%d", ArgInt(args, key, -1));
	}

	std::string PlotText(const JObj& args)
	{
		int x = ArgInt(args, "x", -1), y = ArgInt(args, "y", -1);
		CvPlot* p = GC.getMap().plot(x, y);
		CvCity* c = p ? p->getPlotCity() : NULL;
		return c ? McpFormat("%s (%d, %d)", c->getName().c_str(), x, y) : McpFormat("(%d, %d)", x, y);
	}

	// What the acting player sees on the attacked tile: the city and its owner, or the
	// defending unit and its owner; just the tile when it is out of sight.
	std::string TargetText(const JObj& args)
	{
		std::string ignored;
		CvPlayer* me = Me(ignored);
		int x = ArgInt(args, "x", -1), y = ArgInt(args, "y", -1);
		CvPlot* p = GC.getMap().plot(x, y);
		if(!me || !p || !p->isVisible(me->getTeam())) return PlotText(args);
		CvCity* c = p->getPlotCity();
		if(c) return McpFormat("%s's city %s (%d, %d)", CivName(c->getOwner()).c_str(), c->getName().c_str(), x, y);
		UnitHandle hDefender = p->getBestDefender(NO_PLAYER, me->GetID());
		CvUnit* d = hDefender.pointer();
		if(!d) return PlotText(args);
		return McpFormat("%s %s at (%d, %d)", CivName(d->getOwner()).c_str(), UnitTypeName(d->getUnitType()), x, y);
	}

	std::string ValueText(const JVal& v);

	// Every argument as "name value", with ids and coordinates turned into names.
	std::string ArgsText(const JObj& args)
	{
		std::string out;
		for(JObj::const_iterator it = args.begin(); it != args.end(); ++it)
		{
			const std::string& k = it->first;
			std::string part;
			if(k == "unit_id") part = UnitText(args);
			else if(k == "city_id") part = CityText(args, "city_id");
			else if(k == "from_city_id" || k == "to_city_id") part = Words(k.substr(0, k.size() - 8)) + " " + CityText(args, k.c_str());
			else if(k == "y") continue;
			else if(k == "x") part = "at " + PlotText(args);
			else if(it->second.is<bool>()) { if(!it->second.get<bool>()) continue; part = Words(k); }
			else part = Words(k) + " " + ValueText(it->second);
			if(!out.empty()) out += ", ";
			out += part;
		}
		return out;
	}

	std::string ValueText(const JVal& v)
	{
		int i = 0;
		if(IsInt(v, i)) return McpFormat("%d", i);
		if(v.is<std::string>()) return TypeName(v.get<std::string>());
		if(v.is<bool>()) return v.get<bool>() ? "yes" : "no";
		if(v.is<double>()) return McpFormat("%g", v.get<double>());
		if(v.is<JObj>()) return "(" + ArgsText(v.get<JObj>()) + ")";
		if(v.is<JArr>())
		{
			const JArr& a = v.get<JArr>();
			std::string out;
			for(size_t k = 0; k < a.size(); ++k) out += (k ? ", " : "") + ValueText(a[k]);
			return out;
		}
		return "";
	}

	std::string IdList(const JObj& args)
	{
		JObj::const_iterator it = args.find("ids");
		if(it == args.end() || !it->second.is<JArr>()) return "";
		const JArr& a = it->second.get<JArr>();
		std::string out;
		for(size_t k = 0; k < a.size(); ++k) out += (k ? ", #" : " #") + ValueText(a[k]);
		return out;
	}

	std::string Joined(const std::string& a, const std::string& b)
	{
		return a.empty() ? b : (b.empty() ? a : a + "; " + b);
	}
}

const char* McpLogCategory(const McpToolDef& tool, const JObj& args)
{
	switch(tool.logAs)
	{
	case LOG_MOVE:
	case LOG_UNIT:
	{
		CvUnit* u = ArgUnit(args);
		if(!u) return MCP_LOG_ALWAYS;
		if(u->isFound()) return MCP_LOG_SETTLERS;
		if(u->IsCombatUnit()) return (tool.logAs == LOG_MOVE) ? MCP_LOG_MOVEMENT : MCP_LOG_MILITARY;
		return (u->workRate(true) > 0) ? MCP_LOG_WORKERS : MCP_LOG_CIVILIAN;
	}
	case LOG_COMBAT: return MCP_LOG_COMBAT;
	case LOG_CITY: return MCP_LOG_CITY;
	case LOG_EMPIRE: return MCP_LOG_EMPIRE;
	case LOG_DIPLOMACY: return MCP_LOG_DIPLOMACY;
	case LOG_MESSAGES: return MCP_LOG_MESSAGES;
	case LOG_TURN: return MCP_LOG_TURN;
	default: return MCP_LOG_ALWAYS;
	}
}

std::string McpDescribeCall(const McpToolDef& tool, const JObj& args)
{
	const std::string name = tool.name;
	if(name == "move_unit") return UnitText(args) + " moves to " + PlotText(args) + (ArgBool(args, "preview_only", false) ? " (preview only)" : "");
	if(name == "attack") return UnitText(args) + " attacks " + TargetText(args) + (ArgBool(args, "preview_only", false) ? " (preview only)" : "");
	if(name == "city_attack") return CityText(args, "city_id") + " bombards " + TargetText(args);
	if(name == "unit_action")
	{
		std::string s = UnitText(args) + ": " + Words(ArgStr(args, "action"));
		if(HasArg(args, "build")) s += " " + TypeName(ArgStr(args, "build"));
		if(HasArg(args, "x")) s += " at " + PlotText(args);
		return s;
	}
	if(name == "promote_unit") return UnitText(args) + ": promote to " + TypeName(ArgStr(args, "promotion"));
	if(name == "upgrade_unit") return UnitText(args) + ": upgrade";
	if(name == "set_production") return CityText(args, "city_id") + ": produce " + TypeName(ArgStr(args, "item")) + (ArgBool(args, "append", false) ? " (added to the queue)" : "");
	if(name == "buy") return CityText(args, "city_id") + ": buy " + TypeName(ArgStr(args, "item")) + (ArgBool(args, "with_faith", false) ? " with faith" : "");
	if(name == "set_research") return "Research " + TypeName(ArgStr(args, "tech"));
	if(name == "adopt_policy") return "Adopt " + TypeName(ArgStr(args, "policy"));
	if(name == MCP_TOOL_END_TURN)
	{
		std::string summary = ArgStr(args, "summary");
		return std::string(ArgBool(args, "force", false) ? "End turn, leaving idle units as they are" : "End turn") + (summary.empty() ? "" : ": " + summary);
	}
	if(name == "acknowledge_messages") return "Read game messages" + IdList(args);
	if(name == "set_identity") return "The player names itself";
	std::string rest = ArgsText(args);
	return Words(name) + (rest.empty() ? "" : ": " + rest);
}

std::string McpDescribeResult(const McpToolDef& tool, const JVal& result)
{
	if(!result.is<JObj>()) return "";
	const JObj& r = result.get<JObj>();
	const std::string name = tool.name;
	if(name == "set_identity") return "now playing: " + ArgStr(r, "playing_as");
	if(name == "move_unit" && ArgBool(r, "previewed_only", false))
		return McpFormat("path found, %d turns", ArgInt(r, "turns", 0));
	if(name == "move_unit" && !ArgBool(r, "arrived", true))
		return McpFormat("on the way, about %d turns in all", ArgInt(r, "estimated_turns_total", 0));
	if(name != "attack" && name != "city_attack") return "";

	JObj::const_iterator est = r.find("estimate");
	if(ArgBool(r, "previewed_only", false) && est != r.end() && est->second.is<JObj>())
	{
		const JObj& e = est->second.get<JObj>();
		std::string s = McpFormat("expected damage to the target: %d", ArgInt(e, "expected_damage_to_target", 0));
		if(HasArg(e, "expected_damage_to_you")) s += McpFormat(", to us: %d", ArgInt(e, "expected_damage_to_you", 0));
		return s;
	}
	std::string target;
	if(ArgBool(r, "city_captured", false)) target = "city captured";
	else if(ArgBool(r, "target_destroyed", false)) target = "target destroyed";
	else if(HasArg(r, "target_hp")) target = McpFormat("target has %d HP left", ArgInt(r, "target_hp", 0));
	std::string us;
	if(ArgBool(r, "your_unit_destroyed", false)) us = "our unit was destroyed";
	else if(ArgInt(r, "damage_taken", 0) > 0) us = McpFormat("we took %d damage", ArgInt(r, "damage_taken", 0));
	return Joined(target, us);
}
