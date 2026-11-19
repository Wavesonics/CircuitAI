/*
 * DecisionTrace.h
 *
 * Records one AI decision (the branch path taken, its inputs and the scored
 * options) and sends it to brain clients as a "decision" message.
 *
 * Usage at a decision site:
 *   CTraceScope trace(circuit, "factory", unit);
 *   if (trace) { trace->Step("economy ready", "yes"); }
 * Nested code adds to the same record without plumbing:
 *   if (CDecisionRecord* tr = CDecisionRecord::Active(circuit)) { ... }
 *
 * Records exist only while a brain client listens, so call sites guard any
 * work done purely for tracing (names, extra math) behind the null check.
 */

#ifndef SRC_CIRCUIT_BRAIN_DECISIONTRACE_H_
#define SRC_CIRCUIT_BRAIN_DECISIONTRACE_H_

#include "brain/JsonOut.h"

#include "AIFloat3.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace circuit {

class CCircuitAI;
class CCircuitUnit;
class CBrainServer;
class IUnitTask;

class CDecisionRecord final {
public:
	struct SOption {
		std::string label;
		int def = -1;
		float score = NAN;
		float x = NAN, z = NAN;
		uint64_t task = 0;
		const char* reject = nullptr;  // why this option was ruled out
		std::vector<std::pair<const char*, float>> parts;  // score components

		SOption& Def(int d) { def = d; return *this; }
		SOption& Score(float s) { score = s; return *this; }
		SOption& Pos(const springai::AIFloat3& p) { x = p.x; z = p.z; return *this; }
		SOption& Task(uint64_t t) { task = t; return *this; }
		SOption& Part(const char* name, float v) { parts.emplace_back(name, v); return *this; }
		SOption& Reject(const char* why) { reject = why; return *this; }
	};

	CDecisionRecord(CCircuitAI* circuit, const char* kind, CCircuitUnit* unit);

	static CDecisionRecord* Active(CCircuitAI* circuit);

	void Step(const char* node, const char* outcome) { path.emplace_back(node, outcome); }
	template<typename T> void Input(const char* key, T v) { inputs.Field(key, v); }
	// Options live in storage reserved up front, so returned references stay valid
	SOption& Option(std::string label);
	// A ruled-out option worth showing (e.g. on the map). Capped separately so
	// rejections never crowd out scored options; past the cap only counts.
	SOption* RejectedOption(std::string label, const char* reason);
	void Reject(const char* reason) { ++rejected[reason]; }
	// Picks the option added last; no-op if it overflowed MAX_OPTIONS
	void ChooseLast() { chosen = lastOverflowed ? -1 : static_cast<int>(options.size()) - 1; }
	void Choose(int index) { chosen = index; }
	void Result(std::string text) { result = std::move(text); }
	// Describes the task a decision produced, e.g. "recruit armpw" or "wait"
	void ResultTask(const IUnitTask* task);
	int OptionCount() const { return static_cast<int>(options.size()); }

	std::string Serialize() const;

private:
	static constexpr size_t MAX_OPTIONS = 64;
	static constexpr int MAX_REJECTED_OPTIONS = 16;

	const char* kind;
	int frame;
	int unitId = -1;
	int unitDef = -1;
	float x = NAN, z = NAN;
	uint64_t resultTask = 0;
	std::vector<std::pair<const char*, const char*>> path;
	CJsonOut inputs;
	std::vector<SOption> options;
	SOption overflow;  // receives options past MAX_OPTIONS
	bool lastOverflowed = false;
	int omitted = 0;
	int rejectedOptions = 0;
	std::map<std::string, int> rejected;
	int chosen = -1;
	std::string result;
};

// Owns the active record for one decision and emits it on scope exit
class CTraceScope final {
public:
	CTraceScope(CCircuitAI* circuit, const char* kind, CCircuitUnit* unit = nullptr);
	~CTraceScope();

	CTraceScope(const CTraceScope&) = delete;
	CTraceScope& operator=(const CTraceScope&) = delete;

	explicit operator bool() const { return record != nullptr; }
	CDecisionRecord* operator->() const { return record.get(); }

private:
	CBrainServer* brain = nullptr;
	std::unique_ptr<CDecisionRecord> record;
};

// Stable id for a task, matching the ids in brain state snapshots
uint64_t BrainTaskId(const void* task);

// Short label for a task kind, e.g. "mex", "recruit" or "fight raid"
std::string BrainTaskLabel(const IUnitTask* task);

} // namespace circuit

#endif // SRC_CIRCUIT_BRAIN_DECISIONTRACE_H_
