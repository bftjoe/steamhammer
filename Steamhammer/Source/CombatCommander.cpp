#include <queue>

#include "CombatCommander.h"

#include "Bases.h"
#include "Micro.h"
#include "Random.h"
#include "UnitUtil.h"

using namespace UAlbertaBot;

// Squad priorities: Which can steal units from others.
// Anyone can steal from the Idle squad.
const size_t IdlePriority = 0;
const size_t OverlordPriority = 1;
const size_t AttackPriority = 2;
const size_t ReconPriority = 3;
const size_t BaseDefensePriority = 4;
const size_t ScoutDefensePriority = 5;
const size_t DropPriority = 6;         // don't steal from Drop squad for anything else

// The attack squads.
const int DefendFrontRadius = 400;
const int AttackRadius = 800;

// Reconnaissance squad.
const int ReconTargetTimeout = 40 * 24;
const int ReconRadius = 400;

CombatCommander::CombatCommander() 
	: the(The::Root())
	, _initialized(false)
	, _goAggressive(true)
	, _reconTarget(BWAPI::Positions::Invalid)   // it will be changed later
	, _lastReconTargetChange(0)
{
}

// Called once at the start of the game.
// You can also create new squads at other times.
void CombatCommander::initializeSquads()
{
	// The idle squad includes workers at work (not idle at all) and unassigned overlords.
    SquadOrder idleOrder(SquadOrderTypes::Idle, BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation()), 100, "Chill out");
	_squadData.addSquad(Squad("Idle", idleOrder, IdlePriority));

    // The overlord squad doesn't care what order it is given.
    // It analyzes the situation for itself. (The regular squad orders make no sense for it.)
	if (BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Zerg)
	{
		SquadOrder ovieOrder(SquadOrderTypes::Idle, BWAPI::Positions::Origin, 0, "React");
		_squadData.addSquad(Squad("Overlord", ovieOrder, OverlordPriority));
	}
    
    // The ground squad will pressure an enemy base.
	SquadOrder attackOrder(getAttackOrder(nullptr));
	_squadData.addSquad(Squad("Ground", attackOrder, AttackPriority));

	// The flying squad separates air units so they can act independently.
	_squadData.addSquad(Squad("Flying", attackOrder, AttackPriority));

	// The recon squad carries out reconnaissance in force to deny enemy bases.
	// It is filled in when enough units are available.
	Squad & reconSquad = Squad("Recon", idleOrder, ReconPriority);
	reconSquad.setCombatSimRadius(200);  // combat sim includes units in a smaller radius than for a combat squad
	reconSquad.setFightVisible(true);    // combat sim sees only visible enemy units (not all known enemies)
	_squadData.addSquad(reconSquad);

	BWAPI::Position ourBasePosition = BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation());

    // the scout defense squad will handle chasing the enemy worker scout
	if (Config::Micro::ScoutDefenseRadius > 0)
	{
		SquadOrder enemyScoutDefense(SquadOrderTypes::Defend, ourBasePosition, Config::Micro::ScoutDefenseRadius, "Get the scout");
		_squadData.addSquad(Squad("ScoutDefense", enemyScoutDefense, ScoutDefensePriority));
	}

	// If we're expecting to drop, create a drop squad.
	// It is initially ordered to hold ground until it can load up and go.
	if (StrategyManager::Instance().dropIsPlanned())
    {
		SquadOrder doDrop(SquadOrderTypes::Hold, ourBasePosition, AttackRadius, "Wait for transport");
		_squadData.addSquad(Squad("Drop", doDrop, DropPriority));
    }
}

void CombatCommander::update(const BWAPI::Unitset & combatUnits)
{
    if (!_initialized)
    {
        initializeSquads();
		_initialized = true;
	}

    _combatUnits = combatUnits;

	int frame8 = BWAPI::Broodwar->getFrameCount() % 8;

	if (frame8 == 1)
	{
		updateIdleSquad();
		updateOverlordSquad();
		updateDropSquads();
		updateScoutDefenseSquad();
		updateBaseDefenseSquads();
		updateReconSquad();
		updateAttackSquads();
	}
	else if (frame8 % 4 == 2)
	{
		doComsatScan();
	}

	loadOrUnloadBunkers();

	_squadData.update();          // update() all the squads

	cancelDyingItems();
}

void CombatCommander::updateIdleSquad()
{
    Squad & idleSquad = _squadData.getSquad("Idle");
    for (const auto unit : _combatUnits)
    {
        // if it hasn't been assigned to a squad yet, put it in the low priority idle squad
        if (_squadData.canAssignUnitToSquad(unit, idleSquad))
        {
			_squadData.assignUnitToSquad(unit, idleSquad);
		}
    }
}

// Put all overlords which are not otherwise assigned into the Overlord squad.
void CombatCommander::updateOverlordSquad()
{
	// If we don't have an overlord squad, then do nothing.
	// It is created in initializeSquads().
	if (!_squadData.squadExists("Overlord"))
	{
		return;
	}

	Squad & ovieSquad = _squadData.getSquad("Overlord");
	for (const auto unit : _combatUnits)
	{
		if (unit->getType() == BWAPI::UnitTypes::Zerg_Overlord && _squadData.canAssignUnitToSquad(unit, ovieSquad))
		{
			_squadData.assignUnitToSquad(unit, ovieSquad);
		}
	}
}

