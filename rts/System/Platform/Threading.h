/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _THREADING_H_
#define _THREADING_H_

#include <string>
#ifdef WIN32
	#include <windows.h> // HANDLE & DWORD
#else
	#include <pthread.h>
#endif
#ifdef __APPLE__
	#include <libkern/OSAtomic.h> // OSAtomicIncrement64
#endif
#include "System/Platform/Win/win32.h"
#include <boost/cstdint.hpp>
#include "lib/gml/gml_base.h"
#include "System/Platform/ThreadingConfig.h"
//#include "lib/boost/atomic.hpp"

#define DEBUG_MULTITHREADED_SIM (0 && GML_ENABLE_SIM) // enable debug output, requires GML
#define DEBUG_THREADED_PATH (0 && GML_ENABLE_SIM) // enable debug output, requires GML
#define DEBUG_STABLE_UPDATE (0 && GML_ENABLE_SIM) // enable debug output, requires GML

namespace Threading {
	/**
	 * Generic types & functions to handle OS native threads
	 */
#ifdef WIN32
	typedef DWORD     NativeThreadId;
	typedef HANDLE    NativeThreadHandle;
#else
	typedef pthread_t NativeThreadId;
	typedef pthread_t NativeThreadHandle;
#endif
	NativeThreadHandle GetCurrentThread();
	NativeThreadId GetCurrentThreadId();
	inline bool NativeThreadIdsEqual(const NativeThreadId thID1, const NativeThreadId thID2);

	/**
	 * Sets the affinity of the current thread
	 *
	 * Interpret <cores_bitmask> as a bit-mask indicating on which of the available system CPU's (which
	 * are numbered logically from 1 to N) we want to run.
	 * Note: that this approach will fail when N > 32.
	 */
	boost::uint32_t SetAffinity(boost::uint32_t cores_bitmask, bool hard = true);
	void SetAffinityHelper(const char *threadName, boost::uint32_t affinity);
	unsigned GetAvailableCores();

	/**
	 * Inform the OS kernel that we are a cpu-intensive task
	 */
	void SetThreadScheduler();

	/**
	 * Used to detect the main-thread which runs SDL, GL, Input, Sim, ...
	 */
	void SetMainThread();
	bool IsMainThread();
	bool IsMainThread(NativeThreadId threadID);


	/**
	 * GML Functions
	 */
	void SetSimThread(bool set);
	bool IsSimThread();
	void SetBatchThread(bool set);
	bool IsBatchThread();

#if MULTITHREADED_SIM
	extern void MultiThreadSimErrorFunc();
	extern void ThreadNotUnitOwnerErrorFunc();
	#define ASSERT_SINGLETHREADED_SIM() do { if (DEBUG_MULTITHREADED_SIM && Threading::multiThreadedSim && (Threading::IsSimThread() || GML::ThreadNumber()>GML_MAX_NUM_THREADS)) Threading::MultiThreadSimErrorFunc(); } while(false)
	#define ASSERT_THREAD_OWNS_UNIT() do { if (DEBUG_MULTITHREADED_SIM && Threading::multiThreadedSim && Threading::threadCurrentUnitIDs[GML::ThreadNumber()] != id) { Threading::ThreadNotUnitOwnerErrorFunc(); } } while (false)
	inline void SetMultiThreadedSim(bool mts) { multiThreadedSim = mts; }
	extern int threadCurrentUnitIDs[];
	inline void SetThreadCurrentUnitID(int id) { if (DEBUG_MULTITHREADED_SIM) threadCurrentUnitIDs[GML::ThreadNumber()] = id; }
#else
	#define ASSERT_SINGLETHREADED_SIM()
	#define ASSERT_THREAD_OWNS_UNIT()
	inline void SetMultiThreadedSim(bool mts) {}
	inline void SetThreadCurrentUnitID(int id) {}
#endif
#if THREADED_PATH
	extern void NonThreadedPathErrorFunc();
	#define ASSERT_NONTHREADED_PATH() do { if (DEBUG_THREADED_PATH && Threading::threadedPath) Threading::NonThreadedPathErrorFunc(); } while(false)
	inline void SetThreadedPath(bool tp) { threadedPath = tp; }
#else
	#define ASSERT_NONTHREADED_PATH()
	inline void SetThreadedPath(bool tp) {}
#endif

	/**
	 * Give the current thread a name (posix-only)
	 */
	void SetThreadName(std::string newname);

	/**
	 * Used to raise errors in the main-thread issued by worker-threads
	 */
	struct Error {
		Error() : flags(0) {}
		Error(const std::string& _caption, const std::string& _message, const unsigned int _flags) : caption(_caption), message(_message), flags(_flags) {}

		std::string caption;
		std::string message;
		unsigned int flags;
	};
	void SetThreadError(const Error& err);
	Error* GetThreadError();


	/**
	 * A 64bit atomic counter
	 */
	struct AtomicCounterInt64;
};

//
// Inlined Definitions
//
namespace Threading {
	bool NativeThreadIdsEqual(const NativeThreadId thID1, const NativeThreadId thID2)
	{
	#ifdef WIN32
		return (thID1 == thID2);
	#else
		return pthread_equal(thID1, thID2);
	#endif
	}


	struct AtomicCounterInt64 {
	public:
		AtomicCounterInt64(boost::int64_t start = 0) : num(start) {}

		boost::int64_t operator++() {
	#ifdef _MSC_VER
			return InterlockedIncrement64(&num);
	#elif defined(__APPLE__)
			return OSAtomicIncrement64(&num);
	#else // assuming GCC (__sync_fetch_and_add is a builtin)
			return __sync_fetch_and_add(&num, boost::int64_t(1)) + 1;
	#endif
		}
	private:
	#ifdef _MSC_VER
		volatile __declspec(align(8)) boost::int64_t num;
	#else
		volatile __attribute__ ((aligned (8))) boost::int64_t num;
	#endif
	};

	struct AtomicCounterInt {
	public:
		AtomicCounterInt(boost::int32_t start = 0) : num(start) {}

		boost::int32_t operator=(boost::int32_t val) {
	#ifdef _MSC_VER
			InterlockedExchange((volatile unsigned int *)&num, val);
	#elif defined(__APPLE__)
			OSAtomicTestAndSet(&(boost::uint32_t*)num, val);
	#else // assuming GCC (__sync_fetch_and_add is a builtin)
			__sync_lock_test_and_set(&num, boost::int32_t(1));
	#endif
			return val;
		}
		boost::int32_t operator++() {
	#ifdef _MSC_VER
			return InterlockedIncrement((volatile unsigned int*)&num);
	#elif defined(__APPLE__)
			return OSAtomicIncrement32(&num);
	#else // assuming GCC (__sync_fetch_and_add is a builtin)
			return __sync_fetch_and_add(&num, boost::int32_t(1)) + 1;
	#endif
		}
	private:
	#ifdef _MSC_VER
		volatile __declspec(align(4)) boost::int32_t num;
	#else
		volatile __attribute__ ((aligned (4))) boost::int32_t num;
	#endif
	};
	/*
	struct AtomicCount {
		AtomicCount(int c) : cnt(c) {}
		boost::atomic<int> cnt;
		int operator=(int c) { cnt.exchange(c, boost::memory_order_relaxed); return c; }
		int operator++() {
			return cnt.fetch_add(1, boost::memory_order_relaxed) + 1;
		}
	};
	*/
}

#endif // _THREADING_H_
