/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef SOLID_OBJECT_H
#define SOLID_OBJECT_H

#include "WorldObject.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "System/Vec2.h"
#include "System/Misc/BitwiseEnum.h"
#include "System/Platform/Threading.h"
#include "System/Sync/SyncedFloat3.h"
#include "System/Sync/SyncedPrimitive.h"
#include <deque>


struct CollisionVolume;
struct SolidObjectDef;
struct MoveDef;
struct SolidObjectGroundDecal;

struct DamageArray;
class CUnit;

enum TerrainChangeTypes {
	TERRAINCHANGE_DAMAGE_RECALCULATION = 0, // update after regular explosion or terraform event
	TERRAINCHANGE_SQUARE_TYPEMAP_INDEX = 1, // update after typemap-index of a square changed (Lua)
	TERRAINCHANGE_TYPEMAP_SPEED_VALUES = 2, // update after speed-values of a terrain-type changed (Lua)
	TERRAINCHANGE_OBJECT_INSERTED      = 3,
	TERRAINCHANGE_OBJECT_INSERTED_YM   = 4,
	TERRAINCHANGE_OBJECT_DELETED       = 5,
};

enum YardmapStates {
	YARDMAP_OPEN        = 0,    // always free      (    walkable      buildable)
	//YARDMAP_WALKABLE    = 4,    // open for walk    (    walkable, not buildable)
	YARDMAP_YARD        = 1,    // walkable when yard is open
	YARDMAP_YARDINV     = 2,    // walkable when yard is closed
	YARDMAP_BLOCKED     = 0xFF & ~YARDMAP_YARDINV, // always block     (not walkable, not buildable)

	// helpers
	YARDMAP_YARDBLOCKED = YARDMAP_YARD,
	YARDMAP_YARDFREE    = ~YARDMAP_YARD,
	YARDMAP_GEO         = YARDMAP_BLOCKED,
};
typedef BitwiseEnum<YardmapStates> YardMapStatus;



class CSolidObject: public CWorldObject {
public:
	CR_DECLARE(CSolidObject)

	enum PhysicalState {
		OnGround,
		Floating,
		Hovering,
		Flying,
	};
	enum DamageType {
		DAMAGE_EXPLOSION_WEAPON = 0, // weapon-projectile that triggered GameHelper::Explosion (weaponDefID >= 0)
		DAMAGE_EXPLOSION_DEBRIS = 1, // piece-projectile that triggered GameHelper::Explosion (weaponDefID < 0)
		DAMAGE_COLLISION_GROUND = 2, // ground collision
		DAMAGE_COLLISION_OBJECT = 3, // object collision
		DAMAGE_EXTSOURCE_FIRE   = 4,
		DAMAGE_EXTSOURCE_WATER  = 5, // lava/acid/etc
		DAMAGE_EXTSOURCE_KILLED = 6,
	};
	enum DelayOpType {
		SCRIPT_STOPMOVING,
		SCRIPT_STARTMOVING,
		SCRIPT_LANDED,
		SCRIPT_MOVERATE,
		CAI_SLOWUPDATE,
		CAI_STOPMOVE,
		FAIL,
		ACTIVATE,
		DEACTIVATE,
		BLOCK,
		UNBLOCK,
		UNITUNIT_COLLISION,
		UNITFEAT_COLLISION,
		BUGGEROFF,
		KILLUNIT,
		MOVE,
		UNRESERVEPAD,
		CHECKNOTIFY,
		MOVE_FEATURE,
		MOVE_UNIT,
		DODAMAGE,
		CHANGE_SPEED,
		KILL,
		SET_SKIDDING,
		UPDATE_MIDAIMPOS,
		ADDBUILDPOWER,
		GETAIRBASEPIECEPOS,
		MOVE_UNIT_OLDPOS
	};
	struct DelayOp {
		DelayOp(DelayOpType t) : type(t), obj(NULL), vec(ZeroVector), damage(0.0f), dmgtype(0) {}
		DelayOp(DelayOpType t, const CSolidObject *o) : type(t), obj(o), vec(ZeroVector), damage(0.0f), dmgtype(0) {}
		DelayOp(DelayOpType t, const CSolidObject *o, bool bs) : type(t), obj(o), vec(ZeroVector), bset(bs), dmgtype(0) {}
		DelayOp(DelayOpType t, int d) : type(t), data(d), vec(ZeroVector), damage(0.0f), dmgtype(0) {}
		DelayOp(DelayOpType t, const CSolidObject *o, const float3& v) : type(t), obj(o), vec(v), damage(0.0f), dmgtype(0) {}
		DelayOp(DelayOpType t, const CSolidObject *o, const float3& v, bool rel, bool tc) : type(t), obj(o), vec(v), relative(rel), terrcheck(tc) {}
		DelayOp(DelayOpType t, const CSolidObject *o, bool crs, const float3& v) : type(t), obj(o), vec(v), crush(crs), dmgtype(0) {}
		DelayOp(DelayOpType t, const CSolidObject *o, float dmg, const float3& impulse, int dt) : type(t), obj(o), vec(impulse), damage(dmg), dmgtype(dt) {}
		DelayOp(DelayOpType t, const CSolidObject *o, const float3& add, float mul) : type(t), obj(o), vec(add), mult(mul), dmgtype(0) {}
		DelayOp(DelayOpType t, float amt, const CSolidObject *o) : type(t), obj(o), vec(ZeroVector), amount(amt), dmgtype(0) {}
		DelayOpType type;
		union {
			const CSolidObject *obj;
			int data;
		};
		float3 vec;
		union {
			float damage, mult, amount;
			bool relative, crush, bset;
		};
		union {
			int dmgtype;
			bool terrcheck;
		};
	};

