#include "CvGameCoreDLLPCH.h"
#include "McpPopups.h"
#include "McpToolSupport.h"
#include "McpSeat.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvPlot.h"
#include "CvMap.h"
#include "CvGlobals.h"
#include "CvTechClasses.h"
#include "CvInfos.h"

using namespace McpTool;

const char* McpPopupDialogToClose(int iType)
{
	switch(iType)
	{
	case BUTTONPOPUP_GOODY_HUT_REWARD: return "GoodyHutPopup";
	case BUTTONPOPUP_TECH_AWARD: return "TechAwardPopup";
	case BUTTONPOPUP_NATURAL_WONDER_REWARD: return "NaturalWonderPopup";
	case BUTTONPOPUP_NEW_ERA: return "NewEraPopup";
	case BUTTONPOPUP_WONDER_COMPLETED_ACTIVE_PLAYER: return "WonderPopup";
	case BUTTONPOPUP_BARBARIAN_CAMP_REWARD: return "BarbarianCampPopup";
	case BUTTONPOPUP_GOLDEN_AGE_REWARD: return "GoldenAgePopup";
	case BUTTONPOPUP_GREAT_PERSON_REWARD: return "GreatPersonRewardPopup";
	case BUTTONPOPUP_CITY_STATE_GREETING: return "CityStateGreetingPopup";
	case BUTTONPOPUP_TEXT: return "TextPopup";
	case BUTTONPOPUP_ADVISOR_COUNSEL: return "AdvisorCounselPopup";
	case BUTTONPOPUP_ADVISOR_INFO: return "AdvisorInfoPopup";
	default: return "";
	}
}

namespace
{
	// The engine's reward text is empty for rewards whose description needs a number filled in.
	// iAmount is the popup's one granted number (gold, else culture, else faith), as the engine's own popup carries it.
	std::string GoodySummary(int iGoody, int iAmount)
	{
		Database::SingleResult kResult;
		if(iGoody < 0 || !DB.SelectAt(kResult, "GoodyHuts", iGoody)) return "a reward";
		CvGoodyInfo k;
		k.CacheResult(kResult);
		std::string s = k.GetType();
		if(s.compare(0, 6, "GOODY_") == 0) s = s.substr(6);
		for(size_t i = 0; i < s.size(); ++i) s[i] = (s[i] == '_') ? ' ' : (char)tolower((unsigned char)s[i]);
		if(iAmount > 0 && k.getCulture() > 0) s += McpFormat(" (+%d culture)", iAmount);
		else if(iAmount > 0 && (k.getFaith() > 0 || k.isPantheonFaith() || k.getProphetPercent() > 0)) s += McpFormat(" (+%d faith)", iAmount);
		if(k.getPopulation() > 0) s += McpFormat(" (+%d population)", k.getPopulation());
		if(k.getExperience() > 0) s += McpFormat(" (+%d experience)", k.getExperience());
		return s;
	}

	// With VMC's GLOBAL_CS_GIFTS option a city-state greeting's text names the gift's kind, and a unit gift's
	// value is the unit's type (0 when none was given); without the option the value is gold.
	std::string GiftPhrase(const std::string& kind, int iValue)
	{
		if(iValue <= 0) return "";
		if(kind == "UNIT") return McpFormat(" (they gave a unit: %s)", UnitTypeName((UnitTypes)iValue));
		std::string s = kind.empty() ? "gold" : kind;
		for(size_t i = 0; i < s.size(); ++i) s[i] = (char)tolower((unsigned char)s[i]);
		return McpFormat(" (they gifted %d %s)", iValue, s.c_str());
	}
}

std::string McpDescribePopup(PlayerTypes ePlayer, int iType, int iData1, int iData2, int iData3, bool bOption1, const std::string& text)
{
	if(iType == MCP_POPUP_TEXT) return text;
	switch(iType)
	{
	case BUTTONPOPUP_GOODY_HUT_REWARD:
		return "Ancient ruins explored: " + (text.empty() ? GoodySummary(iData1, iData2) : text);
	case BUTTONPOPUP_TECH_AWARD:
	{
		CvTechEntry* t = (iData3 >= 0) ? GC.getTechInfo((TechTypes)iData3) : NULL;
		return t ? McpFormat("Researched %s: ", t->GetDescription()) + Localized(t->GetHelp()) : "Researched a technology";
	}
	case BUTTONPOPUP_NATURAL_WONDER_REWARD:
	{
		CvPlot* p = GC.getMap().plot(iData1, iData2);
		std::string s = McpFormat("Discovered a natural wonder%s at (%d,%d)", (p && p->getFeatureType() != NO_FEATURE) ? McpFormat(" (%s)", FeatureName(p->getFeatureType())).c_str() : "", iData1, iData2);
		if(bOption1) s += ", first to find it";
		if(iData3 > 0) s += McpFormat(", +%d gold", iData3);
		return s;
	}
	case BUTTONPOPUP_NEW_ERA:
		return McpFormat("Entered the %s", EraName((EraTypes)iData1));
	case BUTTONPOPUP_WONDER_COMPLETED_ACTIVE_PLAYER:
		return McpFormat("Completed the wonder %s", BuildingName((BuildingTypes)iData1));
	case BUTTONPOPUP_BARBARIAN_CAMP_REWARD:
		return McpFormat("Cleared a barbarian camp: +%d gold", iData1);
	case BUTTONPOPUP_GOLDEN_AGE_REWARD:
		return "A Golden Age has begun";
	case BUTTONPOPUP_GREAT_PERSON_REWARD:
		return McpFormat("A %s has appeared", UnitTypeName((UnitTypes)iData1));
	case BUTTONPOPUP_CITY_STATE_GREETING:
	case BUTTONPOPUP_CITY_STATE_MESSAGE:
	{
		std::string who = (iData1 >= 0 && iData1 < MAX_CIV_PLAYERS) ? CivName((PlayerTypes)iData1) : "a city-state";
		if(iType == BUTTONPOPUP_CITY_STATE_MESSAGE)
			return who + ": " + text + " (their diplomacy screen is open in the game; a human must close it)";
		std::string s = "Met the city-state of " + who;
		s += GiftPhrase(text, iData2);
		return s;
	}
	case BUTTONPOPUP_CITY_CAPTURED:
	{
		PlayerTypes eMe = ePlayer;
		CvCity* c = (eMe != NO_PLAYER) ? GET_PLAYER(eMe).getCity(iData1) : NULL;
		std::string s = McpFormat(bOption1 ? "Acquired %s (city id %d) by buying out its city-state" : "Captured %s (city id %d)", c ? c->getName().c_str() : "a city", iData1);
		if(iData2 > 0) s += McpFormat(", plundering %d gold", iData2);
		if(iData3 > 0) s += McpFormat(" and %d culture", iData3);
		s += ". Decide its fate with city_task: annex, puppet, raze";
		PlayerTypes eOrig = c ? c->getOriginalOwner() : NO_PLAYER;
		if(c && eOrig != NO_PLAYER && eOrig != eMe && GET_PLAYER(eMe).CanLiberatePlayerCity(eOrig)) s += " or liberate (" + CivName(eOrig) + ")";
		return s;
	}
	case BUTTONPOPUP_ADVISOR_COUNSEL:
	case BUTTONPOPUP_ADVISOR_INFO:
		return "Advisor: " + Localized(text.c_str());
	case BUTTONPOPUP_TEXT:
		return Localized(text.c_str());
	default:
		return "";
	}
}
