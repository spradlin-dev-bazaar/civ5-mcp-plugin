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
#include "CvCultureClasses.h"
#include "CvCity.h"
#include "CvUnit.h"
#include "CvInfos.h"

using namespace McpTool;

// Never "GenericPopup": it resets itself only through its own buttons, so closing it from outside stops it showing
// any later question for the rest of the game.
const char* McpPopupDialogToClose(int iType)
{
	switch(iType)
	{
	case BUTTONPOPUP_GOODY_HUT_REWARD: return "GoodyHutPopup";
	case BUTTONPOPUP_TECH_AWARD: return "TechAwardPopup";
	case BUTTONPOPUP_NATURAL_WONDER_REWARD: return "NaturalWonderPopup";
	case BUTTONPOPUP_NEW_ERA: return "NewEraPopup";
	case BUTTONPOPUP_WONDER_COMPLETED_ACTIVE_PLAYER: return "WonderPopup";
	case BUTTONPOPUP_GREAT_WORK_COMPLETED_ACTIVE_PLAYER: return "GreatWorkSplash";
	case BUTTONPOPUP_WHOS_WINNING: return "WhosWinningPopup";
	case BUTTONPOPUP_LEAGUE_SPLASH: return "LeagueSplash";
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
	if(iType == MCP_POPUP_LEADER_MESSAGE)
	{
		PlayerTypes eLeader = (PlayerTypes)iData1;
		std::string who = (eLeader >= 0 && eLeader < MAX_CIV_PLAYERS) ? CivName(eLeader) : "Another civilization";
		std::string s = text.empty() ? who + "'s leader spoke to you" : who + "'s leader says: " + text;
		if(iData2 == DIPLO_UI_STATE_AI_DECLARED_WAR) s += " (they have declared war on you)";
		return s;
	}
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
	case BUTTONPOPUP_GREAT_WORK_COMPLETED_ACTIVE_PLAYER:
	{
		CvGameCulture* pCulture = GC.getGame().GetGameCulture();
		if(iData1 < 0 || iData1 >= pCulture->GetNumGreatWorks()) return "";
		// A swap's notification passes the civilization the work came from.
		bool bSwapped = iData2 >= 0 && iData2 < MAX_PLAYERS && iData2 != ePlayer;
		std::string s = (bSwapped ? "Received the great work " : "Created the great work ") + Localized(pCulture->GetGreatWorkName(iData1).c_str());
		std::string artist = Localized(pCulture->GetGreatWorkArtist(iData1).c_str());
		if(!artist.empty()) s += " by " + artist;
		CvCity* pCity = pCulture->GetGreatWorkCity(iData1);
		if(pCity) s += McpFormat(", now in %s", pCity->getName().c_str());
		return s;
	}
	case BUTTONPOPUP_BARBARIAN_CAMP_REWARD:
		return McpFormat("Cleared a barbarian camp: +%d gold", iData1);
	case BUTTONPOPUP_WHOS_WINNING:
		return "The game showed its Who's Winning ranking of the civilizations";
	case BUTTONPOPUP_RETURN_CIVILIAN:
	{
		CvUnit* u = (iData1 >= 0 && iData1 < MAX_PLAYERS) ? GET_PLAYER((PlayerTypes)iData1).getUnit(iData3) : NULL;
		std::string what = u ? McpFormat("a %s (unit id %d)", UnitTypeName(u->getUnitType()), iData3) : std::string("a civilian unit");
		return McpFormat("Captured %s that belonged to %s; it is yours unless a human gives it back. The game asks on screen, and only a human can answer there; until then, later pop-ups may wait behind the question or never show", what.c_str(), CivName((PlayerTypes)iData2).c_str());
	}
	case BUTTONPOPUP_LEAGUE_SPLASH:
		if(bOption1) return McpFormat("The World Congress was founded, hosted by %s (congress shows when it meets and what is proposed)", CivName((PlayerTypes)iData2).c_str());
		return McpFormat("The World Congress has called a special session, hosted by %s (congress shows what it votes on)", CivName((PlayerTypes)iData2).c_str());
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
		CvCity* c = (ePlayer != NO_PLAYER) ? GET_PLAYER(ePlayer).getCity(iData1) : NULL;
		std::string s = McpFormat(bOption1 ? "Acquired %s (city id %d) by buying out its city-state" : "Captured %s (city id %d)", c ? c->getName().c_str() : "a city", iData1);
		if(iData2 > 0) s += McpFormat(", plundering %d gold", iData2);
		if(iData3 > 0) s += McpFormat(" and %d culture", iData3);
		if(c && CaptureUndecided(c)) s += ". Decide its fate with city_task: " + CaptureChoices(&GET_PLAYER(ePlayer), c);
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
