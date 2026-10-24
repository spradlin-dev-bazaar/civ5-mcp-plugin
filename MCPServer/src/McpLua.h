#pragma once

struct lua_State;

// An after-hook on CvLuaGame::RegisterMembers, which leaves the Game table on top of the Lua stack.
void McpAddGameMethods(lua_State* L);
