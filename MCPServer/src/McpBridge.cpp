#include "CvGameCoreDLLPCH.h"
#include <deque>
#include <map>
#include <algorithm>
#include "McpBridge.h"
#include "McpCrash.h"
#include "McpHttp.h"
#include "McpTools.h"
#include "McpToolSupport.h"
#include "McpPopups.h"
#include "McpSeat.h"
#include "McpDiplomacy.h"
#include "McpLogText.h"
#include "McpSeatPlayback.h"
#include "McpVersion.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvGlobals.h"

#define MCP_PORT 7676
#define MCP_JOB_TIMEOUT_MS 15000
#define MCP_START_RETRY_MS 10000
#define MCP_GAME_IDLE_MS 3000
#define MCP_MAX_CHAT_WAITING 50
#define MCP_WAIT_DEFAULT_SECONDS 25.0
#define MCP_WAIT_MAX_SECONDS 55.0
#define MCP_MAX_LOG_ENTRIES 2000
#define MCP_MAX_POPUPS 200                     // kept per player
#define MCP_SESSION_IDLE_MS (2 * 60 * 1000)    // no request for this long: the model has most likely gone
#define MCP_SESSION_EXPIRE_MS (10 * 60 * 1000) // no request for this long: the session is dropped

struct McpPendingPopup
{
	int player; // -1: the player on screen when drained
	int type, data1, data2, data3;
	bool option1;
	std::string text;
	bool onScreen;
};

struct McpJob
{
	std::string session;
	std::string tool;
	picojson::object args;
	picojson::value result;
	std::string error;
	bool done;
	bool abandoned;
	bool retrying;
	DWORD firstTryMs;
	HANDLE event;

	McpJob() : done(false), abandoned(false), retrying(false), firstTryMs(0), event(CreateEvent(NULL, TRUE, FALSE, NULL)) {}
	~McpJob() { CloseHandle(event); }
};

namespace
{
	picojson::value JsonRpcResponse(const picojson::value& id, const picojson::value& result)
	{
		picojson::object o;
		o["jsonrpc"] = picojson::value("2.0");
		o["id"] = id;
		o["result"] = result;
		return picojson::value(o);
	}

	picojson::value JsonRpcError(const picojson::value& id, int code, const std::string& message)
	{
		picojson::object err;
		err["code"] = picojson::value((double)code);
		err["message"] = picojson::value(message);
		picojson::object o;
		o["jsonrpc"] = picojson::value("2.0");
		o["id"] = id;
		o["error"] = picojson::value(err);
		return picojson::value(o);
	}

	// Guards against DNS rebinding: a browser page may only reach the server if it
	// was itself served from this machine.
	bool OriginAllowed(const McpHttpRequest& req)
	{
		std::map<std::string, std::string>::const_iterator it = req.headers.find("origin");
		if(it == req.headers.end()) return true;
		const std::string& origin = it->second;
		size_t schemeEnd = origin.find("://");
		if(schemeEnd == std::string::npos) return false;
		std::string host = origin.substr(schemeEnd + 3);
		size_t hostEnd = host.find_first_of(":/");
		if(hostEnd != std::string::npos) host = host.substr(0, hostEnd);
		for(size_t i = 0; i < host.size(); ++i) host[i] = (char)tolower((unsigned char)host[i]);
		return host == "localhost" || host == "127.0.0.1";
	}
}

struct McpBridge::Impl
{
	// All fields are read and written under lock.
	struct Session
	{
		McpSession info;
		bool briefingDue;
		bool myTurn;
		int diploSeq; // grows when a new request reaches the session's civilization
		int diploCount;
		bool chatWaiting; // the human's messages to the session's seat await a status
		DWORD lastSeenMs; // a time read before taking the lock can be older than this, and now - lastSeenMs then wraps
		int seatBeforeLoad;
		Session() : briefingDue(true), myTurn(false), diploSeq(0), diploCount(0), chatWaiting(false), lastSeenMs(0), seatBeforeLoad(-1) {}
		bool Idle(DWORD now) const { return now - lastSeenMs > MCP_SESSION_IDLE_MS; }
	};

	McpHttpServer* pServer;
	DWORD lastStartAttemptMs;
	bool startFailureShown;
	CRITICAL_SECTION lock; // guards jobs, log, sessions, popups and every job state transition
	std::deque<McpJob*> jobs;
	std::deque<McpLogEntry> log;
	std::map<std::string, Session> sessions;
	bool inJob;               // game thread: a job is running, for the session named by currentToken
	std::string currentToken;
	std::vector<McpModInfo> activeMods;
	int logSeq;
	int lastLogTurn;
	std::vector<McpPendingPopup> pendingPopups; // written by the UI thread, drained by Tick
	int pendingDropped;
	std::vector<McpPopup> popups;
	int popupSeq;
	std::string dialogsToClose; // newline-joined; LLMLog.lua looks each name up under /InGame/
	unsigned int gameSeed;
	volatile bool gameLoaded;
	std::map<int, std::vector<std::string> > chat;         // per seat: the human's messages no status has delivered yet
	std::vector<std::pair<int, std::string> > pendingChat; // written by the UI thread, drained by Tick
	int unreadReplies;
	int totalReplies;

	// Written by the game thread, read by the HTTP thread.
	volatile int gameTurn;
	volatile bool testingMode;
	volatile bool shuttingDown;
	volatile bool screenHumanTurn; // the human on screen may act
	volatile int hiddenPlayers;    // live sessions playing an opponent of the player on screen
	volatile DWORD lastTickMs;

	Impl();
	~Impl();

	// game thread
	void Tick();
	void TryStart();
	void Shutdown();
	void UpdateSessions();
	bool RunJob(McpJob* pJob); // true: run it again on a later frame
	void Append(int player, const std::string& who, const char* category, const std::string& text);
	void DrainPopups();
	void DrainChat();
	void ResetForNewGame();
	Session* CurrentSession(); // under lock

	// any thread
	int GetLog(std::vector<McpLogEntry>& out);
	bool GameRunning() const
	{
		DWORD last = lastTickMs;
		return last != 0 && GetTickCount() - last < MCP_GAME_IDLE_MS;
	}
	void Touch(const std::string& token); // creates the session when new
	void KeepAlive(const std::string& token); // an ended session stays ended
	void EndSession(const std::string& token);
	// The session's flags; false when it no longer exists.
	bool Flags(const std::string& token, bool& bMyTurn, int& iDiploSeq, bool& bChatWaiting, int& iSeat);

