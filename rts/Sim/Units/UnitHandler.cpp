/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <cassert>
#include "System/mmgr.h"

#include "lib/gml/gmlmut.h"
#include "lib/gml/gml_base.h"
#include "UnitHandler.h"
#include "Unit.h"
#include "UnitDefHandler.h"
#include "CommandAI/BuilderCAI.h"
#include "CommandAI/Command.h"
#include "Game/GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Map/ReadMap.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Misc/AirBaseHandler.h"
#include "Sim/Misc/GroundBlockingObjectMap.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/MoveTypes/MoveType.h"
#include "Sim/Path/IPathManager.h"
#include "System/Config/ConfigHandler.h"
#include "System/EventHandler.h"
#include "System/EventBatchHandler.h"
#include "System/Log/ILog.h"
#include "System/TimeProfiler.h"
#include "System/myMath.h"
#include "System/Sync/SyncTracer.h"
#include "System/creg/STL_Deque.h"
#include "System/creg/STL_List.h"
#include "System/creg/STL_Set.h"

CONFIG(int, SimThreadCount).defaultValue(0).safemodeValue(1).minimumValue(0).maximumValue(GML_MAX_NUM_THREADS);

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CUnitHandler* uh = NULL;

CR_BIND(CUnitHandler, );
CR_REG_METADATA(CUnitHandler, (
	CR_MEMBER(activeUnits),
	CR_MEMBER(units),
	CR_MEMBER(freeUnitIDs),
	CR_MEMBER(maxUnits),
	CR_MEMBER(maxUnitRadius),
	CR_MEMBER(unitsToBeRemoved),
	CR_MEMBER(morphUnitToFeature),
	CR_MEMBER(builderCAIs),
	CR_MEMBER(unitsByDefs),
	CR_POSTLOAD(PostLoad),
	CR_SERIALIZER(Serialize)
));



void CUnitHandler::PostLoad()
{
	// reset any synced stuff that is not saved
	slowUpdateIterator = activeUnits.end();
}


CUnitHandler::CUnitHandler()
:
	maxUnitRadius(0.0f),
	morphUnitToFeature(true),
	maxUnits(0),
	stopThread(false),
	simThreadingStage(UPDATE_MOVETYPE),
	atomicCount(-1),
	simBarrier(NULL)
{
	// note: the number of active teams can change at run-time, so
	// the team unit limit should be recalculated whenever one dies
	// or spawns (but that would get complicated)
	for (unsigned int n = 0; n < teamHandler->ActiveTeams(); n++) {
		maxUnits += teamHandler->Team(n)->maxUnits;
	}

	size_t numThreads = std::max(0, configHandler->GetInt("SimThreadCount"));
	if (numThreads == 0) {
		unsigned lcpu = Threading::GetAvailableCores();
		unsigned pcpu = Threading::GetPhysicalCores();
		LOG_L(L_WARNING, "CPU: L = %d,  P = %d", lcpu, pcpu);
		// deduct all logical cores dedicated to the rendering/sim main threads
		numThreads = std::max((unsigned)GML::NumMainSimThreads(), lcpu - GML::NumMainThreads() * (int)(lcpu / pcpu) + GML::NumMainSimThreads()); // add the main sim thread
	}
	simNumExtraThreads = (!modInfo.multiThreadSim) ? 0 : std::max((size_t)0, numThreads - GML::NumMainSimThreads());
	LOG_L(L_WARNING, "CPU: N = %d,  S = %d", numThreads, simNumExtraThreads);
	Threading::SimThreadCount(simNumExtraThreads + GML::NumMainSimThreads() + (modInfo.asyncPathFinder ? 1 : 0));

	if (simNumExtraThreads > 0)
		LOG("[Threading] Simulation multithreading is enabled with %d threads", simNumExtraThreads + GML::NumMainSimThreads());
	else
		LOG("[Threading] Simulation multithreading is disabled");

	units.resize(maxUnits, NULL);
	unitsByDefs.resize(teamHandler->ActiveTeams(), std::vector<CUnitSet>(unitDefHandler->unitDefs.size()));

	{
		std::vector<unsigned int> freeIDs(units.size());

		// id's are used as indices, so they must lie in [0, units.size() - 1]
		// (furthermore all id's are treated equally, none have special status)
		for (unsigned int id = 0; id < freeIDs.size(); id++) {
			freeIDs[id] = id;
		}

		// randomize the unit ID's so that Lua widgets can not
		// easily determine enemy unit counts from ID's alone
		// (shuffle twice for good measure)
		SyncedRNG rng;

		std::random_shuffle(freeIDs.begin(), freeIDs.end(), rng);
		std::random_shuffle(freeIDs.begin(), freeIDs.end(), rng);
		std::copy(freeIDs.begin(), freeIDs.end(), std::front_inserter(freeUnitIDs));
	}

	slowUpdateIterator = activeUnits.end();
	airBaseHandler = new CAirBaseHandler();
	InitThreads();
}