// Update the small recon squad which tries to find and deny enemy bases.
// Units available to the recon squad each have a "weight".
// Weights sum to no more than maxWeight, set below.
void CombatCommander::updateReconSquad()
{
	const int maxWeight = 12;
	Squad & reconSquad = _squadData.getSquad("Recon");

	chooseReconTarget();

	// If nowhere needs seeing, disband the squad. We're done.
	if (!_reconTarget.isValid())
	{
		reconSquad.clear();
		return;
	}

	// Issue the order.
	SquadOrder reconOrder(SquadOrderTypes::Attack, _reconTarget, ReconRadius, "Reconnaissance in force");
	reconSquad.setSquadOrder(reconOrder);

	// Special case: If we started on an island, then the recon squad consists
	// entirely of one flying detector. (Life is easy for zerg, hard for terran.)
	if (Bases::Instance().isIslandStart())
	{
		if (reconSquad.getUnits().size() == 0)
		{
			for (const auto unit : _combatUnits)
			{
				if (unit->getType().isDetector() && _squadData.canAssignUnitToSquad(unit, reconSquad))
				{
					_squadData.assignUnitToSquad(unit, reconSquad);
					break;
				}
			}
		}
		return;
	}

	// What is already in the squad?
	int squadWeight = 0;
	int nMarines = 0;
	int nMedics = 0;
	for (const auto unit : reconSquad.getUnits())
	{
		squadWeight += weighReconUnit(unit);
		if (unit->getType() == BWAPI::UnitTypes::Terran_Marine)
		{
			++nMarines;
		}
		else if (unit->getType() == BWAPI::UnitTypes::Terran_Medic)
		{
			++nMedics;
		}
	}

	// If everything except the detector is gone, let the detector go too.
	// It can't carry out reconnaissance in force.
	if (squadWeight == 0 && !reconSquad.isEmpty())
	{
		reconSquad.clear();
	}

	// What is available to put into the squad?
	int availableWeight = 0;
	for (const auto unit : _combatUnits)
	{
		availableWeight += weighReconUnit(unit);
	}

	// The allowed weight of the recon squad. It should steal few units.
	int weightLimit = availableWeight >= 24
		? 2 + (availableWeight - 24) / 6
		: 0;
	if (weightLimit > maxWeight)
	{
		weightLimit = maxWeight;
	}

	// If the recon squad weighs more than it should, clear it and continue.
	// Also if all marines are gone, but medics remain.
	// Units will be added back in if they should be.
	if (squadWeight > weightLimit ||
		nMarines == 0 && nMedics > 0)
	{
		reconSquad.clear();
		squadWeight = 0;
		nMarines = nMedics = 0;
	}

	bool hasDetector = reconSquad.hasDetector();
	bool wantDetector = wantSquadDetectors();

	if (hasDetector && !wantDetector)
	{
		for (BWAPI::Unit unit : reconSquad.getUnits())
		{
			if (unit->getType().isDetector())
			{
				reconSquad.removeUnit(unit);
				break;
			}
		}
		hasDetector = false;
	}

	// Add units up to the weight limit.
	// In this loop, add no medics, and few enough marines to allow for 2 medics.
	for (const auto unit : _combatUnits)
	{
		if (squadWeight >= weightLimit)
		{
			break;
		}
		BWAPI::UnitType type = unit->getType();
		int weight = weighReconUnit(type);
		if (weight > 0 && squadWeight + weight <= weightLimit && _squadData.canAssignUnitToSquad(unit, reconSquad))
		{
			if (type == BWAPI::UnitTypes::Terran_Marine)
			{
				if (nMarines * weight < maxWeight - 2 * weighReconUnit(BWAPI::UnitTypes::Terran_Medic))
				{
					_squadData.assignUnitToSquad(unit, reconSquad);
					squadWeight += weight;
					nMarines += 1;
				}
			}
			else if (type != BWAPI::UnitTypes::Terran_Medic)
			{
				_squadData.assignUnitToSquad(unit, reconSquad);
				squadWeight += weight;
			}
		}
		else if (type.isDetector() && wantDetector && !hasDetector && _squadData.canAssignUnitToSquad(unit, reconSquad))
		{
			_squadData.assignUnitToSquad(unit, reconSquad);
			hasDetector = true;
		}
	}

	// Finally, fill in any needed medics.
	if (nMarines > 0 && nMedics < 2)
	{
		for (const auto unit : _combatUnits)
		{
			if (squadWeight >= weightLimit || nMedics >= 2)
			{
				break;
			}
			if (unit->getType() == BWAPI::UnitTypes::Terran_Medic &&
				_squadData.canAssignUnitToSquad(unit, reconSquad))
			{
				_squadData.assignUnitToSquad(unit, reconSquad);
				squadWeight += weighReconUnit(BWAPI::UnitTypes::Terran_Medic);
				nMedics += 1;
			}
		}
	}
}

// The recon squad is allowed up to a certain "weight" of units.
int CombatCommander::weighReconUnit(const BWAPI::Unit unit) const
{
	return weighReconUnit(unit->getType());
}

// The recon squad is allowed up to a certain "weight" of units.
int CombatCommander::weighReconUnit(const BWAPI::UnitType type) const
{
	if (type == BWAPI::UnitTypes::Zerg_Zergling) return 2;
	if (type == BWAPI::UnitTypes::Zerg_Hydralisk) return 3;
	if (type == BWAPI::UnitTypes::Terran_Marine) return 2;
	if (type == BWAPI::UnitTypes::Terran_Medic) return 2;
	if (type == BWAPI::UnitTypes::Terran_Vulture) return 4;
	if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) return 6;
	if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) return 6;
	if (type == BWAPI::UnitTypes::Protoss_Zealot) return 4;
	if (type == BWAPI::UnitTypes::Protoss_Dragoon) return 4;
	if (type == BWAPI::UnitTypes::Protoss_Dark_Templar) return 4;

	return 0;
}

// Keep the same reconnaissance target or switch to a new one, depending.
void CombatCommander::chooseReconTarget()
{
	bool change = false;       // switch targets?

	BWAPI::Position nextTarget = getReconLocation();

	// There is nowhere that we need to see. Change to the invalid target.
	if (!nextTarget.isValid())
	{
		change = true;
	}

	// If the current target is invalid, we're starting up.
	else if (!_reconTarget.isValid())
	{
		change = true;
	}

	// If we have spent too long on one target, then probably we haven't been able to reach it.
	else if (BWAPI::Broodwar->getFrameCount() - _lastReconTargetChange >= ReconTargetTimeout)
	{
		change = true;
	}

	// If the target is in sight (of any unit, not only the recon squad) and empty of enemies, we're done.
	else if (BWAPI::Broodwar->isVisible(_reconTarget.x / 32, _reconTarget.y / 32))
	{
		BWAPI::Unitset enemies;
		MapGrid::Instance().getUnits(enemies, _reconTarget, ReconRadius, false, true);
		// We don't particularly care about air units, even when we could engage them.
		for (auto it = enemies.begin(); it != enemies.end(); )
		{
			if ((*it)->isFlying())
			{
				it = enemies.erase(it);
			}
			else
			{
				++it;
			}
		}
		if (enemies.empty())
		{
			change = true;
		}
	}

	if (change)
	{
		_reconTarget = nextTarget;
		_lastReconTargetChange = BWAPI::Broodwar->getFrameCount();
	}
}

// Choose an empty base location for the recon squad to check out.
// Called only by setReconTarget().
BWAPI::Position CombatCommander::getReconLocation() const
{
	std::vector<Base *> choices;

	BWAPI::Position startPosition = Bases::Instance().myStartingBase()->getPosition();

	// The choices are neutral bases reachable by ground.
	// Or, if we started on an island, the choices are neutral bases anywhere.
	for (Base * base : Bases::Instance().getBases())
	{
		if (base->owner == BWAPI::Broodwar->neutral() &&
			(Bases::Instance().isIslandStart() || Bases::Instance().connectedToStart(base->getTilePosition())))
		{
			choices.push_back(base);
		}
	}

	// BWAPI::Broodwar->printf("%d recon squad choices", choices.size());

	// If there are none, return an invalid position.
	if (choices.empty())
	{
		return BWAPI::Positions::Invalid;
	}

	// Choose randomly.
	// We may choose the same target we already have. That's OK; if there's another choice,
	// we'll probably switch to it soon.
	Base * base = choices.at(Random::Instance().index(choices.size()));
	return base->getPosition();
}