	CSolidObject();
	virtual ~CSolidObject();

	virtual bool AddBuildPower(float amount, CUnit* builder) { return false; }
	virtual void DoDamage(const DamageArray& damages, const float3& impulse, CUnit* attacker, int weaponDefID) {}
	virtual void Kill(const float3& impulse, bool crushKill);
	virtual int GetBlockingMapID() const { return -1; }

	virtual void ForcedMove(const float3& newPos, bool snapToGround = true) {}
	virtual void ForcedSpin(const float3& newDir) {}

	void Move3D(const float3& v, bool relative) {
		const float3& dv = relative? v: (v - pos);

		pos += dv;
		midPos += dv;
		aimPos += dv;
	}

	void Move1D(const float v, int d, bool relative) {
		const float dv = relative? v: (v - pos[d]);

		pos[d] += dv;
		midPos[d] += dv;
		aimPos[d] += dv;
	}

	// this should be called whenever the direction
	// vectors are changed (ie. after a rotation) in
	// eg. movetype code
	void UpdateMidAndAimPos() {
		midPos = GetMidPos();
		aimPos = GetAimPos();
	}
	void SetMidAndAimPos(const float3& mp, const float3& ap, bool relative) {
		SetMidPos(mp, relative);
		SetAimPos(ap, relative);
	}

	/**
	 * Adds this object to the GroundBlockingMap if and only if its collidable
	 * property is set (blocking), else does nothing (except call UnBlock()).
	 */
	void Block();
	/**
	 * Removes this object from the GroundBlockingMap if it is currently marked
	 * on it, does nothing otherwise.
	 */
	void UnBlock();

	int2 GetMapPos() const { return (GetMapPos(pos)); }
	int2 GetMapPos(const float3& position) const;

	YardMapStatus GetGroundBlockingMaskAtPos(float3 gpos) const;

private:
	void SetMidPos(const float3& mp, bool relative) {
		if (relative) {
			relMidPos = mp; midPos = GetMidPos();
		} else {
			midPos = mp; relMidPos = midPos - pos;
		}
	}
	void SetAimPos(const float3& ap, bool relative) {
		if (relative) {
			relAimPos = ap; aimPos = GetAimPos();
		} else {
			aimPos = ap; relAimPos = aimPos - pos;
		}
	}

