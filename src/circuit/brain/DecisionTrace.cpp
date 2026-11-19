/*
 * DecisionTrace.cpp
 */

#include "brain/DecisionTrace.h"
#include "brain/BrainServer.h"
#include "CircuitAI.h"
#include "task/builder/BuilderTask.h"
#include "task/fighter/FighterTask.h"
#include "task/static/RecruitTask.h"
#include "unit/CircuitUnit.h"

namespace circuit {

uint64_t BrainTaskId(const void* task)
{
	// Pointer identity, masked to stay exact in a JS double
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(task)) & ((uint64_t(1) << 53) - 1);
}

CDecisionRecord::CDecisionRecord(CCircuitAI* circuit, const char* kind, CCircuitUnit* unit)
	: kind(kind)
	, frame(circuit->GetLastFrame())
{
	inputs.BeginObj();
	if (unit != nullptr) {
		unitId = unit->GetId();
		unitDef = unit->GetCircuitDef()->GetId();
		const springai::AIFloat3& pos = unit->GetPos(frame);
		x = pos.x;
		z = pos.z;
	}
}

CDecisionRecord* CDecisionRecord::Active(CCircuitAI* circuit)
{
	CBrainServer* brain = circuit->GetBrain();
	return (brain != nullptr) ? brain->GetActiveTrace() : nullptr;
}

CDecisionRecord::SOption& CDecisionRecord::Option(std::string label)
{
	lastOverflowed = options.size() >= MAX_OPTIONS;
	if (lastOverflowed) {
		++omitted;
		overflow = SOption();
		return overflow;
	}
	options.emplace_back();
	options.back().label = std::move(label);
	return options.back();
}

std::string BrainTaskLabel(const IUnitTask* task)
{
	static const char* const typeNames[] = {"none", "player", "idle", "wait", "retreat", "builder", "factory", "fighter"};
	static const char* const fightNames[] = {
		"rally", "guard", "defend", "scout", "raid", "attack", "bomb", "melee", "arty", "aa", "ah", "support", "super"
	};
	if (task == nullptr) {
		return "none";
	}
	const int type = static_cast<int>(task->GetType());
	std::string result = typeNames[(type >= 0 && type < 8) ? type : 0];
	if (auto* bt = dynamic_cast<const IBuilderTask*>(task)) {
		if (dynamic_cast<const CRecruitTask*>(task) != nullptr) {
			result = "recruit";
		}
		for (const auto& kv : IBuilderTask::GetBuildNames()) {
			if (kv.second == bt->GetBuildType()) {
				result = kv.first;
				break;
			}
		}
		if (bt->GetBuildDef() != nullptr) {
			result += ' ';
			result += bt->GetBuildDef()->GetDef()->GetName();
		}
	} else if (auto* ft = dynamic_cast<const IFighterTask*>(task)) {
		const int fight = static_cast<int>(ft->GetFightType());
		result = std::string("fight ") + ((fight >= 0 && fight < 13) ? fightNames[fight] : "?");
	}
	return result;
}

void CDecisionRecord::ResultTask(const IUnitTask* task)
{
	result = BrainTaskLabel(task);
	resultTask = (task != nullptr) ? BrainTaskId(task) : 0;
}

std::string CDecisionRecord::Serialize() const
{
	CJsonOut js;
	js.BeginObj()
		.Field("t", "decision")
		.Field("kind", kind)
		.Field("frame", frame);
	if (unitId >= 0) {
		js.Field("unit", unitId).Field("unitDef", unitDef);
		js.Key("pos").BeginArr().Val(x).Val(z).EndArr();
	}

	js.Key("path").BeginArr();
	for (const auto& step : path) {
		js.BeginArr().Val(step.first).Val(step.second).EndArr();
	}
	js.EndArr();

	CJsonOut in = inputs;
	in.EndObj();
	js.Key("inputs").Raw(in.Str());

	js.Key("options").BeginArr();
	for (const SOption& o : options) {
		js.BeginObj().Field("label", o.label);
		if (o.def >= 0) {
			js.Field("def", o.def);
		}
		if (!std::isnan(o.score)) {
			js.Field("score", o.score);
		}
		if (!std::isnan(o.x)) {
			js.Key("pos").BeginArr().Val(o.x).Val(o.z).EndArr();
		}
		if (o.task != 0) {
			js.Field("task", o.task);
		}
		if (o.reject != nullptr) {
			js.Field("reject", o.reject);
		}
		if (!o.parts.empty()) {
			js.Key("parts").BeginObj();
			for (const auto& part : o.parts) {
				js.Field(part.first, part.second);
			}
			js.EndObj();
		}
		js.EndObj();
	}
	js.EndArr();
	if (omitted > 0) {
		js.Field("omitted", omitted);
	}

	js.Key("rejected").BeginObj();
	for (const auto& kv : rejected) {
		js.Field(kv.first.c_str(), kv.second);
	}
	js.EndObj();

	js.Field("chosen", chosen);
	if (!result.empty()) {
		js.Field("result", result);
	}
	if (resultTask != 0) {
		js.Field("resultTask", resultTask);
	}
	js.EndObj();
	return js.Take();
}

CTraceScope::CTraceScope(CCircuitAI* circuit, const char* kind, CCircuitUnit* unit)
{
	CBrainServer* server = circuit->GetBrain();
	if ((server == nullptr) || (server->GetActiveTrace() != nullptr)
		|| !server->TryStartTrace(circuit->GetLastFrame()))
	{
		return;
	}
	brain = server;
	record = std::make_unique<CDecisionRecord>(circuit, kind, unit);
	brain->SetActiveTrace(record.get());
}

CTraceScope::~CTraceScope()
{
	if (record != nullptr) {
		brain->EndTrace(record.get());
	}
}

} // namespace circuit