// Form the ground squad and the flying squad, the main attack squads.
// NOTE Arbiters and guardians go into the ground squad.
//      Devourers, scourge, and carriers are flying squad if it exists, otherwise ground.
//      Other air units always go into the flying squad.
void CombatCommander::updateAttackSquads()
{
    Squad & groundSquad = _squadData.getSquad("Ground");
	Squad & flyingSquad = _squadData.getSquad("Flying");

	// If safe, include exactly 1 detector in each squad.
	bool groundDetector = groundSquad.hasDetector();
	bool groundSquadExists = groundSquad.hasCombatUnits();

	bool flyingDetector = flyingSquad.hasDetector();
	bool flyingSquadExists = false;
	for (const auto unit : flyingSquad.getUnits())
	{
		if (isFlyingSquadUnit(unit->getType()))
		{
			flyingSquadExists = true;
			break;
		}
	}

	// No opverlord if it is both in danger and unnecessary to counter cloaked enemies.
	bool wantDetector = wantSquadDetectors();
    
    // Drop detectors that are no longer wanted.
    // The detectors become squadless and will be reassigned on the next cycle.
	if (!wantDetector)
	{
		if (groundDetector)
		{
			for (const auto unit : groundSquad.getUnits())
			{
				if (unit->getType().isDetector())
				{
					groundSquad.removeUnit(unit);
					break;
				}
			}
			groundDetector = false;
		}
		if (flyingDetector)
		{
			for (const auto unit : flyingSquad.getUnits())
			{
				if (unit->getType().isDetector())
				{
					flyingSquad.removeUnit(unit);
					break;
				}
			}
			flyingDetector = false;
		}
	}

    for (const auto unit : _combatUnits)
    {
		// Each squad gets at most 1 detector. Priority to the ground squad which can't see uphill otherwise.
		if (unit->getType().isDetector())
		{
			if (wantDetector)
			{
				if (groundSquadExists && !groundDetector && _squadData.canAssignUnitToSquad(unit, groundSquad))
				{
					groundDetector = true;
					_squadData.assignUnitToSquad(unit, groundSquad);
				}
				else if (flyingSquadExists && !flyingDetector && _squadData.canAssignUnitToSquad(unit, groundSquad))
				{
					flyingDetector = true;
					_squadData.assignUnitToSquad(unit, flyingSquad);
				}
			}
		}

		else if (isFlyingSquadUnit(unit->getType()))
		{
			if (_squadData.canAssignUnitToSquad(unit, flyingSquad))
			{
				_squadData.assignUnitToSquad(unit, flyingSquad);
			}
		}

		// Certain flyers go into the flying squad only if it already exists.
		// Otherwise they go into the ground squad.
		else if (isOptionalFlyingSquadUnit(unit->getType()))
		{
			if (flyingSquadExists)
			{
				if (groundSquad.containsUnit(unit))
				{
					groundSquad.removeUnit(unit);
				}
				if (_squadData.canAssignUnitToSquad(unit, flyingSquad))
				{
					_squadData.assignUnitToSquad(unit, flyingSquad);
				}
			}
			else
			{
				if (flyingSquad.containsUnit(unit))
				{
					flyingSquad.removeUnit(unit);
				}
				if (_squadData.canAssignUnitToSquad(unit, groundSquad))
				{
					_squadData.assignUnitToSquad(unit, groundSquad);
				}
			}
		}

		// isGroundSquadUnit() is defined as a catchall, so it has to go last.
		else if (isGroundSquadUnit(unit->getType()))
		{
			if (_squadData.canAssignUnitToSquad(unit, groundSquad))
			{
				_squadData.assignUnitToSquad(unit, groundSquad);
			}
		}
	}

	SquadOrder groundAttackOrder(getAttackOrder(&groundSquad));
	groundSquad.setSquadOrder(groundAttackOrder);

	SquadOrder flyingAttackOrder(getAttackOrder(&flyingSquad));
	flyingSquad.setSquadOrder(flyingAttackOrder);
}

// Unit definitely belongs in the Flying squad.
bool CombatCommander::isFlyingSquadUnit(const BWAPI::UnitType type) const
{
	return
		type == BWAPI::UnitTypes::Zerg_Mutalisk ||
		type == BWAPI::UnitTypes::Terran_Wraith ||
		type == BWAPI::UnitTypes::Terran_Valkyrie ||
		type == BWAPI::UnitTypes::Terran_Battlecruiser ||
		type == BWAPI::UnitTypes::Protoss_Corsair ||
		type == BWAPI::UnitTypes::Protoss_Scout;
}

// Unit belongs in the Flying squad if the Flying squad exists, otherwise the Ground squad.
bool CombatCommander::isOptionalFlyingSquadUnit(const BWAPI::UnitType type) const
{
	return
		type == BWAPI::UnitTypes::Zerg_Scourge ||
		type == BWAPI::UnitTypes::Zerg_Devourer ||
		type == BWAPI::UnitTypes::Protoss_Carrier;
}

// Unit belongs in the ground squad.
// With the current definition, it includes everything except workers, so it captures
// everything that is not already taken: It should be the last condition checked.
bool CombatCommander::isGroundSquadUnit(const BWAPI::UnitType type) const
{
	return
		!type.isWorker();
}