	// HTTP thread
	static void HttpHandler(const McpHttpRequest& req, McpHttpResponse& res);
	void HandleHttp(const McpHttpRequest& req, McpHttpResponse& res);
	picojson::value HandleRpc(const picojson::value& request, const std::string& token, bool& bIsNotification);
	picojson::value CallTool(const std::string& token, const std::string& name, const picojson::object& args, bool& bIsError);
	picojson::value AdvanceRound(const std::string& token, const picojson::object& args, bool& bIsError);
	picojson::value RunOnGameThread(const std::string& token, const std::string& tool, const picojson::object& args, std::string& error);
	bool SessionWakes(const std::string& token, int iDiploSeqAtStart);
};

McpBridge& McpBridge::Get()
{
	static McpBridge instance;
	return instance;
}

McpBridge::McpBridge() : m_pImpl(new Impl()) {}
McpBridge::~McpBridge() { delete m_pImpl; }
void McpBridge::Tick() { m_pImpl->Tick(); }
void McpBridge::Shutdown() { m_pImpl->Shutdown(); }
void McpBridge::NoteGameLoaded() { m_pImpl->gameLoaded = true; }

// ---------------------------------------------------------------- sessions

McpBridge::Impl::Session* McpBridge::Impl::CurrentSession()
{
	if(!inJob) return NULL;
	std::map<std::string, Session>::iterator it = sessions.find(currentToken);
	return (it == sessions.end()) ? NULL : &it->second;
}

void McpBridge::Impl::Touch(const std::string& token)
{
	EnterCriticalSection(&lock);
	Session& s = sessions[token];
	s.info.token = token;
	s.lastSeenMs = GetTickCount();
	LeaveCriticalSection(&lock);
}

void McpBridge::Impl::KeepAlive(const std::string& token)
{
	EnterCriticalSection(&lock);
	std::map<std::string, Session>::iterator it = sessions.find(token);
	if(it != sessions.end()) it->second.lastSeenMs = GetTickCount();
	LeaveCriticalSection(&lock);
}

// A seat the session held stays claimed, waiting for a player, so the same model can take it back at once.
// Safe during the session's own job too: the job then finds no CurrentSession() and carries on.
void McpBridge::Impl::EndSession(const std::string& token)
{
	EnterCriticalSection(&lock);
	sessions.erase(token);
	LeaveCriticalSection(&lock);
}

bool McpBridge::Impl::Flags(const std::string& token, bool& bMyTurn, int& iDiploSeq, bool& bChatWaiting, int& iSeat)
{
	EnterCriticalSection(&lock);
	std::map<std::string, Session>::iterator it = sessions.find(token);
	bool bFound = it != sessions.end();
	if(bFound)
	{
		bMyTurn = it->second.myTurn;
		iDiploSeq = it->second.diploSeq;
		bChatWaiting = it->second.chatWaiting;
		iSeat = it->second.info.seat;
	}
	LeaveCriticalSection(&lock);
	return bFound;
}

McpSession McpBridge::Current()
{
	McpSession out;
	EnterCriticalSection(&m_pImpl->lock);
	Impl::Session* s = m_pImpl->CurrentSession();
	if(s) out = s->info;
	LeaveCriticalSection(&m_pImpl->lock);
	return out;
}

void McpBridge::SetCurrentIdentity(const McpIdentity& identity)
{
	EnterCriticalSection(&m_pImpl->lock);
	Impl::Session* s = m_pImpl->CurrentSession();
	if(s) s->info.identity = identity;
	LeaveCriticalSection(&m_pImpl->lock);
}

void McpBridge::SetCurrentSeat(int seat)
{
	EnterCriticalSection(&m_pImpl->lock);
	Impl::Session* s = m_pImpl->CurrentSession();
	if(s)
	{
		s->info.seat = seat;
		s->seatBeforeLoad = -1;
	}
	LeaveCriticalSection(&m_pImpl->lock);
}

std::vector<McpSession> McpBridge::Sessions()
{
	std::vector<McpSession> out;
	EnterCriticalSection(&m_pImpl->lock);
	DWORD now = GetTickCount();
	for(std::map<std::string, Impl::Session>::iterator it = m_pImpl->sessions.begin(); it != m_pImpl->sessions.end(); ++it)
	{
		McpSession s = it->second.info;
		s.idle = it->second.Idle(now);
		out.push_back(s);
	}
	LeaveCriticalSection(&m_pImpl->lock);
	return out;
}

// For the screen a named session wins over one that has not named itself yet.
bool McpBridge::SessionForSeat(int seat, McpSession& out)
{
	bool bFound = false;
	EnterCriticalSection(&m_pImpl->lock);
	DWORD now = GetTickCount();
	for(std::map<std::string, Impl::Session>::iterator it = m_pImpl->sessions.begin(); it != m_pImpl->sessions.end(); ++it)
	{
		if(it->second.info.seat != seat || it->second.Idle(now)) continue;
		if(bFound && it->second.info.identity.model.empty()) continue;
		out = it->second.info;
		bFound = true;
		if(!out.identity.model.empty()) break;
	}
	LeaveCriticalSection(&m_pImpl->lock);
	return bFound;
}

void McpBridge::UnbindSeat(int seat)
{
	EnterCriticalSection(&m_pImpl->lock);
	for(std::map<std::string, Impl::Session>::iterator it = m_pImpl->sessions.begin(); it != m_pImpl->sessions.end(); ++it)
		if(it->second.info.seat == seat)
		{
			it->second.info.seat = MCP_SEAT_LOST;
			it->second.seatBeforeLoad = -1;
		}
	LeaveCriticalSection(&m_pImpl->lock);
}

bool McpBridge::ScreenPlayed()
{
	McpSession s;
	return SessionForSeat(MCP_SEAT_SCREEN, s);
}

std::vector<McpModInfo> McpBridge::ActiveMods()
{
	EnterCriticalSection(&m_pImpl->lock);
	std::vector<McpModInfo> mods = m_pImpl->activeMods;
	LeaveCriticalSection(&m_pImpl->lock);
	return mods;
}

void McpBridge::SetActiveMods(const std::vector<McpModInfo>& mods)
{
	EnterCriticalSection(&m_pImpl->lock);
	m_pImpl->activeMods = mods;
	LeaveCriticalSection(&m_pImpl->lock);
	MarkGameBriefingDue(); // the UI reports mods once per loaded game
}

bool McpBridge::TakeGameBriefingDue()
{
	bool bDue = false;
	EnterCriticalSection(&m_pImpl->lock);
	Impl::Session* s = m_pImpl->CurrentSession();
	if(s)
	{
		bDue = s->briefingDue;
		s->briefingDue = false;
	}
	LeaveCriticalSection(&m_pImpl->lock);
	return bDue;
}