CUnitHandler::~CUnitHandler()
{
	CleanThreads();
	for (std::list<CUnit*>::iterator usi = activeUnits.begin(); usi != activeUnits.end(); ++usi) {
		// ~CUnit dereferences featureHandler which is destroyed already
		(*usi)->delayedWreckLevel = -1;
		delete (*usi);
	}

	delete airBaseHandler;
}


bool CUnitHandler::AddUnit(CUnit *unit)
{
	if (freeUnitIDs.empty()) {
		// should be unreachable (all code that goes through
		// UnitLoader::LoadUnit --> Unit::PreInit checks the
		// unit limit first)
		assert(false);
		return false;
	}

	unit->id = freeUnitIDs.front();
	freeUnitIDs.pop_front();
	units[unit->id] = unit;

	std::list<CUnit*>::iterator ui = activeUnits.begin();
	if (ui != activeUnits.end()) {
		// randomize this to make the slow-update order random (good if one
		// builds say many buildings at once and then many mobile ones etc)
		const unsigned int insertionPos = gs->randFloat() * activeUnits.size();

		for (unsigned int n = 0; n < insertionPos; ++n) {
			++ui;
		}
	}
	activeUnits.insert(ui, unit);

	teamHandler->Team(unit->team)->AddUnit(unit, CTeam::AddBuilt);
	unitsByDefs[unit->team][unit->unitDef->id].insert(unit);

	maxUnitRadius = std::max(unit->radius, maxUnitRadius);
	return true;
}

void CUnitHandler::DeleteUnit(CUnit* unit)
{
	unitsToBeRemoved.push_back(unit);
	(eventBatchHandler->GetUnitCreatedDestroyedBatch()).dequeue_synced(unit);
}


void CUnitHandler::DeleteUnitNow(CUnit* delUnit)
{
	delUnit->ExecuteDelayOps();
	int delTeam = 0;
	int delType = 0;

	std::list<CUnit*>::iterator usi;

	for (usi = activeUnits.begin(); usi != activeUnits.end(); ++usi) {
		if (*usi == delUnit) {
			if (slowUpdateIterator != activeUnits.end() && *usi == *slowUpdateIterator) {
				++slowUpdateIterator;
			}
			delTeam = delUnit->team;
			delType = delUnit->unitDef->id;

			GML_STDMUTEX_LOCK(dque); // DeleteUnitNow

			teamHandler->Team(delTeam)->RemoveUnit(delUnit, CTeam::RemoveDied);

			activeUnits.erase(usi);
			freeUnitIDs.push_back(delUnit->id);
			unitsByDefs[delTeam][delType].erase(delUnit);

			units[delUnit->id] = NULL;

			CSolidObject::SetDeletingRefID(delUnit->id);
			delete delUnit;
			CSolidObject::SetDeletingRefID(-1);

			break;
		}
	}

#ifdef _DEBUG
	for (usi = activeUnits.begin(); usi != activeUnits.end(); /* no post-op */) {
		if (*usi == delUnit) {
			LOG_L(L_ERROR, "Duplicated unit found in active units on erase");
			usi = activeUnits.erase(usi);
		} else {
			++usi;
		}
	}
#endif
}

