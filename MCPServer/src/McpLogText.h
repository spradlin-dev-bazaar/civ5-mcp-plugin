#pragma once

#include <string>
#include "picojson.h"

struct McpToolDef;

// Categories the AI Log window filters on; mod/LLMLog.lua lists the same names.
#define MCP_LOG_ALWAYS "always"
#define MCP_LOG_REASONING "reasoning"
#define MCP_LOG_TURN "turn"
#define MCP_LOG_MOVEMENT "movement" // military units moving
#define MCP_LOG_MILITARY "military" // military units' other orders
#define MCP_LOG_WORKERS "workers"
#define MCP_LOG_SETTLERS "settlers"
#define MCP_LOG_CIVILIAN "civilian" // great people, caravans, missionaries and other civilians
#define MCP_LOG_COMBAT "combat"
#define MCP_LOG_CITY "city"
#define MCP_LOG_EMPIRE "empire"
#define MCP_LOG_DIPLOMACY "diplomacy"
#define MCP_LOG_MESSAGES "messages"
#define MCP_LOG_NOTIFICATIONS "notifications"
#define MCP_LOG_CHAT "chat" // messages between the human and a teammate model

// Game thread. How an action call reads in the log, before it runs.
const char* McpLogCategory(const McpToolDef& tool, const picojson::object& args);
std::string McpDescribeCall(const McpToolDef& tool, const picojson::object& args);
// A line about the outcome, or "" when the call itself says enough.
std::string McpDescribeResult(const McpToolDef& tool, const picojson::value& result);