void McpBridge::MarkGameBriefingDue()
{
	EnterCriticalSection(&m_pImpl->lock);
	for(std::map<std::string, Impl::Session>::iterator it = m_pImpl->sessions.begin(); it != m_pImpl->sessions.end(); ++it)
		it->second.briefingDue = true;
	LeaveCriticalSection(&m_pImpl->lock);
}

// ---------------------------------------------------------------- log and popups

void McpBridge::LogLine(const char* category, const std::string& text)
{
	if(!m_pImpl->inJob) { m_pImpl->Append(-1, "", category, text); return; }
	McpSession s = Current();
	m_pImpl->Append((int)McpPlayerOf(s), McpIdentityLabel(s.identity), category, text);
}

void McpBridge::LogLineFor(int player, const char* category, const std::string& text)
{
	m_pImpl->Append(player, player >= 0 ? McpSeatLabel((PlayerTypes)player) : "", category, text);
}

int McpBridge::GetLog(std::vector<McpLogEntry>& out) { return m_pImpl->GetLog(out); }
int McpBridge::HiddenPlayers() const { return m_pImpl->hiddenPlayers; }

void McpBridge::SendChat(int player, const std::string& text)
{
	EnterCriticalSection(&m_pImpl->lock);
	m_pImpl->pendingChat.push_back(std::make_pair(player, text));
	LeaveCriticalSection(&m_pImpl->lock);
}

int McpBridge::UnreadReplies()
{
	EnterCriticalSection(&m_pImpl->lock);
	int n = m_pImpl->unreadReplies;
	LeaveCriticalSection(&m_pImpl->lock);
	return n;
}

int McpBridge::TotalReplies()
{
	EnterCriticalSection(&m_pImpl->lock);
	int n = m_pImpl->totalReplies;
	LeaveCriticalSection(&m_pImpl->lock);
	return n;
}

void McpBridge::MarkRepliesSeen()
{
	EnterCriticalSection(&m_pImpl->lock);
	m_pImpl->unreadReplies = 0;
	LeaveCriticalSection(&m_pImpl->lock);
}

std::vector<std::string> McpBridge::TakeChat(int player)
{
	std::vector<std::string> out;
	EnterCriticalSection(&m_pImpl->lock);
	std::map<int, std::vector<std::string> >::iterator it = m_pImpl->chat.find(player);
	if(it != m_pImpl->chat.end()) out.swap(it->second);
	LeaveCriticalSection(&m_pImpl->lock);
	return out;
}

void McpBridge::ReplyChat(const std::string& text)
{
	McpSession s = Current();
	m_pImpl->Append((int)McpPlayerOf(s), "", MCP_LOG_CHAT, McpIdentityLabel(s.identity) + ": " + text);
	EnterCriticalSection(&m_pImpl->lock);
	++m_pImpl->unreadReplies;
	++m_pImpl->totalReplies;
	LeaveCriticalSection(&m_pImpl->lock);
}

void McpBridge::NotePopup(int player, int iType, int iData1, int iData2, int iData3, bool bOption1, const std::string& text, bool bOnScreen)
{
	McpPendingPopup p;
	p.player = player; p.type = iType; p.data1 = iData1; p.data2 = iData2; p.data3 = iData3; p.option1 = bOption1; p.text = text; p.onScreen = bOnScreen;
	EnterCriticalSection(&m_pImpl->lock);
	if(m_pImpl->pendingPopups.size() < MCP_MAX_POPUPS) m_pImpl->pendingPopups.push_back(p);
	else ++m_pImpl->pendingDropped;
	LeaveCriticalSection(&m_pImpl->lock);
}

std::string McpBridge::TakeDialogsToClose()
{
	std::string out;
	EnterCriticalSection(&m_pImpl->lock);
	out.swap(m_pImpl->dialogsToClose);
	LeaveCriticalSection(&m_pImpl->lock);
	return out;
}

void McpBridge::SetTestingMode(bool bOn) { m_pImpl->testingMode = bOn; }
bool McpBridge::TestingMode() const { return m_pImpl->testingMode; }

int McpBridge::CountUnread(int player)
{
	int n = 0;
	EnterCriticalSection(&m_pImpl->lock);
	for(size_t i = 0; i < m_pImpl->popups.size(); ++i)
		if(m_pImpl->popups[i].player == player && !m_pImpl->popups[i].read) ++n;
	LeaveCriticalSection(&m_pImpl->lock);
	return n;
}

int McpBridge::Acknowledge(int player, const std::vector<int>* ids)
{
	int n = 0;
	EnterCriticalSection(&m_pImpl->lock);
	for(size_t i = 0; i < m_pImpl->popups.size(); ++i)
	{
		McpPopup& p = m_pImpl->popups[i];
		if(p.read || p.player != player) continue;
		bool bWanted = (ids == NULL);
		for(size_t k = 0; !bWanted && k < ids->size(); ++k) bWanted = ((*ids)[k] == p.id);
		if(!bWanted) continue;
		p.read = true;
		++n;
		if(!p.dialog.empty()) m_pImpl->dialogsToClose += p.dialog + "\n";
	}
	LeaveCriticalSection(&m_pImpl->lock);
	return n;
}

std::vector<McpPopup> McpBridge::GetPopupsSince(int player, int iTurn)
{
	std::vector<McpPopup> out;
	EnterCriticalSection(&m_pImpl->lock);
	for(size_t i = 0; i < m_pImpl->popups.size(); ++i)
		if(m_pImpl->popups[i].player == player && (m_pImpl->popups[i].turn >= iTurn || !m_pImpl->popups[i].read)) out.push_back(m_pImpl->popups[i]);
	LeaveCriticalSection(&m_pImpl->lock);
	return out;
}

// ---------------------------------------------------------------- game thread

McpBridge::Impl::Impl()
	: pServer(NULL), lastStartAttemptMs(0), startFailureShown(false), inJob(false), logSeq(0), lastLogTurn(-1), pendingDropped(0), popupSeq(0), gameSeed(0), gameLoaded(false), unreadReplies(0), totalReplies(0),
	  gameTurn(-1), testingMode(false), shuttingDown(false), screenHumanTurn(false), hiddenPlayers(0), lastTickMs(0)
{
	McpInstallCrashHandler();
	InitializeCriticalSection(&lock);
}

McpBridge::Impl::~Impl()
{
	Shutdown(); // last resort if CivPluginShutdown never ran
	DeleteCriticalSection(&lock);
}

