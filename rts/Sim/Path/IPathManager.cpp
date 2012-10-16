/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "IPathManager.h"
#include "Default/PathManager.h"
#include "QTPFS/PathManager.hpp"
#include "Game/GlobalUnsynced.h"
#include "Sim/Misc/ModInfo.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/Platform/CrashHandler.h"
#include "System/TimeProfiler.h"

IPathManager* pathManager = NULL;
boost::thread* IPathManager::pathBatchThread = NULL;

IPathManager* IPathManager::GetInstance(unsigned int type, bool async) {
	static IPathManager* pm = NULL;

	if (pm == NULL) {
		const char* fmtStr = "[IPathManager::GetInstance] using %s path-manager in %s mode";
		const char* typeStr = "";

		switch (type) {
			case PFS_TYPE_DEFAULT: { typeStr = "DEFAULT"; pm = new       CPathManager(); } break;
			case PFS_TYPE_QTPFS:   { typeStr = "QTPFS";   pm = new QTPFS::PathManager(); } break;
		}
		if (async)
			pathBatchThread = new boost::thread(boost::bind<void, IPathManager, IPathManager*>(&IPathManager::AsynchronousThread, pm));

		LOG(fmtStr, typeStr, async ? "asynchronous" : "synchronous");
	}

	return pm;
}

IPathManager::IPathManager() : pathRequestID(0), wait(false), stopThread(false) {
}


IPathManager::~IPathManager() {
	if (pathBatchThread != NULL) {
		{
			boost::mutex::scoped_lock preqLock(preqMutex);
			stopThread = true;
			if (wait) {
				wait = false;
				cond.notify_one();
			}
		}
		pathBatchThread->join();
		pathBatchThread = NULL;
	}
}


int IPathManager::GetPathID(int cid) {
	std::map<int, unsigned int>::iterator it = newPathCache.find(cid);
	if (it != newPathCache.end())
		return it->second;

	PathData* p = GetPathDataRaw(cid);
	return (p == NULL) ? -1 : p->pathID;
}

#define NOTIFY_PATH_THREAD(stm) \
	bool waited; \
	{ \
		boost::mutex::scoped_lock preqLock(preqMutex); \
		stm \
		if ((waited = wait)) \
			wait = false;\
	} \
	if (waited) \
		cond.notify_one();

bool IPathManager::PathUpdated(MT_WRAP unsigned int pathID) {
	if (!Threading::threadedPath) {
		if (!modInfo.asyncPathFinder)
			return PathUpdated(ST_CALL pathID);
		PathData* p = GetPathData(pathID);
		return (p != NULL && p->pathID >= 0) ? PathUpdated(ST_CALL p->pathID) : false;
	}
	PathData* p;
	NOTIFY_PATH_THREAD(
		p = GetNewPathData(pathID);
		pathOps.push_back(PathOpData(PATH_UPDATED, pathID));
	)
	return (p != NULL && p->pathID >= 0) ? p->updated : false;
}


void IPathManager::UpdatePath(MT_WRAP const CSolidObject* owner, unsigned int pathID) {
	if (!Threading::threadedPath) {
		if (!modInfo.asyncPathFinder)
			return UpdatePath(ST_CALL owner, pathID);
		PathData* p = GetPathData(pathID);
		if (p != NULL && p->pathID >= 0)
			UpdatePath(ST_CALL owner, p->pathID);
		return;
	}
	NOTIFY_PATH_THREAD(
		pathOps.push_back(PathOpData(UPDATE_PATH, owner, pathID));
	)
}


bool IPathManager::IsFailPath(unsigned int pathID) {
	if (!modInfo.asyncPathFinder)
		return false;
	boost::mutex::scoped_lock preqLock(preqMutex);

	PathData* p = GetNewPathData(pathID);
	return (p == NULL) || (p->pathID == 0);
}


void IPathManager::DeletePath(MT_WRAP unsigned int pathID) {
	if (!Threading::threadedPath) {
		if (!modInfo.asyncPathFinder)
			return DeletePath(ST_CALL pathID);
		PathData* p = GetPathData(pathID);
		if (p != NULL && p->pathID >= 0)
			DeletePath(ST_CALL p->pathID);
		pathInfos.erase(pathID);
		return;
	}
	PathData* p;
	NOTIFY_PATH_THREAD(
		p = GetNewPathData(pathID);
		pathOps.push_back(PathOpData(DELETE_PATH, pathID));
	)
	if (p) {
		p->deleted = true;
	}
}


float3 IPathManager::NextWayPoint(
	MT_WRAP
	unsigned int pathID,
	float3 callerPos,
	float minDistance,
	int numRetries,
	const CSolidObject *owner,
	bool synced
	) {
		if (!Threading::threadedPath) {
			if (!modInfo.asyncPathFinder)
				return NextWayPoint(ST_CALL pathID, callerPos, minDistance, numRetries, owner, synced);
			PathData* p = GetPathData(pathID);
			if (p == NULL || p->pathID < 0)
				return callerPos;
			p->nextWayPoint = NextWayPoint(ST_CALL p->pathID, callerPos, minDistance, numRetries, owner, synced);
			return p->nextWayPoint;
		}
		PathData* p;
		NOTIFY_PATH_THREAD(
			p = GetNewPathData(pathID);
			pathOps.push_back(PathOpData(NEXT_WAYPOINT, pathID, callerPos, minDistance, numRetries, owner, synced));
		)
		if (p == NULL || p->pathID < 0)
			return callerPos;
		return p->nextWayPoint;
}


