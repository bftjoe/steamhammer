#include "UnitUtil.h"

using namespace UAlbertaBot;

// Building morphed from another, not constructed.
bool UnitUtil::IsMorphedBuildingType(BWAPI::UnitType type)
{
	return
		type == BWAPI::UnitTypes::Zerg_Sunken_Colony ||
		type == BWAPI::UnitTypes::Zerg_Spore_Colony ||
		type == BWAPI::UnitTypes::Zerg_Lair ||
		type == BWAPI::UnitTypes::Zerg_Hive ||
		type == BWAPI::UnitTypes::Zerg_Greater_Spire;
}

bool UnitUtil::IsCombatUnit(BWAPI::Unit unit)
{
    UAB_ASSERT(unit != nullptr, "Unit was null");
    if (!unit)
    {
        return false;
    }

    // no workers or buildings allowed
    if (unit && unit->getType().isWorker() || unit->getType().isBuilding())
    {
        return false;
    }

    // check for various types of combat units
    if (unit->getType().canAttack() || 
        unit->getType() == BWAPI::UnitTypes::Terran_Medic ||
        unit->getType() == BWAPI::UnitTypes::Protoss_High_Templar ||
        unit->getType() == BWAPI::UnitTypes::Protoss_Observer ||
        unit->isFlying() && unit->getType().spaceProvided() > 0)
    {
        return true;
	}

	return false;
}

bool UnitUtil::IsValidUnit(BWAPI::Unit unit)
{
	return unit
		&& unit->exists()
		&& (unit->isCompleted() || IsMorphedBuildingType(unit->getType()))
		&& unit->getHitPoints() > 0
		&& unit->getType() != BWAPI::UnitTypes::Unknown
		&& unit->getPosition().isValid();
}

Rect UnitUtil::GetRect(BWAPI::Unit unit)
{
	Rect r;

	r.x = unit->getLeft();
	r.y = unit->getTop();
	r.height = unit->getBottom() - unit->getTop();
	r.width = unit->getLeft() - unit->getRight();

	return r;
}

double UnitUtil::GetDistanceBetweenTwoRectangles(Rect & rect1, Rect & rect2)
{
	Rect & mostLeft = rect1.x < rect2.x ? rect1 : rect2;
	Rect & mostRight = rect2.x < rect1.x ? rect1 : rect2;
	Rect & upper = rect1.y < rect2.y ? rect1 : rect2;
	Rect & lower = rect2.y < rect1.y ? rect1 : rect2;

	int diffX = std::max(0, mostLeft.x == mostRight.x ? 0 : mostRight.x - (mostLeft.x + mostLeft.width));
	int diffY = std::max(0, upper.y == lower.y ? 0 : lower.y - (upper.y + upper.height));

	return std::sqrtf(static_cast<float>(diffX*diffX + diffY*diffY));
}

bool UnitUtil::CanAttack(BWAPI::Unit attacker, BWAPI::Unit target)
{
	return GetWeapon(attacker, target) != BWAPI::UnitTypes::None;
}

bool UnitUtil::CanAttackAir(BWAPI::Unit unit)
{
	return unit->getType().airWeapon() != BWAPI::WeaponTypes::None;
}

bool UnitUtil::CanAttackGround(BWAPI::Unit unit)
{
	return unit->getType().groundWeapon() != BWAPI::WeaponTypes::None;
}

double UnitUtil::CalculateLTD(BWAPI::Unit attacker, BWAPI::Unit target)
{
	BWAPI::WeaponType weapon = GetWeapon(attacker, target);

	if (weapon == BWAPI::WeaponTypes::None || weapon.damageCooldown() <= 0)
	{
		return 0;
	}

	return double(weapon.damageAmount()) / weapon.damageCooldown();
}

BWAPI::WeaponType UnitUtil::GetWeapon(BWAPI::Unit attacker, BWAPI::Unit target)
{
	return target->isFlying() ? attacker->getType().airWeapon() : attacker->getType().groundWeapon();
}

BWAPI::WeaponType UnitUtil::GetWeapon(BWAPI::UnitType attacker, BWAPI::UnitType target)
{
	return target.isFlyer() ? attacker.airWeapon() : attacker.groundWeapon();
}