// Despite the name, this supports only 1 drop squad which has 1 transport.
// Furthermore, it can only drop once and doesn't know how to reset itself to try again.
// Still, it's a start and it can be effective.
void CombatCommander::updateDropSquads()
{
	// If we don't have a drop squad, then we don't want to drop.
	// It is created in initializeSquads().
	if (!_squadData.squadExists("Drop"))
    {
		return;
    }

    Squad & dropSquad = _squadData.getSquad("Drop");

	// The squad is initialized with a Hold order.
	// There are 3 phases, and in each phase the squad is given a different order:
	// Collect units (Hold); load the transport (Load); go drop (Drop).

	if (dropSquad.getSquadOrder().getType() == SquadOrderTypes::Drop)
	{
		// If it has already been told to Drop, we issue a new drop order in case the
		// target has changed.
		/* TODO not yet supported by the drop code
		SquadOrder dropOrder = SquadOrder(SquadOrderTypes::Drop, getDropLocation(dropSquad), 300, "Go drop!");
		dropSquad.setSquadOrder(dropOrder);
		*/
		return;
	}

	// If we get here, we haven't been ordered to Drop yet.

    // What units do we have, what units do we need?
	BWAPI::Unit transportUnit = nullptr;
    int transportSpotsRemaining = 8;      // all transports are the same size
	bool anyUnloadedUnits = false;
	const auto & dropUnits = dropSquad.getUnits();

    for (const auto unit : dropUnits)
    {
		if (unit->exists())
		{
			if (unit->isFlying() && unit->getType().spaceProvided() > 0)
			{
				transportUnit = unit;
			}
			else
			{
				transportSpotsRemaining -= unit->getType().spaceRequired();
				if (!unit->isLoaded())
				{
					anyUnloadedUnits = true;
				}
			}
		}
    }

	if (transportUnit && transportSpotsRemaining == 0)
	{
		if (anyUnloadedUnits)
		{
			// The drop squad is complete. Load up.
			// See Squad::loadTransport().
			SquadOrder loadOrder(SquadOrderTypes::Load, transportUnit->getPosition(), AttackRadius, "Load up");
			dropSquad.setSquadOrder(loadOrder);
		}
		else
		{
			// We're full. Change the order to Drop.
			SquadOrder dropOrder = SquadOrder(SquadOrderTypes::Drop, getDropLocation(dropSquad), 300, "Go drop!");
			dropSquad.setSquadOrder(dropOrder);
		}
	}
	else
    {
		// The drop squad is not complete. Look for more units.
        for (const auto unit : _combatUnits)
        {
            // If the squad doesn't have a transport, try to add one.
			if (!transportUnit &&
				unit->getType().spaceProvided() > 0 && unit->isFlying() &&
				_squadData.canAssignUnitToSquad(unit, dropSquad))
            {
                _squadData.assignUnitToSquad(unit, dropSquad);
				transportUnit = unit;
            }

            // If the unit fits and is good to drop, add it to the squad.
			// Rewrite unitIsGoodToDrop() to select the units of your choice to drop.
			// Simplest to stick to units that occupy the same space in a transport, to avoid difficulties
			// like "add zealot, add dragoon, can't add another dragoon--but transport is not full, can't go".
			else if (unit->getType().spaceRequired() <= transportSpotsRemaining &&
				unitIsGoodToDrop(unit) &&
				_squadData.canAssignUnitToSquad(unit, dropSquad))
            {
				_squadData.assignUnitToSquad(unit, dropSquad);
                transportSpotsRemaining -= unit->getType().spaceRequired();
            }
        }
    }
}

void CombatCommander::updateScoutDefenseSquad() 
{
	if (Config::Micro::ScoutDefenseRadius == 0 || _combatUnits.empty())
    { 
        return; 
    }

    // Get the region of our starting base.
    BWTA::Region * myRegion = BWTA::getRegion(Bases::Instance().myStartingBase()->getTilePosition());
    if (!myRegion || !myRegion->getCenter().isValid())
    {
        return;
    }

    // Get all of the visible enemy units in this region.
	BWAPI::Unitset enemyUnitsInRegion;
    for (const auto unit : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (BWTA::getRegion(unit->getTilePosition()) == myRegion)
        {
            enemyUnitsInRegion.insert(unit);
        }
    }

	Squad & scoutDefenseSquad = _squadData.getSquad("ScoutDefense");

	// Is exactly one enemy worker here?
    bool assignScoutDefender = enemyUnitsInRegion.size() == 1 && (*enemyUnitsInRegion.begin())->getType().isWorker();

	if (assignScoutDefender)
	{
		if (scoutDefenseSquad.isEmpty())
		{
			// The enemy worker to catch.
			BWAPI::Unit enemyWorker = *enemyUnitsInRegion.begin();

			BWAPI::Unit workerDefender = findClosestWorkerToTarget(_combatUnits, enemyWorker);

			if (workerDefender)
			{
				// grab it from the worker manager and put it in the squad
				if (_squadData.canAssignUnitToSquad(workerDefender, scoutDefenseSquad))
				{
					_squadData.assignUnitToSquad(workerDefender, scoutDefenseSquad);
				}
			}
		}
	}
    // Otherwise the squad should be empty. If not, make it so.
    else if (!scoutDefenseSquad.isEmpty())
    {
        scoutDefenseSquad.clear();
    }
}