// Static destruction runs under the loader lock while the DLL unloads; the HTTP
// thread can never finish exiting then, the join times out and the thread is
// left running in unmapped code. So the game thread calls this first.
void McpBridge::Impl::Shutdown()
{
	shuttingDown = true;
	McpUninstallCrashHandler();
	if(pServer)
	{
		EnterCriticalSection(&lock);
		for(size_t i = 0; i < jobs.size(); ++i)
		{
			McpJob* pJob = jobs[i];
			pJob->error = "the game is shutting down";
			pJob->done = true;
			SetEvent(pJob->event);
		}
		jobs.clear();
		LeaveCriticalSection(&lock);

		// Left allocated when a thread outlives the join; Bazaar then keeps this DLL loaded.
		if(pServer->Stop(MCP_JOB_TIMEOUT_MS + 5000)) delete pServer;
		pServer = NULL;
	}
}

void McpBridge::Impl::TryStart()
{
	DWORD now = GetTickCount();
	if(lastStartAttemptMs != 0 && now - lastStartAttemptMs < MCP_START_RETRY_MS) return;
	lastStartAttemptMs = now;

	shuttingDown = false;
	pServer = new McpHttpServer();
	if(pServer->Start(MCP_PORT, &Impl::HttpHandler))
	{
		McpDiagLog(McpFormat("MCP bridge listening on http://127.0.0.1:%d/mcp", MCP_PORT));
		return;
	}
	std::string msg = "MCP bridge FAILED to start: " + pServer->lastError();
	McpDiagLog(msg);
	if(!startFailureShown)
	{
		startFailureShown = true;
		Append(-1, "", MCP_LOG_ALWAYS, msg + " (retrying)");
	}
	delete pServer;
	pServer = NULL;
}

// Per session: drops it when its model has been silent for long; a new session in a
// game without seats plays the screen, as the one model always did; a seat whose slot
// is no longer a seat is lost; a seat lost to a game load comes back when it is still waiting
// under the model's own name; then its turn and diplomacy flags.
void McpBridge::Impl::UpdateSessions()
{
	PlayerTypes eActive = GC.getGame().getActivePlayer();
	screenHumanTurn = (eActive != NO_PLAYER) && GET_PLAYER(eActive).isHuman() && McpIsPlayerTurn(GET_PLAYER(eActive));
	bool bAnySeat = McpAnySeat();
	std::vector<PlayerTypes> unbound = McpUnboundSeats();
	int iHidden = 0;
	EnterCriticalSection(&lock);
	DWORD now = GetTickCount();
	for(std::map<std::string, Session>::iterator it = sessions.begin(); it != sessions.end();)
	{
		Session& s = it->second;
		if(now - s.lastSeenMs > MCP_SESSION_EXPIRE_MS)
		{
			sessions.erase(it++);
			continue;
		}
		if(s.info.seat == MCP_SEAT_UNDECIDED && !bAnySeat) s.info.seat = MCP_SEAT_SCREEN;
		if(s.info.seat >= 0 && !McpIsSeat((PlayerTypes)s.info.seat)) s.info.seat = MCP_SEAT_LOST;
		if(s.info.seat == MCP_SEAT_LOST && s.seatBeforeLoad >= 0)
		{
			std::vector<PlayerTypes>::iterator own = std::find(unbound.begin(), unbound.end(), (PlayerTypes)s.seatBeforeLoad);
			if(own != unbound.end() && McpSeatLabelMatches(*own, McpIdentityLabel(s.info.identity)))
			{
				for(std::map<std::string, Session>::iterator other = sessions.begin(); other != sessions.end(); ++other)
				{
					if(other->second.info.seat != *own) continue;
					other->second.info.seat = MCP_SEAT_LOST; // an idle session of the same model
					other->second.seatBeforeLoad = -1;
				}
				s.info.seat = *own;
				unbound.erase(own);
			}
			s.seatBeforeLoad = -1; // one try, right after the load: a later match would be a seat someone claimed since
		}
		if(s.info.seat >= 0 && !s.Idle(now) && !McpOnScreenTeam((PlayerTypes)s.info.seat)) ++iHidden;
		PlayerTypes ePlayer = McpPlayerOf(s.info);
		s.myTurn = ePlayer != NO_PLAYER && McpIsPlayerTurn(GET_PLAYER(ePlayer));
		std::map<int, std::vector<std::string> >::const_iterator c = chat.find((int)ePlayer);
		s.chatWaiting = c != chat.end() && !c->second.empty();
		int iRequests = ePlayer != NO_PLAYER ? McpCountDiploRequestsFor(ePlayer) : 0;
		if(iRequests > s.diploCount) ++s.diploSeq;
		s.diploCount = iRequests;
		++it;
	}
	hiddenPlayers = iHidden;
	LeaveCriticalSection(&lock);
}

void McpBridge::Impl::Tick()
{
	DWORD now = GetTickCount();
	lastTickMs = (now == 0) ? 1 : now;
	if(pServer == NULL) TryStart();
	int iTurn = GC.getGame().getGameTurn();
	if(gameLoaded || CvPreGame::mapRandomSeed() != gameSeed || iTurn < gameTurn) ResetForNewGame();
	gameTurn = iTurn;
	DrainChat();
	UpdateSessions();
	McpKeepSeatTurnVisible();
	McpRemindHumanOfDeals();
	DrainPopups();

	for(;;)
	{
		McpJob* pJob = NULL;
		EnterCriticalSection(&lock);
		if(!jobs.empty())
		{
			pJob = jobs.front();
			jobs.pop_front();
		}
		LeaveCriticalSection(&lock);
		if(pJob == NULL) break;

		currentToken = pJob->session;
		inJob = true;
		bool bRetry = RunJob(pJob);
		inJob = false;
		UpdateSessions();
		if(bRetry)
		{
			// Jobs behind it wait for the retry window; each MCP client sends one call at a time.
			EnterCriticalSection(&lock);
			jobs.push_front(pJob);
			LeaveCriticalSection(&lock);
			break;
		}

		EnterCriticalSection(&lock);
		pJob->done = true;
		bool bAbandoned = pJob->abandoned;
		if(!bAbandoned) SetEvent(pJob->event);
		LeaveCriticalSection(&lock);
		if(bAbandoned) delete pJob;
	}
}