void IPathManager::GetPathWayPoints(
	MT_WRAP
	unsigned int pathID,
	std::vector<float3>& points,
	std::vector<int>& starts
	) {
		if (!modInfo.asyncPathFinder)
			return GetPathWayPoints(ST_CALL pathID, points, starts);
		ScopedDisableThreading sdt;
		PathData* p = GetPathData(pathID);
		if (p == NULL || p->pathID < 0)
			return;
		return GetPathWayPoints(ST_CALL p->pathID, points, starts);
}


unsigned int IPathManager::RequestPath(
	MT_WRAP
	const MoveDef* moveDef,
	const float3& startPos,
	const float3& goalPos,
	float goalRadius,
	CSolidObject* caller,
	bool synced
	) {
		if (!Threading::threadedPath) {
			if (!modInfo.asyncPathFinder)
				return RequestPath(ST_CALL moveDef, startPos, goalPos, goalRadius, caller, synced);
			int cid = ++pathRequestID;
			pathInfos[cid] = PathData(RequestPath(ST_CALL moveDef, startPos, goalPos, goalRadius, caller, synced), startPos);
			return cid;
		}
		int cid;
		NOTIFY_PATH_THREAD(
			cid = ++pathRequestID;
			newPathInfos[cid] = PathData(-1, startPos);
			pathOps.push_back(PathOpData(REQUEST_PATH, cid, moveDef, startPos, goalPos, goalRadius, caller, synced));
		)
		return cid;
}


void IPathManager::AsynchronousThread() {
	streflop::streflop_init<streflop::Simple>();
	Threading::SetAffinityHelper("Path", configHandler->GetUnsigned("SetCoreAffinityPath"));

	while(true) {
		std::vector<PathOpData> pops;
		{
			boost::mutex::scoped_lock preqLock(preqMutex);
			if (stopThread)
				return;
			if (pathOps.empty()) {
				if (wait)
					cond.notify_one();
				wait = true;
				cond.wait(preqLock);
			}
			pathOps.swap(pops);
		}

		SCOPED_TIMER("IPathManager::AsynchronousThread");

		for (std::vector<PathOpData>::iterator i = pops.begin(); i != pops.end(); ++i) {
			PathOpData &cid = *i;
			unsigned int pid;
			switch(cid.type) {
				case REQUEST_PATH:
					pid = RequestPath(ST_CALL cid.moveDef, cid.startPos, cid.goalPos, cid.goalRadius, const_cast<CSolidObject*>(cid.owner), cid.synced);
					newPathCache[cid.pathID] = pid;
					pathUpdates[cid.pathID].push_back(PathUpdateData(REQUEST_PATH, pid));
					break;
				case NEXT_WAYPOINT:
					pid = GetPathID(cid.pathID);
					if (pid >= 0)
						pathUpdates[cid.pathID].push_back(PathUpdateData(NEXT_WAYPOINT, NextWayPoint(ST_CALL pid, cid.startPos, cid.minDistance, cid.numRetries, cid.owner, cid.synced)));
					break;
				case UPDATE_PATH:
					pid = GetPathID(cid.pathID);
					if (pid >= 0)
						UpdatePath(ST_CALL cid.owner, pid);
					break;
				case PATH_UPDATED:
					pid = GetPathID(cid.pathID);
					if (pid >= 0)
						pathUpdates[cid.pathID].push_back(PathUpdateData(PATH_UPDATED, PathUpdated(ST_CALL pid)));
					break;
					/*			case TERRAIN_CHANGE:
					TerrainChange(ST_CALL cid.cx1, cid.cz1, cid.cx2, cid.cz2);
					break;*/
				case DELETE_PATH:
					pid = GetPathID(cid.pathID);
					if (pid >= 0) {
						DeletePath(ST_CALL pid);
						pathUpdates[cid.pathID].push_back(PathUpdateData(DELETE_PATH));
					}
					newPathCache.erase(cid.pathID);
					break;
				default:
					LOG_L(L_ERROR,"Invalid path request %d", cid.type);
			}
		}
	}
}


void IPathManager::SynchronizeThread() {
	ASSERT_SINGLETHREADED_SIM();
	if (pathBatchThread == NULL)
		return;

	SCOPED_TIMER("IPathManager::SynchronizeThread"); // lots of waiting here means the asynchronous mechanism is ineffient

	boost::mutex::scoped_lock preqLock(preqMutex);
	if (!wait) {
		wait = true;
		cond.wait(preqLock);
	}
	for (std::map<unsigned int, PathData>::iterator i = newPathInfos.begin(); i != newPathInfos.end(); ++i) {
		pathInfos[i->first] = i->second;
	}
	newPathInfos.clear();

	for (std::map<unsigned int, std::vector<PathUpdateData> >::iterator i  = pathUpdates.begin(); i != pathUpdates.end(); ++i) {
		for (std::vector<PathUpdateData>::iterator v  = i->second.begin(); v != i->second.end(); ++v) {
			PathUpdateData &u = *v;
			switch(u.type) {
				case REQUEST_PATH:
					pathInfos[i->first].pathID = u.pathID;
					break;
				case NEXT_WAYPOINT:
					pathInfos[i->first].nextWayPoint = u.wayPoint;
					break;
				case PATH_UPDATED:
					pathInfos[i->first].updated = u.updated;
					break;
				case DELETE_PATH:
					pathInfos.erase(i->first);
					break;
				default:
					LOG_L(L_ERROR,"Invalid path update %d", u.type);
			}
		}
	}

	newPathCache.clear();
	pathUpdates.clear();
}
