/*
 * BrainSnapshot.cpp
 *
 * Serialises the AI's world model. Message shapes are documented in
 * util/brainview/PROTOCOL.md; keep the two in step.
 */

#include "brain/BrainServer.h"
#include "brain/DecisionTrace.h"
#include "brain/JsonOut.h"
#include "CircuitAI.h"
#include "map/InfluenceMap.h"
#include "map/ThreatMap.h"
#include "module/EconomyManager.h"
#include "module/BuilderManager.h"
#include "module/FactoryManager.h"
#include "module/MilitaryManager.h"
#include "resource/MetalManager.h"
#include "setup/SetupManager.h"
#include "spring/SpringMap.h"
#include "task/builder/BuilderTask.h"
#include "task/fighter/DefendTask.h"
#include "task/fighter/FighterTask.h"
#include "task/fighter/SquadTask.h"
#include "task/static/RecruitTask.h"
#include "terrain/TerrainManager.h"
#include "unit/CircuitUnit.h"
#include "unit/enemy/EnemyManager.h"
#include "unit/enemy/EnemyUnit.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>

namespace circuit {

using namespace springai;

namespace {

// Indexed by IFighterTask::FightType
const char* const FIGHT_NAMES[] = {
	"rally", "guard", "defend", "scout", "raid", "attack", "bomb", "melee", "arty", "aa", "ah", "support", "super"
};
static_assert(sizeof(FIGHT_NAMES) / sizeof(FIGHT_NAMES[0]) == static_cast<size_t>(IFighterTask::FightType::_SIZE_));

// Indexed by IUnitTask::Type
const char* const TASK_TYPE_NAMES[] = {"nil", "player", "idle", "wait", "retreat", "builder", "factory", "fighter"};

void WritePos(CJsonOut& js, const AIFloat3& pos)
{
	js.BeginArr().Val(pos.x).Val(pos.z).EndArr();
}

std::vector<std::string> RoleNamesByType()
{
	std::vector<std::string> names;
	for (const auto& kv : CCircuitDef::GetRoleNames()) {
		const int type = kv.second.type;
		if (type < 0) {
			continue;
		}
		if (static_cast<size_t>(type) >= names.size()) {
			names.resize(type + 1);
		}
		names[type] = kv.first;
	}
	return names;
}

}  // namespace

std::string CBrainServer::MakeHello() const
{
	CJsonOut js;
	js.BeginObj()
		.Field("t", "hello")
		.Field("proto", PROTOCOL_VERSION)
		.Field("ai", circuit->GetSkirmishAIId())
		.Field("team", circuit->GetTeamId())
		.Field("allyTeam", circuit->GetAllyTeamId())
		.Field("side", circuit->GetSideName())
		.Field("frame", circuit->GetLastFrame())
	.EndObj();
	return js.Take();
}

const std::string& CBrainServer::GetMeta()
{
	if (!meta.empty()) {
		return meta;
	}

	CJsonOut js;
	js.BeginObj().Field("t", "meta");

	CMap* map = circuit->GetMap();
	js.Key("map").BeginObj()
		.Field("name", map->GetName())
		.Field("width", CTerrainManager::GetTerrainWidth())
		.Field("height", CTerrainManager::GetTerrainHeight())
	.EndObj();

	// Heightmap averaged down to 4x4 squares (32 elmos per cell)
	{
		const int sqW = map->GetWidth();
		const int sqH = map->GetHeight();
		FloatVec heights;
		map->GetHeightMap(heights);
		constexpr int DS = 4;
		const int w = sqW / DS;
		const int h = sqH / DS;
		std::vector<float> out(w * h, 0.f);
		float minH = 0.f, maxH = 0.f;
		if (heights.size() >= static_cast<size_t>(sqW * sqH) && w > 0 && h > 0) {
			minH = maxH = heights[0];
			for (int z = 0; z < h; ++z) {
				for (int x = 0; x < w; ++x) {
					float sum = 0.f;
					for (int dz = 0; dz < DS; ++dz) {
						for (int dx = 0; dx < DS; ++dx) {
							sum += heights[(z * DS + dz) * sqW + x * DS + dx];
						}
					}
					const float v = sum / (DS * DS);
					out[z * w + x] = v;
					minH = std::min(minH, v);
					maxH = std::max(maxH, v);
				}
			}
		}
		js.Key("heights").BeginObj()
			.Field("w", w).Field("h", h).Field("cell", DS * SQUARE_SIZE)
			.Field("min", minH).Field("max", maxH)
			.Key("data").Floats(out.data(), out.size())
		.EndObj();
	}

	CThreatMap* threatMap = circuit->GetThreatMap();
	js.Key("threatGrid").BeginObj()
		.Field("w", threatMap->GetThreatMapWidth())
		.Field("h", threatMap->GetThreatMapHeight())
		.Field("cell", threatMap->GetSquareSize())
	.EndObj();
	CInfluenceMap* inflMap = circuit->GetInflMap();
	js.Key("inflGrid").BeginObj()
		.Field("w", inflMap->GetWidth())
		.Field("h", inflMap->GetHeight())
		.Field("cell", inflMap->GetSquareSize())
	.EndObj();

	js.Key("roles").BeginArr();
	for (const std::string& name : RoleNamesByType()) {
		js.Val(name);
	}
	js.EndArr();

	js.Key("buildTypes").BeginArr();
	for (int i = 0; i < BrainBuildTypeCount(); ++i) {
		js.Val(BrainBuildTypeName(i));
	}
	js.EndArr();

	js.Key("fightTypes").BeginArr();
	for (const char* name : FIGHT_NAMES) {
		js.Val(name);
	}
	js.EndArr();

	js.Key("taskTypes").BeginArr();
	for (const char* name : TASK_TYPE_NAMES) {
		js.Val(name);
	}
	js.EndArr();

	// [id, name, mainRole, metalCost, isMobile]
	js.Key("defs").BeginArr();
	for (CCircuitDef& cdef : circuit->GetCircuitDefs()) {
		js.BeginArr()
			.Val(cdef.GetId())
			.Val(cdef.GetDef()->GetName())
			.Val(static_cast<int>(cdef.GetMainRole()))
			.Val(cdef.GetCostM())
			.Val(cdef.IsMobile())
		.EndArr();
	}
	js.EndArr();

	CMetalManager* metalMgr = circuit->GetMetalManager();
	// [x, z, income]
	js.Key("metalSpots").BeginArr();
	for (const CMetalData::SMetal& spot : metalMgr->GetSpots()) {
		js.BeginArr().Val(spot.position.x).Val(spot.position.z).Val(spot.income).EndArr();
	}
	js.EndArr();
	// [x, z, radius, income, [spot indices]]
	js.Key("clusters").BeginArr();
	for (const CMetalData::SCluster& cl : metalMgr->GetClusters()) {
		js.BeginArr().Val(cl.position.x).Val(cl.position.z).Val(cl.radius).Val(cl.income).BeginArr();
		for (int idx : cl.idxSpots) {
			js.Val(idx);
		}
		js.EndArr().EndArr();
	}
	js.EndArr();
	// [clusterA, clusterB]
	js.Key("clusterEdges").BeginArr();
	const CMetalData::ClusterGraph& graph = metalMgr->GetClusterGraph();
	for (CMetalData::ClusterGraph::EdgeIt e(graph); e != lemon::INVALID; ++e) {
		js.BeginArr().Val(graph.id(graph.u(e))).Val(graph.id(graph.v(e))).EndArr();
	}
	js.EndArr();

	js.EndObj();
	meta = js.Take();
	return meta;
}

std::string CBrainServer::MakeState(int frame)
{
	InitTunables();
	CJsonOut js;
	js.BeginObj()
		.Field("t", "state")
		.Field("frame", frame)
		.Field("droppedDecisions", droppedTraces);
	droppedTraces = 0;

	CSetupManager* setupMgr = circuit->GetSetupManager();
	CEnemyManager* enemyMgr = circuit->GetEnemyManager();
	js.Key("basePos"); WritePos(js, setupMgr->GetBasePos());
	js.Key("startPos"); WritePos(js, setupMgr->GetStartPos());
	js.Key("enemyPos"); WritePos(js, enemyMgr->GetEnemyPos());

	WriteEconomy(js);
	WriteUnits(js, frame);
	WriteTasks(js);
	WriteEnemies(js);
	WriteLayers(js);

	std::string open;
	CMetalManager* metalMgr = circuit->GetMetalManager();
	const size_t spotCount = metalMgr->GetSpots().size();
	open.reserve(spotCount);
	for (size_t i = 0; i < spotCount; ++i) {
		open += metalMgr->IsOpenSpot(static_cast<int>(i)) ? '1' : '0';
	}
	js.Field("openSpots", open);

	js.Key("control");
	WriteControl(js);

	js.EndObj();
	return js.Take();
}

void CBrainServer::WriteEconomy(CJsonOut& js) const
{
	CEconomyManager* eco = circuit->GetEconomyManager();
	js.Key("eco").BeginObj()
		.Field("mInc", eco->GetAvgMetalIncome())
		.Field("eInc", eco->GetAvgEnergyIncome())
		.Field("mCur", eco->GetMetalCur())
		.Field("mStore", eco->GetMetalStore())
		.Field("mPull", eco->GetMetalPull())
		.Field("eCur", eco->GetEnergyCur())
		.Field("eStore", eco->GetEnergyStore())
		.Field("ecoFactor", eco->GetEcoFactor())
		.Field("excessed", eco->IsExcessed())
		.Field("metalEmpty", eco->IsMetalEmpty())
		.Field("metalFull", eco->IsMetalFull())
		.Field("energyStalling", eco->IsEnergyStalling())
		.Field("energyEmpty", eco->IsEnergyEmpty())
		.Field("energyFull", eco->IsEnergyFull())
	.EndObj();
}

void CBrainServer::WriteUnits(CJsonOut& js, int frame) const
{
	// [id, defId, x, z, health 0..1, taskId]
	js.Key("units").BeginArr();
	for (const auto& kv : circuit->GetTeamUnits()) {
		CCircuitUnit* unit = kv.second;
		const AIFloat3& pos = unit->GetPos(frame);
		js.BeginArr()
			.Val(unit->GetId())
			.Val(unit->GetCircuitDef()->GetId())
			.Val(pos.x).Val(pos.z)
			.Val(unit->GetHealthPercent())
			.Val(BrainTaskId(unit->GetTask()))
		.EndArr();
	}
	js.EndArr();
}

void CBrainServer::WriteTasks(CJsonOut& js) const
{
	std::unordered_set<const IUnitTask*> seen;
	std::vector<IUnitTask*> tasks;
	auto add = [&](IUnitTask* t) {
		if ((t != nullptr) && !t->IsDead() && seen.insert(t).second) {
			tasks.push_back(t);
		}
	};
	for (const auto& kv : circuit->GetTeamUnits()) {
		add(kv.second->GetTask());
	}
	// Queued but unassigned work is part of the decision space too
	CBuilderManager* builderMgr = circuit->GetBuilderManager();
	for (int bt = 0; bt < static_cast<int>(IBuilderTask::BuildType::_SIZE_); ++bt) {
		for (IBuilderTask* t : builderMgr->GetTasks(static_cast<IBuilderTask::BuildType>(bt))) {
			add(t);
		}
	}
	for (CRecruitTask* t : circuit->GetFactoryManager()->GetTasks()) {
		add(t);
	}
	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	for (int ft = 0; ft < static_cast<int>(IFighterTask::FightType::_SIZE_); ++ft) {
		for (IFighterTask* t : militaryMgr->GetTasks(static_cast<IFighterTask::FightType>(ft))) {
			add(t);
		}
	}

	js.Key("tasks").BeginArr();
	for (IUnitTask* task : tasks) {
		js.BeginObj()
			.Field("id", BrainTaskId(task))
			.Field("type", static_cast<int>(task->GetType()))
			.Field("prio", static_cast<int>(task->GetPriority()))
			.Field("state", static_cast<int>(task->GetState()))
			.Field("n", static_cast<int>(task->GetAssignees().size()));

		if (auto* bt = dynamic_cast<IBuilderTask*>(task)) {
			js.Field("build", static_cast<int>(bt->GetBuildType()));
			js.Key("pos"); WritePos(js, bt->GetPosition());
			if (bt->GetBuildDef() != nullptr) {
				js.Field("def", bt->GetBuildDef()->GetId());
			}
			js.Field("costM", bt->GetCostM());
			js.Field("progressM", bt->GetBuildPowerM());
			if (auto* rt = dynamic_cast<CRecruitTask*>(bt)) {
				js.Field("recruit", static_cast<int>(rt->GetRecruitType()));
			}
		} else if (auto* ft = dynamic_cast<IFighterTask*>(task)) {
			js.Field("fight", static_cast<int>(ft->GetFightType()));
			js.Key("pos"); WritePos(js, ft->GetPosition());
			js.Field("power", ft->GetAttackPower());
			if (ft->GetTarget() != nullptr) {
				js.Field("target", ft->GetTarget()->GetId());
				js.Key("targetPos"); WritePos(js, ft->GetTarget()->GetPos());
			}
			if (auto* sq = dynamic_cast<ISquadTask*>(task)) {
				if (sq->GetLeader() != nullptr) {
					js.Field("leader", sq->GetLeader()->GetId());
				}
			}
			if (auto* dt = dynamic_cast<CDefendTask*>(task)) {
				// A gathering squad: becomes a `promote` squad at `promoteAt` power
				js.Field("promote", static_cast<int>(dt->GetPromote()));
				js.Field("promoteAt", dt->GetPromotePower());
			}
		}
		js.EndObj();
	}
	js.EndArr();
}

void CBrainServer::WriteEnemies(CJsonOut& js) const
{
	CEnemyManager* enemyMgr = circuit->GetEnemyManager();

	// [id (-1 fake), defId (-1 unknown), x, z, losBits, cost]
	js.Key("enemies").BeginArr();
	for (const SEnemyData& e : enemyMgr->GetHostileDatas()) {
		js.BeginArr()
			.Val(e.id)
			.Val((e.cdef != nullptr) ? e.cdef->GetId() : -1)
			.Val(e.pos.x).Val(e.pos.z)
			.Val(static_cast<int>(e.losStatus))
			.Val(e.cost)
		.EndArr();
	}
	js.EndArr();

	// [x, z, cost, influence, vagueMetric, unitCount]
	js.Key("groups").BeginArr();
	for (const CEnemyManager::SEnemyGroup& g : enemyMgr->GetEnemyGroups()) {
		js.BeginArr()
			.Val(g.pos.x).Val(g.pos.z)
			.Val(g.cost).Val(g.influence).Val(g.vagueMetric)
			.Val(static_cast<int>(g.units.size()))
		.EndArr();
	}
	js.EndArr();

	CMilitaryManager* militaryMgr = circuit->GetMilitaryManager();
	// roleInfos and enemyInfos are both indexed by role type, sized to the role count
	const CCircuitDef::RoleT roleCount = CCircuitDef::GetRoleNames().size();
	js.Key("military").BeginObj()
		.Field("armyCost", militaryMgr->GetArmyCost())
		.Field("enemyMobileThreat", enemyMgr->GetMobileThreat())
		.Field("enemyStaticThreat", enemyMgr->GetStaticThreat())
		.Field("enemyMobileCost", enemyMgr->GetEnemyMobileCost())
		.Field("airValid", enemyMgr->IsAirValid());
	js.Key("roleCost").BeginArr();
	for (CCircuitDef::RoleT r = 0; r < roleCount; ++r) {
		js.Val(militaryMgr->GetRoleCost(r));
	}
	js.EndArr();
	js.Key("enemyRoleCost").BeginArr();
	for (CCircuitDef::RoleT r = 0; r < roleCount; ++r) {
		js.Val(enemyMgr->GetEnemyCost(r));
	}
	js.EndArr();
	js.EndObj();
}

void CBrainServer::WriteLayers(CJsonOut& js) const
{
	const CThreatMap* threatMap = circuit->GetThreatMap();
	const CInfluenceMap* inflMap = circuit->GetInflMap();

	auto layer = [&js](const char* name, const FloatVec& v) {
		js.Key(name).Floats(v.data(), v.size());
	};
	js.Key("layers").BeginObj();
	layer("threatAir", threatMap->GetDefAirThreat());
	layer("threatSurf", threatMap->GetDefSurfThreat());
	layer("threatAmph", threatMap->GetDefAmphThreat());
	layer("threatCloak", threatMap->GetCloakThreat());
	layer("inflEnemy", inflMap->GetEnemyInflLayer());
	layer("inflAlly", inflMap->GetAllyInflLayer());
	layer("inflAllyDefend", inflMap->GetAllyDefendInflLayer());
	layer("influence", inflMap->GetInfluenceLayer());
	js.EndObj();
}

} // namespace circuit
