#pragma once

struct CivHostInfo;
struct lua_State;

// For an after-hook on CvLuaGame::RegisterMembers, which leaves the Game table on top of the Lua stack.
void McpAddGameMethods(const CivHostInfo* host, lua_State* L);
