/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef UNITHANDLER_H
#define UNITHANDLER_H

#include <vector>
#include <list>

#include "UnitDef.h"
#include "UnitSet.h"
#include "CommandAI/Command.h"

class CUnit;
class CBuilderCAI;
class CFeature;
class CLoadSaveInterface;
struct BuildInfo;


enum BuildSquareStatus {
	BUILDSQUARE_BLOCKED     = 0,
	BUILDSQUARE_OCCUPIED    = 1,
	BUILDSQUARE_RECLAIMABLE = 2,
	BUILDSQUARE_OPEN        = 3
};


class CUnitHandler
{
	CR_DECLARE_STRUCT(CUnitHandler)

public:
	CUnitHandler();
	~CUnitHandler();

	void Update();
	void DeleteUnit(CUnit* unit);
	void DeleteUnitNow(CUnit* unit);
	bool AddUnit(CUnit* unit);
	void Serialize(creg::ISerializer& s) {}
	void PostLoad();

	unsigned int MaxUnits() const { return maxUnits; }

	///< test if a unit can be built at specified position
	BuildSquareStatus TestUnitBuildSquare(
		const BuildInfo&,
		CFeature*&,
		int allyteam,
		bool synced,
		std::vector<float3>* canbuildpos = NULL,
		std::vector<float3>* featurepos = NULL,
		std::vector<float3>* nobuildpos = NULL,
		const std::vector<Command>* commands = NULL
	);
	/// Returns true if a unit of type unitID can be built, false otherwise
	bool CanBuildUnit(const UnitDef* unitdef, int team) const;

	void AddBuilderCAI(CBuilderCAI*);
	void RemoveBuilderCAI(CBuilderCAI*);
	float GetBuildHeight(const float3& pos, const UnitDef* unitdef, bool synced = true);

	Command GetBuildCommand(const float3& pos, const float3& dir);


	// note: negative ID's are implicitly converted
	CUnit* GetUnitUnsafe(unsigned int unitID) const { return units[unitID]; }
	CUnit* GetUnit(unsigned int unitID) const { return (unitID < MaxUnits()? units[unitID]: NULL); }


	std::vector< std::vector<CUnitSet> > unitsByDefs; ///< units sorted by team and unitDef

	std::list<CUnit*> activeUnits;                    ///< used to get all active units
	std::vector<CUnit*> units;                        ///< used to get units from IDs (0 if not created)
	std::list<CBuilderCAI*> builderCAIs; //FIXME use std::set? (std::set is ingeneral not syncsafe, but when using a custom compare func it can)

	float maxUnitRadius;                              ///< largest radius of any unit added so far
	bool morphUnitToFeature;

private:
	///< test a single mapsquare for build possibility
	BuildSquareStatus TestBuildSquare(const float3& pos, const UnitDef *unitdef,CFeature *&feature, int allyteam, bool synced);

private:
	std::list<unsigned int> freeUnitIDs;
	std::vector<CUnit*> unitsToBeRemoved;            ///< units that will be removed at start of next update
	std::list<CUnit*>::iterator slowUpdateIterator;

	///< global unit-limit (derived from the per-team limit)
	unsigned int maxUnits;
};

extern CUnitHandler* uh;

#endif /* UNITHANDLER_H */
