/*
 * BrainAlerts.cpp
 *
 * Alerts for brain clients: deaths of key units (commanders, experimentals,
 * factories, superweapons, big defences and economy) on every side, teams
 * losing their last commander, and our units coming under attack. Shapes are
 * in util/brainview/PROTOCOL.md.
 */

#include "brain/BrainServer.h"
#include "brain/BrainEventUtil.h"
#include "brain/JsonOut.h"
#include "CircuitAI.h"
#include "task/fighter/FighterTask.h"
#include "terrain/TerrainManager.h"
#include "unit/CircuitUnit.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"

#include "Unit.h"
#include "UnitDef.h"
#include "WeaponDef.h"

#include <algorithm>

namespace circuit {

using namespace springai;
using namespace brain;

namespace {
constexpr float DEFENCE_MIN_COST = 1000.f;  // armed structures from this metal cost are key units
constexpr float STRUCTURE_MIN_COST = 300.f;  // unarmed structures from this cost (T2 mex, fusion, radar)
constexpr int SECTOR_COLS = 8;  // matches brain_mcp.py's sectors
constexpr int ATTACK_GATHER = FRAMES_PER_SEC * 3;  // hits gathered before a sector's first report
constexpr int ATTACK_GAP = FRAMES_PER_SEC * 25;  // least time between reports for one sector
constexpr int KEY_ATTACK_GAP = FRAMES_PER_SEC * 10;  // least time between reports for one key unit

const char* const CATEGORY_NAMES[] = {"", "commander", "experimental", "factory", "superweapon", "defence",
	"economy", "structure"};

bool IsOffensive(CCircuitUnit* unit)
{
	IUnitTask* task = unit->GetTask();
	if ((task == nullptr) || (task->GetType() != IUnitTask::Type::FIGHTER)) {
		return false;
	}
	switch (static_cast<IFighterTask*>(task)->GetFightType()) {
		case IFighterTask::FightType::ATTACK:
		case IFighterTask::FightType::RAID:
		case IFighterTask::FightType::BOMB:
		case IFighterTask::FightType::SCOUT:
			return true;
		default:
			return false;
	}
}

bool IsNanoframe(CCircuitUnit* unit)
{
	return (unit->GetTask() != nullptr) && (unit->GetTask()->GetType() == IUnitTask::Type::NIL);
}
}  // namespace

uint8_t CBrainServer::CategoryOf(int defId)
{
	if (defCategories.empty()) {
		for (CCircuitDef& cdef : circuit->GetCircuitDefs()) {
			const bool isStatic = !cdef.IsMobile();
			const auto params = cdef.GetDef()->GetCustomParams();
			const auto tech = params.find("techlevel");
			const auto group = params.find("unitgroup");
			KeyCategory cat = KC_NONE;
			if (cdef.IsRoleComm()) {
				cat = KC_COMMANDER;
			} else if (isStatic && cdef.IsBuilder()) {
				cat = KC_FACTORY;
			} else if ((tech != params.end()) && (tech->second == "3")) {
				cat = KC_EXPERIMENTAL;
			} else if ((DefFlagsOf(cdef.GetId()) != 0) || (isStatic && cdef.IsRoleSuper())) {
				cat = KC_SUPER;
			} else if (isStatic && cdef.IsAttacker() && (cdef.GetCostM() >= DEFENCE_MIN_COST)) {
				cat = KC_DEFENCE;
			} else if (isStatic && !cdef.IsAttacker() && (cdef.GetCostM() >= STRUCTURE_MIN_COST)) {
				const bool isEco = (group != params.end()) && ((group->second == "energy") || (group->second == "metal"));
				cat = isEco ? KC_ECONOMY : KC_STRUCTURE;
			}
			if (cdef.GetId() >= static_cast<int>(defCategories.size())) {
				defCategories.resize(cdef.GetId() + 1, KC_NONE);
			}
			defCategories[cdef.GetId()] = cat;
		}
		if (defCategories.empty()) {
			defCategories.push_back(KC_NONE);  // marks the table as built
		}
	}
	return ((defId >= 0) && (defId < static_cast<int>(defCategories.size()))) ? defCategories[defId] : KC_NONE;
}

void CBrainServer::EmitKeyDeath(int frame, const SDeath& d)
{
	int killerDef;
	const Side killer = SideOf(d.attackerId, &killerDef);
	int killerTeam = -1;
	if (killer == SIDE_OURS) {
		killerTeam = circuit->GetTeamId();
	} else if (killer == SIDE_ENEMY) {
		killerTeam = circuit->GetEnemyInfo(d.attackerId)->GetUnit()->GetTeam();
	} else if (killer == SIDE_ALLY) {
		killerTeam = circuit->GetFriendlyUnit(d.attackerId)->GetUnit()->GetTeam();
	}
	const uint8_t cat = CategoryOf(d.def);
	CJsonOut js;
	js.BeginObj()
		.Field("t", "event").Field("seq", ++eventSeq).Field("kind", "keyDeath").Field("frame", frame)
		.Field("category", CATEGORY_NAMES[cat]).Field("side", d.side)
		.Field("unit", d.unitId).Field("def", d.def).Field("team", d.team).Field("allyTeam", d.allyTeam);
	js.Key("pos"); WritePos(js, d.x, d.z);
	js.Field("killerSide", SIDE_NAMES[killer]).Field("killerDef", killerDef).Field("killerTeam", killerTeam)
		.Field("weapon", circuit->IsValidWeaponDefId(d.weaponDefId)
				? circuit->GetWeaponDef(d.weaponDefId)->GetDef()->GetName()
				: (d.weaponDefId < -1) ? SpecialDamageName(d.weaponDefId) : "")
		.Field("commandersLeft", d.commandersLeft);
	js.EndObj();
	Emit(js.Take());

	if ((cat != KC_COMMANDER) || (d.commandersLeft != 0) || !commanderlessTeams.insert(d.team).second) {
		return;
	}
	CJsonOut te;
	te.BeginObj()
		.Field("t", "event").Field("seq", ++eventSeq).Field("kind", "teamEliminated").Field("frame", frame)
		.Field("side", d.side).Field("team", d.team).Field("allyTeam", d.allyTeam)
		.Field("reason", "last commander died");
	te.EndObj();
	Emit(te.Take());
}

void CBrainServer::OnEnemyDestroyed(CEnemyInfo* enemy, int attackerId)
{
	// Sent only for enemies in our radar or sight; attackerId only when the killer is allied
	CCircuitDef* cdef = enemy->GetCircuitDef();
	if ((cdef == nullptr) || enemy->IsBeingBuilt() || (CategoryOf(cdef->GetId()) == KC_NONE)) {
		return;
	}
	const AIFloat3& pos = enemy->GetPos();
	EmitKeyDeath(circuit->GetLastFrame(), SDeath{"enemy", enemy->GetId(), cdef->GetId(),
			enemy->GetUnit()->GetTeam(), enemy->GetUnit()->GetAllyTeam(), pos.x, pos.z, attackerId, -1, -1});
}

void CBrainServer::PollAllyKeyUnits(int frame)
{
	// The engine tells an AI nothing when an allied unit dies, so allied key
	// units are noticed missing from the friendly list
	std::map<int, SAllyKeyUnit> now;
	for (const auto& kv : circuit->GetFriendlyUnits()) {
		CAllyUnit* u = kv.second;
		if ((u->GetCircuitDef() == nullptr) || (CategoryOf(u->GetCircuitDef()->GetId()) == KC_NONE)
			|| (circuit->GetTeamUnit(kv.first) != nullptr))
		{
			continue;
		}
		auto it = allyKeyUnits.find(kv.first);
		int team;
		if (it != allyKeyUnits.end()) {
			team = it->second.team;
		} else {
			if (u->GetUnit()->IsBeingBuilt()) {
				continue;  // a cancelled nanoframe would read as a death
			}
			team = u->GetUnit()->GetTeam();
		}
		if (team == circuit->GetTeamId()) {
			continue;  // ours, but not registered with us yet
		}
		const AIFloat3& pos = u->GetPos(frame);
		now[kv.first] = SAllyKeyUnit{u->GetCircuitDef()->GetId(), team, pos.x, pos.z};
	}
	for (const auto& kv : allyKeyUnits) {
		if (now.find(kv.first) != now.end()) {
			continue;
		}
		const SAllyKeyUnit& k = kv.second;
		int left = -1;
		if (CategoryOf(k.def) == KC_COMMANDER) {
			left = 0;
			for (const auto& n : now) {
				left += ((n.second.team == k.team) && (CategoryOf(n.second.def) == KC_COMMANDER)) ? 1 : 0;
			}
		}
		EmitKeyDeath(frame, SDeath{"ally", kv.first, k.def, k.team, circuit->GetAllyTeamId(), k.x, k.z, -1, -1, left});
	}
	allyKeyUnits = std::move(now);
}

void CBrainServer::NoteAttack(CCircuitUnit* unit, int attackerId, int weaponDefId)
{
	if (attackerId >= 0) {
		if ((circuit->GetTeamUnit(attackerId) != nullptr) || (circuit->GetFriendlyUnit(attackerId) != nullptr)) {
			return;  // friendly fire, or our own death explosions
		}
	} else if (!circuit->IsValidWeaponDefId(weaponDefId)) {
		return;  // collisions, fire, water
	}
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	const int def = unit->GetCircuitDef()->GetId();
	int attackerDef = -1;
	if (attackerId >= 0) {
		CEnemyInfo* enemy = circuit->GetEnemyInfo(attackerId);
		if ((enemy != nullptr) && (enemy->GetCircuitDef() != nullptr)) {
			attackerDef = enemy->GetCircuitDef()->GetId();
		}
	}

	const uint8_t cat = CategoryOf(def);
	if ((cat != KC_NONE) && !IsNanoframe(unit)) {
		auto it = keyAttackEmit.find(unit->GetId());
		if ((it == keyAttackEmit.end()) || (frame - it->second >= KEY_ATTACK_GAP)) {
			keyAttackEmit[unit->GetId()] = frame;
			CJsonOut js;
			js.BeginObj()
				.Field("t", "event").Field("seq", ++eventSeq).Field("kind", "keyUnderAttack").Field("frame", frame)
				.Field("category", CATEGORY_NAMES[cat]).Field("unit", unit->GetId()).Field("def", def);
			js.Key("pos"); WritePos(js, pos.x, pos.z);
			js.Field("health", unit->GetHealthPercent())
				.Field("attackerSeen", attackerId >= 0).Field("attackerDef", attackerDef)
				.Field("weapon", circuit->IsValidWeaponDefId(weaponDefId)
						? circuit->GetWeaponDef(weaponDefId)->GetDef()->GetName() : "");
			js.EndObj();
			Emit(js.Take());
		}
	}

	const float size = static_cast<float>(CTerrainManager::GetTerrainWidth()) / SECTOR_COLS;
	const int col = std::clamp(static_cast<int>(pos.x / size), 0, SECTOR_COLS - 1);
	const int row = std::max(static_cast<int>(pos.z / size), 0);
	SAttackCell& c = attackCells[row * SECTOR_COLS + col];
	if (c.firstFrame < 0) {
		c.firstFrame = frame;
	}
	if (c.damaged.emplace(unit->GetId(), def).second) {
		c.sumX += pos.x;
		c.sumZ += pos.z;
		if (IsOffensive(unit)) {
			c.offensive.insert(unit->GetId());
		}
	}
	if (attackerId >= 0) {
		c.attackers.emplace(attackerId, attackerDef);
	} else {
		++c.unseenHits;
	}
}

void CBrainServer::FlushAttacks(int frame)
{
	for (auto it = attackCells.begin(); it != attackCells.end();) {
		SAttackCell& c = it->second;
		if (c.firstFrame < 0) {
			if (frame - c.lastEmit > ATTACK_GAP) {
				it = attackCells.erase(it);
			} else {
				++it;
			}
			continue;
		}
		if ((frame - c.firstFrame < ATTACK_GATHER) || ((c.lastEmit >= 0) && (frame - c.lastEmit < ATTACK_GAP))) {
			++it;
			continue;
		}
		const int n = static_cast<int>(c.damaged.size());
		std::map<int, int> units, attackers;
		int structures = 0;
		for (const auto& kv : c.damaged) {
			++units[kv.second];
			CCircuitDef* cdef = circuit->GetCircuitDefSafe(kv.second);
			structures += ((cdef != nullptr) && !cdef->IsMobile()) ? 1 : 0;
		}
		for (const auto& kv : c.attackers) {
			++attackers[kv.second];
		}
		CJsonOut js;
		js.BeginObj()
			.Field("t", "event").Field("seq", ++eventSeq).Field("kind", "underAttack").Field("frame", c.firstFrame)
			.Field("endFrame", frame);
		js.Key("pos"); WritePos(js, c.sumX / n, c.sumZ / n);
		js.Field("damaged", n).Field("structures", structures)
			.Field("offensive", static_cast<int>(c.offensive.size()));
		js.Key("units"); WriteCounts(js, units);
		js.Key("attackers"); WriteCounts(js, attackers);
		js.Field("unseenHits", c.unseenHits);
		js.EndObj();
		Emit(js.Take());

		c = SAttackCell();
		c.lastEmit = frame;
		++it;
	}
}

} // namespace circuit
