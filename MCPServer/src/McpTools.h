#pragma once

#include <string>
#include "picojson.h"

#define MCP_TOOL_ADVANCE_ROUND "advance_round"
#define MCP_TOOL_END_TURN "end_turn"
#define MCP_TOOL_SKIP_HUMAN_TURN "skip_human_turn"
#define MCP_TOOL_STATUS "status"

enum McpLogKind
{
	LOG_NONE,
	LOG_MOVE, // LOG_MOVE and LOG_UNIT are split by the kind of unit in the unit_id argument
	LOG_UNIT,
	LOG_COMBAT,
	LOG_CITY,
	LOG_EMPIRE,
	LOG_DIPLOMACY,
	LOG_MESSAGES,
	LOG_TURN
};

// A tool exposed over MCP. The handler runs on the game thread; it fills error
// (and returns any value) to report a failure to the model.
struct McpToolDef
{
	const char* name;
	const char* description;
	const char* inputSchema; // JSON text
	picojson::value (*handler)(const picojson::object& args, std::string& error); // NULL: the bridge runs it on the HTTP thread (see CallTool)
	McpLogKind logAs; // LOG_NONE for queries; actions are echoed into the in-game log under this kind
	bool waitsForTurn;  // the HTTP thread blocks until it is the model's turn before running the handler
};

const McpToolDef* McpGetTools(size_t& count);
const McpToolDef* McpFindTool(const std::string& name);
// "" when args match the tool's schema; otherwise the first unknown, mistyped or missing argument.
std::string McpCheckArgs(const McpToolDef& tool, const picojson::object& args);
