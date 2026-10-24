#pragma once

#include <string>
#include "picojson.h"

// Game thread. Draws the tiles within radius of (cx, cy) as the model's civilization knows
// them, with the map tool's fog rules, and returns the picture as PNG bytes; summary gets a
// line of text about what is in it, and cities and units the roster of what the picture shows,
// with exact coordinates. Empty on failure, with the reason in summary.
std::string McpRenderMapPng(PlayerTypes eMe, int cx, int cy, int radius, std::string& summary, picojson::array& cities, picojson::array& units);

std::string McpBase64(const std::string& bytes);
