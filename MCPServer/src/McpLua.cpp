#include "CvGameCoreDLLPCH.h"
#include "CivPluginApi.h"
#include "McpLua.h"
#include "McpBridge.h"
#include "McpSeat.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvGlobals.h"

namespace
{
	//int, table, int GetLLMLog(); seq, then the { Category, Text, Who, Civ } lines for the player on screen and its teammates, oldest first, then how many connected models play an opponent.
	int lGetLLMLog(lua_State* L)
	{
		std::vector<McpLogEntry> entries;
		int seq = McpBridge::Get().GetLog(entries);
		lua_pushinteger(L, seq);
		lua_createtable(L, (int)entries.size(), 0);
		int n = 0;
		for(size_t i = 0; i < entries.size(); ++i)
		{
			if(!entries[i].teammate) continue;
			lua_createtable(L, 0, 4);
			lua_pushlstring(L, entries[i].category.data(), entries[i].category.size());
			lua_setfield(L, -2, "Category");
			lua_pushlstring(L, entries[i].text.data(), entries[i].text.size());
			lua_setfield(L, -2, "Text");
			lua_pushlstring(L, entries[i].who.data(), entries[i].who.size());
			lua_setfield(L, -2, "Who");
			lua_pushlstring(L, entries[i].civ.data(), entries[i].civ.size());
			lua_setfield(L, -2, "Civ");
			lua_rawseti(L, -2, ++n);
		}
		lua_pushinteger(L, McpBridge::Get().HiddenPlayers());
		return 3;
	}

	//int, string, string GetLLMIdentity(); how many models have named themselves, then the model and label when exactly one has.
	int lGetLLMIdentity(lua_State* L)
	{
		std::vector<McpSession> sessions = McpBridge::Get().Sessions();
		int iNamed = 0;
		McpIdentity only;
		for(size_t i = 0; i < sessions.size(); ++i)
		{
			if(sessions[i].identity.model.empty() || sessions[i].idle) continue;
			++iNamed;
			only = sessions[i].identity;
		}
		lua_pushinteger(L, iNamed);
		lua_pushstring(L, iNamed == 1 ? only.model.c_str() : "");
		lua_pushstring(L, iNamed == 1 ? McpIdentityLabel(only).c_str() : "");
		return 3;
	}

	//void LLMSetActiveMods(string mods); one mod per line, "name<\t>description".
	int lLLMSetActiveMods(lua_State* L)
	{
		const char* szMods = lua_tostring(L, 1);
		std::vector<McpModInfo> mods;
		std::string all = szMods ? szMods : "";
		size_t start = 0;
		while(start < all.size())
		{
			size_t end = all.find('\n', start);
			if(end == std::string::npos) end = all.size();
			std::string line = all.substr(start, end - start);
			size_t tab = line.find('\t');
			McpModInfo mod;
			mod.name = line.substr(0, tab);
			if(tab != std::string::npos) mod.description = line.substr(tab + 1);
			if(!mod.name.empty()) mods.push_back(mod);
			start = end + 1;
		}
		McpBridge::Get().SetActiveMods(mods);
		return 0;
	}

	//void LLMNotePopup(int type, int data1, int data2, int data3, bool option1, string text);
	int lLLMNotePopup(lua_State* L)
	{
		if(!lua_isnumber(L, 1)) return 0;
		const int iType = lua_tointeger(L, 1);
		const int iData1 = lua_tointeger(L, 2);
		const int iData2 = lua_tointeger(L, 3);
		const int iData3 = lua_tointeger(L, 4);
		const bool bOption1 = lua_toboolean(L, 5) != 0;
		const char* szText = lua_tostring(L, 6);
		McpBridge::Get().NotePopup(-1, iType, iData1, iData2, iData3, bOption1, szText ? szText : "", true);
		return 0;
	}

	//string LLMDialogsToClose();
	int lLLMDialogsToClose(lua_State* L)
	{
		std::string names = McpBridge::Get().TakeDialogsToClose();
		lua_pushlstring(L, names.data(), names.size());
		return 1;
	}

	//table GetLLMTeammates(); the AI seats on the on-screen player's team, as { Player, Name } entries.
	// Name is the model's, or the civilization's while no model has named itself on that seat.
	int lGetLLMTeammates(lua_State* L)
	{
		std::vector<PlayerTypes> seats = McpTeammateSeats();
		lua_createtable(L, (int)seats.size(), 0);
		for(size_t i = 0; i < seats.size(); ++i)
		{
			std::string name = McpSeatLabel(seats[i]);
			if(name.empty()) name = GET_PLAYER(seats[i]).getCivilizationShortDescription();
			lua_createtable(L, 0, 2);
			lua_pushinteger(L, (int)seats[i]);
			lua_setfield(L, -2, "Player");
			lua_pushlstring(L, name.data(), name.size());
			lua_setfield(L, -2, "Name");
			lua_rawseti(L, -2, (int)i + 1);
		}
		return 1;
	}

	//void LLMChat(int player, string text); the human's message to a teammate seat.
	int lLLMChat(lua_State* L)
	{
		const int iPlayer = lua_tointeger(L, 1);
		const char* szText = lua_tostring(L, 2);
		if(szText && *szText) McpBridge::Get().SendChat(iPlayer, szText);
		return 0;
	}

	//int, int LLMChatUnread(); replies written since the log window was last open, then replies ever.
	int lLLMChatUnread(lua_State* L)
	{
		lua_pushinteger(L, McpBridge::Get().UnreadReplies());
		lua_pushinteger(L, McpBridge::Get().TotalReplies());
		return 2;
	}

	//void LLMChatSeen();
	int lLLMChatSeen(lua_State* L)
	{
		McpBridge::Get().MarkRepliesSeen();
		return 0;
	}

	void AddMethod(const CivHostInfo* host, lua_State* L, const char* name, CivLuaFunction fn)
	{
		host->PushLuaFunction(host->pluginIndex, L, fn);
		lua_setfield(L, -2, name);
	}
}

void McpAddGameMethods(const CivHostInfo* host, lua_State* L)
{
	AddMethod(host, L, "GetLLMLog", &lGetLLMLog);
	AddMethod(host, L, "GetLLMIdentity", &lGetLLMIdentity);
	AddMethod(host, L, "LLMNotePopup", &lLLMNotePopup);
	AddMethod(host, L, "LLMSetActiveMods", &lLLMSetActiveMods);
	AddMethod(host, L, "LLMDialogsToClose", &lLLMDialogsToClose);
	AddMethod(host, L, "GetLLMTeammates", &lGetLLMTeammates);
	AddMethod(host, L, "LLMChat", &lLLMChat);
	AddMethod(host, L, "LLMChatUnread", &lLLMChatUnread);
	AddMethod(host, L, "LLMChatSeen", &lLLMChatSeen);
}
