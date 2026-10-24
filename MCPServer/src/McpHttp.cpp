#include "CvGameCoreDLLPCH.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "McpHttp.h"

namespace
{
	const unsigned int NO_SOCKET = (unsigned int)INVALID_SOCKET;

	std::string ToLower(const std::string& s)
	{
		std::string r(s);
		for(size_t i = 0; i < r.size(); ++i)
			r[i] = (char)tolower((unsigned char)r[i]);
		return r;
	}

	std::string Trim(const std::string& s)
	{
		size_t a = s.find_first_not_of(" \t\r\n");
		if(a == std::string::npos) return "";
		size_t b = s.find_last_not_of(" \t\r\n");
		return s.substr(a, b - a + 1);
	}

	bool SendAll(SOCKET s, const std::string& data)
	{
		size_t sent = 0;
		while(sent < data.size())
		{
			int n = send(s, data.data() + sent, (int)(data.size() - sent), 0);
			if(n <= 0) return false;
			sent += n;
		}
		return true;
	}

	const char* ReasonPhrase(int status)
	{
		switch(status)
		{
		case 200: return "OK";
		case 202: return "Accepted";
		case 400: return "Bad Request";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 500: return "Internal Server Error";
		default:  return "Unknown";
		}
	}

	void WriteResponse(SOCKET s, const McpHttpResponse& res)
	{
		char head[512];
		_snprintf_s(head, sizeof(head), _TRUNCATE,
		            "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n",
		            res.status, ReasonPhrase(res.status), res.contentType.c_str(), (unsigned)res.body.size());
		std::string out(head);
		if(!res.sessionId.empty()) out += "Mcp-Session-Id: " + res.sessionId + "\r\n";
		out += "\r\n";
		out += res.body;
		SendAll(s, out);
	}

	bool ReadRequest(SOCKET s, McpHttpRequest& req)
	{
		std::string buf;
		char chunk[4096];
		size_t headerEnd = std::string::npos;
		while(headerEnd == std::string::npos)
		{
			int n = recv(s, chunk, sizeof(chunk), 0);
			if(n <= 0) return false;
			buf.append(chunk, n);
			headerEnd = buf.find("\r\n\r\n");
			if(buf.size() > 1024 * 1024) return false;
		}

		std::string headerBlock = buf.substr(0, headerEnd);
		std::string rest = buf.substr(headerEnd + 4);

		size_t lineEnd = headerBlock.find("\r\n");
		std::string requestLine = headerBlock.substr(0, lineEnd);
		size_t sp1 = requestLine.find(' ');
		size_t sp2 = requestLine.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
		if(sp1 == std::string::npos || sp2 == std::string::npos) return false;
		req.method = requestLine.substr(0, sp1);
		req.path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

		size_t pos = (lineEnd == std::string::npos) ? headerBlock.size() : lineEnd + 2;
		while(pos < headerBlock.size())
		{
			size_t next = headerBlock.find("\r\n", pos);
			if(next == std::string::npos) next = headerBlock.size();
			std::string line = headerBlock.substr(pos, next - pos);
			size_t colon = line.find(':');
			if(colon != std::string::npos)
				req.headers[ToLower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
			pos = next + 2;
		}

		size_t contentLength = 0;
		std::map<std::string, std::string>::const_iterator it = req.headers.find("content-length");
		if(it != req.headers.end())
			contentLength = (size_t)atoi(it->second.c_str());
		if(contentLength > 16 * 1024 * 1024) return false;

		while(rest.size() < contentLength)
		{
			int n = recv(s, chunk, sizeof(chunk), 0);
			if(n <= 0) return false;
			rest.append(chunk, n);
		}
		req.body = rest.substr(0, contentLength);
		return true;
	}
}

McpHttpServer::McpHttpServer()
	: m_listenSocket(NO_SOCKET), m_thread(NULL), m_stop(false), m_handler(NULL)
{
	InitializeCriticalSection(&m_lock);
}

McpHttpServer::~McpHttpServer()
{
	Stop(2000);
	DeleteCriticalSection(&m_lock);
}

bool McpHttpServer::Start(unsigned short port, McpHttpHandler handler)
{
	m_handler = handler;

	WSADATA wsa;
	if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		m_lastError = "WSAStartup failed";
		return false;
	}

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(s == INVALID_SOCKET)
	{
		m_lastError = "socket() failed";
		WSACleanup();
		return false;
	}