void CombatCommander::updateBaseDefenseSquads() 
{
	if (_combatUnits.empty()) 
    { 
        return; 
    }
     
    BWTA::BaseLocation * enemyBaseLocation = InformationManager::Instance().getEnemyMainBaseLocation();
    BWTA::Region * enemyRegion = nullptr;
    if (enemyBaseLocation)
    {
        enemyRegion = BWTA::getRegion(enemyBaseLocation->getTilePosition());
    }

	// for each of our occupied regions
	for (BWTA::Region * myRegion : InformationManager::Instance().getOccupiedRegions(BWAPI::Broodwar->self()))
	{
        // don't defend inside the enemy region, this will end badly when we are stealing gas
        if (myRegion == enemyRegion)
        {
            continue;
        }

		BWAPI::Position regionCenter = myRegion->getCenter();
		if (!regionCenter.isValid())
		{
			continue;
		}

		// all of the enemy units in this region
		BWAPI::Unitset enemyUnitsInRegion;
        for (const auto unit : BWAPI::Broodwar->enemy()->getUnits())
        {
            // If it's a harmless air unit, don't worry about it for base defense.
            if (unit->getType() == BWAPI::UnitTypes::Zerg_Overlord ||
				unit->getType() == BWAPI::UnitTypes::Protoss_Observer ||
				unit->isLifted())  // floating terran building
            {
                continue;
            }

            if (BWTA::getRegion(unit->getTilePosition()) == myRegion)
            {
                enemyUnitsInRegion.insert(unit);
            }
        }

        // we ignore the first enemy worker in our region since we assume it is a scout
		// This is because we can't catch it early. Should skip this check when we can.
		// TODO drop this when we are able to catch it
        for (const auto unit : enemyUnitsInRegion)
        {
            if (unit->getType().isWorker())
            {
                enemyUnitsInRegion.erase(unit);
                break;
            }
        }

        std::stringstream squadName;
		BWAPI::TilePosition tile(regionCenter);
		squadName << "Base " << tile.x << "," << tile.y;
        
		// if there's nothing in this region to worry about
        if (enemyUnitsInRegion.empty())
        {
            // if a defense squad for this region exists, empty it
            if (_squadData.squadExists(squadName.str()))
            {
				_squadData.getSquad(squadName.str()).clear();
			}
            
            // Finished with this region, nothing to defend here.
            continue;
        }
        else 
        {
            // if we don't have a squad assigned to this region already, create one
            if (!_squadData.squadExists(squadName.str()))
            {
                SquadOrder defendRegion(SquadOrderTypes::Defend, regionCenter, 32 * 25, "Defend region");
                _squadData.addSquad(Squad(squadName.str(), defendRegion, BaseDefensePriority));
			}
        }

		int numEnemyFlyingInRegion = std::count_if(enemyUnitsInRegion.begin(), enemyUnitsInRegion.end(), [](BWAPI::Unit u) { return u->isFlying(); });
		int numEnemyGroundInRegion = enemyUnitsInRegion.size() - numEnemyFlyingInRegion;
		bool enemyHasAntiAir = std::any_of(enemyUnitsInRegion.begin(), enemyUnitsInRegion.end(), UnitUtil::CanAttackAir);

		// assign units to the squad
		UAB_ASSERT(_squadData.squadExists(squadName.str()), "Squad should exist: %s", squadName.str().c_str());
        Squad & defenseSquad = _squadData.getSquad(squadName.str());

		// A simpleminded way of figuring out how much defense we need.
		const int numDefendersPerEnemyUnit = 2;

	    int flyingDefendersNeeded = numDefendersPerEnemyUnit * numEnemyFlyingInRegion;
	    int groundDefendersNeeded = numDefendersPerEnemyUnit * numEnemyGroundInRegion;

		// Count static defense as air defenders.
		// Ignore bunkers; they're more complicated.
		for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
			if ((unit->getType() == BWAPI::UnitTypes::Terran_Missile_Turret ||
				unit->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
				unit->getType() == BWAPI::UnitTypes::Zerg_Spore_Colony) &&
				unit->isCompleted() && unit->isPowered() &&
				BWTA::getRegion(unit->getTilePosition()) == myRegion)
			{
				flyingDefendersNeeded -= 3;
			}
		}
		flyingDefendersNeeded = std::max(flyingDefendersNeeded, 2);

		// Count static defense as ground defenders.
		// Ignore bunkers; they're more complicated.
		// Cannons are double-counted as air and ground, which can be a mistake.
		bool sunkenDefender = false;
		for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
			if ((unit->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
				unit->getType() == BWAPI::UnitTypes::Zerg_Sunken_Colony) &&
				unit->isCompleted() && unit->isPowered() &&
				BWTA::getRegion(unit->getTilePosition()) == myRegion)
			{
				sunkenDefender = true;
				groundDefendersNeeded -= 4;
			}
		}
		groundDefendersNeeded = std::max(groundDefendersNeeded, 2);

		// Pull workers only in narrow conditions.
		// Pulling workers (as implemented) can lead to big losses.
		bool pullWorkers =
			Config::Micro::WorkersDefendRush &&
			(!sunkenDefender && numZerglingsInOurBase() > 0 || buildingRush());

		updateDefenseSquadUnits(defenseSquad, flyingDefendersNeeded, groundDefendersNeeded, pullWorkers, enemyHasAntiAir);
    }

    // for each of our defense squads, if there aren't any enemy units near the position, clear the squad
	// TODO partially overlaps with "is enemy in region check" above, could cause oscillating behavior
	for (const auto & kv : _squadData.getSquads())
	{
		const Squad & squad = kv.second;
		const SquadOrder & order = squad.getSquadOrder();

		if (order.getType() != SquadOrderTypes::Defend || squad.isEmpty())
		{
			continue;
		}

		bool enemyUnitInRange = false;
		for (const auto unit : BWAPI::Broodwar->enemy()->getUnits())
		{
			if (unit->getDistance(order.getPosition()) < order.getRadius())
			{
				enemyUnitInRange = true;
				break;
			}
		}

		if (!enemyUnitInRange)
		{
			_squadData.getSquad(squad.getName()).clear();
		}
	}
}

void CombatCommander::updateDefenseSquadUnits(Squad & defenseSquad, const size_t & flyingDefendersNeeded, const size_t & groundDefendersNeeded, bool pullWorkers, bool enemyHasAntiAir)
{
	// if there's nothing left to defend, clear the squad
	if (flyingDefendersNeeded == 0 && groundDefendersNeeded == 0)
	{
		defenseSquad.clear();
		return;
	}

	bool hasDetector = defenseSquad.hasDetector();
	bool wantDetector = wantSquadDetectors();

	if (hasDetector && !wantDetector)
	{
		for (BWAPI::Unit unit : defenseSquad.getUnits())
		{
			if (unit->getType().isDetector())
			{
				defenseSquad.removeUnit(unit);
				break;
			}
		}
		hasDetector = false;
	}
	else if (!hasDetector && wantDetector)
	{
		for (BWAPI::Unit unit : _combatUnits)
		{
			if (unit->getType().isDetector() && _squadData.canAssignUnitToSquad(unit, defenseSquad))
			{
				_squadData.assignUnitToSquad(unit, defenseSquad);
				hasDetector = true;
				break;
			}
		}
	}

	const BWAPI::Unitset & squadUnits = defenseSquad.getUnits();

	// NOTE Defenders can be double-counted as both air and ground defenders. It can be a mistake.
	size_t flyingDefendersInSquad = std::count_if(squadUnits.begin(), squadUnits.end(), UnitUtil::CanAttackAir);
	size_t groundDefendersInSquad = std::count_if(squadUnits.begin(), squadUnits.end(), UnitUtil::CanAttackGround);

	// add flying defenders if we still need them
	size_t flyingDefendersAdded = 0;
	BWAPI::Unit defenderToAdd;
	while (flyingDefendersNeeded > flyingDefendersInSquad + flyingDefendersAdded &&
		(defenderToAdd = findClosestDefender(defenseSquad, defenseSquad.getSquadOrder().getPosition(), true, false, enemyHasAntiAir)))
	{
		_squadData.assignUnitToSquad(defenderToAdd, defenseSquad);
		++flyingDefendersAdded;
	}

	// add ground defenders if we still need them
	size_t groundDefendersAdded = 0;
	while (groundDefendersNeeded > groundDefendersInSquad + groundDefendersAdded &&
		(defenderToAdd = findClosestDefender(defenseSquad, defenseSquad.getSquadOrder().getPosition(), false, pullWorkers, enemyHasAntiAir)))
	{
		if (defenderToAdd->getType().isWorker())
		{
			UAB_ASSERT(pullWorkers, "pulled worker defender mistakenly");
			WorkerManager::Instance().setCombatWorker(defenderToAdd);
		}
		_squadData.assignUnitToSquad(defenderToAdd, defenseSquad);
		++groundDefendersAdded;
	}
}