void McpBridge::Impl::DrainPopups()
{
	std::vector<McpPendingPopup> pending;
	EnterCriticalSection(&lock);
	pending.swap(pendingPopups);
	int iLost = pendingDropped;
	pendingDropped = 0;
	LeaveCriticalSection(&lock);
	if(iLost > 0) Append(-1, "", MCP_LOG_MESSAGES, McpFormat("[bridge] %d game message(s) were lost: more than %d arrived before the game thread read them", iLost, MCP_MAX_POPUPS));
	bool bScreenPlayed = McpBridge::Get().ScreenPlayed();
	PlayerTypes eActive = GC.getGame().getActivePlayer();
	for(size_t i = 0; i < pending.size(); ++i)
	{
		const McpPendingPopup& p = pending[i];
		if(p.onScreen && !bScreenPlayed) // the human's own popup
		{
			const char* dialog = McpPopupDialogToClose(p.type);
			if(testingMode && *dialog)
			{
				EnterCriticalSection(&lock);
				dialogsToClose += std::string(dialog) + "\n";
				LeaveCriticalSection(&lock);
				Append(-1, "", MCP_LOG_TURN, std::string("Testing: closed the human's ") + dialog + " unread");
			}
			continue;
		}
		McpPopup popup;
		popup.turn = GC.getGame().getGameTurn();
		popup.player = p.onScreen ? (int)eActive : p.player;
		popup.dialog = p.onScreen ? McpPopupDialogToClose(p.type) : "";
		popup.text = McpTool::StripMarkup(McpDescribePopup((PlayerTypes)popup.player, p.type, p.data1, p.data2, p.data3, p.option1, p.text));
		if(popup.text.empty())
		{
			if(popup.dialog.empty()) continue;
			popup.text = "The game showed a " + popup.dialog + " dialog with no text";
		}
		popup.read = false;
		int iDroppedUnread = 0;
		EnterCriticalSection(&lock);
		popup.id = ++popupSeq;
		popups.push_back(popup);
		int iKept = 0;
		for(size_t k = 0; k < popups.size(); ++k)
			if(popups[k].player == popup.player) ++iKept;
		for(size_t k = 0; iKept > MCP_MAX_POPUPS && k < popups.size();)
		{
			if(popups[k].player != popup.player) { ++k; continue; }
			if(!popups[k].read)
			{
				++iDroppedUnread;
				if(!popups[k].dialog.empty()) dialogsToClose += popups[k].dialog + "\n";
			}
			popups.erase(popups.begin() + k);
			--iKept;
		}
		LeaveCriticalSection(&lock);
		if(iDroppedUnread > 0) McpBridge::Get().LogLineFor(popup.player, MCP_LOG_MESSAGES, McpFormat("[bridge] %d unread game message(s) dropped: more than %d were kept", iDroppedUnread, MCP_MAX_POPUPS));
		McpBridge::Get().LogLineFor(popup.player, MCP_LOG_MESSAGES, McpFormat("Game message #%d: ", popup.id) + popup.text);
	}
}

// The window only offers living teammates, so a message to any other seat means the seat
// changed since the human typed; they are told in the log rather than left wondering.
void McpBridge::Impl::DrainChat()
{
	std::vector<std::pair<int, std::string> > pending;
	EnterCriticalSection(&lock);
	pending.swap(pendingChat);
	LeaveCriticalSection(&lock);
	for(size_t i = 0; i < pending.size(); ++i)
	{
		PlayerTypes e = (PlayerTypes)pending[i].first;
		if(e < 0 || e >= MAX_MAJOR_CIVS || !McpIsSeat(e) || !McpOnScreenTeam(e) || !GET_PLAYER(e).isAlive())
		{
			Append(-1, "", MCP_LOG_CHAT, "Not sent, that seat is no longer a teammate: " + pending[i].second);
			continue;
		}
		Append((int)e, "", MCP_LOG_CHAT, "You: " + pending[i].second);
		McpSession s;
		if(!McpBridge::Get().SessionForSeat((int)e, s)) Append(-1, "", MCP_LOG_CHAT, "(no AI is connected to that seat right now; it reads this when one joins)");
		EnterCriticalSection(&lock);
		std::vector<std::string>& waiting = chat[(int)e];
		if(waiting.size() >= MCP_MAX_CHAT_WAITING) waiting.erase(waiting.begin());
		waiting.push_back(McpFormat("turn %d: ", GC.getGame().getGameTurn()) + pending[i].second);
		LeaveCriticalSection(&lock);
	}
}

// Keeps pendingPopups: the UI may already have reported the first popups of the game just
// started or loaded. Sessions keep their names but lose their seats: the slots belong to that game.
void McpBridge::Impl::ResetForNewGame()
{
	EnterCriticalSection(&lock);
	gameLoaded = false;
	gameSeed = CvPreGame::mapRandomSeed();
	popups.clear();
	popupSeq = 0;
	dialogsToClose.clear();
	chat.clear();
	pendingChat.clear();
	unreadReplies = 0;
	testingMode = false;
	for(std::map<std::string, Session>::iterator it = sessions.begin(); it != sessions.end(); ++it)
	{
		if(it->second.info.seat >= 0)
		{
			it->second.seatBeforeLoad = it->second.info.seat;
			it->second.info.seat = MCP_SEAT_LOST;
		}
		it->second.diploCount = 0;
		it->second.chatWaiting = false;
	}
	LeaveCriticalSection(&lock);
	McpGameId();
	McpResetDiploRequests();
	McpResetBriefing();
}

namespace
{
	bool s_retryRequested = false;
	const DWORD MCP_RETRY_WINDOW_MS = 4000;
}

void McpBridge::RetryLater() { s_retryRequested = true; }

