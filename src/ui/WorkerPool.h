/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_WORKER_POOL_H
#define AIRTIME_WORKER_POOL_H


#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <stdint.h>
#include <stdlib.h>

#ifdef __HAIKU__
#include <OS.h>

extern "C" status_t _kern_set_thread_affinity(thread_id id,
	const void* userMask, size_t size);
#endif


namespace airtime {

/*!	The processors of the fastest kind when they are not all alike, as the
	RK3588's four Cortex-A76 beside its four A55; none otherwise. */
inline std::vector<int>
fast_processors()
{
	std::vector<int> fast;
#ifdef __HAIKU__
	cpu_topology_node_info nodes[256];
	uint32 count = 256;
	if (get_cpu_topology_info(nodes, &count) != B_OK)
		return fast;
	uint64 fastest = 0;
	uint64 slowest = UINT64_MAX;
	for (uint32 i = 0; i < count; i++) {
		if (nodes[i].type != B_TOPOLOGY_CORE)
			continue;
		fastest = std::max(fastest, nodes[i].data.core.default_frequency);
		slowest = std::min(slowest, nodes[i].data.core.default_frequency);
	}
	if (fastest == 0 || fastest == slowest)
		return fast;
	// A core's logical processors follow it, numbered as the scheduler
	// numbers them.
	uint64 frequency = 0;
	for (uint32 i = 0; i < count; i++) {
		if (nodes[i].type == B_TOPOLOGY_CORE)
			frequency = nodes[i].data.core.default_frequency;
		else if (nodes[i].type == B_TOPOLOGY_SMT && frequency == fastest
			&& nodes[i].id < 64) {
			fast.push_back((int)nodes[i].id);
		}
	}
#endif
	return fast;
}


/*!	Keeps the calling thread on the given processors. */
inline bool
keep_on_processors(const std::vector<int>& processors)
{
#ifdef __HAIKU__
	if (processors.empty())
		return false;
	uint32 mask[2] = {0, 0};	// the kernel's CPUSet, 64 processors
	for (int cpu : processors)
		mask[cpu / 32] |= 1u << (cpu % 32);
	return _kern_set_thread_affinity(find_thread(NULL), mask, sizeof(mask))
		== B_OK;
#else
	return false;
#endif
}


/*!	A few threads that split a picture's rows between them. Run() returns
	when every band is done.

	The rows go out in small bands, each taken by whichever thread is free.
	Where the cores differ, the work stays on the fast ones: on the RK3588
	an A55 takes six times as long as an A76 over a picture, so its share
	only ever came in last; there is a thread for each fast core, and the
	caller waits rather than work wherever it happens to be. Where there are
	only one or two fast cores among more slow ones (the Allwinner A733's
	two A76 and six A55), every core works: the fast ones are throttled
	first there, below the slow ones (416-780 MHz against 780-1014 when a
	4K film had heated the board), and with the A55s scaling and tone
	mapping a 4K film's picture held 24-28 ms where the A76s alone took
	50-60. AIRTIME_ALL_CORES uses every core anywhere. */
class WorkerPool {
public:
	WorkerPool(int threads = 0)
		:
		fStop(false),
		fGeneration(0),
		fPending(0),
		fCallerWorks(true)
	{
		if (threads <= 0) {
			fFast = fast_processors();
			unsigned cpus = std::thread::hardware_concurrency();
			if (getenv("AIRTIME_ALL_CORES") != NULL
				|| (fFast.size() <= 2 && cpus >= 2 * fFast.size() + 2)) {
				fFast.clear();
			}
			threads = !fFast.empty() ? (int)fFast.size()
				: cpus > 8 ? 8 : cpus > 1 ? (int)cpus : 2;
			fCallerWorks = fFast.empty();
			if (getenv("AIRTIME_POOL_THREADS") != NULL)
				threads = std::max(1, atoi(getenv("AIRTIME_POOL_THREADS")));
		}
		fCount = threads;
		for (int i = fCallerWorks ? 1 : 0; i < threads; i++)
			fThreads.emplace_back(&WorkerPool::_Work, this, i);
	}

	~WorkerPool()
	{
		{
			std::lock_guard<std::mutex> lock(fLock);
			fStop = true;
			fWake.notify_all();
		}
		for (std::thread& thread : fThreads)
			thread.join();
	}

	// Calls function(first, last) for bands of [0, rows).
	void Run(int rows, const std::function<void(int, int)>& function)
	{
		{
			std::lock_guard<std::mutex> lock(fLock);
			fFunction = function;
			fRows = rows;
			fGrain = std::max(1, rows / (fCount * 6));
			fNext.store(0);
			fPending = (int)fThreads.size();
			fGeneration++;
			fWake.notify_all();
		}
		if (fCallerWorks)
			_Bands();
		std::unique_lock<std::mutex> lock(fLock);
		while (fPending > 0)
			fDone.wait(lock);
		fFunction = nullptr;
	}

private:
	void _Bands()
	{
		for (;;) {
			int first = fNext.fetch_add(fGrain);
			if (first >= fRows)
				return;
			fFunction(first, std::min(first + fGrain, fRows));
		}
	}

	void _Work(int index)
	{
		keep_on_processors(fFast);
		uint64_t seen = 0;
		for (;;) {
			{
				std::unique_lock<std::mutex> lock(fLock);
				while (!fStop && fGeneration == seen)
					fWake.wait(lock);
				if (fStop)
					return;
				seen = fGeneration;
			}
			_Bands();
			std::lock_guard<std::mutex> lock(fLock);
			if (--fPending == 0)
				fDone.notify_all();
		}
	}

	std::vector<std::thread>	fThreads;
	std::vector<int>			fFast;
	int							fCount;
	std::mutex					fLock;
	std::condition_variable		fWake;
	std::condition_variable		fDone;
	bool						fStop;
	uint64_t					fGeneration;
	int							fPending;
	bool						fCallerWorks;
	int							fRows;
	int							fGrain;
	std::atomic<int>			fNext;
	std::function<void(int, int)> fFunction;
};

}	// namespace airtime

#endif	// AIRTIME_WORKER_POOL_H