void CUnitHandler::Update()
{
	{
		GML_STDMUTEX_LOCK(runit); // Update

		if (!unitsToBeRemoved.empty()) {
			IPathManager::ScopedDisableThreading sdt;
			GML_RECMUTEX_LOCK(obj); // Update

			while (!unitsToBeRemoved.empty()) {
				eventHandler.DeleteSyncedObjects(); // the unit destructor may invoke eventHandler, so we need to call these for every unit to clear invaild references from the batching systems

				GML_RECMUTEX_LOCK(unit); // Update

				eventHandler.DeleteSyncedUnits();

				GML_RECMUTEX_LOCK(proj); // Update - projectile drawing may access owner() and lead to crash
				GML_RECMUTEX_LOCK(sel);  // Update - unit is removed from selectedUnits in ~CObject, which is too late.
				GML_RECMUTEX_LOCK(quad); // Update - make sure unit does not get partially deleted before before being removed from the quadfield

				CUnit* delUnit = unitsToBeRemoved.back();
				unitsToBeRemoved.pop_back();

				DeleteUnitNow(delUnit);
			}
		}

		eventHandler.UpdateUnits();
	}

	GML::UpdateTicks();

	#define VECTOR_SANITY_CHECK(v)                              \
		assert(!math::isnan(v.x) && !math::isinf(v.x)); \
		assert(!math::isnan(v.y) && !math::isinf(v.y)); \
		assert(!math::isnan(v.z) && !math::isinf(v.z));
	#define MAPPOS_SANITY_CHECK(unit)                          \
		if (unit->unitDef->IsGroundUnit()) {                   \
			assert(unit->pos.x >= -(float3::maxxpos * 16.0f)); \
			assert(unit->pos.x <=  (float3::maxxpos * 16.0f)); \
			assert(unit->pos.z >= -(float3::maxzpos * 16.0f)); \
			assert(unit->pos.z <=  (float3::maxzpos * 16.0f)); \
		}
	#define UNIT_SANITY_CHECK(unit)                 \
		VECTOR_SANITY_CHECK(unit->pos);             \
		VECTOR_SANITY_CHECK(unit->midPos);          \
		VECTOR_SANITY_CHECK(unit->relMidPos);       \
		VECTOR_SANITY_CHECK(unit->speed);           \
		VECTOR_SANITY_CHECK(unit->deathSpeed);      \
		VECTOR_SANITY_CHECK(unit->residualImpulse); \
		VECTOR_SANITY_CHECK(unit->rightdir);        \
		VECTOR_SANITY_CHECK(unit->updir);           \
		VECTOR_SANITY_CHECK(unit->frontdir);        \
		MAPPOS_SANITY_CHECK(unit);

	{
		SCOPED_TIMER("Unit::MoveType::Update");
		Threading::SetMultiThreadedSim(modInfo.multiThreadSim);
		Threading::SetThreadedPath(modInfo.asyncPathFinder);
		// Use the current thread as thread zero. FIRE!
		simThreadingStage = UPDATE_MOVETYPE;
		MoveTypeThreadFunc(0);
		Threading::SetMultiThreadedSim(false);

		if (modInfo.asyncPathFinder) {
			int nBlockOps = 0;
			// threaded pathing can run also during ExecuteDelayOps, since Block/UnBlock is further delayed
			for (std::list<CUnit*>::iterator i = activeUnits.begin(); i != activeUnits.end(); ++i) {
				CUnit* u = *i;
				if (!u->delayOps.empty()) {
					int block = u->ExecuteDelayOps(); // can generate new delay ops, but it will execute these also
					if (block) {
						CUnit::updateOps[u->id] = block;
						blockOps[nBlockOps++] = u->id;
					}
				}
			}

			IPathManager::ScopedDisableThreading sdt;

			for (int i = 0; i < nBlockOps; ++i) {
				int id = blockOps[i];
				(CUnit::updateOps[id] > 0) ? units[id]->Block() : units[id]->UnBlock();
			}
			if (modInfo.multiThreadSim)
				pathManager->MergePathCaches();
			CSolidObject::UpdateStableData();
		}
	}

	{
		SCOPED_TIMER("Unit::Update");
		std::list<CUnit*>::iterator usi;
		for (usi = activeUnits.begin(); usi != activeUnits.end(); ++usi) {
			CUnit* unit = *usi;

			UNIT_SANITY_CHECK(unit);

			if (unit->deathScriptFinished) {
				// there are many ways to fiddle with "deathScriptFinished", so a unit may
				// arrive here without having been properly killed (and isDead still false),
				// which can result in MT deadlocking -- FIXME verify this
				// (KU returns early if isDead)
				unit->KillUnit(false, true, NULL);

				DeleteUnit(unit);
			} else {
				unit->Update();
			}

			UNIT_SANITY_CHECK(unit);
		}
	}


	{
		SCOPED_TIMER("Unit::MoveType::SlowUpdate");

		Threading::SetMultiThreadedSim(modInfo.multiThreadSim);
		// Use the current thread as thread zero. FIRE!
		simThreadingStage = SLOW_UPDATE_MOVETYPE;
		MoveTypeThreadFunc(0);

		Threading::SetMultiThreadedSim(false);
		// Use the current thread as thread zero. FIRE!
		simThreadingStage = DELAYED_SLOW_UPDATE_MOVETYPE;
		MoveTypeThreadFunc(0);
	}

	{
		SCOPED_TIMER("Unit::SlowUpdate");

		// reset the iterator every <UNIT_SLOWUPDATE_RATE> frames
		if ((gs->frameNum & (UNIT_SLOWUPDATE_RATE - 1)) == 0) {
			slowUpdateIterator = activeUnits.begin();
		}

		// stagger the SlowUpdate's
		int n = (activeUnits.size() / UNIT_SLOWUPDATE_RATE) + 1;

		for (; slowUpdateIterator != activeUnits.end() && n != 0; ++slowUpdateIterator) {
			CUnit* unit = *slowUpdateIterator;

			UNIT_SANITY_CHECK(unit);
			unit->SlowUpdate();
			UNIT_SANITY_CHECK(unit);

			n--;
		}
	}
}


