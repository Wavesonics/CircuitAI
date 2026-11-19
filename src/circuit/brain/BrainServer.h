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

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Json {
	class Value;
}

namespace circuit {

class CCircuitAI;
class CCircuitUnit;
class CEnemyInfo;
class CJsonOut;
class CDecisionRecord;

class CBrainServer final {
public:
	static constexpr int PROTOCOL_VERSION = 2;

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

	// BrainEvents.cpp and BrainAlerts.cpp: combat events (nukes, large blasts,
	// loss bursts, key unit deaths, units under attack). Tracked whether or not
	// a client listens, so a late client gets recent ones.
	void OnUnitDamaged(CCircuitUnit* unit, int attackerId, int weaponDefId, bool paralyzer);
	void OnUnitDestroyed(CCircuitUnit* unit, int attackerId, int weaponDefId);
	void OnEnemyDamaged(CEnemyInfo* enemy, int attackerId, int weaponDefId);
	void OnEnemyDestroyed(CEnemyInfo* enemy, int attackerId);

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
	void WriteSilos(CJsonOut& js, int frame);

	// BrainEvents.cpp
	enum WeaponClass: uint8_t {WC_NONE = 0, WC_LARGE, WC_NUKE, WC_ANTI};
	enum DefFlags: uint8_t {DF_NUKE = 1, DF_ANTI = 2};
	enum Side: uint8_t {SIDE_UNKNOWN = 0, SIDE_OURS, SIDE_ALLY, SIDE_ENEMY};
	struct SBlast {
		int weaponDefId;
		WeaponClass cls;
		bool paralyzer;
		int firstFrame, lastFrame;
		float sumX = 0.f, sumZ = 0.f;  // over hit positions, ours and enemy
		int points = 0;
		Side side;
		bool attackerSeen;
		int attackerDef;  // -1 unless the attacker was seen
		std::set<int> oursHit, enemyHit;
		int oursKilled = 0;
		float oursKilledCost = 0.f;
	};
	struct SLoss {
		int frame;
		float cost;
	};
	struct SLossBurst {
		int firstFrame, lastFrame;
		float sumX = 0.f, sumZ = 0.f;
		std::vector<SLoss> losses;
		std::map<int, int> byDef;  // our unit def: count
		std::map<int, int> byWeapon;  // weapon def (-1 unknown): count
		std::map<int, int> byAttacker;  // attacker def (-1 unseen): count
	};
	void InitWeaponClasses();
	WeaponClass ClassOf(int weaponDefId);
	uint8_t DefFlagsOf(int defId);
	Side SideOf(int attackerId, int* attackerDef) const;
	SBlast* FindBlast(int weaponDefId, float x, float z, int frame);
	void UpdateEvents(int frame);
	void PollSilos(int frame);
	void CloseBlast(const SBlast& blast);
	void CloseLossBurst(const SLossBurst& burst);
	// keepAllGame: replayed to new clients for the whole game, not just while recent
	void Emit(std::string&& line, bool keepAllGame = false);

	// BrainAlerts.cpp
	enum KeyCategory: uint8_t {KC_NONE = 0, KC_COMMANDER, KC_EXPERIMENTAL, KC_FACTORY, KC_SUPER, KC_DEFENCE,
		KC_ECONOMY, KC_STRUCTURE};
	uint8_t CategoryOf(int defId);
	struct SDeath {
		const char* side;
		int unitId, def, team, allyTeam;
		float x, z;
		int attackerId, weaponDefId;
		int commandersLeft;  // on the dead commander's team, -1 when unknown or not a commander
	};
	void EmitKeyDeath(int frame, const SDeath& d);
	void PollAllyKeyUnits(int frame);
	void NoteAttack(CCircuitUnit* unit, int attackerId, int weaponDefId);
	void FlushAttacks(int frame);
	struct SAttackCell {
		int firstFrame = -1;  // first hit not yet reported, -1 when none
		int lastEmit = -1;
		float sumX = 0.f, sumZ = 0.f;
		std::map<int, int> damaged;  // our unit id: def
		std::set<int> offensive;  // damaged units in attack, raid, bomb or scout squads
		std::map<int, int> attackers;  // seen attacker unit id: def (-1 if unidentified)
		int unseenHits = 0;
	};

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

	std::vector<WeaponClass> weaponClasses;  // by weapon def id, filled on first use
	std::vector<int> weaponSources;  // by weapon def id: a unit def mounting it, or -1
	std::vector<uint8_t> defFlags;  // DefFlags by unit def id
	std::vector<uint8_t> defCategories;  // KeyCategory by unit def id
	std::vector<SBlast> blasts;  // still collecting hits
	std::vector<SLossBurst> lossBursts;  // still collecting losses
	struct SLaunch {
		int frame;
		float x, z;  // target, or the launcher when the target is unknown
		bool hasTarget;
	};
	std::deque<SLaunch> launches;  // our recent nuke launches, to claim hidden-attacker impacts
	std::map<int, int> stockpiles;  // nuke and antinuke unit id: stockpile at the last poll
	struct SAllyKeyUnit {
		int def, team;
		float x, z;
	};
	std::map<int, SAllyKeyUnit> allyKeyUnits;  // by unit id, allied teams other than ours
	std::set<int> commanderlessTeams;  // teams already reported eliminated
	std::map<int, SAttackCell> attackCells;  // by sector index
	std::map<int, int> keyAttackEmit;  // key unit id: frame of its last keyUnderAttack
	std::deque<std::string> recentEvents;  // replayed to new clients
	std::vector<std::string> keyEvents;  // commander deaths and eliminations, all game, also replayed
	int eventSeq = 0;
};

} // namespace circuit

#endif // SRC_CIRCUIT_BRAIN_BRAINSERVER_H_