// Choose a defender to join the base defense squad.
BWAPI::Unit CombatCommander::findClosestDefender(const Squad & defenseSquad, BWAPI::Position pos, bool flyingDefender, bool pullWorkers, bool enemyHasAntiAir)
{
	BWAPI::Unit closestDefender = nullptr;
	int minDistance = 99999;

	for (const auto unit : _combatUnits) 
	{
		if ((flyingDefender && !UnitUtil::CanAttackAir(unit)) ||
			(!flyingDefender && !UnitUtil::CanAttackGround(unit)))
        {
            continue;
        }

        if (!_squadData.canAssignUnitToSquad(unit, defenseSquad))
        {
            continue;
        }

		int dist = unit->getDistance(pos);

		// Pull workers only if requested, and not from distant bases.
		if (unit->getType().isWorker() && (!pullWorkers || dist > 24 * 32))
        {
            continue;
        }

        // If the enemy can't shoot up, prefer air units as defenders.
		if (!enemyHasAntiAir && unit->isFlying())
		{
			dist -= 12 * 32;     // may become negative - that's OK
		}

		if (dist < minDistance)
        {
            closestDefender = unit;
            minDistance = dist;
        }
	}

	return closestDefender;
}

// NOTE This implementation is kind of cheesy. Orders ought to be delegated to a squad.
void CombatCommander::loadOrUnloadBunkers()
{
	if (BWAPI::Broodwar->self()->getRace() != BWAPI::Races::Terran)
	{
		return;
	}

	for (const auto bunker : BWAPI::Broodwar->self()->getUnits())
	{
		if (bunker->getType() == BWAPI::UnitTypes::Terran_Bunker)
		{
			// BWAPI::Broodwar->drawCircleMap(bunker->getPosition(), 12 * 32, BWAPI::Colors::Cyan);
			// BWAPI::Broodwar->drawCircleMap(bunker->getPosition(), 18 * 32, BWAPI::Colors::Orange);
			
			// Are there enemies close to the bunker?
			bool enemyIsNear = false;

			// 1. Is any enemy unit within a small radius?
			BWAPI::Unitset enemiesNear = BWAPI::Broodwar->getUnitsInRadius(bunker->getPosition(), 12 * 32,
				BWAPI::Filter::IsEnemy);
			if (enemiesNear.empty())
			{
				// 2. Is a fast enemy unit within a wider radius?
				enemiesNear = BWAPI::Broodwar->getUnitsInRadius(bunker->getPosition(), 18 * 32,
					BWAPI::Filter::IsEnemy &&
						(BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Vulture ||
						 BWAPI::Filter::GetType == BWAPI::UnitTypes::Zerg_Mutalisk)
					);
				enemyIsNear = !enemiesNear.empty();
			}
			else
			{
				enemyIsNear = true;
			}

			if (enemyIsNear)
			{
				// Load one marine at a time if there is free space.
				if (bunker->getSpaceRemaining() > 0)
				{
					BWAPI::Unit marine = BWAPI::Broodwar->getClosestUnit(
						bunker->getPosition(),
						BWAPI::Filter::IsOwned && BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Marine,
						12 * 32);
					if (marine)
					{
						bunker->load(marine);
					}
				}
			}
			else
			{
				bunker->unloadAll();
			}
		}
	}
}

// Should squads have detectors assigned?
// Yes if the enemy has cloaked units, otherwise no if the detectors are in danger of dying.
bool CombatCommander::wantSquadDetectors() const
{
	return
		BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Protoss ||      // observers should be safe
		!InformationManager::Instance().enemyHasAntiAir() ||
		InformationManager::Instance().enemyCloakedUnitsSeen();
}

// Scan enemy cloaked units.
void CombatCommander::doComsatScan()
{
	if (BWAPI::Broodwar->self()->getRace() != BWAPI::Races::Terran)
	{
		return;
	}

	if (UnitUtil::GetCompletedUnitCount(BWAPI::UnitTypes::Terran_Comsat_Station) == 0)
	{
		return;
	}

	// Does the enemy have undetected cloaked units that we may be able to engage?
	for (const auto unit : BWAPI::Broodwar->enemy()->getUnits())
	{
		if (unit->isVisible() &&
			(!unit->isDetected() || unit->getOrder() == BWAPI::Orders::Burrowing) &&
			unit->getPosition().isValid())
		{
			// At most one scan per call. We don't check whether it succeeds.
			(void) Micro::Scan(unit->getPosition());
			// Also make sure the Info Manager knows that the enemy can burrow.
			InformationManager::Instance().enemySeenBurrowing();
			break;
		}
	}
}

// What units do you want to drop into the enemy base from a transport?
bool CombatCommander::unitIsGoodToDrop(const BWAPI::Unit unit) const
{
	return
		unit->getType() == BWAPI::UnitTypes::Protoss_Dark_Templar ||
		unit->getType() == BWAPI::UnitTypes::Terran_Vulture;
}

// Get our money back at the last moment for stuff that is about to be destroyed.
// It is not ideal: A building which is destined to die only after it is completed
// will be completed and die.
// Special case for a zerg sunken colony while it is morphing: It will lose up to
// 100 hp when the morph finishes, so cancel if it would be weak when it finishes.
// NOTE See BuildingManager::cancelBuilding() for another way to cancel buildings.
void CombatCommander::cancelDyingItems()
{
	for (const auto unit : BWAPI::Broodwar->self()->getUnits())
	{
		BWAPI::UnitType type = unit->getType();
		if (unit->isUnderAttack() &&
			(	type.isBuilding() && !unit->isCompleted() ||
				type == BWAPI::UnitTypes::Zerg_Egg ||
				type == BWAPI::UnitTypes::Zerg_Lurker_Egg ||
				type == BWAPI::UnitTypes::Zerg_Cocoon
			) &&
			(	unit->getHitPoints() < 30 ||
				type == BWAPI::UnitTypes::Zerg_Sunken_Colony && unit->getHitPoints() < 130 && unit->getRemainingBuildTime() < 24
			))
		{
			if (unit->canCancelMorph())
			{
				unit->cancelMorph();
			}
			else if (unit->canCancelConstruction())
			{
				unit->cancelConstruction();
			}
		}
	}
}

BWAPI::Position CombatCommander::getDefendLocation()
{
	return BWTA::getRegion(InformationManager::Instance().getMyMainBaseLocation()->getTilePosition())->getCenter();
}

// How good is it to pull this worker for combat?
int CombatCommander::workerPullScore(BWAPI::Unit worker)
{
	return
		(worker->getHitPoints() == worker->getType().maxHitPoints() ? 10 : 0) +
		(worker->getShields() == worker->getType().maxShields() ? 4 : 0) +
		(worker->isCarryingGas() ? -3 : 0) +
		(worker->isCarryingMinerals() ? -2 : 0);
}