inline void UpdateMoveType(CUnit *unit) {
	UNIT_SANITY_CHECK(unit);

	if (unit->moveType->Update())
		unit->QueMove();
	if (!unit->pos.IsInBounds() && (unit->speed.SqLength() > (MAX_UNIT_SPEED * MAX_UNIT_SPEED))) {
		// this unit is not coming back, kill it now without any death
		// sequence (so deathScriptFinished becomes true immediately)
		unit->QueKillUnit(false);
	}

	UNIT_SANITY_CHECK(unit);
	GML::GetTicks(unit->lastUnitUpdate);
}


void CUnitHandler::MoveTypeThreadFunc(int i) {
	if (simNumExtraThreads > 0) {
		if (i > 0) {
			streflop::streflop_init<streflop::Simple>();
			GML::ThreadNumber(GML_MAX_NUM_THREADS + i);
			char threadName[32];
			sprintf(threadName,"SimMT%d", i);
			Threading::SetAffinityHelper(threadName, configHandler->GetUnsigned("SetCoreAffinitySimMT"));
		}
		do {
			if (i == 0) {
				atomicCount %= -1;
				if (simThreadingStage == SLOW_UPDATE_MOVETYPE) {
					memset(CUnit::updateOps, 0, sizeof(CUnit::updateOps));
				}
			}
			simBarrier->wait();
			if (stopThread)
				break;
			int curPos = 0;
			if (simThreadingStage == PROJECTILE_COLLISION) {
				ph->ProjectileCollisionThreadFunc();
			}
			else if (simThreadingStage == UPDATE_MOVETYPE) {
				const int countEnd = activeUnits.size();
				std::list<CUnit*>::iterator usi = activeUnits.begin();
				while(true) {
					int nextPos = ++atomicCount;
					if (nextPos >= countEnd) break;
					while(curPos < nextPos) { ++usi; ++curPos; }
					CUnit *unit = *usi;
					Threading::SetThreadCurrentUnitID(unit->id);
					UpdateMoveType(unit);
				}
			} else if (simThreadingStage == SLOW_UPDATE_MOVETYPE) {
				const int countEnd = (activeUnits.size() / UNIT_SLOWUPDATE_RATE) + 1;
				std::list<CUnit*>::iterator sui = ((gs->frameNum & (UNIT_SLOWUPDATE_RATE - 1)) == 0) ? activeUnits.begin() : slowUpdateIterator;

				while(true) {
					int nextPos = ++atomicCount;
					if (nextPos >= countEnd) break;
					while(curPos < nextPos && sui != activeUnits.end()) { ++sui; ++curPos; }
					if (sui == activeUnits.end()) break;

					CUnit *unit = *sui;
					Threading::SetThreadCurrentUnitID(unit->id);
					unit->moveType->SlowUpdate();
				}
			} else if (simThreadingStage == DELAYED_SLOW_UPDATE_MOVETYPE) {
				const int countEnd = 4;
				while(true) {
					int nextPos = ++atomicCount;
					if (nextPos >= countEnd)
						break;
					switch(nextPos) {
						case 0:
							for (int i = 0; i < MAX_UNITS; ++i) {
								if (CUnit::updateOps[i] & CUnit::UPDATE_LOS) {
									units[i]->QueUpdateLOS(false);
								}
							}
							break;
						case 1:
							for (int i = 0; i < MAX_UNITS; ++i) {
								if (CUnit::updateOps[i] & CUnit::UPDATE_RADAR) {
									units[i]->QueUpdateRadar(false);
								}
							}
							break;
						case 2:
							for (int i = 0; i < MAX_UNITS; ++i) {
								if (CUnit::updateOps[i] & CUnit::UPDATE_QUAD) {
									units[i]->QueUpdateQuad(false);
								}
							}
							break;
						case 3:
							for (int i = 0; i < MAX_UNITS; ++i) {
								if (CUnit::updateOps[i] & CUnit::FIND_PAD) {
									units[i]->QueFindPad(false);
								}
							}
							break;
						default:
							LOG_L(L_ERROR, "Invalid slow update type");
					}
				}
			}
			simBarrier->wait();
		} while (i > 0);
	}
	else {
		if (simThreadingStage == PROJECTILE_COLLISION) {
			ph->ProjectileCollisionNonThreadFunc();
		} else if (simThreadingStage == UPDATE_MOVETYPE) {
			for (std::list<CUnit*>::iterator usi = activeUnits.begin(); usi != activeUnits.end(); ++usi) {
				CUnit *unit = *usi;
				Threading::SetThreadCurrentUnitID(unit->id);
				UpdateMoveType(unit);
			}
		} else if (simThreadingStage == SLOW_UPDATE_MOVETYPE) {
			std::list<CUnit*>::iterator sui = ((gs->frameNum & (UNIT_SLOWUPDATE_RATE - 1)) == 0) ? activeUnits.begin() : slowUpdateIterator;
			int n = (activeUnits.size() / UNIT_SLOWUPDATE_RATE) + 1;

			for (; sui != activeUnits.end() && n != 0; ++sui) {
				CUnit *unit = *sui;
				Threading::SetThreadCurrentUnitID(unit->id);
				unit->moveType->SlowUpdate();
			}
		} else if (simThreadingStage == DELAYED_SLOW_UPDATE_MOVETYPE) {
			// not needed for singlethreaded sim
		}
	}
}

