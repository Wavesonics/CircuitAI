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

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Json {
	class Value;
}

namespace circuit {

class CCircuitAI;
class CJsonOut;
class CDecisionRecord;

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

	// Decision tracing. At most one record is active at a time; nested
	// decisions fold into the outer one.
	CDecisionRecord* GetActiveTrace() const { return activeTrace; }
	// Spends this kind's rate budget; false when nobody listens or it is used up
	bool TryStartTrace(int frame, const char* kind);
	// A decision made while another was being traced; folded into the outer one
	void NoteSuppressedTrace() { ++droppedTraces; }
	void SetActiveTrace(CDecisionRecord* record) { activeTrace = record; }
	void EndTrace(const CDecisionRecord* record);

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

	// BrainControl.cpp: commands that change the AI's behaviour
	struct STunable {
		std::function<float()> get;
		std::function<void(float)> set;
		float min, max;
	};
	void InitTunables();
	void HandleCommand(SClient& client, const Json::Value& msg);
	std::string CmdSet(const Json::Value& msg);
	std::string CmdReset(const Json::Value& msg);
	std::string CmdLimit(const Json::Value& msg);
	std::string CmdBuild(const Json::Value& msg, CJsonOut& result);
	std::string CmdRecruit(const Json::Value& msg, CJsonOut& result);
	std::string CmdUnits(const Json::Value& msg, CJsonOut& result);
	std::string CmdFocus(const Json::Value& msg);
	void ReleaseAll();
	void ExpireControl(int frame);
	void WriteControl(CJsonOut& js) const;

	// BrainSnapshot.cpp
	std::string MakeHello() const;
	const std::string& GetMeta();
	std::string MakeState(int frame);
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

	std::map<std::string, STunable> tunables;
	std::map<std::string, float> tunedFrom;  // tunable key: value before the first change
	std::map<int, int> limitedFrom;  // def id: unit limit before the first change
	std::map<int, int> controlled;  // unit id: frame it returns to the AI
	int focusUntil = -1;  // frame the attack focus expires

	int stateInterval;  // frames between state snapshots
	int lastStateFrame;

	bool isTracing = true;
	CDecisionRecord* activeTrace = nullptr;
	// Decisions spent per kind in the current one-second window. Per kind so
	// frequent builder decisions cannot starve rare ones like retreats.
	std::map<std::string, int> traceSpent;
	int traceWindowFrame = -1;
	int droppedTraces = 0;  // since the last state, reported there
};

} // namespace circuit

#endif // SRC_CIRCUIT_BRAIN_BRAINSERVER_H_