// A call from a session with no civilization yet (take_seat) is logged after it ran, for
// the civilization it plays then: a claim of an opponent civilization is hidden with the
// rest of that civilization's lines. Other calls are logged before they run, so their
// line comes before anything the call causes.
bool McpBridge::Impl::RunJob(McpJob* pJob)
{
	const McpToolDef* pTool = McpFindTool(pJob->tool);
	if(pTool == NULL)
	{
		pJob->error = "unknown tool: " + pJob->tool;
		return false;
	}

	if(pTool->handler == NULL)
	{
		pJob->error = pJob->tool + " runs on the bridge, not the game thread";
		return false;
	}
	bool bFirstTry = !pJob->retrying;
	bool bLogged = pTool->logAs != LOG_NONE;
	const char* szCategory = bLogged ? McpLogCategory(*pTool, pJob->args) : "";
	McpSession before = McpBridge::Get().Current();
	PlayerTypes ePlayerBefore = McpPlayerOf(before);
	if(bLogged && bFirstTry && ePlayerBefore != NO_PLAYER) Append((int)ePlayerBefore, McpIdentityLabel(before.identity), szCategory, McpDescribeCall(*pTool, pJob->args));
	McpCrashSetContext((pJob->tool + " " + picojson::value(pJob->args).serialize()).c_str());
	s_retryRequested = false;
	pJob->error.clear();
	try
	{
		pJob->result = pTool->handler(pJob->args, pJob->error);
	}
	catch(const std::exception& e)
	{
		McpDiagLog(pJob->tool + " threw an exception: " + e.what());
		pJob->error = std::string("the tool stopped on an internal error (") + e.what() + "); it may have done part of its work, so check before trying again";
		s_retryRequested = false;
	}
	catch(...)
	{
		McpDiagLog(pJob->tool + " threw an exception");
		pJob->error = "the tool stopped on an internal error; it may have done part of its work, so check before trying again";
		s_retryRequested = false;
	}
	McpCrashSetContext("");
	bool bRetry = false;
	if(s_retryRequested)
	{
		DWORD now = GetTickCount();
		if(bFirstTry)
		{
			pJob->retrying = true;
			pJob->firstTryMs = now;
		}
		bRetry = now - pJob->firstTryMs < MCP_RETRY_WINDOW_MS;
	}
	if(!bLogged) return bRetry;
	McpSession s = McpBridge::Get().Current();
	int player = (ePlayerBefore != NO_PLAYER) ? (int)ePlayerBefore : (int)McpPlayerOf(s);
	std::string who = McpIdentityLabel(s.identity);
	if(bFirstTry && ePlayerBefore == NO_PLAYER) Append(player, who, szCategory, McpDescribeCall(*pTool, pJob->args));
	if(bRetry) return true;
	std::string outcome = pJob->error.empty() ? McpDescribeResult(*pTool, pJob->result) : "Refused: " + pJob->error;
	if(!outcome.empty()) Append(player, who, szCategory, "    " + outcome);
	return false;
}

void McpBridge::Impl::Append(int player, const std::string& who, const char* category, const std::string& text)
{
	int iTurn = GC.getGame().getGameTurn();
	// With nobody on screen a seat's line is hidden: the rule is absolute, the moment is transient.
	bool bTeammate = (player >= 0) ? (GC.getGame().getActivePlayer() != NO_PLAYER && McpOnScreenTeam((PlayerTypes)player)) : who.empty();
	std::string civ = (player >= 0 && player != GC.getGame().getActivePlayer()) ? GET_PLAYER((PlayerTypes)player).getCivilizationShortDescription() : "";
	EnterCriticalSection(&lock);
	if(iTurn != lastLogTurn)
	{
		McpLogEntry separator = { MCP_LOG_ALWAYS, McpFormat("---------- Turn %d ----------", iTurn), -1, "", "", true };
		log.push_back(separator);
		++logSeq;
		lastLogTurn = iTurn;
	}
	McpLogEntry entry = { category, text, player, who, civ, bTeammate };
	log.push_back(entry);
	++logSeq;
	while(log.size() > MCP_MAX_LOG_ENTRIES) log.pop_front();
	LeaveCriticalSection(&lock);

	FILogFile* pLog = LOGFILEMGR.GetLog("AIThinking.log", FILogFile::kDontTimeStamp);
	if(pLog) pLog->Msg("[%s] %s%s%s", category, who.c_str(), who.empty() ? "" : ": ", text.c_str());
}

int McpBridge::Impl::GetLog(std::vector<McpLogEntry>& out)
{
	EnterCriticalSection(&lock);
	out.assign(log.begin(), log.end());
	int seq = logSeq;
	LeaveCriticalSection(&lock);
	return seq;
}

// ---------------------------------------------------------------- HTTP thread

void McpBridge::Impl::HttpHandler(const McpHttpRequest& req, McpHttpResponse& res)
{
	McpBridge::Get().m_pImpl->HandleHttp(req, res);
}

void McpBridge::Impl::HandleHttp(const McpHttpRequest& req, McpHttpResponse& res)
{
	if(!OriginAllowed(req))
	{
		res.status = 403;
		res.body = "{\"error\":\"origin not allowed\"}";
		return;
	}
	if(req.path != "/mcp")
	{
		res.status = 404;
		res.body = "{\"error\":\"the MCP endpoint is /mcp\"}";
		return;
	}
	if(req.method == "DELETE")
	{
		std::map<std::string, std::string>::const_iterator itSession = req.headers.find("mcp-session-id");
		if(itSession != req.headers.end()) EndSession(itSession->second);
		res.status = 200;
		res.body.clear();
		return;
	}
	if(req.method != "POST")
	{
		res.status = 405;
		res.body = "{\"error\":\"only POST and DELETE are supported\"}";
		return;
	}

	picojson::value request;
	std::string parseError = picojson::parse(request, req.body);
	if(parseError.empty() && !request.is<picojson::object>())
		parseError = "the body must be a single JSON-RPC object";
	if(!parseError.empty())
	{
		res.status = 400;
		res.body = JsonRpcError(picojson::value(), -32700, "parse error: " + parseError).serialize();
		return;
	}

	// The session is whatever Mcp-Session-Id the client sends; initialize starts a fresh
	// one and hands its id back. An id the server no longer knows (the game restarted, or
	// the client ended it) simply starts a session under that id: the model then names itself
	// again. A client that never sends one shares the session with the empty id.
	std::string token;
	std::map<std::string, std::string>::const_iterator itSession = req.headers.find("mcp-session-id");
	if(itSession != req.headers.end()) token = itSession->second;
	const picojson::object& o = request.get<picojson::object>();
	picojson::object::const_iterator itMethod = o.find("method");
	if(itMethod != o.end() && itMethod->second.is<std::string>() && itMethod->second.get<std::string>() == "initialize")
	{
		token = "civ5-" + McpNewUuid();
		res.sessionId = token;
	}
	Touch(token);

	bool bIsNotification = false;
	picojson::value response = HandleRpc(request, token, bIsNotification);
	KeepAlive(token); // a long wait counts as presence
	if(bIsNotification)
	{
		res.status = 202;
		res.body.clear();
		return;
	}
	res.status = 200;
	res.body = response.serialize();
}

