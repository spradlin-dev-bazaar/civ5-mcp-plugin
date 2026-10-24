#pragma once

// Game thread only. When a seat's turn starts, the engine pauses playing the game's visual
// messages (moves, combat, camera, notifications) as for a hotseat hand-over, and only resumes
// once the turn comes back to the screen player. This lets the reader run again during the seat's
// turn, and all the time while the screen is an observer, so the human watches the seat play
// live. It finds the engine's pause code by its instruction pattern and does nothing, after one
// line in MCPServer.log, if the pattern isn't found exactly once or the addresses it names look
// wrong.
void McpKeepSeatTurnVisible();
