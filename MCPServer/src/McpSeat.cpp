#include "CvGameCoreDLLPCH.h"
#pragma comment(lib, "ole32.lib")
// From objbase.h, which does not compile under the precompiled header's trimmed Windows headers.
extern "C" __declspec(dllimport) HRESULT __stdcall CoCreateGuid(GUID* pguid);
#include "McpSeat.h"
#include "McpBridge.h"
#include "McpDiplomacy.h"
#include "McpLogText.h"
#include "McpPopups.h"
#include "McpToolSupport.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvUnit.h"
#include "CvCity.h"
#include "CvTypes.h"
#include "CvGlobals.h"

namespace
{
	const char* GAME_ID_PREFIX = "civ5-game-";
	const char* LEGACY_SEAT_NAME = "Claude"; // saves from before models named themselves
	const char* NOT_AVAILABLE = "that civilization is not available";

	// The log is readable on screen, so an opponent's civilization is not named in it.
	std::string SeatNameForScreen(PlayerTypes e)
	{
		return McpOnScreenTeam(e) ? GET_PLAYER(e).getCivilizationShortDescription() : "an opponent civilization";
	}

	PlayerTypes ActiveHuman()
	{
		PlayerTypes eActive = GC.getGame().getActivePlayer();
		return (eActive != NO_PLAYER && GET_PLAYER(eActive).isHuman()) ? eActive : NO_PLAYER;
	}

	std::string Nickname(PlayerTypes e)
	{
		return CvPreGame::nickname(e).c_str();
	}

	// Binds the seat to the current session and announces it in the seat's own log.
	void Bind(PlayerTypes ePlayer, const std::string& label, const std::string& what)
	{
		McpBridge& bridge = McpBridge::Get();
		bridge.UnbindSeat(ePlayer);
		bridge.SetCurrentSeat(ePlayer);
		McpBindSeatLabel(ePlayer, label);
		bridge.LogLineFor(ePlayer, MCP_LOG_TURN, label + " " + what + " " + SeatNameForScreen(ePlayer));
	}
}

bool McpIsSeat(PlayerTypes e)
{
	// In a single-player game the only human slot besides the one on screen is a seat.
	CvGame& game = GC.getGame();
	if(game.isNetworkMultiPlayer() || game.isHotSeat() || game.isPbem()) return false;
	return e != NO_PLAYER && e < MAX_MAJOR_CIVS && e != game.getActivePlayer() && CvPreGame::slotStatus(e) == SS_TAKEN;
}

bool McpPlayedByModel(PlayerTypes e)
{
	return McpIsSeat(e) || (e != NO_PLAYER && e == GC.getGame().getActivePlayer() && McpBridge::Get().ScreenPlayed());
}

bool McpAnySeat()
{
	for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
		if(McpIsSeat((PlayerTypes)i)) return true;
	return false;
}

std::vector<PlayerTypes> McpUnboundSeats()
{
	std::vector<PlayerTypes> out;
	McpSession s;
	for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
		if(McpIsSeat((PlayerTypes)i) && !McpBridge::Get().SessionForSeat(i, s)) out.push_back((PlayerTypes)i);
	return out;
}

bool McpSeatLabelMatches(PlayerTypes e, const std::string& label)
{
	if(label.empty()) return false;
	std::string nick = Nickname(e);
	if(nick == label) return true;
	return nick == LEGACY_SEAT_NAME && label.compare(0, strlen(LEGACY_SEAT_NAME), LEGACY_SEAT_NAME) == 0;
}

std::string McpSeatWaitingText(PlayerTypes e, const std::string& label)
{
	std::string last = Nickname(e);
	std::string by = last.empty() ? "" : " (last played by " + last + ")";
	if(McpSeatLabelMatches(e, label)) return std::string(GET_PLAYER(e).getCivilizationShortDescription()) + " (yours: take it back with take_seat)";
	if(McpOnScreenTeam(e)) return std::string(GET_PLAYER(e).getCivilizationShortDescription()) + by;
	return "an opponent civilization" + by + ": only the model that played it, or take_seat civ=opponent, can take it";
}

