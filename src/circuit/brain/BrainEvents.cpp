/*
 * BrainEvents.cpp
 *
 * Combat events for brain clients: nuke launches and impacts, other large
 * blasts, antinuke shots and bursts of our own losses. Raw engine events are
 * gathered for a moment and sent as one "event" message each, so a nuke that
 * hits forty units is one message. Shapes are in util/brainview/PROTOCOL.md.
 *
 * A skirmish AI cannot see projectiles or enemy weapon fire. Enemy nukes show
 * up only where they hit us (UnitDamaged and UnitDestroyed carry the weapon
 * even when the attacker is hidden) or when our antinuke shoots at one.
 */

#include "brain/BrainServer.h"
#include "brain/BrainEventUtil.h"
#include "brain/JsonOut.h"
#include "CircuitAI.h"
#include "task/fighter/FighterTask.h"
#include "task/static/SuperTask.h"
#include "unit/CircuitUnit.h"
#include "unit/ally/AllyUnit.h"
#include "unit/enemy/EnemyUnit.h"

#include "Unit.h"
#include "UnitDef.h"
#include "WeaponDef.h"
#include "WeaponMount.h"

#include <algorithm>
#include <cmath>

namespace circuit {

using namespace springai;
using namespace brain;

namespace {
constexpr float LARGE_AOE = 150.f;  // damage radius (half the unit file's areaofeffect)
constexpr int BLAST_WINDOW = FRAMES_PER_SEC * 2;  // hits joined into one blast
constexpr int BLAST_QUIET = FRAMES_PER_SEC;  // a blast closes after this long without hits
constexpr int LARGE_MIN_HIT = 3;  // non-nuke blasts are reported from this many of our units hit
constexpr int LARGE_MIN_KILLED = 2;  // ... or this many killed
constexpr float LOSS_RADIUS = 1200.f;
constexpr int LOSS_GAP = FRAMES_PER_SEC * 5;  // a burst closes after this long without losses
constexpr int LOSS_SPAN = FRAMES_PER_SEC * 30;  // and never runs longer than this
constexpr int LOSS_MIN_COUNT = 4;  // bursts are reported from this many units ...
constexpr float LOSS_MIN_COST = 1000.f;  // ... or this much metal
constexpr int PEAK_WINDOW = FRAMES_PER_SEC * 5;
constexpr int MASS_COUNT = 6;  // within PEAK_WINDOW
constexpr float MASS_COST = 3000.f;  // within PEAK_WINDOW
constexpr int LAUNCH_MEMORY = FRAMES_PER_SEC * 180;
constexpr int SILO_POLL = FRAMES_PER_SEC / 2;

}  // namespace

void CBrainServer::InitWeaponClasses()
{
	for (int id = 0; circuit->IsValidWeaponDefId(id); ++id) {
		CWeaponDef* wd = circuit->GetWeaponDef(id);
		WeaponDef* def = wd->GetDef();
		WeaponClass cls = WC_NONE;
		// In BAR only nuke missiles are interceptable, and only antinukes intercept
		if (def->GetTargetable() > 0) {
			cls = WC_NUKE;
		} else if (def->GetInterceptor() > 0) {
			cls = WC_ANTI;
		} else if (wd->GetAoe() >= LARGE_AOE) {
			cls = WC_LARGE;
		}
		weaponClasses.push_back(cls);
	}
	if (weaponClasses.empty()) {
		weaponClasses.push_back(WC_NONE);  // marks the tables as built
	}
	weaponSources.assign(weaponClasses.size(), -1);

	// Read from the mounts: BARb binds weapons only for defs that can attack,
	// which antinukes cannot
	for (CCircuitDef& cdef : circuit->GetCircuitDefs()) {
		uint8_t flags = 0;
		for (WeaponMount* mount : cdef.GetDef()->GetWeaponMounts()) {
			WeaponDef* wd = mount->GetWeaponDef();
			const int id = wd->GetWeaponDefId();
			if (circuit->IsValidWeaponDefId(id)) {
				flags |= (weaponClasses[id] == WC_NUKE) ? DF_NUKE : (weaponClasses[id] == WC_ANTI) ? DF_ANTI : 0;
				if (weaponSources[id] < 0) {
					weaponSources[id] = cdef.GetId();
				}
			}
			delete wd;
			delete mount;
		}
		if (flags != 0) {
			if (cdef.GetId() >= static_cast<int>(defFlags.size())) {
				defFlags.resize(cdef.GetId() + 1, 0);
			}
			defFlags[cdef.GetId()] = flags;
		}
	}
}

CBrainServer::WeaponClass CBrainServer::ClassOf(int weaponDefId)
{
	if (weaponClasses.empty()) {
		InitWeaponClasses();
	}
	return circuit->IsValidWeaponDefId(weaponDefId) ? weaponClasses[weaponDefId] : WC_NONE;
}

uint8_t CBrainServer::DefFlagsOf(int defId)
{
	if (weaponClasses.empty()) {
		InitWeaponClasses();
	}
	return ((defId >= 0) && (defId < static_cast<int>(defFlags.size()))) ? defFlags[defId] : 0;
}

CBrainServer::Side CBrainServer::SideOf(int attackerId, int* attackerDef) const
{
	*attackerDef = -1;
	if (attackerId < 0) {
		return SIDE_UNKNOWN;
	}
	if (CCircuitUnit* unit = circuit->GetTeamUnit(attackerId)) {
		*attackerDef = unit->GetCircuitDef()->GetId();
		return SIDE_OURS;
	}
	if (CEnemyInfo* enemy = circuit->GetEnemyInfo(attackerId)) {
		if (enemy->GetCircuitDef() != nullptr) {
			*attackerDef = enemy->GetCircuitDef()->GetId();
		}
		return SIDE_ENEMY;
	}
	if (CAllyUnit* ally = circuit->GetFriendlyUnit(attackerId)) {
		*attackerDef = ally->GetCircuitDef()->GetId();
		return SIDE_ALLY;
	}
	return SIDE_UNKNOWN;
}

CBrainServer::SBlast* CBrainServer::FindBlast(int weaponDefId, float x, float z, int frame)
{
	const float reach = std::max(circuit->GetWeaponDef(weaponDefId)->GetAoe(), 100.f) * 2.f;
	for (SBlast& b : blasts) {
		if ((b.weaponDefId != weaponDefId) || (frame - b.firstFrame > BLAST_WINDOW) || (b.points == 0)) {
			continue;
		}
		const float dx = b.sumX / b.points - x;
		const float dz = b.sumZ / b.points - z;
		if (dx * dx + dz * dz <= reach * reach) {
			return &b;
		}
	}
	return nullptr;
}

void CBrainServer::OnUnitDamaged(CCircuitUnit* unit, int attackerId, int weaponDefId, bool paralyzer)
{
	NoteAttack(unit, attackerId, weaponDefId);
	const WeaponClass cls = ClassOf(weaponDefId);
	if ((cls != WC_NUKE) && (cls != WC_LARGE)) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	SBlast* b = FindBlast(weaponDefId, pos.x, pos.z, frame);
	int attackerDef;
	const Side side = SideOf(attackerId, &attackerDef);
	if (b == nullptr) {
		blasts.push_back(SBlast{weaponDefId, cls, paralyzer, frame, frame});
		b = &blasts.back();
		b->side = side;
		b->attackerSeen = (attackerId >= 0);
		b->attackerDef = attackerDef;
		if (attackerId < 0) {
			// Hidden attackers are enemies. Our own silo is always visible to us,
			// unless it died after launching, so our launch record settles that case.
			b->side = SIDE_ENEMY;
			const float reach = circuit->GetWeaponDef(weaponDefId)->GetAoe() * 2.f;
			for (const SLaunch& l : launches) {
				if ((cls == WC_NUKE) && l.hasTarget
					&& ((l.x - pos.x) * (l.x - pos.x) + (l.z - pos.z) * (l.z - pos.z) <= reach * reach))
				{
					b->side = SIDE_OURS;
				}
			}
		}
	} else if (!b->attackerSeen && (attackerId >= 0)) {
		b->side = side;
		b->attackerSeen = true;
		b->attackerDef = attackerDef;
	}
	if (b->oursHit.insert(unit->GetId()).second) {
		b->sumX += pos.x;
		b->sumZ += pos.z;
		++b->points;
	}
	b->lastFrame = frame;
}

void CBrainServer::OnEnemyDamaged(CEnemyInfo* enemy, int attackerId, int weaponDefId)
{
	// The engine reports these only for our own team's attacks
	if ((enemy == nullptr) || (ClassOf(weaponDefId) != WC_NUKE)) {
		return;
	}
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = enemy->GetPos();
	SBlast* b = FindBlast(weaponDefId, pos.x, pos.z, frame);
	if (b == nullptr) {
		int attackerDef;
		const Side side = SideOf(attackerId, &attackerDef);
		blasts.push_back(SBlast{weaponDefId, WC_NUKE, false, frame, frame});
		b = &blasts.back();
		b->side = (side == SIDE_UNKNOWN) ? SIDE_OURS : side;
		b->attackerSeen = (attackerId >= 0);
		b->attackerDef = attackerDef;
	}
	if (b->enemyHit.insert(enemy->GetId()).second) {
		b->sumX += pos.x;
		b->sumZ += pos.z;
		++b->points;
	}
	b->lastFrame = frame;
}

void CBrainServer::OnUnitDestroyed(CCircuitUnit* unit, int attackerId, int weaponDefId)
{
	keyAttackEmit.erase(unit->GetId());
	// Nanoframes hold the nil task; losing one is not a combat loss
	if (((unit->GetTask() != nullptr) && (unit->GetTask()->GetType() == IUnitTask::Type::NIL))
		|| IsDeliberateKill(weaponDefId))
	{
		return;
	}
	const int frame = circuit->GetLastFrame();
	const AIFloat3& pos = unit->GetPos(frame);
	const float cost = unit->GetCircuitDef()->GetCostM();

	if (CategoryOf(unit->GetCircuitDef()->GetId()) != KC_NONE) {
		int left = -1;
		if (unit->GetCircuitDef()->IsRoleComm()) {
			left = 0;
			for (const auto& kv : circuit->GetTeamUnits()) {
				CCircuitUnit* u = kv.second;
				if ((u != unit) && u->GetCircuitDef()->IsRoleComm()
					&& ((u->GetTask() == nullptr) || (u->GetTask()->GetType() != IUnitTask::Type::NIL)))
				{
					++left;
				}
			}
		}
		EmitKeyDeath(frame, SDeath{"ours", unit->GetId(), unit->GetCircuitDef()->GetId(), circuit->GetTeamId(),
				circuit->GetAllyTeamId(), pos.x, pos.z, attackerId, weaponDefId, left});
	}

	const WeaponClass cls = ClassOf(weaponDefId);
	if ((cls == WC_NUKE) || (cls == WC_LARGE)) {
		if (SBlast* b = FindBlast(weaponDefId, pos.x, pos.z, frame)) {
			++b->oursKilled;
			b->oursKilledCost += cost;
			b->lastFrame = frame;
		}
	}

	SLossBurst* burst = nullptr;
	for (SLossBurst& lb : lossBursts) {
		const float n = static_cast<float>(lb.losses.size());
		const float dx = lb.sumX / n - pos.x;
		const float dz = lb.sumZ / n - pos.z;
		if ((dx * dx + dz * dz <= LOSS_RADIUS * LOSS_RADIUS) && (frame - lb.firstFrame <= LOSS_SPAN)) {
			burst = &lb;
			break;
		}
	}
	if (burst == nullptr) {
		lossBursts.push_back(SLossBurst{frame, frame});
		burst = &lossBursts.back();
	}
	burst->lastFrame = frame;
	burst->sumX += pos.x;
	burst->sumZ += pos.z;
	burst->losses.push_back(SLoss{frame, cost});
	++burst->byDef[unit->GetCircuitDef()->GetId()];
	++burst->byWeapon[circuit->IsValidWeaponDefId(weaponDefId) ? weaponDefId : std::min(weaponDefId, -1)];
	int attackerDef;
	SideOf(attackerId, &attackerDef);
	++burst->byAttacker[attackerDef];
}

void CBrainServer::PollSilos(int frame)
{
	// WeaponFired reaches AIs only for shots under an attack command, which
	// antinukes never have, so shots are read off stockpile drops instead
	std::map<int, int> seen;
	for (const auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		const uint8_t flags = DefFlagsOf(unit->GetCircuitDef()->GetId());
		if ((flags == 0) || (unit->GetTask() == nullptr) || (unit->GetTask()->GetType() == IUnitTask::Type::NIL)) {
			continue;
		}
		const int stock = unit->GetUnit()->GetStockpile();
		seen[unit->GetId()] = stock;
		auto it = stockpiles.find(unit->GetId());
		if ((it == stockpiles.end()) || (stock >= it->second)) {
			continue;
		}
		const int shots = it->second - stock;
		const AIFloat3& pos = unit->GetPos(frame);
		CJsonOut js;
		js.BeginObj().Field("t", "event").Field("seq", ++eventSeq);
		if (flags & DF_NUKE) {
			SLaunch launch{frame, pos.x, pos.z, false};
			IUnitTask* task = unit->GetTask();
			if ((task->GetType() == IUnitTask::Type::FIGHTER)
				&& (static_cast<IFighterTask*>(task)->GetFightType() == IFighterTask::FightType::SUPER))
			{
				const AIFloat3& target = static_cast<CSuperTask*>(task)->GetTargetPos();
				if (target.x >= 0.f) {
					launch.x = target.x;
					launch.z = target.z;
					launch.hasTarget = true;
				}
			}
			launches.push_back(launch);
			js.Field("kind", "launch");
		} else {
			js.Field("kind", "intercept");
		}
		js.Field("frame", frame).Field("unit", unit->GetId()).Field("def", unit->GetCircuitDef()->GetId())
			.Field("shots", shots).Field("stockpile", stock);
		js.Key("pos"); WritePos(js, pos.x, pos.z);
		if (flags & DF_NUKE) {
			js.Key("target");
			if (launches.back().hasTarget) {
				WritePos(js, launches.back().x, launches.back().z);
			} else {
				js.Null();
			}
		}
		js.EndObj();
		Emit(js.Take());
	}
	stockpiles = std::move(seen);
}

void CBrainServer::UpdateEvents(int frame)
{
	if (frame % SILO_POLL == 0) {
		PollSilos(frame);
	}
	if (frame % FRAMES_PER_SEC == 0) {
		PollAllyKeyUnits(frame);
	}
	FlushAttacks(frame);
	for (auto it = blasts.begin(); it != blasts.end();) {
		if (frame - it->lastFrame > BLAST_QUIET) {
			CloseBlast(*it);
			it = blasts.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = lossBursts.begin(); it != lossBursts.end();) {
		if ((frame - it->lastFrame > LOSS_GAP) || (frame - it->firstFrame > LOSS_SPAN)) {
			CloseLossBurst(*it);
			it = lossBursts.erase(it);
		} else {
			++it;
		}
	}
	while (!launches.empty() && (frame - launches.front().frame > LAUNCH_MEMORY)) {
		launches.pop_front();
	}
}

void CBrainServer::CloseBlast(const SBlast& b)
{
	const int ours = static_cast<int>(b.oursHit.size());
	if ((b.points == 0)
		|| ((b.cls == WC_LARGE) && (ours < LARGE_MIN_HIT) && (b.oursKilled < LARGE_MIN_KILLED)))
	{
		return;
	}
	CWeaponDef* wd = circuit->GetWeaponDef(b.weaponDefId);
	const float cx = b.sumX / b.points;
	const float cz = b.sumZ / b.points;

	CJsonOut js;
	js.BeginObj()
		.Field("t", "event").Field("seq", ++eventSeq).Field("kind", "blast").Field("frame", b.firstFrame)
		.Field("class", (b.cls == WC_NUKE) ? "nuke" : "large")
		.Field("weapon", wd->GetDef()->GetName())
		.Field("sourceDef", weaponSources[b.weaponDefId])
		.Field("aoe", wd->GetAoe())
		.Field("paralyzer", b.paralyzer)
		.Field("side", SIDE_NAMES[b.side])
		.Field("attackerSeen", b.attackerSeen)
		.Field("attackerDef", b.attackerDef);
	js.Key("pos"); WritePos(js, cx, cz);
	js.Field("oursHit", ours)
		.Field("oursKilled", b.oursKilled)
		.Field("oursKilledCost", std::round(b.oursKilledCost))
		.Field("enemyHit", static_cast<int>(b.enemyHit.size()));
	js.EndObj();
	Emit(js.Take());
}

void CBrainServer::CloseLossBurst(const SLossBurst& lb)
{
	const int count = static_cast<int>(lb.losses.size());
	float cost = 0.f;
	int peak = 0;
	float peakCost = 0.f;
	float windowCost = 0.f;
	for (size_t i = 0, j = 0; i < lb.losses.size(); ++i) {
		cost += lb.losses[i].cost;
		windowCost += lb.losses[i].cost;
		while (lb.losses[i].frame - lb.losses[j].frame > PEAK_WINDOW) {
			windowCost -= lb.losses[j].cost;
			++j;
		}
		peak = std::max(peak, static_cast<int>(i - j + 1));
		peakCost = std::max(peakCost, windowCost);
	}
	if ((count < LOSS_MIN_COUNT) && (cost < LOSS_MIN_COST)) {
		return;
	}

	CJsonOut js;
	js.BeginObj()
		.Field("t", "event").Field("seq", ++eventSeq).Field("kind", "losses").Field("frame", lb.firstFrame)
		.Field("endFrame", lb.lastFrame);
	js.Key("pos"); WritePos(js, lb.sumX / count, lb.sumZ / count);
	js.Field("count", count)
		.Field("cost", std::round(cost))
		.Field("peak5s", peak)
		.Field("peak5sCost", std::round(peakCost))
		.Field("massCasualty", (peak >= MASS_COUNT) || (peakCost >= MASS_COST));
	js.Key("units"); WriteCounts(js, lb.byDef);
	js.Key("attackers"); WriteCounts(js, lb.byAttacker);
	js.Key("weapons").BeginArr();
	for (const auto& kv : SortedCounts(lb.byWeapon)) {
		const char* name = circuit->IsValidWeaponDefId(kv.first)
				? circuit->GetWeaponDef(kv.first)->GetDef()->GetName()
				: SpecialDamageName(kv.first);
		const WeaponClass cls = ClassOf(kv.first);
		js.BeginArr().Val(name).Val(kv.second)
			.Val((cls == WC_NUKE) ? "nuke" : (cls == WC_LARGE) ? "large" : "").EndArr();
	}
	js.EndArr();
	js.EndObj();
	Emit(js.Take());
}

void CBrainServer::WriteSilos(CJsonOut& js, int frame)
{
	// [id, defId, x, z, flags, stockpile, queued, finished]
	js.Key("silos").BeginArr();
	for (const auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		const uint8_t flags = DefFlagsOf(unit->GetCircuitDef()->GetId());
		if (flags == 0) {
			continue;
		}
		const AIFloat3& pos = unit->GetPos(frame);
		const bool finished = (unit->GetTask() != nullptr) && (unit->GetTask()->GetType() != IUnitTask::Type::NIL);
		js.BeginArr()
			.Val(unit->GetId())
			.Val(unit->GetCircuitDef()->GetId())
			.Val(pos.x).Val(pos.z)
			.Val(static_cast<int>(flags))
			.Val(finished ? unit->GetUnit()->GetStockpile() : 0)
			.Val(finished ? unit->GetUnit()->GetStockpileQueued() : 0)
			.Val(finished)
		.EndArr();
	}
	js.EndArr();
}

} // namespace circuit