void CUnitHandler::InitThreads() {
	LOG_L(L_WARNING, "INIT");
	simBarrier = new boost::barrier(simNumExtraThreads + 1);

	LOG_L(L_WARNING, "THREAD");
	for (unsigned int i = 1; i <= simNumExtraThreads; i++) {
		simThreads[i] = new boost::thread(boost::bind(&CUnitHandler::MoveTypeThreadFunc, this, i));
	}
	LOG_L(L_WARNING, "FINAL");
}

void CUnitHandler::CleanThreads() {
	stopThread = true;
	simBarrier->wait();
	for (unsigned int i = 1; i <= simNumExtraThreads; i++) {
		simThreads[i]->join();
		delete simThreads[i];
	}

	delete simBarrier;
}

// find the reference height for a build-position
// against which to compare all footprint squares
float CUnitHandler::GetBuildHeight(const float3& pos, const UnitDef* unitdef, bool synced)
{
	const float* orgHeightMap = readmap->GetOriginalHeightMapSynced();
	const float* curHeightMap = readmap->GetCornerHeightMapSynced();

	#ifdef USE_UNSYNCED_HEIGHTMAP
	if (!synced) {
		orgHeightMap = readmap->GetCornerHeightMapUnsynced();
		curHeightMap = readmap->GetCornerHeightMapUnsynced();
	}
	#endif

	const float difH = unitdef->maxHeightDif;

	float minH = readmap->currMinHeight;
	float maxH = readmap->currMaxHeight;

	unsigned int numBorderSquares = 0;
	float sumBorderSquareHeight = 0.0f;

	static const int xsize = 1;
	static const int zsize = 1;

	// top-left footprint corner (sans clamping)
	const int px = (pos.x - (xsize * (SQUARE_SIZE >> 1))) / SQUARE_SIZE;
	const int pz = (pos.z - (zsize * (SQUARE_SIZE >> 1))) / SQUARE_SIZE;
	// top-left and bottom-right footprint corner (clamped)
	const int x1 = std::min(gs->mapx, std::max(0, px));
	const int z1 = std::min(gs->mapy, std::max(0, pz));
	const int x2 = std::max(0, std::min(gs->mapx, x1 + xsize));
	const int z2 = std::max(0, std::min(gs->mapy, z1 + zsize));

	for (int x = x1; x <= x2; x++) {
		for (int z = z1; z <= z2; z++) {
			const float sqOrgH = orgHeightMap[z * gs->mapxp1 + x];
			const float sqCurH = curHeightMap[z * gs->mapxp1 + x];
			const float sqMinH = std::min(sqCurH, sqOrgH);
			const float sqMaxH = std::max(sqCurH, sqOrgH);

			if (x == x1 || x == x2 || z == z1 || z == z2) {
				sumBorderSquareHeight += sqCurH;
				numBorderSquares += 1;
			}

			// restrict the range of [minH, maxH] to
			// the minimum and maximum square height
			// within the footprint
			if (minH < (sqMinH - difH)) { minH = sqMinH - difH; }
			if (maxH > (sqMaxH + difH)) { maxH = sqMaxH + difH; }
		}
	}

	// find the average height of the footprint-border squares
	const float avgH = sumBorderSquareHeight / numBorderSquares;

	// and clamp it to [minH, maxH] if necessary
	if (avgH < minH && minH < maxH) { return (minH + 0.01f); }
	if (avgH > maxH && maxH > minH) { return (maxH - 0.01f); }

	return avgH;
}


