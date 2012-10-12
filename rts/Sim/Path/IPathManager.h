/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef I_PATH_MANAGER_H
#define I_PATH_MANAGER_H

#include <boost/cstdint.hpp> /* Replace with <stdint.h> if appropriate */

#include "PFSTypes.h"
#include "System/float3.h"
#include "System/Platform/Threading.h"
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>

enum ThreadParam { THREAD_DUMMY };

#if THREADED_PATH
#define ST_FUNC ThreadParam dummy,
#define ST_CALL THREAD_DUMMY,
#define MT_WRAP
#else
#define ST_FUNC
#define ST_CALL
#define MT_WRAP ThreadParam dummy,
#endif

struct MoveDef;
class CSolidObject;

class IPathManager {
public:
	static IPathManager* GetInstance(unsigned int type, bool async);

	IPathManager();
	virtual ~IPathManager();

	virtual void MergePathCaches() = 0;

	virtual unsigned int GetPathFinderType() const = 0;
	virtual boost::uint32_t GetPathCheckSum() const { return 0; }

	enum PathRequestType { PATH_NONE, REQUEST_PATH, NEXT_WAYPOINT, DELETE_PATH, UPDATE_PATH, TERRAIN_CHANGE, PATH_UPDATED };

	struct ScopedDisableThreading {
#if THREADED_PATH
		bool oldThreading;
		ScopedDisableThreading(bool sync = true) : oldThreading(Threading::threadedPath) { ASSERT_SINGLETHREADED_SIM(); extern IPathManager* pathManager; if (sync) pathManager->SynchronizeThread(); Threading::threadedPath = false; }
		~ScopedDisableThreading() { Threading::threadedPath = oldThreading; }
#else
		ScopedDisableThreading(bool sync = true) {}
#endif
	};

	/**
	 * returns if a path was changed after RequestPath returned its pathID
	 * this can happen eg. if a PathManager reacts to TerrainChange events
	 * (by re-requesting affected paths without changing their ID's)
	 */

	virtual bool PathUpdated(ST_FUNC unsigned int pathID) { return false; }
	bool PathUpdated(MT_WRAP unsigned int pathID);

	virtual void Update(ST_FUNC int unused = 0) {}
	void Update(MT_WRAP int unused = 0) {
		ScopedDisableThreading sdt;
		Update(ST_CALL unused);
	}

	struct PathData {
		PathData() : pathID(-1), nextWayPoint(ZeroVector), updated(false) {}
		PathData(int pID, const float3& nwp) : pathID(pID), nextWayPoint(nwp), updated(false) {}
		int pathID;
		float3 nextWayPoint;
		bool updated;
	};

	PathData* GetPathData(int cid) {
		std::map<unsigned int, PathData>::iterator pit = pathInfos.find(cid);
		return (pit == pathInfos.end()) ? NULL : &(pit->second);
	}

	bool IsFailPath(unsigned int pathID);

	struct PathOpData {
		PathOpData() : type(PATH_NONE), moveDef(NULL), startPos(ZeroVector), goalPos(ZeroVector), minDistance(0.0f), owner(NULL), synced(false), pathID(-1), numRetries(0) {}
		PathOpData(PathRequestType tp, unsigned int pID, const MoveDef* md, const float3& sp, const float3& gp, float gr, const CSolidObject* own, bool sync):
		type(tp), moveDef(md), startPos(sp), goalPos(gp), goalRadius(gr), owner(own), synced(sync), pathID(pID), numRetries(0) {}
		PathOpData(PathRequestType tp, const CSolidObject* own, unsigned int pID):
		type(tp), moveDef(NULL), startPos(ZeroVector), minDistance(0.0f), owner(own), synced(false), pathID(pID), numRetries(0) {}
		PathOpData(PathRequestType tp, unsigned int pID):
		type(tp), moveDef(NULL), startPos(ZeroVector), goalPos(ZeroVector), minDistance(0.0f), owner(NULL), synced(false), pathID(pID), numRetries(0) {}
		PathOpData(PathRequestType tp, unsigned int pID, const float3& callPos, float minDist, int nRet, const CSolidObject* own, bool sync):
		type(tp), moveDef(NULL), startPos(callPos), goalPos(ZeroVector), minDistance(minDist), owner(own), synced(sync), pathID(pID), numRetries(nRet) {}
		PathOpData(PathRequestType tp, unsigned int x1, unsigned int z1, unsigned int x2, unsigned int z2):
		type(tp), moveDef(NULL), startPos(ZeroVector), goalPos(ZeroVector), cx1(x1), cx2(x2), synced(false), cz1(z1), cz2(z2) {}

