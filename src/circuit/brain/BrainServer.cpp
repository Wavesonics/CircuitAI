/*
 * BrainServer.cpp
 *
 * Transport: listen socket, client buffers and the inbound command channel.
 */

#include "brain/BrainServer.h"
#include "brain/DecisionTrace.h"
#include "CircuitAI.h"
#include "util/Utils.h"

#include "Log.h"

#include "json/json.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace circuit {

namespace {
constexpr int DEFAULT_PORT = 7710;
constexpr int DEFAULT_STATE_INTERVAL = FRAMES_PER_SEC;
constexpr int MIN_STATE_INTERVAL = 1;
constexpr int MAX_STATE_INTERVAL = FRAMES_PER_SEC * 60;
constexpr size_t MAX_CLIENTS = 8;
constexpr int TRACE_BUDGET_PER_KIND = 40;  // decisions per kind per second of game time
constexpr size_t MAX_LINE = 64 * 1024;
// Inbound bytes drained per client per frame, so a chatty client cannot stall the sim
constexpr size_t MAX_READ_PER_UPDATE = 256 * 1024;
// A client this far behind skips state snapshots instead of growing memory
constexpr size_t STATE_SKIP_BACKLOG = 8 * 1024 * 1024;
// A client this far behind is assumed stuck and is dropped
constexpr size_t MAX_BACKLOG = 64 * 1024 * 1024;

bool IsEnabled()
{
	const char* env = std::getenv("BARB_BRAIN");
	if (env == nullptr) {
		return false;
	}
	std::string v(env);
	std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
	return (v == "1") || (v == "true") || (v == "yes") || (v == "on");
}

#ifndef _WIN32
#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;  // macOS: SIGPIPE is suppressed per socket via SO_NOSIGPIPE
#endif

// Portable stand-in for SOCK_NONBLOCK | SOCK_CLOEXEC, which macOS lacks
bool PrepareSocket(int fd)
{
	const int flags = fcntl(fd, F_GETFL, 0);
	if ((flags < 0) || (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)) {
		return false;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
	const int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	return true;
}
#endif

int BasePort()
{
	const char* env = std::getenv("BARB_BRAIN_PORT");
	if (env != nullptr) {
		const int p = std::atoi(env);
		if ((p > 0) && (p < 65536)) {
			return p;
		}
	}
	return DEFAULT_PORT;
}
}  // namespace

std::unique_ptr<CBrainServer> CBrainServer::Create(CCircuitAI* circuit)
{
	if (!IsEnabled()) {
		return nullptr;
	}
#ifdef _WIN32
	circuit->LOG("BrainServer: not supported on Windows");
	return nullptr;
#else
	const int port = BasePort() + circuit->GetSkirmishAIId();
	if (port >= 65536) {
		circuit->LOG("BrainServer: port %i out of range", port);
		return nullptr;
	}
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if ((fd < 0) || !PrepareSocket(fd)) {
		circuit->LOG("BrainServer: socket() failed: %s", std::strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return nullptr;
	}
	const int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	if ((bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) || (listen(fd, 4) < 0)) {
		circuit->LOG("BrainServer: cannot listen on 127.0.0.1:%i: %s", port, std::strerror(errno));
		close(fd);
		return nullptr;
	}
	circuit->LOG("BrainServer: listening on 127.0.0.1:%i", port);
	return std::unique_ptr<CBrainServer>(new CBrainServer(circuit, fd, port));
#endif
}

CBrainServer::CBrainServer(CCircuitAI* circuit, int listenFd, int port)
	: circuit(circuit)
	, listenFd(listenFd)
	, port(port)
	, stateInterval(DEFAULT_STATE_INTERVAL)
	, lastStateFrame(-1)
{
}

CBrainServer::~CBrainServer()
{
#ifndef _WIN32
	for (SClient& c : clients) {
		close(c.fd);
	}
	if (listenFd >= 0) {
		close(listenFd);
	}
#endif
}

void CBrainServer::Update(int frame)
{
	Accept();
	for (SClient& c : clients) {
		Read(c);
	}

	SendStates(frame);

	for (SClient& c : clients) {
		Flush(c);
	}
	for (auto it = clients.begin(); it != clients.end();) {
		if (it->isDead) {
			Close(*it);
			it = clients.erase(it);
		} else {
			++it;
		}
	}
}

bool CBrainServer::TryStartTrace(int frame, const char* kind)
{
	if (!isTracing || clients.empty()) {
		return false;
	}
	if ((traceWindowFrame < 0) || (frame - traceWindowFrame >= FRAMES_PER_SEC)) {
		traceWindowFrame = frame;
		traceSpent.clear();
	}
	int& spent = traceSpent[kind];
	if (spent >= TRACE_BUDGET_PER_KIND) {
		++droppedTraces;
		return false;
	}
	++spent;
	return true;
}

void CBrainServer::EndTrace(const CDecisionRecord* record)
{
	activeTrace = nullptr;
	const std::string line = record->Serialize();
	for (SClient& c : clients) {
		if (c.Backlog() < STATE_SKIP_BACKLOG) {
			Send(c, line);
		}
	}
}

void CBrainServer::SendStates(int frame)
{
	if ((lastStateFrame < 0) || (frame - lastStateFrame >= stateInterval)) {
		lastStateFrame = frame;
		for (SClient& c : clients) {
			c.needsState = true;
		}
	}
	// Snapshots are expensive; build one only if some client is due and can take it
	auto isReceiver = [](const SClient& c) {
		return c.needsState && !c.isDead && (c.Backlog() < STATE_SKIP_BACKLOG);
	};
	if (std::none_of(clients.begin(), clients.end(), isReceiver)) {
		return;
	}
	const std::string state = MakeState(frame);
	for (SClient& c : clients) {
		if (isReceiver(c)) {
			Send(c, state);
			c.needsState = false;
		}
	}
}

void CBrainServer::Accept()
{
#ifndef _WIN32
	while (true) {
		const int fd = accept(listenFd, nullptr, nullptr);
		if (fd < 0) {
			return;  // EAGAIN or a transient error; retry next frame
		}
		if ((clients.size() >= MAX_CLIENTS) || !PrepareSocket(fd)) {
			close(fd);
			continue;
		}
		const int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		clients.push_back(SClient{fd});
		SClient& c = clients.back();
		Send(c, MakeHello());
		Send(c, GetMeta());
	}
#endif
}

void CBrainServer::Read(SClient& client)
{
#ifndef _WIN32
	char tmp[4096];
	size_t total = 0;
	while (!client.isDead && (total < MAX_READ_PER_UPDATE) && (client.in.size() <= MAX_LINE)) {
		const ssize_t n = recv(client.fd, tmp, sizeof(tmp), 0);
		if (n > 0) {
			client.in.append(tmp, n);
			total += n;
		} else if ((n == 0) || ((errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != EINTR))) {
			client.isDead = true;
		} else {
			break;
		}
	}
	size_t start = 0;
	size_t nl;
	while (!client.isDead && ((nl = client.in.find('\n', start)) != std::string::npos)) {
		HandleLine(client, client.in.substr(start, nl - start));
		start = nl + 1;
	}
	client.in.erase(0, start);
	if (client.in.size() > MAX_LINE) {  // no newline within a sane line length
		client.isDead = true;
	}
#endif
}

void CBrainServer::Flush(SClient& client)
{
#ifndef _WIN32
	while (!client.isDead && (client.Backlog() > 0)) {
		const ssize_t n = send(client.fd, client.out.data() + client.outPos, client.Backlog(), SEND_FLAGS);
		if (n > 0) {
			client.outPos += n;
		} else if ((n < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR))) {
			break;
		} else {
			client.isDead = true;
		}
	}
	if (client.outPos == client.out.size()) {
		client.out.clear();
		client.outPos = 0;
	} else if (client.outPos > client.out.size() / 2) {
		client.out.erase(0, client.outPos);
		client.outPos = 0;
	}
#endif
}

void CBrainServer::Send(SClient& client, const std::string& line)
{
	if (client.isDead) {
		return;
	}
	if (client.Backlog() + line.size() > MAX_BACKLOG) {
		client.isDead = true;
		return;
	}
	client.out.append(line);
	client.out += '\n';
}

void CBrainServer::HandleLine(SClient& client, const std::string& line)
{
	if (line.empty()) {
		return;
	}
	std::unique_ptr<Json::CharReader> reader(Json::CharReaderBuilder().newCharReader());
	Json::Value msg;
	JSONCPP_STRING errs;
	if (!reader->parse(line.data(), line.data() + line.size(), &msg, &errs) || !msg.isObject()) {
		Send(client, R"({"t":"error","error":"malformed json"})");
		return;
	}
	const std::string type = msg.get("t", "").asString();
	if (type == "cfg") {
		const Json::Value& every = msg["stateEvery"];
		if (every.isInt()) {
			stateInterval = std::max(MIN_STATE_INTERVAL, std::min(every.asInt(), MAX_STATE_INTERVAL));
		}
		const Json::Value& decisions = msg["decisions"];
		if (decisions.isBool()) {
			isTracing = decisions.asBool();
		}
	} else if (type == "meta") {
		Send(client, GetMeta());
	} else if (type == "cmd") {
		HandleCommand(client, msg);
	} else {
		Send(client, R"({"t":"error","error":"unknown message type"})");
	}
}

void CBrainServer::Close(SClient& client)
{
#ifndef _WIN32
	close(client.fd);
#endif
}

} // namespace circuit
