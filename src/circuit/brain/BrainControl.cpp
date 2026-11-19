/*
 * BrainControl.cpp
 *
 * Commands from brain clients that change what the AI does: tuning values,
 * unit limits, build and recruit orders, direct unit orders and an attack
 * focus. Runs on the main thread from CBrainServer::Update, like the rest of
 * the AI, so it can call into the managers directly.
 */

#include "brain/BrainServer.h"
#include "brain/JsonOut.h"
#include "CircuitAI.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "module/FactoryManager.h"
#include "module/MilitaryManager.h"
#include "resource/MetalManager.h"
#include "task/RetreatTask.h"
#include "task/UnitTask.h"
#include "task/static/RecruitTask.h"
#include "terrain/TerrainManager.h"
#include "unit/CircuitUnit.h"
#include "util/Utils.h"

#include "json/json.h"

#include <algorithm>
#include <cmath>

namespace circuit {

using namespace springai;

namespace {
constexpr int MAX_RECRUIT_COUNT = 20;
// Queued recruits are built before anything the AI picks itself, so a long
// queue would take over production
constexpr size_t MAX_FACTORY_QUEUE = 20;
constexpr float DEFAULT_FOCUS_RADIUS = 1000.f;
// Orders lapse on their own so a client that goes away cannot strand units
// or pin the army to a stale point
constexpr int DEFAULT_CONTROL_SECONDS = 120;
constexpr int MAX_CONTROL_SECONDS = 30 * 60;
constexpr int DEFAULT_FOCUS_SECONDS = 5 * 60;
constexpr int MAX_FOCUS_SECONDS = 60 * 60;

// Structure kinds a build order may name. Geo needs a geothermal spot id the
// order does not resolve, and the rest are AI-internal services.
const char* const BUILD_ORDER_TYPES[] = {
	"factory", "nano", "store", "energy", "defence", "bunker", "big_gun", "radar", "sonar", "convert", "mex"
};

bool ReadSeconds(const Json::Value& msg, int def, int max, int& out)
{
	const Json::Value& v = msg["seconds"];
	if (v.isNull()) {
		out = def;
		return true;
	}
	if (!v.isInt() || (v.asInt() < 1) || (v.asInt() > max)) {
		return false;
	}
	out = v.asInt();
	return true;
}

bool ReadPos(const Json::Value& msg, AIFloat3& pos)
{
	if (!msg["x"].isNumeric() || !msg["z"].isNumeric()) {
		return false;
	}
	pos = AIFloat3(msg["x"].asFloat(), 0.f, msg["z"].asFloat());
	CTerrainManager::CorrectPosition(pos);
	return true;
}
}  // namespace

void CBrainServer::InitTunables()
{
	if (!tunables.empty()) {
		return;
	}
	CFactoryManager* factoryMgr = circuit->GetFactoryManager();
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();

	tunables["factory.buildPowerRatio"] = {
		[factoryMgr] { return factoryMgr->GetBuildPowerRatio(); },
		[factoryMgr](float v) { factoryMgr->SetBuildPowerRatio(v); }, 0.f, 10.f};
	tunables["factory.responseWeight"] = {
		[factoryMgr] { return factoryMgr->GetResponseWeight(); },
		[factoryMgr](float v) { factoryMgr->SetResponseWeight(v); }, 0.f, 10.f};
	tunables["quota.scouts"] = {
		[militaryMgr] { return static_cast<float>(militaryMgr->GetMaxScouts()); },
		[militaryMgr](float v) { militaryMgr->SetMaxScouts(static_cast<unsigned>(std::lround(v))); }, 0.f, 50.f};
	tunables["quota.attack"] = {
		[militaryMgr] { return militaryMgr->GetMinAttackers(); },
		[militaryMgr](float v) { militaryMgr->SetMinAttackers(v); }, 0.f, 10000.f};
	tunables["quota.raidMin"] = {
		[militaryMgr] { return militaryMgr->GetRaidMin(); },
		[militaryMgr](float v) { militaryMgr->SetRaid(v, militaryMgr->GetRaidAvg()); }, 0.f, 10000.f};
	tunables["quota.raidAvg"] = {
		[militaryMgr] { return militaryMgr->GetRaidAvg(); },
		[militaryMgr](float v) { militaryMgr->SetRaid(militaryMgr->GetRaidMin(), v); }, 0.f, 10000.f};

	const CCircuitDef::RoleT roleCount = militaryMgr->GetRoleCount();
	for (const auto& kv : CCircuitDef::GetRoleNames()) {
		const int type = kv.second.type;
		if ((type < 0) || (type >= roleCount)) {
			continue;
		}
		const CCircuitDef::RoleT role = type;
		tunables["role." + kv.first + ".maxPercent"] = {
			[militaryMgr, role] { return militaryMgr->GetRoleMaxPercent(role); },
			[militaryMgr, role](float v) { militaryMgr->SetRoleLimits(role, v, militaryMgr->GetRoleFactor(role)); },
			0.f, 1.f};
		tunables["role." + kv.first + ".factor"] = {
			[militaryMgr, role] { return militaryMgr->GetRoleFactor(role); },
			[militaryMgr, role](float v) { militaryMgr->SetRoleLimits(role, militaryMgr->GetRoleMaxPercent(role), v); },
			0.f, 100.f};
	}
}

void CBrainServer::HandleCommand(SClient& client, const Json::Value& msg)
{
	InitTunables();
	const std::string op = msg.get("op", "").asString();
	CJsonOut result;
	result.BeginObj();
	std::string error;
	if (op == "set") {
		error = CmdSet(msg);
	} else if (op == "reset") {
		error = CmdReset(msg);
	} else if (op == "limit") {
		error = CmdLimit(msg);
	} else if (op == "build") {
		error = CmdBuild(msg, result);
	} else if (op == "recruit") {
		error = CmdRecruit(msg, result);
	} else if (op == "units") {
		error = CmdUnits(msg, result);
	} else if (op == "focus") {
		error = CmdFocus(msg);
	} else if (op == "control") {
		// Read-only: current control state, same as in "state"
	} else {
		error = "unknown op '" + op + "'";
	}
	result.EndObj();

	CJsonOut ack;
	ack.BeginObj().Field("t", "ack");
	const Json::Value& id = msg["id"];
	if (id.isInt()) {
		ack.Field("id", id.asInt());
	} else if (id.isString()) {
		ack.Field("id", id.asString());
	}
	ack.Field("op", op).Field("ok", error.empty());
	if (!error.empty()) {
		ack.Field("error", error);
	}
	ack.Key("result").Raw(result.Str());
	ack.Key("control");
	WriteControl(ack);
	ack.EndObj();
	Send(client, ack.Take());
}

std::string CBrainServer::CmdSet(const Json::Value& msg)
{
	const std::string key = msg.get("key", "").asString();
	auto it = tunables.find(key);
	if (it == tunables.end()) {
		return "unknown key '" + key + "'";
	}
	if (!msg["value"].isNumeric() || !std::isfinite(msg["value"].asFloat())) {
		return "value must be a number";
	}
	const STunable& tn = it->second;
	const float value = msg["value"].asFloat();
	if ((value < tn.min) || (value > tn.max)) {
		return utils::string_format("value must be within [%g, %g]", tn.min, tn.max);
	}
	tunedFrom.emplace(key, tn.get());  // keeps the first original
	tn.set(value);
	return "";
}

std::string CBrainServer::CmdReset(const Json::Value& msg)
{
	// {"key": k} resets one tunable; no key undoes every change: tunables, unit
	// limits, the attack focus and units under brain control
	const Json::Value& key = msg["key"];
	if (key.isString()) {
		auto it = tunedFrom.find(key.asString());
		if (it == tunedFrom.end()) {
			return "'" + key.asString() + "' was not changed";
		}
		tunables[it->first].set(it->second);
		tunedFrom.erase(it);
		return "";
	}
	for (const auto& kv : tunedFrom) {
		tunables[kv.first].set(kv.second);
	}
	tunedFrom.clear();
	for (const auto& kv : limitedFrom) {
		CCircuitDef* cdef = circuit->GetCircuitDefSafe(kv.first);
		if (cdef != nullptr) {
			cdef->SetMaxThisUnit(kv.second);
		}
	}
	limitedFrom.clear();
	circuit->GetMilitaryManager()->ClearFocus();
	focusUntil = -1;
	ReleaseAll();
	return "";
}

void CBrainServer::ReleaseAll()
{
	for (const auto& kv : controlled) {
		CCircuitUnit* unit = circuit->GetTeamUnit(kv.first);
		if ((unit != nullptr) && !unit->IsDead() && (unit->GetTask() != nullptr)
			&& (unit->GetTask()->GetType() == IUnitTask::Type::PLAYER))
		{
			circuit->UnitControl(unit, true);
		}
	}
	controlled.clear();
}

void CBrainServer::ExpireControl(int frame)
{
	for (auto it = controlled.begin(); it != controlled.end();) {
		CCircuitUnit* unit = circuit->GetTeamUnit(it->first);
		const bool isHeld = (unit != nullptr) && !unit->IsDead() && (unit->GetTask() != nullptr)
				&& (unit->GetTask()->GetType() == IUnitTask::Type::PLAYER);
		if (!isHeld) {
			it = controlled.erase(it);  // died, or something else took it back
		} else if (frame >= it->second) {
			circuit->UnitControl(unit, true);
			it = controlled.erase(it);
		} else {
			++it;
		}
	}
	if ((focusUntil >= 0) && (frame >= focusUntil)) {
		circuit->GetMilitaryManager()->ClearFocus();
		focusUntil = -1;
	}
}

std::string CBrainServer::CmdLimit(const Json::Value& msg)
{
	const std::string name = msg.get("def", "").asString();
	CCircuitDef* cdef = circuit->GetCircuitDef(name.c_str());
	if (cdef == nullptr) {
		return "unknown unit '" + name + "'";
	}
	const Json::Value& limit = msg["limit"];
	if (limit.isNull()) {
		auto it = limitedFrom.find(cdef->GetId());
		if (it != limitedFrom.end()) {
			cdef->SetMaxThisUnit(it->second);
			limitedFrom.erase(it);
		}
		return "";
	}
	if (!limit.isInt() || (limit.asInt() < 0)) {
		return "limit must be a non-negative integer, or null to restore";
	}
	limitedFrom.emplace(cdef->GetId(), cdef->GetMaxThisUnit());
	cdef->SetMaxThisUnit(limit.asInt());
	return "";
}

std::string CBrainServer::CmdBuild(const Json::Value& msg, CJsonOut& result)
{
	const std::string name = msg.get("def", "").asString();
	CCircuitDef* buildDef = circuit->GetCircuitDef(name.c_str());
	if (buildDef == nullptr) {
		return "unknown unit '" + name + "'";
	}
	AIFloat3 pos;
	if (!ReadPos(msg, pos)) {
		return "x and z are required";
	}

	bool hasBuilder = false;
	for (const auto& kv : circuit->GetTeamUnits()) {
		if (kv.second->GetCircuitDef()->CanBuild(buildDef)) {
			hasBuilder = true;
			break;
		}
	}
	if (!hasBuilder) {
		return "no unit of this AI can build " + name;
	}

	// Explicit type, else inferred from what the unit is
	std::string typeName = msg.get("type", "").asString();
	if (typeName.empty()) {
		if (buildDef->IsMex()) {
			typeName = "mex";
		} else if (circuit->GetFactoryManager()->GetFactoryDefs().count(buildDef->GetId()) > 0) {
			typeName = "factory";
		} else if (buildDef->IsRadar()) {
			typeName = "radar";
		} else if (buildDef->IsSonar()) {
			typeName = "sonar";
		} else if (buildDef->IsAssist()) {
			typeName = "nano";
		} else if (circuit->GetEconomyManager()->GetEnergyMake(buildDef) > 0.f) {
			typeName = "energy";
		} else if (buildDef->IsRoleStatic() && (buildDef->GetPower() > THREAT_MIN)) {
			typeName = "defence";
		} else {
			return "can't tell what kind of structure " + name + " is; pass \"type\"";
		}
	}
	const auto& allowed = BUILD_ORDER_TYPES;
	if (std::find(std::begin(allowed), std::end(allowed), typeName) == std::end(allowed)) {
		return "type must be a structure kind such as factory, energy, defence, mex";
	}
	const auto bit = IBuilderTask::GetBuildNames().find(typeName);
	if (bit == IBuilderTask::GetBuildNames().end()) {
		return "unknown type '" + typeName + "'";
	}
	const IBuilderTask::BuildType type = bit->second;

	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	IBuilderTask* task = nullptr;
	if (type == IBuilderTask::BuildType::MEX) {
		CMetalManager* metalMgr = circuit->GetMetalManager();
		const int spot = metalMgr->FindNearestSpot(pos);
		if (spot < 0) {
			return "no metal spot near that position";
		}
		if (!metalMgr->IsOpenSpot(spot)) {
			return "the nearest metal spot is already taken";
		}
		pos = metalMgr->GetSpots()[spot].position;
		task = builderMgr->Enqueue(TaskB::Spot(type, IBuilderTask::Priority::NOW, buildDef, pos, spot));
	} else if (type == IBuilderTask::BuildType::FACTORY) {
		task = builderMgr->Enqueue(TaskB::Factory(IBuilderTask::Priority::NOW, buildDef, pos, nullptr));
	} else {
		task = builderMgr->Enqueue(TaskB::Common(type, IBuilderTask::Priority::NOW, buildDef, pos));
	}
	if (task == nullptr) {
		return "the AI refused the build task";
	}
	result.Field("type", typeName);
	result.Key("pos").BeginArr().Val(pos.x).Val(pos.z).EndArr();
	return "";
}

std::string CBrainServer::CmdRecruit(const Json::Value& msg, CJsonOut& result)
{
	const std::string name = msg.get("def", "").asString();
	CCircuitDef* buildDef = circuit->GetCircuitDef(name.c_str());
	if (buildDef == nullptr) {
		return "unknown unit '" + name + "'";
	}
	const int count = msg.get("count", 1).asInt();
	if ((count < 1) || (count > MAX_RECRUIT_COUNT)) {
		return utils::string_format("count must be within [1, %i]", MAX_RECRUIT_COUNT);
	}

	CFactoryManager* factoryMgr = circuit->GetFactoryManager();
	if (factoryMgr->GetTasks().size() + count > MAX_FACTORY_QUEUE) {
		return utils::string_format("the factory queue holds %i units; at most %i may be queued",
				static_cast<int>(factoryMgr->GetTasks().size()), static_cast<int>(MAX_FACTORY_QUEUE));
	}
	auto isFactoryFor = [&](CCircuitUnit* u) {
		return (u != nullptr) && (factoryMgr->GetFactoryDefs().count(u->GetCircuitDef()->GetId()) > 0)
			&& u->GetCircuitDef()->CanBuild(buildDef);
	};
	CCircuitUnit* factory = nullptr;
	if (msg["factory"].isInt()) {
		factory = circuit->GetTeamUnit(msg["factory"].asInt());
		if (!isFactoryFor(factory)) {
			return "unit " + std::to_string(msg["factory"].asInt()) + " is not a factory that can build " + name;
		}
	} else {
		for (const auto& kv : circuit->GetTeamUnits()) {
			if (isFactoryFor(kv.second)) {
				factory = kv.second;
				break;
			}
		}
		if (factory == nullptr) {
			return "no factory of this AI can build " + name;
		}
	}

	const AIFloat3& pos = factory->GetPos(circuit->GetLastFrame());
	const CCircuitDef* facDef = factory->GetCircuitDef();
	const float radius = std::max(facDef->GetMoveXSize(), facDef->GetMoveZSize()) * SQUARE_SIZE / 2;
	const auto type = buildDef->IsRoleBuilder() ? CRecruitTask::RecruitType::BUILDPOWER : CRecruitTask::RecruitType::FIREPOWER;
	for (int i = 0; i < count; ++i) {
		factoryMgr->Enqueue(TaskS::Recruit(type, CRecruitTask::Priority::HIGH, buildDef, pos, radius));
	}
	result.Field("factory", factory->GetId());
	return "";
}

std::string CBrainServer::CmdUnits(const Json::Value& msg, CJsonOut& result)
{
	const std::string action = msg.get("action", "").asString();
	const Json::Value& ids = msg["units"];
	if (!ids.isArray() || ids.empty()) {
		return "units must be a non-empty array of unit ids";
	}
	AIFloat3 pos;
	const bool needsPos = (action == "move") || (action == "fight");
	if (needsPos && !ReadPos(msg, pos)) {
		return "x and z are required for " + action;
	}
	if (!needsPos && (action != "stop") && (action != "retreat") && (action != "release")) {
		return "action must be move, fight, stop, retreat or release";
	}
	int seconds;
	if (!ReadSeconds(msg, DEFAULT_CONTROL_SECONDS, MAX_CONTROL_SECONDS, seconds)) {
		return utils::string_format("seconds must be an integer within [1, %i]", MAX_CONTROL_SECONDS);
	}

	const int frame = circuit->GetLastFrame();
	const bool needsMobile = needsPos || (action == "retreat");
	result.Key("applied").BeginArr();
	for (const Json::Value& idv : ids) {
		if (!idv.isInt()) {
			continue;
		}
		CCircuitUnit* unit = circuit->GetTeamUnit(idv.asInt());
		if ((unit == nullptr) || unit->IsDead() || (unit->GetTask() == nullptr)) {
			continue;
		}
		if (needsMobile && !unit->GetCircuitDef()->IsMobile()) {
			continue;  // structures cannot move; they are left out of "applied"
		}
		const bool isPlayer = (unit->GetTask()->GetType() == IUnitTask::Type::PLAYER);
		if (action == "release") {
			if (isPlayer) {
				circuit->UnitControl(unit, true);
			}
			controlled.erase(unit->GetId());
		} else if (action == "retreat") {
			if (isPlayer) {
				circuit->UnitControl(unit, true);
				controlled.erase(unit->GetId());
			}
			ITaskModule* mgr = unit->GetTask()->GetManager();
			CRetreatTask* task = (mgr != nullptr) ? mgr->EnqueueRetreat() : nullptr;
			if (task == nullptr) {
				continue;  // this unit's manager has no retreat behaviour (e.g. factories)
			}
			mgr->AssignTask(unit, task);
		} else {
			if (!isPlayer) {
				circuit->UnitControl(unit, false);
			}
			const int until = frame + seconds * FRAMES_PER_SEC;
			controlled[unit->GetId()] = until;
			if (action == "move") {
				unit->CmdMoveTo(pos, 0, until);
			} else if (action == "fight") {
				unit->CmdFightTo(pos, 0, until);
			} else {
				unit->CmdStop();
			}
		}
		result.Val(unit->GetId());
	}
	result.EndArr();
	return "";
}

std::string CBrainServer::CmdFocus(const Json::Value& msg)
{
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	if (msg.get("clear", false).asBool()) {
		militaryMgr->ClearFocus();
		focusUntil = -1;
		return "";
	}
	int seconds;
	if (!ReadSeconds(msg, DEFAULT_FOCUS_SECONDS, MAX_FOCUS_SECONDS, seconds)) {
		return utils::string_format("seconds must be an integer within [1, %i]", MAX_FOCUS_SECONDS);
	}
	AIFloat3 pos;
	if (!ReadPos(msg, pos)) {
		return "x and z are required, or clear: true";
	}
	float radius = DEFAULT_FOCUS_RADIUS;
	if (msg["radius"].isNumeric()) {
		radius = msg["radius"].asFloat();
		if (!std::isfinite(radius) || (radius < 100.f)) {
			return "radius must be at least 100";
		}
	}
	militaryMgr->SetFocus(pos, radius, msg.get("force", false).asBool());
	focusUntil = circuit->GetLastFrame() + seconds * FRAMES_PER_SEC;
	return "";
}

void CBrainServer::WriteControl(CJsonOut& js) const
{
	js.BeginObj();
	js.Key("tuning").BeginObj();
	for (const auto& kv : tunables) {
		js.Field(kv.first.c_str(), kv.second.get());
	}
	js.EndObj();
	js.Key("changed").BeginObj();
	for (const auto& kv : tunedFrom) {
		js.Field(kv.first.c_str(), kv.second);  // key: original value
	}
	js.EndObj();
	// [defId, limit, original]
	js.Key("limits").BeginArr();
	for (const auto& kv : limitedFrom) {
		CCircuitDef* cdef = circuit->GetCircuitDefSafe(kv.first);
		if (cdef != nullptr) {
			js.BeginArr().Val(kv.first).Val(cdef->GetMaxThisUnit()).Val(kv.second).EndArr();
		}
	}
	js.EndArr();
	const CMilitaryManager::SFocus& focus = circuit->GetMilitaryManager()->GetFocus();
	js.Key("focus");
	if (focus.isActive) {
		js.BeginObj().Field("x", focus.pos.x).Field("z", focus.pos.z)
			.Field("radius", focus.radius).Field("force", focus.isForce).Field("until", focusUntil).EndObj();
	} else {
		js.Null();
	}
	// [unitId, frame it returns to the AI]; pruned every frame by ExpireControl
	js.Key("controlled").BeginArr();
	for (const auto& kv : controlled) {
		js.BeginArr().Val(kv.first).Val(kv.second).EndArr();
	}
	js.EndArr();
	js.EndObj();
}

} // namespace circuit
