#pragma once;

#include <Common.h>
#include "MicroManager.h"

namespace UAlbertaBot
{
class MicroRanged : public MicroManager
{
public:

	MicroRanged();

	void executeMicro(const BWAPI::Unitset & targets);
	void assignTargets(const BWAPI::Unitset & targets);

	int getAttackPriority(BWAPI::Unit rangedUnit, BWAPI::Unit target);
	BWAPI::Unit getTarget(BWAPI::Unit rangedUnit, const BWAPI::Unitset & targets);

	bool stayHomeUntilReady(const BWAPI::Unit u) const;
};
}