PlayerTypes McpPlayerOf(const McpSession& s)
{
	if(s.seat >= 0) return McpIsSeat((PlayerTypes)s.seat) ? (PlayerTypes)s.seat : NO_PLAYER;
	return s.seat == MCP_SEAT_SCREEN ? ActiveHuman() : NO_PLAYER;
}

PlayerTypes McpSeat()
{
	return McpPlayerOf(McpBridge::Get().Current());
}

bool McpSeatOnScreen()
{
	return McpBridge::Get().Current().seat == MCP_SEAT_SCREEN;
}

// A computer slot starts with the handicap's extra units for the AI. Taken before anyone has
// moved, it gets a human's start instead: the units are dealt again now that the player counts
// as human.
static void DealHumanStartingUnits(CvPlayer& p)
{
	if(GC.getGame().getElapsedGameTurns() > 0 || p.getNumCities() > 0) return;
	int iLoop;
	for(CvUnit* pUnit = p.firstUnit(&iLoop); pUnit != NULL; pUnit = p.nextUnit(&iLoop))
		if(pUnit->hasMoved()) return;
	for(CvUnit* pUnit = p.firstUnit(&iLoop); pUnit != NULL; pUnit = p.nextUnit(&iLoop))
		pUnit->kill(false);
	CvGameInitialItemsOverrides kOverrides;
	p.initFreeUnits(kOverrides);
	McpBridge::Get().LogLineFor(p.GetID(), MCP_LOG_TURN, "Starts with a human player's units; the computer's extra starting units are gone");
}

bool McpClaimSeat(PlayerTypes ePlayer, std::string& err, bool bAssignedOpponent)
{
	CvGame& game = GC.getGame();
	if(game.isNetworkMultiPlayer() || game.isHotSeat() || game.isPbem()) { err = "seats can only be claimed in a single-player game"; return false; }
	McpBridge& bridge = McpBridge::Get();
	McpSession mine = bridge.Current();
	if(mine.seat >= 0 && McpIsSeat((PlayerTypes)mine.seat))
	{
		err = std::string("already playing ") + GET_PLAYER((PlayerTypes)mine.seat).getCivilizationShortDescription() + "; call release_seat first";
		return false;
	}
	if(ePlayer == NO_PLAYER || ePlayer >= MAX_MAJOR_CIVS) { err = "that is not a major civilization"; return false; }
	CvPlayer& p = GET_PLAYER(ePlayer);
	if(!p.isAlive()) { err = "that civilization is not alive"; return false; }
	std::string label = McpCurrentLabel();
	if(McpIsSeat(ePlayer))
	{
		// A seat another model left: the model's own comes back to it; one on the on-screen
		// team the human may hand to any model; an opponent's stays secret.
		McpSession other;
		if(bridge.SessionForSeat(ePlayer, other)) { err = NOT_AVAILABLE; return false; }
		if(!McpSeatLabelMatches(ePlayer, label) && !McpOnScreenTeam(ePlayer) && !bAssignedOpponent) { err = NOT_AVAILABLE; return false; }
		Bind(ePlayer, label, "is back on");
		return true;
	}
	if(p.isHuman()) { err = "that civilization is played by the human on screen"; return false; }
	if(p.isTurnActive()) { err = "that civilization is taking its turn right now; claim it during the human's turn"; return false; }

	CvPreGame::setSlotStatus(ePlayer, SS_TAKEN);
	PlayerTypes eActive = game.getActivePlayer();
	if(eActive != NO_PLAYER) CvPreGame::setHandicap(ePlayer, CvPreGame::handicap(eActive));
	DealHumanStartingUnits(p);
	Bind(ePlayer, label, "now plays");
	return true;
}

bool McpReleaseSeat(std::string& err)
{
	McpBridge& bridge = McpBridge::Get();
	McpSession mine = bridge.Current();
	PlayerTypes e = (mine.seat >= 0) ? (PlayerTypes)mine.seat : NO_PLAYER;
	if(e == NO_PLAYER || !McpIsSeat(e)) { err = "you hold no seat"; return false; }
	if(GET_PLAYER(e).isTurnActive()) { err = "end your turn first; the seat can be released while another player's turn is running"; return false; }
	bridge.LogLineFor(e, MCP_LOG_TURN, "The game's AI now plays " + SeatNameForScreen(e));
	CvPreGame::setSlotStatus(e, SS_COMPUTER);
	CvPreGame::setNickname(e, CvString(""));
	CvPreGame::setLeaderName(e, CvString(""));
	CvPreGame::setHandicap(e, (HandicapTypes)GC.getAI_HANDICAP());
	McpDropDiploRequestsFor(e);
	bridge.SetCurrentSeat(MCP_SEAT_LOST);
	return true;
}

