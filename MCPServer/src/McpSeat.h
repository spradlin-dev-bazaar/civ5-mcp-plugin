#pragma once

#include <string>
#include <vector>
#include "CivPluginApi.h"

struct McpIdentity;
struct McpSession;
class CvUnit;

// Reads the game's slot data without a lock. Game thread only, except McpTeammateSeats and
// McpSeatLabel, which the AI Log window also calls from the UI thread.

// A seat is a human slot that take_seat made out of a computer slot in a single-player
// game; several models can each hold one. A seat is never the player on screen, so the
// game's popups for it are recorded for its model instead of shown.
bool McpIsSeat(PlayerTypes e);
// Game thread. A civilization some model plays: a seat, or the one on screen while a session plays it.
bool McpPlayedByModel(PlayerTypes e);
bool McpAnySeat();
// Seats no live session plays right now (a model that reconnects takes its seat back).
std::vector<PlayerTypes> McpUnboundSeats();
// Whether a seat's saved label is this model's: the same label, or the "Claude" of saves
// made before models named themselves, for any Claude model.
bool McpSeatLabelMatches(PlayerTypes e, const std::string& label);
// How a waiting seat is described to a model: by name when it is the model's own or on the
// on-screen team, otherwise only as an opponent civilization.
std::string McpSeatWaitingText(PlayerTypes e, const std::string& label);

// The civilization a session plays: its seat, the active human for one playing the screen,
// NO_PLAYER for one that is undecided or whose seat is lost.
PlayerTypes McpPlayerOf(const McpSession& s);
// McpPlayerOf the current session.
PlayerTypes McpSeat();
// True when the current session plays the civilization on screen (it holds no seat).
bool McpSeatOnScreen();

// Game thread. Makes a computer-controlled civilization the current session's seat, or
// takes back a seat no live session plays (the model's own, or one on the on-screen team
// that the human assigns it); or gives the session's seat back to the game's AI.
// err names the reason on failure without revealing which civilizations other models play.
// bAssignedOpponent: the seat was picked for the model by take_seat civ=opponent, which may
// hand over a waiting opponent seat another model left without the model naming it.
bool McpClaimSeat(PlayerTypes ePlayer, std::string& err, bool bAssignedOpponent = false);
bool McpReleaseSeat(std::string& err);
// The one opponent seat waiting for a player, if there is exactly one; NO_PLAYER otherwise.
PlayerTypes McpWaitingOpponentSeat();

// Game thread. A UUID naming this game across reloads and connections, so a model can keep
// memories per game. Kept in the saved game name, which only multiplayer lobbies show;
// a game without one gets one when it starts or loads, and keeps it once saved.
std::string McpGameId();
// A fresh UUID as text.
std::string McpNewUuid();

// A model's name from its identity, e.g. "Claude Opus 5.5"; "" until it has named itself.
std::string McpIdentityLabel(const McpIdentity& id);
// The current session's label.
std::string McpCurrentLabel();
// The label of the model playing a civilization: its live session's, or the last one saved
// with the seat; "" for a civilization no model plays.
std::string McpSeatLabel(PlayerTypes e);
// Game thread. Saves the model's label with the seat and shows it after the seat's leader,
// e.g. "Dido (Claude Opus 5.5)".
void McpBindSeatLabel(PlayerTypes e, const std::string& label);

// Game thread. True when e is the player on screen or on its team (also when nobody is on screen).
// After the human's elimination the screen is an observer; its team stays the eliminated one's.
bool McpOnScreenTeam(PlayerTypes e);
// The living seats on the on-screen player's team: the ones the human can chat with.
std::vector<PlayerTypes> McpTeammateSeats();

// Game thread, from CvPlayer::setAlive just before a dead player's slot is closed. When the
// player on screen dies while a seat lives on, the screen moves to a free observer slot the way
// the game's own AI autoplay does, so the engine never sees its own slot close (which shows the
// defeat screen and stops the map). True when the dead slot must stay open: no observer slot
// was free, so the dead slot itself became the observer.
bool McpKeepDeadSlotOpen(PlayerTypes eDead);
// Whether any seat is still alive: what keeps the game going after the human's elimination.
bool McpAnySeatAlive();
// True while the screen is an observer slot (the human eliminated, watching on).
bool McpScreenIsObserver();
// What the engine is told when it asks whether a player is human. While the screen is an
// observer, seats are shown as computer players: a human slot's turn that is not the screen's
// makes the engine wait for a hotseat hand-over, holding playback and the camera the whole
// turn. The game core itself keeps seeing seats as human, so their play is unchanged.
bool McpEngineSeesAsHuman(PlayerTypes e);

// Game thread. Records a popup the game only shows to the player on screen, when
// it is for a seat. Same arguments as the game's popup info.
void McpNoteSeatPopup(PlayerTypes ePlayer, int iType, int iData1, int iData2, int iData3, bool bOption1, const std::string& text);
// Game thread. Echoes a notification for a civilization a model plays into its log.
void McpNoteSeatNotification(PlayerTypes ePlayer, const char* szMessage, const char* szSummary);
// Game thread, just before the game drops a unit's queued move. A seat's model is told, as a
// game message, that the move will not continue and why; other owners' units are ignored.
void McpNoteOrdersCancelled(CvUnit* u, CivOrdersCancelledReason reason);