BuildSquareStatus CUnitHandler::TestUnitBuildSquare(
	const BuildInfo& buildInfo,
	CFeature*& feature,
	int allyteam,
	bool synced,
	std::vector<float3>* canbuildpos,
	std::vector<float3>* featurepos,
	std::vector<float3>* nobuildpos,
	const std::vector<Command>* commands)
{
	feature = NULL;

	const int xsize = buildInfo.GetXSize();
	const int zsize = buildInfo.GetZSize();
	const float3 pos = buildInfo.pos;

	const int x1 = (pos.x - (xsize * 0.5f * SQUARE_SIZE));
	const int z1 = (pos.z - (zsize * 0.5f * SQUARE_SIZE));
	const int z2 = z1 + zsize * SQUARE_SIZE;
	const int x2 = x1 + xsize * SQUARE_SIZE;
	const float bh = GetBuildHeight(pos, buildInfo.def, synced);

	BuildSquareStatus canBuild = BUILDSQUARE_OPEN;

	if (buildInfo.def->needGeo) {
		canBuild = BUILDSQUARE_BLOCKED;
		const std::vector<CFeature*>& features = qf->GetFeaturesExact(pos, std::max(xsize, zsize) * 6);

		// look for a nearby geothermal feature if we need one
		for (std::vector<CFeature*>::const_iterator fi = features.begin(); fi != features.end(); ++fi) {
			if ((*fi)->def->geoThermal
				&& math::fabs((*fi)->pos.x - pos.x) < (xsize * 4 - 4)
				&& math::fabs((*fi)->pos.z - pos.z) < (zsize * 4 - 4)) {
				canBuild = BUILDSQUARE_OPEN;
				break;
			}
		}
	}

	if (commands != NULL) {
		// this is only called in unsynced context (ShowUnitBuildSquare)
		assert(!synced);

		for (int x = x1; x < x2; x += SQUARE_SIZE) {
			for (int z = z1; z < z2; z += SQUARE_SIZE) {
				BuildSquareStatus tbs = TestBuildSquare(float3(x, pos.y, z), buildInfo.def, feature, gu->myAllyTeam, synced);

				if (tbs != BUILDSQUARE_BLOCKED) {
					//??? what does this do?
					for (std::vector<Command>::const_iterator ci = commands->begin(); ci != commands->end(); ++ci) {
						BuildInfo bc(*ci);
						if (std::max(bc.pos.x - x - SQUARE_SIZE, x - bc.pos.x) * 2 < bc.GetXSize() * SQUARE_SIZE &&
							std::max(bc.pos.z - z - SQUARE_SIZE, z - bc.pos.z) * 2 < bc.GetZSize() * SQUARE_SIZE) {
							tbs = BUILDSQUARE_BLOCKED;
							break;
						}
					}
				}

				switch (tbs) {
					case BUILDSQUARE_OPEN:
						canbuildpos->push_back(float3(x, bh, z));
						break;
					case BUILDSQUARE_RECLAIMABLE:
					case BUILDSQUARE_OCCUPIED:
						featurepos->push_back(float3(x, bh, z));
						break;
					case BUILDSQUARE_BLOCKED:
						nobuildpos->push_back(float3(x, bh, z));
						break;
				}

				canBuild = std::min(canBuild, tbs);
			}
		}
	} else {
		// this can be called in either context
		for (int x = x1; x < x2; x += SQUARE_SIZE) {
			for (int z = z1; z < z2; z += SQUARE_SIZE) {
				canBuild = std::min(canBuild, TestBuildSquare(float3(x, bh, z), buildInfo.def, feature, allyteam, synced));

				if (canBuild == BUILDSQUARE_BLOCKED) {
					return BUILDSQUARE_BLOCKED;
				}
			}
		}
	}

	return canBuild;
}



