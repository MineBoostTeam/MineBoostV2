// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 ConsumptionHUD -- background sampler for the client
// process's own RAM/CPU/GPU usage, shown by Hud::drawConsumptionHud() in
// src/client/hud.cpp.
//
// Runs on its own thread rather than sampling inline in the render loop:
// on Windows, getting per-process GPU usage means asking PDH's "GPU
// Engine" counters for a rate, which needs two samples a short delay
// apart to produce a valid value (PDH_CSTATUS_INVALID_DATA on the very
// first collection) -- doing that synchronously every frame would stall
// rendering. Everything here is cheap to read from the render thread
// (plain std::atomic<float> loads); the actual OS queries only happen
// once a second on this thread.

#pragma once

#include <atomic>
#include "threading/thread.h"

class PerfMonitor : public Thread
{
public:
	static PerfMonitor &get();

	// All three return the most recent sample; -1.0f means "not
	// available on this platform/build" (currently: GPU usage outside
	// Windows -- see sampleGpu() in perfmonitor.cpp).
	float getRamMb() const { return m_ram_mb.load(std::memory_order_relaxed); }
	float getCpuPercent() const { return m_cpu_percent.load(std::memory_order_relaxed); }
	float getGpuPercent() const { return m_gpu_percent.load(std::memory_order_relaxed); }

protected:
	void *run() override;

private:
	PerfMonitor();
	~PerfMonitor() override;
	DISABLE_CLASS_COPY(PerfMonitor)

	void sampleRamAndCpu();
	void sampleGpu();

	std::atomic<float> m_ram_mb{0.0f};
	std::atomic<float> m_cpu_percent{0.0f};
	std::atomic<float> m_gpu_percent{-1.0f};

#ifdef _WIN32
	unsigned long long m_last_cpu_100ns = 0;
	unsigned long long m_last_wall_100ns = 0;
	int m_num_cores = 1;
	// Opaque PDH_HQUERY -- kept as void* here so this header doesn't need
	// to drag <pdh.h> (and the Windows.h it implies) into every
	// translation unit that just wants to read a sampled value.
	void *m_pdh_query = nullptr;
	// Opaque std::vector<PDH_HCOUNTER>* -- the handles PdhAddCounterW()
	// returned for every "pid_<our PID>_..." GPU Engine instance found on
	// the last rescan; summed each sample in sampleGpu().
	void *m_pdh_counters = nullptr;
	// Re-built periodically (see kGpuRescanIntervalS in perfmonitor.cpp)
	// rather than maintained incrementally, since new GPU "engine"
	// instances (3D, Copy, VideoDecode, ...) for this process can appear
	// or disappear over time and PDH has no per-instance add/remove
	// notification -- simplest correct approach is to periodically
	// re-enumerate and rebuild the counter list from scratch.
	int m_gpu_rescan_counter = 0;
#else
	long m_last_cpu_ticks = 0;
	double m_last_wall_seconds = 0;
#endif
};