		PathRequestType type;
		const MoveDef* moveDef;
		float3 startPos, goalPos;
		union {
			float goalRadius, minDistance;
			int cx1;
		};
		union {
			const CSolidObject* owner;
			int cx2;
		};
		bool synced;
		union {	int pathID, cz1; };
		union { int numRetries, cz2; };
	};

	struct PathUpdateData {
		PathUpdateData() : type(PATH_NONE), pathID(0), wayPoint(ZeroVector) {}
		PathUpdateData(PathRequestType t) : type(t), pathID(0), wayPoint(ZeroVector) {}
		PathUpdateData(PathRequestType t, unsigned int pID) : type(t), pathID(pID), wayPoint(ZeroVector) {}
		PathUpdateData(PathRequestType t, const float3& wP) : type(t), pathID(0), wayPoint(wP) {}
		PathUpdateData(PathRequestType t, const bool u) : type(t), updated(u), wayPoint(ZeroVector) {}
		PathRequestType type;
		union {
			unsigned int pathID;
			bool updated;
		};
		float3 wayPoint;
	};

	std::map<unsigned int, PathData> pathInfos;
	std::vector<PathOpData> pathOps;
	std::map<unsigned int, std::vector<PathUpdateData> > pathUpdates;
	std::map<int, unsigned int> newPathCache;
	static boost::thread *pathBatchThread;

	virtual void UpdatePath(ST_FUNC const CSolidObject* owner, unsigned int pathID) {}
	void UpdatePath(MT_WRAP const CSolidObject* owner, unsigned int pathID);

	/**
	 * When a path is no longer used, call this function to release it from
	 * memory.
	 * @param pathID
	 *     The path-id returned by RequestPath.
	 */
	virtual void DeletePath(ST_FUNC unsigned int pathID) {}
	void DeletePath(MT_WRAP unsigned int pathID);

	/**
	 * Returns the next waypoint of the path.
	 *
	 * @param pathID
	 *     The path-id returned by RequestPath.
	 * @param callerPos
	 *     The current position of the user of the path.
	 *     This extra information is needed to keep the path connected to its
	 *     user.
	 * @param minDistance
	 *     Could be used to set a minimum required distance between callerPos
	 *     and the returned waypoint.
	 * @param numRetries
	 *     Dont set this, used internally
	 * @param owner
	 *     The unit the path is used for, or NULL.
	 * @param synced
	 *     Whether this evaluation has to run synced or unsynced.
	 *     If false, this call may not change any state of the path manager
	 *     that could alter paths requested in the future.
	 *     example: if (synced == false) turn of heat-mapping
	 * @return
	 *     the next waypoint of the path, or (-1,-1,-1) in case no new
	 *     waypoint could be found.
	 */
	virtual float3 NextWayPoint(
		ST_FUNC
		unsigned int pathID,
		float3 callerPos,
		float minDistance = 0.0f,
		int numRetries = 0,
		const CSolidObject *owner = NULL,
		bool synced = true
	) { return ZeroVector; }
	float3 NextWayPoint(
		MT_WRAP
		unsigned int pathID,
		float3 callerPos,
		float minDistance = 0.0f,
		int numRetries = 0,
		const CSolidObject *owner = NULL,
		bool synced = true
	);

	/**
	 * Returns all waypoints of a path. Different segments of a path might
	 * have different resolutions, or each segment might be represented at
	 * multiple different resolution levels. In the former case, a subset
	 * of waypoints (those belonging to i-th resolution path SEGMENTS) are
	 * stored between points[starts[i]] and points[starts[i + 1]], while in
	 * the latter case ALL waypoints (of the i-th resolution PATH) are stored
	 * between points[starts[i]] and points[starts[i + 1]]
	 *
	 * @param pathID
	 *     The path-id returned by RequestPath.
	 * @param points
	 *     The list of waypoints.
	 * @param starts
	 *     The list of starting indices for the different resolutions
	 */
	virtual void GetPathWayPoints(
		ST_FUNC
		unsigned int pathID,
		std::vector<float3>& points,
		std::vector<int>& starts
	) const {}
	void GetPathWayPoints(
		MT_WRAP
		unsigned int pathID,
		std::vector<float3>& points,
		std::vector<int>& starts
	);