PlayerTypes McpWaitingOpponentSeat()
{
	std::vector<PlayerTypes> waiting = McpUnboundSeats();
	PlayerTypes eFound = NO_PLAYER;
	for(size_t i = 0; i < waiting.size(); ++i)
	{
		if(McpOnScreenTeam(waiting[i])) continue;
		if(eFound != NO_PLAYER) return NO_PLAYER;
		eFound = waiting[i];
	}
	return eFound;
}

// The team of the human eliminated while the screen watches on; not kept across a reload.
static TeamTypes s_eliminatedTeam = NO_TEAM;

bool McpOnScreenTeam(PlayerTypes e)
{
	PlayerTypes eActive = GC.getGame().getActivePlayer();
	if(eActive == NO_PLAYER) return true;
	TeamTypes eScreen = CvPreGame::slotStatus(eActive) == SS_OBSERVER && s_eliminatedTeam != NO_TEAM ? s_eliminatedTeam : GET_PLAYER(eActive).getTeam();
	return GET_PLAYER(e).getTeam() == eScreen;
}

bool McpKeepDeadSlotOpen(PlayerTypes eDead)
{
	CvGame& game = GC.getGame();
	if(eDead != game.getActivePlayer() || CvPreGame::slotStatus(eDead) != SS_TAKEN || !McpAnySeatAlive()) return false;
	s_eliminatedTeam = GET_PLAYER(eDead).getTeam();
	McpBridge::Get().LogLineFor(-1, MCP_LOG_ALWAYS, "Your civilization has been eliminated. The game goes on so the AI players can finish it; you can watch to the end.");
	if(game.CanMoveActivePlayerToObserver())
	{
		game.ActivateObserverSlot();
		return false;
	}
	CvPreGame::setSlotStatus(eDead, SS_OBSERVER);
	return true;
}