BuildSquareStatus CUnitHandler::TestBuildSquare(const float3& pos, const UnitDef* unitdef, CFeature*& feature, int allyteam, bool synced)
{
	if (!pos.IsInMap()) {
		return BUILDSQUARE_BLOCKED;
	}

	BuildSquareStatus ret = BUILDSQUARE_OPEN;
	const int yardxpos = int(pos.x + 4) / SQUARE_SIZE;
	const int yardypos = int(pos.z + 4) / SQUARE_SIZE;
	CSolidObject* s = groundBlockingObjectMap->GroundBlocked(yardxpos, yardypos);

	if (s != NULL) {
		CFeature* f;
		if ((f = dynamic_cast<CFeature*>(s))) {
			if ((allyteam < 0) || f->IsInLosForAllyTeam(allyteam)) {
				if (!f->def->reclaimable) {
					return BUILDSQUARE_BLOCKED;
				}
				ret = BUILDSQUARE_RECLAIMABLE;
				feature = f;
			}
		} else if (!dynamic_cast<CUnit*>(s) || (allyteam < 0) ||
				(static_cast<CUnit*>(s)->losStatus[allyteam] & LOS_INLOS)) {
			if (s->immobile) {
				return BUILDSQUARE_BLOCKED;
			} else {
				ret = BUILDSQUARE_OCCUPIED;
			}
		}
	}

	const float groundHeight = ground->GetHeightReal(pos.x, pos.z, synced);

	if (!unitdef->floatOnWater || groundHeight > 0.0f) {
		// if we are capable of floating, only test local
		// height difference IF terrain is above sea-level
		const float* orgHeightMap = readmap->GetOriginalHeightMapSynced();
		const float* curHeightMap = readmap->GetCornerHeightMapSynced();

		#ifdef USE_UNSYNCED_HEIGHTMAP
		if (!synced) {
			orgHeightMap = readmap->GetCornerHeightMapUnsynced();
			curHeightMap = readmap->GetCornerHeightMapUnsynced();
		}
		#endif

		const int sqx = pos.x / SQUARE_SIZE;
		const int sqz = pos.z / SQUARE_SIZE;
		const float orgH = orgHeightMap[sqz * gs->mapxp1 + sqx];
		const float curH = curHeightMap[sqz * gs->mapxp1 + sqx];
		const float difH = unitdef->maxHeightDif;

		if (pos.y > std::max(orgH + difH, curH + difH)) { return BUILDSQUARE_BLOCKED; }
		if (pos.y < std::min(orgH - difH, curH - difH)) { return BUILDSQUARE_BLOCKED; }
	}

	if (!unitdef->IsAllowedTerrainHeight(groundHeight))
		ret = BUILDSQUARE_BLOCKED;

	return ret;
}