	int exclusive = 1;
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if(bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
	{
		char msg[128];
		_snprintf_s(msg, sizeof(msg), _TRUNCATE, "bind() to 127.0.0.1:%u failed (error %d); is another game instance running?", (unsigned)port, WSAGetLastError());
		m_lastError = msg;
		closesocket(s);
		WSACleanup();
		return false;
	}
	if(listen(s, 4) == SOCKET_ERROR)
	{
		m_lastError = "listen() failed";
		closesocket(s);
		WSACleanup();
		return false;
	}

	m_listenSocket = (unsigned int)s;
	m_stop = false;
	m_thread = CreateThread(NULL, 0, ThreadMain, this, 0, NULL);
	if(m_thread == NULL)
	{
		m_lastError = "CreateThread failed";
		closesocket(s);
		m_listenSocket = NO_SOCKET;
		WSACleanup();
		return false;
	}
	return true;
}

bool McpHttpServer::Stop(unsigned long joinTimeoutMs)
{
	if(m_thread == NULL) return true;
	m_stop = true;
	if(m_listenSocket != NO_SOCKET)
	{
		closesocket((SOCKET)m_listenSocket);
		m_listenSocket = NO_SOCKET;
	}
	bool bAllStopped = WaitForSingleObject((HANDLE)m_thread, joinTimeoutMs) == WAIT_OBJECT_0;
	CloseHandle((HANDLE)m_thread);
	m_thread = NULL;

	EnterCriticalSection(&m_lock);
	std::vector<Connection> open = m_connections;
	m_connections.clear();
	LeaveCriticalSection(&m_lock);
	for(size_t i = 0; i < open.size(); ++i)
		if(open[i].socket != NO_SOCKET) shutdown((SOCKET)open[i].socket, SD_BOTH);
	for(size_t i = 0; i < open.size(); ++i)
	{
		if(WaitForSingleObject((HANDLE)open[i].thread, joinTimeoutMs) != WAIT_OBJECT_0) bAllStopped = false;
		CloseHandle((HANDLE)open[i].thread);
	}
	WSACleanup();
	return bAllStopped;
}

unsigned long __stdcall McpHttpServer::ConnectionMain(void* arg)
{
	ConnectionStart* pStart = (ConnectionStart*)arg;
	McpHttpServer* pServer = pStart->server;
	unsigned int socket = pStart->socket;
	delete pStart;
	pServer->ServeConnection(socket);
	pServer->MarkClosed(socket);
	closesocket((SOCKET)socket);
	return 0;
}

void McpHttpServer::MarkClosed(unsigned int socket)
{
	EnterCriticalSection(&m_lock);
	for(size_t i = 0; i < m_connections.size(); ++i)
		if(m_connections[i].socket == socket) m_connections[i].socket = NO_SOCKET;
	LeaveCriticalSection(&m_lock);
}

void McpHttpServer::ForgetFinished()
{
	EnterCriticalSection(&m_lock);
	for(size_t i = 0; i < m_connections.size();)
	{
		if(WaitForSingleObject((HANDLE)m_connections[i].thread, 0) == WAIT_OBJECT_0)
		{
			CloseHandle((HANDLE)m_connections[i].thread);
			m_connections.erase(m_connections.begin() + i);
		}
		else ++i;
	}
	LeaveCriticalSection(&m_lock);
}

unsigned long __stdcall McpHttpServer::ThreadMain(void* arg)
{
	((McpHttpServer*)arg)->Run();
	return 0;
}

void McpHttpServer::Run()
{
	while(!m_stop)
	{
		SOCKET c = accept((SOCKET)m_listenSocket, NULL, NULL);
		if(c == INVALID_SOCKET)
		{
			if(m_stop) break;
			Sleep(50);
			continue;
		}
		ForgetFinished();
		ConnectionStart* pStart = new ConnectionStart();
		pStart->server = this;
		pStart->socket = (unsigned int)c;
		HANDLE hThread = CreateThread(NULL, 0, ConnectionMain, pStart, CREATE_SUSPENDED, NULL);
		if(hThread == NULL)
		{
			delete pStart;
			closesocket(c);
			continue;
		}
		Connection conn;
		conn.socket = (unsigned int)c;
		conn.thread = hThread;
		EnterCriticalSection(&m_lock);
		m_connections.push_back(conn);
		LeaveCriticalSection(&m_lock);
		ResumeThread(hThread);
	}
}

void McpHttpServer::ServeConnection(unsigned int socket)
{
	SOCKET s = (SOCKET)socket;
	int timeoutMs = 10000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

	McpHttpRequest req;
	McpHttpResponse res;
	if(!ReadRequest(s, req))
	{
		if(m_stop) return;
		res.status = 400;
		res.body = "{\"error\":\"malformed or incomplete request\"}";
		WriteResponse(s, res);
		return;
	}

	try
	{
		m_handler(req, res);
	}
	catch(...)
	{
		res.status = 500;
		res.contentType = "application/json";
		res.body = "{\"error\":\"unhandled exception in request handler\"}";
	}
	WriteResponse(s, res);
}