std::string McpNewUuid()
{
	GUID g;
	if(FAILED(CoCreateGuid(&g))) return "";
	char id[64];
	_snprintf_s(id, sizeof(id), _TRUNCATE, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	            g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
	return id;
}

std::string McpGameId()
{
	std::string name = CvPreGame::gameName().c_str();
	if(name.compare(0, strlen(GAME_ID_PREFIX), GAME_ID_PREFIX) == 0) return name;
	std::string uuid = McpNewUuid();
	if(uuid.empty()) return "";
	std::string id = GAME_ID_PREFIX + uuid;
	CvPreGame::setGameName(CvString(id));
	return id;
}

std::string McpIdentityLabel(const McpIdentity& id)
{
	std::string label;
	const std::string* parts[] = { &id.provider, &id.model, &id.version };
	for(int i = 0; i < 3; ++i)
	{
		if(parts[i]->empty()) continue;
		if(!label.empty()) label += " ";
		label += *parts[i];
	}
	return label;
}

std::string McpCurrentLabel()
{
	return McpIdentityLabel(McpBridge::Get().Current().identity);
}

std::string McpSeatLabel(PlayerTypes e)
{
	if(e == NO_PLAYER) return "";
	McpSession s;
	bool bSeat = McpIsSeat(e);
	if(bSeat ? McpBridge::Get().SessionForSeat(e, s) : (e == GC.getGame().getActivePlayer() && McpBridge::Get().SessionForSeat(MCP_SEAT_SCREEN, s)))
	{
		std::string label = McpIdentityLabel(s.identity);
		if(!label.empty()) return label;
	}
	return bSeat ? Nickname(e) : "";
}

void McpBindSeatLabel(PlayerTypes e, const std::string& label)
{
	if(!McpIsSeat(e)) return;
	CvPreGame::setNickname(e, CvString(label));
	CvPreGame::setLeaderName(e, CvString(label.empty() ? std::string() : std::string(GET_PLAYER(e).getLeaderInfo().GetDescription()) + " (" + label + ")"));
}

void McpNoteSeatPopup(PlayerTypes ePlayer, int iType, int iData1, int iData2, int iData3, bool bOption1, const std::string& text)
{
	if(!McpIsSeat(ePlayer)) return; // the on-screen player's popups arrive through the UI
	McpBridge::Get().NotePopup(ePlayer, iType, iData1, iData2, iData3, bOption1, text, false);
}

void McpNoteSeatNotification(PlayerTypes ePlayer, const char* szMessage, const char* szSummary)
{
	if(!McpPlayedByModel(ePlayer)) return;
	const char* szText = (szMessage && *szMessage) ? szMessage : szSummary;
	if(!szText || !*szText) return;
	// City-state quest notifications end with the game's "RIGHT-CLICK to dismiss" hint, meant for the notification panel.
	std::string text = szText;
	std::string dismiss = Localization::Lookup("TXT_KEY_MINOR_QUEST_BLOCKING_TT").toUTF8();
	size_t at = text.find(dismiss);
	if(at != std::string::npos) text.erase(at);
	McpBridge::Get().LogLineFor(ePlayer, MCP_LOG_NOTIFICATIONS, "Notification: " + text);
}

namespace
{
	const char* OrdersCancelledText(CivOrdersCancelledReason reason)
	{
		switch(reason)
		{
		case CIV_ORDERS_PROMOTION_READY: return "it earned a promotion; choose one with promote_unit, then order it again";
		case CIV_ORDERS_CANNOT_LAND: return "an air unit can no longer land there (no room, or it is out of range)";
		case CIV_ORDERS_NO_ROUTE_TO_BUILD: return "no route can be built that way any more";
		case CIV_ORDERS_ROUTE_BLOCKED: return "the next tile on the route can no longer be entered";
		case CIV_ORDERS_NO_PATH: return "no path leads there any more: something now blocks every route (units, closed borders or newly seen terrain)";
		case CIV_ORDERS_PATH_LONGER: return "the way there now takes more than two turns longer than at the start of this turn's move (something newly seen is in the way); order it again if you still want it there";
		case CIV_ORDERS_DESTINATION_BLOCKED: return "it would end this turn on the destination, which it can no longer enter (a unit stands there, or the tile is now closed to it); order it again if you still want it there";
		default: return "the game cancelled its orders";
		}
	}
}

void McpNoteOrdersCancelled(CvUnit* u, CivOrdersCancelledReason reason)
{
	if(!u || !McpIsSeat(u->getOwner())) return;
	const MissionData* m = u->GetHeadMissionData();
	if(!m || (m->eMissionType != CvTypes::getMISSION_MOVE_TO() && m->eMissionType != CvTypes::getMISSION_ROUTE_TO())) return;
	CvUnitEntry* info = GC.getUnitInfo(u->getUnitType());
	std::string text = McpFormat("Your %s (id %d) at (%d,%d) will not continue to (%d,%d): %s", info ? info->GetDescription() : "unit", u->GetID(), u->getX(), u->getY(), m->iData1, m->iData2, OrdersCancelledText(reason));
	McpBridge::Get().NotePopup(u->getOwner(), MCP_POPUP_TEXT, 0, 0, 0, false, text, false);
}

std::vector<PlayerTypes> McpTeammateSeats()
{
	std::vector<PlayerTypes> out;
	for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		PlayerTypes e = (PlayerTypes)i;
		if(McpIsSeat(e) && McpOnScreenTeam(e) && GET_PLAYER(e).isAlive()) out.push_back(e);
	}
	return out;
}

bool McpAnySeatAlive()
{
	for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
		if(McpIsSeat((PlayerTypes)i) && GET_PLAYER((PlayerTypes)i).isAlive()) return true;
	return false;
}

bool McpScreenIsObserver()
{
	PlayerTypes eActive = GC.getGame().getActivePlayer();
	return eActive != NO_PLAYER && CvPreGame::slotStatus(eActive) == SS_OBSERVER;
}

bool McpEngineSeesAsHuman(PlayerTypes e)
{
	return !(McpIsSeat(e) && McpScreenIsObserver());
}