void CUnitHandler::AddBuilderCAI(CBuilderCAI* b)
{
	GML_STDMUTEX_LOCK(cai); // AddBuilderCAI

	builderCAIs.push_back(b);
}

void CUnitHandler::RemoveBuilderCAI(CBuilderCAI* b)
{
	GML_STDMUTEX_LOCK(cai); // RemoveBuilderCAI

	builderCAIs.remove(b);
}



/**
 * Returns a build Command that intersects the ray described by pos and dir from
 * the command queues of the units 'units' on team number 'team'.
 * @brief returns a build Command that intersects the ray described by pos and dir
 * @return the build Command, or a Command with id 0 if none is found
 */
Command CUnitHandler::GetBuildCommand(const float3& pos, const float3& dir) {
	float3 tempF1 = pos;

	GML_STDMUTEX_LOCK(cai); // GetBuildCommand

	CCommandQueue::iterator ci;
	for (std::list<CUnit*>::const_iterator ui = activeUnits.begin(); ui != activeUnits.end(); ++ui) {
		const CUnit* unit = *ui;

		if (unit->team != gu->myTeam) {
			continue;
		}

		ci = unit->commandAI->commandQue.begin();

		for (; ci != unit->commandAI->commandQue.end(); ++ci) {
			const Command& cmd = *ci;

			if (cmd.GetID() < 0 && cmd.params.size() >= 3) {
				BuildInfo bi(cmd);
				tempF1 = pos + dir * ((bi.pos.y - pos.y) / dir.y) - bi.pos;

				if (bi.def && (bi.GetXSize() / 2) * SQUARE_SIZE > math::fabs(tempF1.x) && (bi.GetZSize() / 2) * SQUARE_SIZE > math::fabs(tempF1.z)) {
					return cmd;
				}
			}
		}
	}

	Command c(CMD_STOP);
	return c;
}


bool CUnitHandler::CanBuildUnit(const UnitDef* unitdef, int team) const
{
	if (teamHandler->Team(team)->AtUnitLimit()) {
		return false;
	}
	if (unitsByDefs[team][unitdef->id].size() >= unitdef->maxThisUnit) {
		return false;
	}

	return true;
}
