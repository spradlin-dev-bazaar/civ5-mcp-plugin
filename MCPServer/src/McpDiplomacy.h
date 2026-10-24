#pragma once

#include <string>
#include "picojson.h"

class CvDeal;

// Diplomacy for the model's seat without the leader screens. Game thread only.

// Called by the game where it would open a leader screen for the player on
// screen. Returns true when the request was recorded for the model's off-screen
// seat instead, so the game must not queue or show it.
bool McpInterceptRequest(PlayerTypes eFrom, PlayerTypes eTo, DiploUIStateTypes eState, const char* szMessage, int iExtraGameData);
bool McpInterceptDealRequest(PlayerTypes eFrom, PlayerTypes eTo, const CvDeal* pDeal, DiploUIStateTypes eState, const char* szMessage);

// VMC's DoEqualizeDealWithHuman asks IsDealWithHumanAcceptable about the player on screen, not the one the deal is with.
void McpBeginEqualizing(PlayerTypes eWith);
void McpEndEqualizing();
PlayerTypes McpEqualizingWith(PlayerTypes eAsked);

// A proposed deal between humans was accepted, declined, withdrawn or expired.
void McpNoteDealResolved(PlayerTypes eFrom, PlayerTypes eTo, PlayerTypes eActBy, bool bAccepted, bool bStillValid);

// Requests still waiting for the model's answer.
picojson::value McpDiploRequestsJson();
int McpCountDiploRequests();
int McpCountDiploRequestsFor(PlayerTypes e);
// Forgets the requests waiting for a civilization (its seat was given back to the game's AI).
void McpDropDiploRequestsFor(PlayerTypes e);

// Game thread, every frame. A deal a seat proposed to the human on screen is announced with a
// notification that the game drops at the end of the human's turn, which Auto End Turn
// reaches at once; so it is announced again at the start of each of their turns until answered.
void McpRemindHumanOfDeals();
void McpResetDiploRequests();
// For status: what awaits the seat's answer; empty when nothing does.
std::string McpDiploWaitingNote();