picojson::value McpBridge::Impl::HandleRpc(const picojson::value& request, const std::string& token, bool& bIsNotification)
{
	const picojson::object& o = request.get<picojson::object>();
	picojson::object::const_iterator itMethod = o.find("method");
	picojson::object::const_iterator itId = o.find("id");
	picojson::object::const_iterator itParams = o.find("params");
	std::string method = (itMethod != o.end() && itMethod->second.is<std::string>()) ? itMethod->second.get<std::string>() : "";
	picojson::value id = (itId != o.end()) ? itId->second : picojson::value();
	picojson::object params;
	if(itParams != o.end() && itParams->second.is<picojson::object>())
		params = itParams->second.get<picojson::object>();

	if(method.find("notifications/") == 0 || itId == o.end())
	{
		bIsNotification = true;
		return picojson::value();
	}

	if(method == "initialize")
	{
		std::string version = "2025-06-18";
		picojson::object::const_iterator itV = params.find("protocolVersion");
		if(itV != params.end() && itV->second.is<std::string>() && !itV->second.get<std::string>().empty())
			version = itV->second.get<std::string>();
		picojson::object caps;
		caps["tools"] = picojson::value(picojson::object());
		picojson::object info;
		info["name"] = picojson::value("civ5-mcp-plugin");
		info["version"] = picojson::value(MCP_SERVER_VERSION);
		picojson::object result;
		result["protocolVersion"] = picojson::value(version);
		result["capabilities"] = picojson::value(caps);
		result["serverInfo"] = picojson::value(info);
		result["instructions"] = picojson::value(
		    "You are playing Civilization V as the active human player. First, name yourself with set_identity: "
		    "your provider (Claude, ChatGPT, Gemini...), model (Opus, Fable, GPT...) and version (5.5...); actions are refused until you do, "
		    "and you must do it again after every reconnect. Then call wait_for_my_turn until it is your turn, "
		    "use the query tools, act, write your reasoning with log, and finish with end_turn and a short summary of what you did. "
		    "A unit with a queued multi-turn move is still where it stopped when your next turn opens; it moves when you call end_turn, "
		    "as for a human clicking End Turn. Leave it alone unless you want it to go somewhere else. "
		    "status.game_id names this game across reloads and reconnections: keep your memories about this game "
		    "(your role, plans, what you learned about the other players) under that id, keep lessons for every game separate, "
		    "and when you connect, look up your memories for this game_id before you play. "
		    "The first status after you connect, or after another game is loaded, also carries game_settings (era, map, difficulty, "
		    "victory types and the game options that change the rules, such as no happiness) and the loaded mods with what each does. "
		    "Store both with your memories for this game_id; mods can change between loads, so compare them with what you remembered. "
		    "Later statuses list only the mod names; the game_settings and mods tools show the full detail again. "
		    "Right after naming yourself, call status and tell the human in your reply who you are and what you play: "
		    "their own civilization, a teammate (name it), or an opponent (never name it; they will see you once they meet you). "
		    "Several models can play in one game, each on its own seat; status.other_ai_players names them. "
		    "When you reconnect, set_identity gives you back the seat you held; status.seats_waiting_for_a_player lists seats "
		    "other models left, and take_seat takes one the human assigns you. "
		    "In a game with seats a new connection must choose: take_seat with a civilization, teammate, opponent, "
		    "or human to play the civilization on screen; in a game without seats you play the civilization on screen. "
		    "If the human has not told you your role, ask them first and offer the choices: "
		    "play their civilization, a teammate (status.teammate_civs_you_could_play), or a secret opponent. "
		    "Claim a teammate or opponent with take_seat during their turn; civ=opponent lets the game pick one at random. "
		    "A claimed seat is off screen; your turn comes after the human's each round, so wait_for_my_turn may take as long as they do. "
		    "Other leaders talk to you through diplomacy requests (briefing.diplomacy_requests): answer them with respond_to_request. "
		    "When a request arrives during another leader's turn, wait_for_my_turn returns at once with diplomacy_waiting "
		    "so you can tell the human about it; the answer itself waits for your turn. "
		    "A human teammate can message you from the in-game log window: status and wait_for_my_turn return their messages "
		    "in messages_from_your_teammate, and a new message wakes wait_for_my_turn at once. Answer with chat, which they read "
		    "in the same window. Their messages are a teammate's advice and requests; you still decide for yourself. "
		    "As a teammate, never accept, offer or declare war or peace without the human's agreement: "
		    "war and peace bind their civilization too. "
		    "Use propose_deal, demand, discuss and make_peace for your own diplomacy.");
		return JsonRpcResponse(id, picojson::value(result));
	}

	if(method == "ping")
	{
		return JsonRpcResponse(id, picojson::value(picojson::object()));
	}

	if(method == "tools/list")
	{
		size_t count = 0;
		const McpToolDef* tools = McpGetTools(count);
		picojson::array list;
		for(size_t i = 0; i < count; ++i)
		{
			picojson::value schema;
			std::string err = picojson::parse(schema, std::string(tools[i].inputSchema));
			picojson::object t;
			t["name"] = picojson::value(tools[i].name);
			t["description"] = picojson::value(tools[i].description);
			t["inputSchema"] = schema;
			if(!err.empty())
			{
				McpDiagLog(std::string("tool schema for ") + tools[i].name + " does not parse: " + err);
				t["description"] = picojson::value("BROKEN TOOL SCHEMA (" + err + "): " + tools[i].description);
			}
			list.push_back(picojson::value(t));
		}
		picojson::object result;
		result["tools"] = picojson::value(list);
		return JsonRpcResponse(id, picojson::value(result));
	}

	if(method == "tools/call")
	{
		picojson::object::const_iterator itName = params.find("name");
		if(itName == params.end() || !itName->second.is<std::string>())
			return JsonRpcError(id, -32602, "tools/call needs a name");
		picojson::object args;
		picojson::object::const_iterator itArgs = params.find("arguments");
		if(itArgs != params.end() && itArgs->second.is<picojson::object>())
			args = itArgs->second.get<picojson::object>();

		bool bIsError = false;
		picojson::value value = CallTool(token, itName->second.get<std::string>(), args, bIsError);

		// A tool that hands back a picture puts it under image_png_base64; it travels as MCP
		// image content next to the rest of the result.
		picojson::array content;
		picojson::object image;
		if(value.is<picojson::object>() && value.get<picojson::object>().count("image_png_base64"))
		{
			picojson::object o = value.get<picojson::object>();
			image["type"] = picojson::value("image");
			image["data"] = o["image_png_base64"];
			image["mimeType"] = picojson::value("image/png");
			o.erase("image_png_base64");
			value = picojson::value(o);
		}
		picojson::object text;
		text["type"] = picojson::value("text");
		text["text"] = picojson::value(value.is<std::string>() ? value.get<std::string>() : value.serialize());
		content.push_back(picojson::value(text));
		if(!image.empty()) content.push_back(picojson::value(image));
		picojson::object result;
		result["content"] = picojson::value(content);
		result["isError"] = picojson::value(bIsError);
		return JsonRpcResponse(id, picojson::value(result));
	}

	return JsonRpcError(id, -32601, "method not found: " + method);
}