	/**
	 * Generate a path from startPos to the target defined by
	 * (goalPos, goalRadius).
	 * If no complete path from startPos to goalPos could be found,
	 * then a path getting as "close" as possible to target is generated.
	 *
	 * @param moveDef
	 *     Defines the move details of the unit to use the path.
	 * @param startPos
	 *     The starting location of the requested path.
	 * @param goalPos
	 *     The center of the path goal area.
	 * @param goalRadius
	 *     Use goalRadius to define a goal area within any square that could
	 *     be accepted as path goal.
	 *     If a singular goal position is wanted, use goalRadius = 0.
	 * @param caller
	 *     The unit or feature the path will be used for.
	 * @param synced
	 *     Whether this evaluation has to run synced or unsynced.
	 *     If false, this call may not change any state of the path manager
	 *     that could alter paths requested in the future.
	 *     example: if (synced == false) turn off heat-mapping
	 * @return
	 *     a path-id >= 1 on success, 0 on failure
	 *     Failure means, no path getting "closer" to goalPos then startPos
	 *     could be found
	 */
	virtual unsigned int RequestPath(
		ST_FUNC
		const MoveDef* moveDef,
		const float3& startPos,
		const float3& goalPos,
		float goalRadius = 8.0f,
		CSolidObject* caller = 0,
		bool synced = true
	) { return 0; }
	unsigned int RequestPath(
		MT_WRAP
		const MoveDef* moveDef,
		const float3& startPos,
		const float3& goalPos,
		float goalRadius = 8.0f,
		CSolidObject* caller = 0,
		bool synced = true
	);

	int GetPathID(int cid);

	void ThreadFunc();

	void SynchronizeThread();

	/**
	 * Whenever there are any changes in the terrain
	 * (examples: explosions, new buildings, etc.)
	 * this function will be called.
	 * @param x1
	 *     First corners X-axis value, defining the rectangular area
	 *     affected by the changes.
	 * @param z1
	 *     First corners Z-axis value, defining the rectangular area
	 *     affected by the changes.
	 * @param x2
	 *     Second corners X-axis value, defining the rectangular area
	 *     affected by the changes.
	 * @param z2
	 *     Second corners Z-axis value, defining the rectangular area
	 *     affected by the changes.
	 */
	virtual void TerrainChange(ST_FUNC unsigned int x1, unsigned int z1, unsigned int x2, unsigned int z2, unsigned int type) {}
	void TerrainChange(MT_WRAP unsigned int x1, unsigned int z1, unsigned int x2, unsigned int z2, unsigned int type) {
		ScopedDisableThreading sdt;
		TerrainChange(ST_CALL x1, z1 ,x2, z2, type);
	}

	virtual bool SetNodeExtraCosts(ST_FUNC const float* costs, unsigned int sizex, unsigned int sizez, bool synced) { return false; }
	bool SetNodeExtraCosts(MT_WRAP const float* costs, unsigned int sizex, unsigned int sizez, bool synced) {
		ScopedDisableThreading sdt;
		return SetNodeExtraCosts(ST_CALL costs, sizex, sizez, synced);
	}

	virtual bool SetNodeExtraCost(ST_FUNC unsigned int x, unsigned int z, float cost, bool synced) { return false; }
	bool SetNodeExtraCost(MT_WRAP unsigned int x, unsigned int z, float cost, bool synced) {
		ScopedDisableThreading sdt;
		return SetNodeExtraCost(ST_CALL x, z, cost, synced);
	}

	virtual float GetNodeExtraCost(ST_FUNC unsigned int x, unsigned int z, bool synced) const { return 0.0f; }
	float GetNodeExtraCost(MT_WRAP unsigned int x, unsigned int z, bool synced) {
		ScopedDisableThreading sdt;
		return GetNodeExtraCost(ST_CALL x, z, synced);
	}

	virtual const float* GetNodeExtraCosts(ST_FUNC bool synced) const { return NULL; }
	const float* GetNodeExtraCosts(MT_WRAP bool synced) {
		ScopedDisableThreading sdt;
		return GetNodeExtraCosts(ST_CALL synced);
	}

	unsigned int pathRequestID;
	boost::mutex preqMutex;
	bool wait;
	boost::condition_variable cond;
	volatile bool stopThread;
};

extern IPathManager* pathManager;

#endif