// Pull workers off of mining and into the attack squad.
// The argument n can be zero or negative or huge. Nothing awful will happen.
// Tries to pull the "best" workers for combat, as decided by workerPullScore() above.
void CombatCommander::pullWorkers(int n)
{
	auto compare = [](BWAPI::Unit left, BWAPI::Unit right)
	{
		return workerPullScore(left) < workerPullScore(right);
	};

	std::priority_queue<BWAPI::Unit, std::vector<BWAPI::Unit>, decltype(compare)> workers;

	Squad & groundSquad = _squadData.getSquad("Ground");

	for (const auto unit : _combatUnits)
	{
		if (unit->getType().isWorker() &&
			WorkerManager::Instance().isFree(unit) &&
			_squadData.canAssignUnitToSquad(unit, groundSquad))
		{
			workers.push(unit);
		}
	}

	int nLeft = n;

	while (nLeft > 0 && !workers.empty())
	{
		BWAPI::Unit worker = workers.top();
		workers.pop();
		_squadData.assignUnitToSquad(worker, groundSquad);
		--nLeft;
	}
}

// Release workers from the attack squad.
void CombatCommander::releaseWorkers()
{
	Squad & groundSquad = _squadData.getSquad("Ground");
	groundSquad.releaseWorkers();
}

void CombatCommander::drawSquadInformation(int x, int y)
{
	_squadData.drawSquadInformation(x, y);
}

// Create an attack order for the given squad (which may be null).
// For a squad with ground units, ignore targets which are not accessible by ground.
SquadOrder CombatCommander::getAttackOrder(const Squad * squad)
{
	// 1. Clear any obstacles around our bases.
	// Most maps don't have any such thing, but see e.g. Arkanoid and Sparkle.
	// Only ground squads are sent to clear obstacles.
	// NOTE Turned off because there is a bug affecting the map Pathfinder.
	if (false && squad && squad->getUnits().size() > 0 && squad->hasGround() && squad->canAttackGround())
	{
		// We check our current bases (formerly plus 2 bases we may want to take next).
		/*
		Base * baseToClear1 = nullptr;
		Base * baseToClear2 = nullptr;
		BWAPI::TilePosition nextBasePos = MapTools::Instance().getNextExpansion(false, true, false);
		if (nextBasePos.isValid())
		{
			// The next mineral-optional expansion.
			baseToClear1 = Bases::Instance().getBaseAtTilePosition(nextBasePos);
		}
		nextBasePos = MapTools::Instance().getNextExpansion(false, true, true);
		if (nextBasePos.isValid())
		{
			// The next gas expansion.
			baseToClear2 = Bases::Instance().getBaseAtTilePosition(nextBasePos);
		}
		*/

		// Then pick any base with blockers and clear it.
		// The blockers are neutral buildings, and we can get their initial positions.
		// || base == baseToClear1 || base == baseToClear2
		const int squadPartition = squad->mapPartition();
		for (Base * base : Bases::Instance().getBases())
		{
			if (base->getBlockers().size() > 0 &&
				(base->getOwner() == BWAPI::Broodwar->self()) &&
				squadPartition == the.partitions.id(base->getPosition()))
			{
				BWAPI::Unit target = *(base->getBlockers().begin());
				return SquadOrder(SquadOrderTypes::DestroyNeutral, target->getInitialPosition(), 256, "Destroy neutrals");
			}
		}
	}

	// 2. If we're defensive, look for a front line to hold. No attacks.
	if (!_goAggressive)
	{
		return SquadOrder(SquadOrderTypes::Attack, getDefenseLocation(), DefendFrontRadius, "Defend front");
	}

	// 3. Otherwise we are aggressive. Look for a spot to attack.
	return SquadOrder(SquadOrderTypes::Attack, getAttackLocation(squad), AttackRadius, "Attack enemy");
}

// Choose a point of attack for the given squad (which may be null).
// For a squad with ground units, ignore targets which are not accessible by ground.
BWAPI::Position CombatCommander::getAttackLocation(const Squad * squad)
{
	// Know where the squad is. If it's empty or unknown, assume it is at our start position.
	// NOTE In principle, different members of the squad may be in different map partitions,
	// unable to reach each others' positions by ground. We ignore that complication.
	const int squadPartition = squad
		? squad->mapPartition()
		: the.partitions.id(Bases::Instance().myStartingBase()->getTilePosition());

	// Ground and air considerations.
	bool hasGround = true;
	bool hasAir = false;
	bool canAttackGround = true;
	bool canAttackAir = false;
	if (squad)
	{
		hasGround = squad->hasGround();
		hasAir = squad->hasAir();
		canAttackGround = squad->canAttackGround();
		canAttackAir = squad->canAttackAir();
	}

	// 1. Attack the enemy base with the weakest static defense.
	// Only if the squad can attack ground. Lift the command center and it is no longer counted as a base.
	if (canAttackGround)
	{
		Base * targetBase = nullptr;
		int bestScore = -99999;
		for (Base * base : Bases::Instance().getBases())
		{
			if (base->getOwner() == BWAPI::Broodwar->enemy())
			{
				// Ground squads ignore enemy bases which they cannot reach.
				if (hasGround && squadPartition != the.partitions.id(base->getTilePosition()))
				{
					continue;
				}

				int score = 0;     // the final score will be 0 or negative
				std::vector<UnitInfo> enemies;
				InformationManager::Instance().getNearbyForce(enemies, base->getPosition(), BWAPI::Broodwar->enemy(), 600);
				for (const auto & enemy : enemies)
				{
					// Count enemies that are buildings or slow-moving units good for defense.
					if (enemy.type.isBuilding() ||
						enemy.type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
						enemy.type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
						enemy.type == BWAPI::UnitTypes::Protoss_Reaver ||
						enemy.type == BWAPI::UnitTypes::Zerg_Lurker ||
						enemy.type == BWAPI::UnitTypes::Zerg_Guardian)
					{
						// If the unit could attack (some units of) the squad, count it.
						if (hasGround && UnitUtil::TypeCanAttackGround(enemy.type) ||			// doesn't recognize casters
							hasAir && UnitUtil::TypeCanAttackAir(enemy.type) ||					// doesn't recognize casters
							enemy.type == enemy.type == BWAPI::UnitTypes::Protoss_High_Templar)	// spellcaster
						{
							--score;
						}
					}
				}
				if (score > bestScore)
				{
					targetBase = base;
					bestScore = score;
				}
			}
		}
		if (targetBase)
		{
			return targetBase->getPosition();
		}
	}

	// 2. Attack known enemy buildings.
	// We assume that a terran can lift the buildings; otherwise, the squad must be able to attack ground.
	if (canAttackGround || BWAPI::Broodwar->enemy()->getRace() == BWAPI::Races::Terran)
	{
		for (const auto & kv : InformationManager::Instance().getUnitInfo(BWAPI::Broodwar->enemy()))
		{
			const UnitInfo & ui = kv.second;

			// 1. Special case for refinery buildings because their ground reachability is tricky to check.
			// 2. We only know that a building is lifted while it is in sight. That can cause oscillatory
			// behavior--we move away, can't see it, move back because now we can attack it, see it is lifted, ....
			if (ui.type.isBuilding() &&
				ui.lastPosition.isValid() &&
				!ui.goneFromLastPosition &&
				(!hasGround || (!ui.type.isRefinery() && squadPartition == the.partitions.id(ui.lastPosition))))
			{
				if (ui.unit->exists() && ui.unit->isLifted())
				{
					// The building is lifted. Only if the squad can hit it.
					if (canAttackAir)
					{
						return ui.lastPosition;
					}
				}
				else
				{
					// The building is not known for sure to be lifted.
					return ui.lastPosition;
				}
			}
		}
	}

	// 3. Attack visible enemy units.
	// TODO score the units and attack the most important / most vulnerable
	const BWAPI::Position squadCenter = squad
		? squad->calcCenter()
		: Bases::Instance().myStartingBase()->getPosition();
	for (const auto unit : BWAPI::Broodwar->enemy()->getUnits())
	{
		if (unit->getType() == BWAPI::UnitTypes::Zerg_Larva ||
			!unit->exists() ||
			!unit->isDetected() ||
			!unit->getPosition().isValid())
		{
			continue;
		}

		// Ground squads ignore enemy units which are not accessible by ground, except when nearby.
		// The "when nearby" exception allows a chance to attack enemies that are in range,
		// even if they are beyond a barrier. It's very rough.
		if (hasGround &&
			squadPartition != the.partitions.id(unit->getPosition()) &&
			unit->getDistance(squadCenter) > 300)
		{
			continue;
		}

		if (unit->isFlying() && canAttackAir || !unit->isFlying() && canAttackGround)
		{
			return unit->getPosition();
		}
	}

	// 4. We can't see anything, so explore the map until we find something.
	return MapGrid::Instance().getLeastExplored(hasGround && !hasAir, squadPartition);
}

