/*
 * BrainEventUtil.h
 *
 * Helpers shared by BrainEvents.cpp and BrainAlerts.cpp.
 */

#ifndef SRC_CIRCUIT_BRAIN_BRAINEVENTUTIL_H_
#define SRC_CIRCUIT_BRAIN_BRAINEVENTUTIL_H_

#include "brain/JsonOut.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace circuit {
namespace brain {

inline const char* const SIDE_NAMES[] = {"unknown", "ours", "ally", "enemy"};

// Negative weapon ids name engine damage sources, see CSolidObject::DamageType
inline const char* SpecialDamageName(int weaponDefId)
{
	switch (-weaponDefId) {
		case 2: return "ground collision";
		case 3: return "collision";
		case 4: return "fire";
		case 5: return "water";
		case 7: return "crushed";
		case 8: return "aircraft crash";
		case 17: return "unit script";
		case 18: return "kamikaze";
		default: return "unknown";
	}
}

// Engine kills that are not combat losses: reclaim, self-destruct timer,
// cheats, a factory cancelling its build
inline bool IsDeliberateKill(int weaponDefId)
{
	return (weaponDefId == -10) || (weaponDefId == -11) || (weaponDefId == -12) || (weaponDefId == -16);
}

inline void WritePos(CJsonOut& js, float x, float z)
{
	js.BeginArr().Val(std::round(x)).Val(std::round(z)).EndArr();
}

// Largest count first
inline std::vector<std::pair<int, int>> SortedCounts(const std::map<int, int>& counts)
{
	std::vector<std::pair<int, int>> sorted(counts.begin(), counts.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
	return sorted;
}

// [[key, count]...], largest count first
inline void WriteCounts(CJsonOut& js, const std::map<int, int>& counts)
{
	js.BeginArr();
	for (const auto& kv : SortedCounts(counts)) {
		js.BeginArr().Val(kv.first).Val(kv.second).EndArr();
	}
	js.EndArr();
}

}  // namespace brain
}  // namespace circuit

#endif // SRC_CIRCUIT_BRAIN_BRAINEVENTUTIL_H_
