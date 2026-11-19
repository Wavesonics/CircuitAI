/*
 * SquadTask.h
 *
 *  Created on: Jan 23, 2016
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_SQUADTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_SQUADTASK_H_

#include "task/fighter/FighterTask.h"
#include "unit/CoreUnit.h"
#include "terrain/path/MicroPather.h"

#include <memory>

namespace circuit {

class ISquadTask: public IFighterTask {
protected:
	ISquadTask(ITaskModule* mgr, FightType type, float powerMod);
public:
	virtual ~ISquadTask();

	virtual void AssignTo(CCircuitUnit* unit) override;
	virtual void RemoveAssignee(CCircuitUnit* unit) override;

	virtual void Merge(ISquadTask* task);
	virtual void CheckMembers() override;

	const std::map<float, std::set<CCircuitUnit*>>& GetRangeUnits() const { return rangeUnits; }

	CCircuitUnit* GetLeader() const { return leader; }
	const springai::AIFloat3& GetLeaderPos(int frame) const;

private:
	void FindLeader(decltype(units)::iterator itBegin, decltype(units)::iterator itEnd);

	bool IsMergeSafe() const;
	ISquadTask* CheckMergeTask();

protected:
	ISquadTask* GetMergeTask();
	bool IsMustRegroup();
	void ActivePath(float speed = NO_SPEED_LIMIT);
	NSMicroPather::HitFunc GetHitTest() const;
	// Logs (once per unit) an assignee that lost its travel action or belongs to
	// another task. Squad code dereferences GetTravelAct() for every assignee,
	// so such a unit would crash the engine.
	void ReportBrokenMember(CCircuitUnit* unit, const char* where);
	// Drops a unit that another task owns without touching the unit itself
	void ForgetMember(CCircuitUnit* unit);
	void Attack(const int frame);
	void Attack(const int frame, const bool isGround);

	float lowestRange;
	float highestRange;
	float lowestSpeed;
	float highestSpeed;
	// NOTE: Using unit instead of area directly may save from processing UpdateAreaUsers
	CCircuitUnit* leader;  // slowest, weakest unit, true leader
	springai::AIFloat3 groupPos;
	springai::AIFloat3 prevGroupPos;
	std::shared_ptr<CPathInfo> pPath;

	std::map<float, std::set<CCircuitUnit*>> rangeUnits;

private:
	std::set<ICoreUnit::Id> reportedMembers;

protected:
	int groupFrame;
	int attackFrame;

#ifdef DEBUG_VIS
public:
	virtual void Log() override;
#endif
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_SQUADTASK_H_