// Choose a point of attack for the given drop squad.
BWAPI::Position CombatCommander::getDropLocation(const Squad & squad)
{
	// 0. If we're defensive, stay at the start location.
	/* unneeded
	if (!_goAggressive)
	{
		return BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation());
	}
	*/

	// Otherwise we are aggressive. Look for a spot to attack.

	// 1. The enemy main base, if known.
	if (InformationManager::Instance().getEnemyMainBaseLocation())
	{
		return InformationManager::Instance().getEnemyMainBaseLocation()->getPosition();
	}

	// 2. Any known enemy base.
	/* TODO not ready yet
	Base * targetBase = nullptr;
	int bestScore = -99999;
	for (Base * base : Bases::Instance().getBases())
	{
		if (base->getOwner() == BWAPI::Broodwar->enemy())
		{
			int score = 0;     // the final score will be 0 or negative
			std::vector<UnitInfo> enemies;
			InformationManager::Instance().getNearbyForce(enemies, base->getPosition(), BWAPI::Broodwar->enemy(), 600);
			for (const auto & enemy : enemies)
			{
				if (enemy.type.isBuilding() && (UnitUtil::TypeCanAttackGround(enemy.type) || enemy.type.isDetector()))
				{
					--score;
				}
			}
			if (score > bestScore)
			{
				targetBase = base;
				bestScore = score;
			}
		}
		if (targetBase)
		{
			return targetBase->getPosition();
		}
	}
	*/

	// 3. Any known enemy buildings.
	for (const auto & kv : InformationManager::Instance().getUnitInfo(BWAPI::Broodwar->enemy()))
	{
		const UnitInfo & ui = kv.second;

		if (ui.type.isBuilding() && ui.lastPosition.isValid() && !ui.goneFromLastPosition)
		{
			return ui.lastPosition;
		}
	}

	// 4. We can't see anything, so explore the map until we find something.
	return MapGrid::Instance().getLeastExplored();
}

// We're being defensive. Get the location to defend.
BWAPI::Position CombatCommander::getDefenseLocation()
{
	// We are guaranteed to always have a main base location, even if it has been destroyed.
	Base * base = Bases::Instance().myStartingBase();

	// We may have taken our natural. If so, call that the front line.
	Base * natural = Bases::Instance().myNaturalBase();
	if (natural && BWAPI::Broodwar->self() == natural->getOwner())
	{
		base = natural;
	}

	return base->getPosition();

}

// Choose one worker to pull for scout defense.
BWAPI::Unit CombatCommander::findClosestWorkerToTarget(BWAPI::Unitset & unitsToAssign, BWAPI::Unit target)
{
    UAB_ASSERT(target != nullptr, "target was null");

    if (!target)
    {
        return nullptr;
    }

    BWAPI::Unit closestMineralWorker = nullptr;
	int closestDist = Config::Micro::ScoutDefenseRadius + 128;    // more distant workers do not get pulled
    
	for (const auto unit : unitsToAssign)
	{
		if (unit->getType().isWorker() && WorkerManager::Instance().isFree(unit))
		{
			int dist = unit->getDistance(target);
			if (unit->isCarryingMinerals())
			{
				dist += 96;
			}

            if (dist < closestDist)
            {
                closestMineralWorker = unit;
                dist = closestDist;
            }
		}
	}

    return closestMineralWorker;
}

int CombatCommander::numZerglingsInOurBase() const
{
    const int concernRadius = 300;
    int zerglings = 0;
	
	BWTA::BaseLocation * main = InformationManager::Instance().getMyMainBaseLocation();
	BWAPI::Position myBasePosition(main->getPosition());

    for (auto unit : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Zergling &&
			unit->getDistance(myBasePosition) < concernRadius)
        {
			++zerglings;
		}
    }

	return zerglings;
}

// Is an enemy building near our base? If so, we may pull workers.
bool CombatCommander::buildingRush() const
{
	// If we have units, there will be no need to pull workers.
	if (InformationManager::Instance().weHaveCombatUnits())
	{
		return false;
	}

	BWTA::BaseLocation * main = InformationManager::Instance().getMyMainBaseLocation();
	BWAPI::Position myBasePosition(main->getPosition());

    for (const auto unit : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (unit->getType().isBuilding() && unit->getDistance(myBasePosition) < 1200)
        {
            return true;
        }
    }

    return false;
}

CombatCommander & CombatCommander::Instance()
{
	static CombatCommander instance;
	return instance;
}
