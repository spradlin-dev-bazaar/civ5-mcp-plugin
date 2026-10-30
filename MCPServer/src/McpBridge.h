#pragma once

#include <string>
#include <vector>

// A game popup the model must read. Modal for the model the way it is for a human:
// action tools are refused until it is acknowledged, and the game dialog is
// only closed once it has been.
struct McpPopup
{
	int id;
	int turn;
	int player; // the player it was shown to (PlayerTypes)
	std::string text;
	std::string dialog; // InGame dialog context to close on acknowledgement; "" if a human must close it
	bool read;
};

// Which model is playing, as it named itself with set_identity, e.g. Claude / Opus / 5.5.
struct McpIdentity
{
	std::string provider;
	std::string model;
	std::string version;
};

// One connected model: an MCP session. Several can play at once, each on its own seat.
enum McpSeatState
{
	MCP_SEAT_SCREEN = -1,   // no seat: plays the civilization on screen
	MCP_SEAT_LOST = -2,     // held a seat it no longer has (the game was reloaded, it released the seat, or another session took it over); must claim one again
	MCP_SEAT_UNDECIDED = -3 // new session in a game that has seats; must choose one (or the screen)
};
struct McpSession
{
	std::string token;     // the Mcp-Session-Id the client sends
	McpIdentity identity;
	int seat;              // PlayerTypes of the claimed seat, or a McpSeatState
	bool idle;             // no request for a while: its seat may be taken over, and it is not counted as playing
	McpSession() : seat(MCP_SEAT_UNDECIDED), idle(false) {}
};

// A mod loaded in the current game: "Name (v N)" and its description, on one line.
struct McpModInfo
{
	std::string name;
	std::string description;
};

// One line of the in-game log; category is one of the MCP_LOG_ names.
struct McpLogEntry
{
	std::string category;
	std::string text;
	int player;       // the civilization it was written for (PlayerTypes), or -1 for the bridge itself
	std::string who;  // the model's label at the time, "" for the bridge
	std::string civ;  // that civilization's name, "" for the one on screen or the bridge
	bool teammate;    // written while that civilization was the one on screen or on its team; lines
	                  // written as an opponent are never shown on screen, even after the model moves seats
};

// Serves MCP (JSON-RPC over HTTP) to one or more models. Requests arrive on HTTP
// threads; anything that reads or changes game state is queued and run by Tick() on
// the game thread, so game code never runs off its own thread. Each request carries
// its session; while a job runs, that session is the current one.
class McpBridge
{
public:
	static McpBridge& Get();

	// Game thread, once per frame.
	void Tick();

	// Game thread. Stops serving and joins the HTTP thread. Called from CivPluginShutdown;
	// static destruction is too late (see the implementation).
	void Shutdown();

	// Game thread, from an after-hook on CvGame::Read: a save was loaded, so the next Tick starts this game's state fresh.
	void NoteGameLoaded();

	// A tool that cannot finish this frame (a unit is still animating) calls this
	// instead of failing; the bridge runs it again on a later frame, for a few seconds.
	static void RetryLater();

	// Game thread, during a job: the session whose tool is running. Outside a job the
	// token is "". Identity is empty until the model calls set_identity; every new MCP
	// session starts empty, so a model that reconnects names itself again.
	McpSession Current();
	void SetCurrentIdentity(const McpIdentity& identity);
	void SetCurrentSeat(int seat);
	// Any thread. Every session as of now, idle ones included (see McpSession::idle).
	std::vector<McpSession> Sessions();
	// Any thread. The live (not idle) session on a seat, if any; false when nobody is.
	bool SessionForSeat(int seat, McpSession& out);
	// Any thread. Takes a seat away from whichever session holds it (it becomes lost).
	void UnbindSeat(int seat);
	// Any thread. True when a live session plays the civilization on screen.
	bool ScreenPlayed();

	// Any thread. The mods loaded in this game; the in-game UI script reports them when a
	// game loads, since only the UI's Modding API can list them.
	std::vector<McpModInfo> ActiveMods();
	void SetActiveMods(const std::vector<McpModInfo>& mods);

	// Game thread. The game's settings and mods go to each model once per MCP session and
	// per loaded game (marked for every session when the UI reports that game's mods), in
	// the next status; Take returns true at most once per mark, for the current session.
	bool TakeGameBriefingDue();
	void MarkGameBriefingDue();

	// Game thread. Appends to the in-game log for the current session's civilization
	// (LogLine) or for a given one (LogLineFor), inserting a separator when the turn changed.
	// A named session with no civilization writes hidden lines; the bridge's own are shown.
	void LogLine(const char* category, const std::string& text);
	void LogLineFor(int player, const char* category, const std::string& text);

	// Any thread. The returned sequence number grows with every line ever appended,
	// so callers can detect new lines even after old ones were trimmed.
	int GetLog(std::vector<McpLogEntry>& out);
	// Any thread. How many live sessions play an opponent of the player on screen, as of the last Tick.
	int HiddenPlayers() const;

	// Records a popup; it is described and logged on the next Tick. bOnScreen means the
	// game's dialog is open for the player on screen: it is a model's only when a session
	// plays that civilization, and is then closed once the model has read it. Other popups
	// name the civilization they are for: a seat, or the one on screen while a model plays it.
	// UI thread (Lua) or game thread: it only queues, and must not read the game.
	void NotePopup(int player, int iType, int iData1, int iData2, int iData3, bool bOption1, const std::string& text, bool bOnScreen);

	// UI thread (Lua). Dialog names whose messages were acknowledged since the last
	// call, one per line; the Lua side closes them.
	std::string TakeDialogsToClose();

	// Testing only: enables the tools that drive the idle human's turn and closes
	// that human's popups and leader screens unread. Off by default, and off again whenever a game is started or loaded.
	void SetTestingMode(bool bOn);
	bool TestingMode() const;

	// Chat between the human on screen and a teammate's seat. The human's message is logged for
	// that seat and held until its model takes it; the model's reply is logged for the seat and
	// counts as unread until the human opens the log window.
	// UI thread (Lua).
	void SendChat(int player, const std::string& text);
	int UnreadReplies();
	int TotalReplies(); // ever, so the UI can notice a new one even while the window is open
	void MarkRepliesSeen();
	// Game thread, during a job. TakeChat returns the held messages, oldest first, and drops them:
	// a status whose reply never reaches the model (its call timed out) loses them, but the
	// human's log keeps every line.
	std::vector<std::string> TakeChat(int player);
	void ReplyChat(const std::string& text);

	// Game thread. Popups shown to one player.
	std::vector<McpPopup> GetPopupsSince(int player, int iTurn);
	int CountUnread(int player);
	// ids NULL = all. Returns how many were newly marked read.
	int Acknowledge(int player, const std::vector<int>* ids);

private:
	McpBridge();
	~McpBridge();

	struct Impl;
	friend struct Impl;
	Impl* m_pImpl;
};
