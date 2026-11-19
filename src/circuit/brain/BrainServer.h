/*
 * BrainServer.h
 *
 * Streams the AI's world model and decisions to local clients over TCP as
 * newline-delimited JSON. Enabled by BARB_BRAIN=1 (or true/yes/on);
 * listens on 127.0.0.1:(BARB_BRAIN_PORT or 7710) + skirmishAIId.
 *
 * Everything runs on the AI's main thread: sockets are non-blocking and polled
 * from CCircuitAI::Update, so engine callbacks are never touched elsewhere.
 */

#ifndef SRC_CIRCUIT_BRAIN_BRAINSERVER_H_
#define SRC_CIRCUIT_BRAIN_BRAINSERVER_H_

#include <memory>
#include <string>
#include <vector>

namespace circuit {

class CCircuitAI;
class CJsonOut;

class CBrainServer final {
public:
	static constexpr int PROTOCOL_VERSION = 1;

	// nullptr unless enabled and the listen socket could be opened
	static std::unique_ptr<CBrainServer> Create(CCircuitAI* circuit);
	~CBrainServer();

	CBrainServer(const CBrainServer&) = delete;
	CBrainServer& operator=(const CBrainServer&) = delete;

	void Update(int frame);

	int GetPort() const { return port; }

private:
	CBrainServer(CCircuitAI* circuit, int listenFd, int port);

	struct SClient {
		int fd;
		std::string in;
		std::string out;
		size_t outPos = 0;
		bool isDead = false;
		bool needsState = true;
		size_t Backlog() const { return out.size() - outPos; }
	};

	void Accept();
	void SendStates(int frame);
	void Read(SClient& client);
	void Flush(SClient& client);
	void Send(SClient& client, const std::string& line);
	void HandleLine(SClient& client, const std::string& line);
	void Close(SClient& client);

	// BrainSnapshot.cpp
	std::string MakeHello() const;
	const std::string& GetMeta();
	std::string MakeState(int frame) const;
	void WriteUnits(CJsonOut& js, int frame) const;
	void WriteTasks(CJsonOut& js) const;
	void WriteEnemies(CJsonOut& js) const;
	void WriteEconomy(CJsonOut& js) const;
	void WriteLayers(CJsonOut& js) const;

	CCircuitAI* circuit;
	int listenFd;
	int port;
	std::vector<SClient> clients;
	std::string meta;  // static map/def data, built once on first connect

	int stateInterval;  // frames between state snapshots
	int lastStateFrame;
};

} // namespace circuit

#endif // SRC_CIRCUIT_BRAIN_BRAINSERVER_H_