// Whether the session's turn is open, a new diplomacy request reached it, or it has no
// civilization to wait for (status then says what to do).
bool McpBridge::Impl::SessionWakes(const std::string& token, int iDiploSeqAtStart)
{
	bool bMyTurn = false, bChatWaiting = false;
	int iDiploSeq = 0, iSeat = MCP_SEAT_SCREEN;
	if(!Flags(token, bMyTurn, iDiploSeq, bChatWaiting, iSeat)) return true;
	return bMyTurn || iDiploSeq != iDiploSeqAtStart || bChatWaiting || iSeat == MCP_SEAT_LOST || iSeat == MCP_SEAT_UNDECIDED;
}

picojson::value McpBridge::Impl::CallTool(const std::string& token, const std::string& name, const picojson::object& args, bool& bIsError)
{
	const McpToolDef* pTool = McpFindTool(name);
	if(pTool == NULL)
	{
		bIsError = true;
		return picojson::value("unknown tool: " + name);
	}
	std::string argError = McpCheckArgs(*pTool, args);
	if(!argError.empty())
	{
		bIsError = true;
		return picojson::value(argError);
	}

	if(pTool->handler == NULL)
	{
		if(name == MCP_TOOL_ADVANCE_ROUND) return AdvanceRound(token, args, bIsError);
		bIsError = true;
		return picojson::value("no bridge handler for " + name);
	}

	if(pTool->waitsForTurn)
	{
		double timeout = MCP_WAIT_DEFAULT_SECONDS;
		picojson::object::const_iterator it = args.find("timeout_seconds");
		if(it != args.end() && it->second.is<double>()) timeout = it->second.get<double>();
		if(timeout < 1.0) timeout = 1.0;
		if(timeout > MCP_WAIT_MAX_SECONDS) timeout = MCP_WAIT_MAX_SECONDS;

		DWORD start = GetTickCount();
		bool bMyTurn = false, bChatWaiting = false;
		int iSeqAtStart = 0, iSeat = 0;
		Flags(token, bMyTurn, iSeqAtStart, bChatWaiting, iSeat);
		while(!shuttingDown && !(GameRunning() && SessionWakes(token, iSeqAtStart)) && GetTickCount() - start < (DWORD)(timeout * 1000.0))
		{
			Sleep(250);
			KeepAlive(token);
		}
	}

	std::string error;
	picojson::value result = RunOnGameThread(token, name, args, error);
	if(!error.empty())
	{
		bIsError = true;
		return picojson::value(error);
	}
	return result;
}

// Solo testing: one call per round instead of end_turn, wait, skip_human_turn, wait.
picojson::value McpBridge::Impl::AdvanceRound(const std::string& token, const picojson::object& args, bool& bIsError)
{
	if(!testingMode) { bIsError = true; return picojson::value("advance_round is a testing tool: call testing_mode with enabled=true first"); }
	std::string error;
	int iTurnBefore = gameTurn;
	picojson::value r = RunOnGameThread(token, MCP_TOOL_END_TURN, args, error);
	if(!error.empty()) { bIsError = true; return picojson::value(error); }
	DWORD start = GetTickCount();
	const DWORD limit = (DWORD)(MCP_WAIT_MAX_SECONDS * 1000.0);
	bool bSkipped = false;
	bool bReopened = false;
	bool bTurnEnded = false;
	while(!shuttingDown && GetTickCount() - start < limit)
	{
		bool bMyTurn = false, bChatWaiting = false;
		int iDiploSeq = 0, iSeat = 0;
		if(!Flags(token, bMyTurn, iDiploSeq, bChatWaiting, iSeat)) break;
		if(!bTurnEnded)
		{
			// Give the turn a few seconds to close before watching for it to come back.
			if(!bMyTurn || GetTickCount() - start >= 5000) bTurnEnded = true;
		}
		else if(GameRunning() && bMyTurn) { bReopened = true; break; } // the turn came back (a unit arrived with moves left) or the round is done
		else if(!bSkipped && screenHumanTurn && !McpBridge::Get().ScreenPlayed())
		{
			picojson::object none;
			RunOnGameThread(token, MCP_TOOL_SKIP_HUMAN_TURN, none, error);
			if(!error.empty()) { bIsError = true; return picojson::value(error); }
			bSkipped = true;
		}
		Sleep(bTurnEnded ? 250 : 100);
		KeepAlive(token);
	}
	picojson::object none;
	r = RunOnGameThread(token, MCP_TOOL_STATUS, none, error);
	if(!error.empty()) { bIsError = true; return picojson::value(error); }
	if(r.is<picojson::object>())
	{
		picojson::object& o = r.get<picojson::object>();
		o["round_advanced"] = picojson::value(gameTurn > iTurnBefore);
		o["turn_reopened"] = picojson::value(gameTurn == iTurnBefore && bReopened);
	}
	return r;
}

picojson::value McpBridge::Impl::RunOnGameThread(const std::string& token, const std::string& tool, const picojson::object& args, std::string& error)
{
	if(shuttingDown)
	{
		error = "the game is shutting down";
		return picojson::value();
	}
	if(!GameRunning())
	{
		error = "no game is running: the game core has not ticked recently (main menu, loading screen, a modal dialog, or Bazaar turned this plugin off after an error, which Bazaar.log records)";
		return picojson::value();
	}

	McpJob* pJob = new McpJob();
	pJob->session = token;
	pJob->tool = tool;
	pJob->args = args;

	EnterCriticalSection(&lock);
	bool bQueued = !shuttingDown;
	if(bQueued) jobs.push_back(pJob);
	LeaveCriticalSection(&lock);
	if(!bQueued)
	{
		delete pJob;
		error = "the game is shutting down";
		return picojson::value();
	}

	WaitForSingleObject(pJob->event, MCP_JOB_TIMEOUT_MS);

	picojson::value result;
	bool bDeleteHere = false;
	EnterCriticalSection(&lock);
	if(pJob->done)
	{
		result = pJob->result;
		error = pJob->error;
		bDeleteHere = true;
	}
	else
	{
		std::deque<McpJob*>::iterator it = std::find(jobs.begin(), jobs.end(), pJob);
		if(it != jobs.end())
		{
			jobs.erase(it);
			bDeleteHere = true;
		}
		else
		{
			pJob->abandoned = true; // running on the game thread, which will delete it
		}
		error = McpFormat("the game did not respond within %d seconds; it may be busy with a dialog or loading", MCP_JOB_TIMEOUT_MS / 1000);
	}
	LeaveCriticalSection(&lock);

	if(bDeleteHere) delete pJob;
	return result;
}
