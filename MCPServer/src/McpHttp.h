#pragma once

#include <string>
#include <map>
#include <vector>
#include <windows.h>

// Minimal HTTP/1.1 server: an accept thread hands each connection to its own
// thread, so a request that waits (for a turn) never blocks another. Knows
// nothing about the game; every request goes to the handler callback.
struct McpHttpRequest
{
	std::string method;
	std::string path;
	std::map<std::string, std::string> headers; // lower-case names
	std::string body;
};

struct McpHttpResponse
{
	int status;
	std::string contentType;
	std::string sessionId; // sent as Mcp-Session-Id when set
	std::string body;

	McpHttpResponse() : status(200), contentType("application/json") {}
};

typedef void (*McpHttpHandler)(const McpHttpRequest& req, McpHttpResponse& res);

class McpHttpServer
{
public:
	McpHttpServer();
	~McpHttpServer();

	// Binds 127.0.0.1:port exclusively and starts the accept loop. Returns false
	// on failure; lastError() then describes why.
	bool Start(unsigned short port, McpHttpHandler handler);

	// Closes the listening and any in-flight connection, then waits up to
	// joinTimeoutMs for the thread. The handler must already be unblocked.
	void Stop(unsigned long joinTimeoutMs);

	const std::string& lastError() const { return m_lastError; }

private:
	static unsigned long __stdcall ThreadMain(void* arg);
	static unsigned long __stdcall ConnectionMain(void* arg);
	void Run();
	void ServeConnection(unsigned int socket);
	void ForgetFinished();
	void MarkClosed(unsigned int socket);

	struct Connection { unsigned int socket; void* thread; };
	struct ConnectionStart { McpHttpServer* server; unsigned int socket; };

	unsigned int m_listenSocket;
	void* m_thread;
	volatile bool m_stop;
	McpHttpHandler m_handler;
	std::string m_lastError;
	std::vector<Connection> m_connections; // guarded by m_lock
	CRITICAL_SECTION m_lock;
};