	float3 GetMidPos() const {
		const float3 dz = (frontdir * relMidPos.z);
		const float3 dy = (updir    * relMidPos.y);
		const float3 dx = (rightdir * relMidPos.x);

		return (pos + dz + dy + dx);
	}
	float3 GetAimPos() const {
		const float3 dz = (frontdir * relAimPos.z);
		const float3 dy = (updir    * relAimPos.y);
		const float3 dx = (rightdir * relAimPos.x);

		return (pos + dz + dy + dx);
	}

public:
	float health;
	float mass;                                 ///< the physical mass of this object (run-time constant)
	float crushResistance;                      ///< how much MoveDef::crushStrength is required to crush this object (run-time constant)

	bool blocking;                              ///< if this object can be collided with at all (NOTE: Some objects could be flat => not collidable.)
	bool crushable;                             ///< whether this object can potentially be crushed during a collision with another object
	bool immobile;                              ///< whether this object can be moved or not (except perhaps along y-axis, to make it stay on ground)
	bool crushKilled;                           ///< true if this object died by being crushed during a collision
	bool blockEnemyPushing;                     ///< if false, object can be pushed during enemy collisions even when modrules forbid it
	bool blockHeightChanges;                    ///< if true, map height cannot change under this object (through explosions, etc.)

	bool luaDraw;                               ///< if true, LuaRules::Draw{Unit, Feature} will be called for this object (UNSYNCED)
	bool noSelect;                              ///< if true, unit/feature can not be selected/mouse-picked by a player (UNSYNCED)

	int xsize;                                  ///< The x-size of this object, according to its footprint. (Note: this is rotated depending on buildFacing!!!)
	int zsize;                                  ///< The z-size of this object, according to its footprint. (Note: this is rotated depending on buildFacing!!!)
	int2 footprint;                             ///< The unrotated x-/z-size of this object, according to its footprint.

	SyncedSshort heading;                       ///< Contains the same information as frontdir, but in a short signed integer.
	PhysicalState physicalState;                ///< The current state of the object within the gameworld. I.e Flying or OnGround.

	bool isMoving;                              ///< = velocity.length() > 0.0
	bool isUnderWater;                          ///< true if this object is completely submerged (pos + height < 0)
	bool isMarkedOnBlockingMap;                 ///< true if this object is currently marked on the GroundBlockingMap

	float3 speed;                               ///< current velocity vector (length in elmos/frame)
	float3 residualImpulse;                     ///< Used to sum up external impulses.

	int team;                                   ///< team that "owns" this object
	int allyteam;                               ///< allyteam that this->team is part of

	const SolidObjectDef* objectDef;            ///< points to a UnitDef or to a FeatureDef instance
	MoveDef* moveDef;                           ///< holds information about the mobility of this object (if NULL, object is either static or aircraft)
	CollisionVolume* collisionVolume;
	SolidObjectGroundDecal* groundDecal;

	SyncedFloat3 frontdir;                      ///< object-local z-axis (in WS)
	SyncedFloat3 rightdir;                      ///< object-local x-axis (in WS)
	SyncedFloat3 updir;                         ///< object-local y-axis (in WS)

	SyncedFloat3 relMidPos;                     ///< local-space vector from pos to midPos (read from model, used to initialize midPos)
	SyncedFloat3 relAimPos;                     ///< local-space vector from pos to aimPos (read from model, used to initialize aimPos)
	SyncedFloat3 midPos;                        ///< mid-position of model in WS, used as center of mass (and many other things)
	SyncedFloat3 aimPos;                        ///< used as aiming position by weapons
	int2 mapPos;                                ///< current position on GroundBlockingObjectMap

