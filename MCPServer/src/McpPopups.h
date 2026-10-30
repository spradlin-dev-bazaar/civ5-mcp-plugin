#pragma once

#include <string>

// Game popups as reported by the Lua UI (type and data of the popup event).

// Readable text, or "" for popup types that carry nothing worth reporting.
// Game thread only: looks up game data.
std::string McpDescribePopup(PlayerTypes ePlayer, int iType, int iData1, int iData2, int iData3, bool bOption1, const std::string& text);

// Not a game popup: a message the bridge composes itself (text is the whole message).
const int MCP_POPUP_TEXT = 100000;
// Not a game popup: a leader screen the game showed to the player on screen while a model plays it, which
// LLMLog.lua leaves for the model (data1 is the leader's player, data2 the DiploUIStateTypes, text the leader's words).
const int MCP_POPUP_LEADER_MESSAGE = 100001;

// For popups that only inform: the InGame dialog context to close on the model's
// behalf (the context ids in the game's UI/InGame/InGame.xml); "" otherwise.
// Pure; safe from any thread.
const char* McpPopupDialogToClose(int iType);