// Tries to take possible range upgrades into account (not quite perfectly).
// Unused.
int UnitUtil::GetAttackRange(BWAPI::Unit attacker, BWAPI::Unit target)
{
	BWAPI::WeaponType weapon = GetWeapon(attacker, target);

	if (weapon == BWAPI::WeaponTypes::None)
	{
		return 0;
	}

	int range = weapon.maxRange();

	// Count range upgrades,
	// for ourselves if we have researched it,
	// for the enemy always (by pessimistic assumption).
	// TODO can we find out the enemy upgrades?
	if (attacker->getType() == BWAPI::UnitTypes::Protoss_Dragoon)
	{
		if (attacker->getPlayer() == BWAPI::Broodwar->enemy() ||
			BWAPI::Broodwar->self()->getUpgradeLevel(BWAPI::UpgradeTypes::Singularity_Charge))
		{
			range = 6 * 32;
		}
	}
	else if (attacker->getType() == BWAPI::UnitTypes::Terran_Marine)
	{
		if (attacker->getPlayer() == BWAPI::Broodwar->enemy() ||
			BWAPI::Broodwar->self()->getUpgradeLevel(BWAPI::UpgradeTypes::U_238_Shells))
		{
			range = 5 * 32;
		}
	}
	else if (attacker->getType() == BWAPI::UnitTypes::Terran_Bunker)
	{
		if (attacker->getPlayer() == BWAPI::Broodwar->enemy() ||
			BWAPI::Broodwar->self()->getUpgradeLevel(BWAPI::UpgradeTypes::U_238_Shells))
		{
			range = 6 * 32;
		}
		else
		{
			range = 5 * 32;
		}
	}
	else if (attacker->getType() == BWAPI::UnitTypes::Terran_Goliath && target->isFlying())
	{
		if (attacker->getPlayer() == BWAPI::Broodwar->enemy() ||
			BWAPI::Broodwar->self()->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters))
		{
			range = 8 * 32;
		}
	}
	else if (attacker->getType() == BWAPI::UnitTypes::Zerg_Hydralisk)
	{
		if (attacker->getPlayer() == BWAPI::Broodwar->enemy() ||
			BWAPI::Broodwar->self()->getUpgradeLevel(BWAPI::UpgradeTypes::Grooved_Spines))
		{
			range = 5 * 32;
		}
	}

    return range;
}

int UnitUtil::GetAttackRangeAssumingUpgrades(BWAPI::UnitType attacker, BWAPI::UnitType target)
{
    BWAPI::WeaponType weapon = GetWeapon(attacker, target);

    if (weapon == BWAPI::WeaponTypes::None)
    {
        return 0;
    }

    int range = weapon.maxRange();

	// Assume that any upgrades are researched.
	if (attacker == BWAPI::UnitTypes::Protoss_Dragoon)
	{
		range = 6 * 32;
	}
	else if (attacker == BWAPI::UnitTypes::Terran_Marine)
	{
		range = 5 * 32;
	}
	else if (attacker == BWAPI::UnitTypes::Terran_Bunker)
	{
		range = 6 * 32;
	}
	else if (attacker == BWAPI::UnitTypes::Terran_Goliath && target.isFlyer())
	{
		range = 8 * 32;
	}
	else if (attacker == BWAPI::UnitTypes::Zerg_Hydralisk)
	{
		range = 5 * 32;
	}

	return range;
}

// All our units, whether completed or not.
size_t UnitUtil::GetAllUnitCount(BWAPI::UnitType type)
{
    size_t count = 0;
    for (const auto & unit : BWAPI::Broodwar->self()->getUnits())
    {
        // trivial case: unit which exists matches the type
        if (unit->getType() == type)
        {
            count++;
        }

        // case where a zerg egg contains the unit type
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Egg && unit->getBuildType() == type)
        {
            count += type.isTwoUnitsInOneEgg() ? 2 : 1;
        }

        // case where a building has started constructing a unit but it doesn't yet have a unit associated with it
        if (unit->getRemainingTrainTime() > 0)
        {
            BWAPI::UnitType trainType = unit->getLastCommand().getUnitType();

            if (trainType == type && unit->getRemainingTrainTime() == trainType.buildTime())
            {
                count++;
            }
        }
    }

    return count;
}

// Only our completed units.
size_t UnitUtil::GetCompletedUnitCount(BWAPI::UnitType type)
{
	size_t count = 0;
	for (const auto & unit : BWAPI::Broodwar->self()->getUnits())
	{
		if (unit->getType() == type && unit->isCompleted())
		{
			count++;
		}
	}

	return count;
}

BWAPI::Unit UnitUtil::GetClosestUnitTypeToTarget(BWAPI::UnitType type, BWAPI::Position target)
{
	BWAPI::Unit closestUnit = nullptr;
	double closestDist = 100000;

	for (auto & unit : BWAPI::Broodwar->self()->getUnits())
	{
		if (unit->getType() == type)
		{
			double dist = unit->getDistance(target);
			if (!closestUnit || dist < closestDist)
			{
				closestUnit = unit;
				closestDist = dist;
			}
		}
	}

	return closestUnit;
}