	float3 drawPos;                             ///< = pos + speed * timeOffset (unsynced)
	float3 drawMidPos;                          ///< = drawPos + relMidPos (unsynced)

	const YardMapStatus* blockMap;              ///< Current (unrotated!) blockmap/yardmap of this object. 0 means no active yardmap => all blocked.
	int buildFacing;                            ///< Orientation of footprint, 4 different states

#if STABLE_UPDATE
	bool stableBlocking;
	float3 stablePos;
	SyncedFloat3 stableMidPos;
	float stableHeight;
	bool stableIsUnderWater;
	float stableRadius;
	int stableXSize;
	int stableZSize;
	float stableMass;
	SyncedFloat3 stableFrontDir;
	SyncedFloat3 stableRightDir;
	SyncedFloat3 stableUpDir;
	float3 stableSpeed;
	bool stableIsMoving;
	bool stableCrushable;
	float stableCrushResistance;
	PhysicalState stablePhysicalState;
	// shall return "stable" values, that do not suddenly change during a sim frame. (for multithreading purposes)
	const bool StableBlocking() const { return stableBlocking; }
	const float3& StablePos() const { return stablePos; }
	const SyncedFloat3& StableMidPos() const { return stableMidPos; }
	const float StableHeight() const { return stableHeight; }
	const bool StableUnderWater() const { return stableIsUnderWater; }
	const float StableRadius() const { return stableRadius; }
	const int StableXSize() const { return stableXSize; }
	const int StableZSize() const { return stableZSize; }
	const float StableMass() const { return stableMass; }
	const SyncedFloat3& StableFrontDir() const { return stableFrontDir; }
	const SyncedFloat3& StableRightDir() const { return stableRightDir; }
	const SyncedFloat3& StableUpDir() const { return stableUpDir; }
	const float3& StableSpeed() const { return stableSpeed; }
	const bool StableIsMoving() const { return stableIsMoving; }
	const bool StableCrushable() const { return stableCrushable; }
	const float StableCrushResistance() const { return stableCrushResistance; }
	const bool StableImmobile() const { return immobile; } // is stable by itself
	const PhysicalState StablePhysicalState() const { return stablePhysicalState; }
	const int StableAllyTeam() const { return allyteam; } // is stable by itself

	virtual void StableUpdate(bool slow);
	void StableSlowUpdate();
#else
	const bool StableBlocking() const { return blocking; }
	const float3& StablePos() const { return pos; }
	const SyncedFloat3& StableMidPos() const { return midPos; }
	const float StableHeight() const { return height; }
	const bool StableUnderWater() const { return isUnderWater; }
	const float StableRadius() const { return radius; }
	const int StableXSize() const { return xsize; }
	const int StableZSize() const { return zsize; }
	const float StableMass() const { return mass; }
	const SyncedFloat3& StableFrontDir() const { return frontdir; }
	const SyncedFloat3& StableRightDir() const { return rightdir; }
	const SyncedFloat3& StableUpDir() const { return updir; }
	const float3& StableSpeed() const { return speed; }
	const bool StableIsMoving() const { return isMoving; }
	const bool StableCrushable() const { return crushable; }
	const float StableCrushResistance() const { return crushResistance; }
	const bool StableImmobile() const { return immobile; }
	const PhysicalState StablePhysicalState() const { return physicalState; }
	const int StableAllyTeam() const { return allyteam; } // is stable by itself
#endif


	std::deque<DelayOp> delayOps;

	static const float DEFAULT_MASS;
	static const float MINIMUM_MASS;
	static const float MAXIMUM_MASS;

	static int deletingRefID;
	static void SetDeletingRefID(int id) { deletingRefID = id; }
	// returns the object (command reference) id of the object currently being deleted,
	// for units this equals unit->id, and for features feature->id + uh->MaxUnits()
	static int GetDeletingRefID() { return deletingRefID; }
	static std::set<CSolidObject *> solidObjects;
	static void UpdateStableData();
};

#endif // SOLID_OBJECT_H
