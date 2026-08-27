// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "client/perfmonitor.h"

#include "porting.h"
#include <memory>
#include <vector>

#ifdef _WIN32
	#include <windows.h>
	#include <psapi.h>
	#include <pdh.h>
	#include <pdhmsg.h>
#else
	#include <cstdio>
	#include <unistd.h>
	#include <sys/resource.h>
	#include <time.h>
#endif

namespace {
	// How often the background thread actually queries the OS. GPU
	// sampling specifically needs two PDH collections at least this far
	// apart to produce a valid rate (see sampleGpu()), so this doubles as
	// that spacing.
	constexpr int kSampleIntervalMs = 1000;
	// How many samples between a full re-enumeration of GPU Engine
	// counter instances for this process (Windows only) -- see the
	// comment on m_gpu_rescan_counter in perfmonitor.h.
	constexpr int kGpuRescanEverySamples = 5;
}

PerfMonitor &PerfMonitor::get()
{
	static PerfMonitor instance;
	return instance;
}

PerfMonitor::PerfMonitor(): Thread("PerfMonitor")
{
#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	m_num_cores = (int)si.dwNumberOfProcessors;
	if (m_num_cores < 1)
		m_num_cores = 1;
#endif
	start();
}

PerfMonitor::~PerfMonitor()
{
	stop();
	wait();
#ifdef _WIN32
	if (m_pdh_query)
		PdhCloseQuery((PDH_HQUERY)m_pdh_query);
	delete (std::vector<PDH_HCOUNTER> *)m_pdh_counters;
#endif
}

void *PerfMonitor::run()
{
	// First sample of CPU usage has nothing to diff against yet, so seed
	// the baseline before the loop instead of reporting a bogus 0%/huge%
	// on the very first real sample.
	sampleRamAndCpu();

	while (!stopRequested()) {
		sleep_ms(kSampleIntervalMs);
		if (stopRequested())
			break;
		sampleRamAndCpu();
		sampleGpu();
	}
	return nullptr;
}

#ifdef _WIN32

void PerfMonitor::sampleRamAndCpu()
{
	PROCESS_MEMORY_COUNTERS pmc;
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
		m_ram_mb.store((float)(pmc.WorkingSetSize / (1024.0 * 1024.0)),
			std::memory_order_relaxed);

	FILETIME creation, exit, kernel, user;
	if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
		ULARGE_INTEGER k, u;
		k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
		u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;
		unsigned long long cpu_100ns = k.QuadPart + u.QuadPart;

		FILETIME wall_ft;
		GetSystemTimeAsFileTime(&wall_ft);
		ULARGE_INTEGER w;
		w.LowPart = wall_ft.dwLowDateTime; w.HighPart = wall_ft.dwHighDateTime;
		unsigned long long wall_100ns = w.QuadPart;

		if (m_last_wall_100ns != 0 && wall_100ns > m_last_wall_100ns) {
			unsigned long long cpu_delta = cpu_100ns - m_last_cpu_100ns;
			unsigned long long wall_delta = wall_100ns - m_last_wall_100ns;
			// Normalized to total available CPU (100% = every core
			// fully busy), matching Task Manager's per-process %CPU
			// convention, not "100% = one core".
			double pct = (100.0 * (double)cpu_delta) /
				((double)wall_delta * m_num_cores);
			if (pct < 0.0) pct = 0.0;
			if (pct > 100.0) pct = 100.0;
			m_cpu_percent.store((float)pct, std::memory_order_relaxed);
		}
		m_last_cpu_100ns = cpu_100ns;
		m_last_wall_100ns = wall_100ns;
	}
}

void PerfMonitor::sampleGpu()
{
	PDH_HQUERY query = (PDH_HQUERY)m_pdh_query;
	auto *counters = (std::vector<PDH_HCOUNTER> *)m_pdh_counters;

	// Every kGpuRescanEverySamples samples, throw away whatever counters
	// we had and re-enumerate from scratch -- see the comment on
	// m_gpu_rescan_counter in perfmonitor.h for why.
	bool need_rescan = (m_gpu_rescan_counter % kGpuRescanEverySamples) == 0;
	m_gpu_rescan_counter++;

	if (!query || need_rescan) {
		if (query)
			PdhCloseQuery(query);
		if (!counters) {
			counters = new std::vector<PDH_HCOUNTER>();
			m_pdh_counters = counters;
		}
		counters->clear(); // PdhCloseQuery() above already invalidated these handles.

		if (PdhOpenQuery(NULL, 0, &query) != ERROR_SUCCESS) {
			m_pdh_query = nullptr;
			m_gpu_percent.store(-1.0f, std::memory_order_relaxed);
			return;
		}
		m_pdh_query = query;

		DWORD pid = GetCurrentProcessId();
		wchar_t pid_tag[32];
		swprintf(pid_tag, 32, L"pid_%lu_", (unsigned long)pid);

		DWORD counter_list_size = 0, instance_list_size = 0;
		PdhEnumObjectItemsW(NULL, NULL, L"GPU Engine", NULL, &counter_list_size,
			NULL, &instance_list_size, PERF_DETAIL_WIZARD, 0);
		if (instance_list_size > 0) {
			std::unique_ptr<wchar_t[]> counter_list(new wchar_t[counter_list_size]);
			std::unique_ptr<wchar_t[]> instance_list(new wchar_t[instance_list_size]);
			PDH_STATUS st = PdhEnumObjectItemsW(NULL, NULL, L"GPU Engine",
				counter_list.get(), &counter_list_size,
				instance_list.get(), &instance_list_size,
				PERF_DETAIL_WIZARD, 0);
			if (st == ERROR_SUCCESS) {
				for (wchar_t *inst = instance_list.get(); *inst != L'\0';
						inst += wcslen(inst) + 1) {
					if (wcsstr(inst, pid_tag) == nullptr)
						continue; // Some other process's GPU engine instance.
					wchar_t path[512];
					swprintf(path, 512,
						L"\\GPU Engine(%s)\\Utilization Percentage", inst);
					PDH_HCOUNTER counter;
					if (PdhAddCounterW(query, path, 0, &counter) == ERROR_SUCCESS)
						counters->push_back(counter);
				}
			}
		}
	}

	if (counters->empty()) {
		// No GPU Engine instance for our PID found (e.g. nothing's been
		// rendered through a GPU-accelerated path yet, or the counter
		// just isn't available on this system) -- report "unavailable"
		// rather than a misleading 0%.
		m_gpu_percent.store(-1.0f, std::memory_order_relaxed);
		return;
	}

	// GPU Engine's "Utilization Percentage" is a PERF_COUNTER_COUNTER
	// (rate) counter -- needs two PdhCollectQueryData() calls spaced
	// apart (see kSampleIntervalMs in run()) to produce a valid value;
	// the very first collection on a freshly (re)built query returns
	// PDH_CSTATUS_INVALID_DATA for every counter, which just means "no
	// value yet, try again next sample" and isn't an error worth logging.
	if (PdhCollectQueryData(query) != ERROR_SUCCESS)
		return;

	double total = 0.0;
	bool got_any = false;
	for (PDH_HCOUNTER counter : *counters) {
		PDH_FMT_COUNTERVALUE value;
		if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, NULL, &value)
				== ERROR_SUCCESS && value.CStatus == ERROR_SUCCESS) {
			total += value.doubleValue;
			got_any = true;
		}
	}

	if (got_any) {
		// Multiple engines (3D, Copy, VideoDecode, ...) can each be
		// individually 0-100%; summing mirrors what Task Manager's
		// per-process GPU column shows, but the total isn't hard-capped
		// at 100 since >1 engine can be concurrently busy on some
		// hardware/driver combinations -- left uncapped deliberately so
		// a genuinely GPU-bound frame (rendering + video decode at once,
		// say) doesn't silently get clamped down to look like less load
		// than it really is.
		m_gpu_percent.store((float)total, std::memory_order_relaxed);
	}
}

#else // !_WIN32

void PerfMonitor::sampleRamAndCpu()
{
#if defined(__linux__)
	FILE *f = fopen("/proc/self/status", "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			long kb = 0;
			if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) {
				m_ram_mb.store((float)(kb / 1024.0), std::memory_order_relaxed);
				break;
			}
		}
		fclose(f);
	}
#else
	// BSD/macOS: ru_maxrss is already in bytes on Darwin, kilobytes on
	// most BSDs. Close enough for a rough HUD readout either way isn't
	// acceptable to silently get wrong, so only report on Linux/Windows
	// where the unit is known for certain; elsewhere RAM just stays at
	// its last (or initial 0) value.
#endif

	struct timespec wall_ts;
	clock_gettime(CLOCK_MONOTONIC, &wall_ts);
	double wall_seconds = wall_ts.tv_sec + wall_ts.tv_nsec / 1e9;

	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) == 0) {
		double cpu_seconds = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
			ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
		if (m_last_wall_seconds > 0.0 && wall_seconds > m_last_wall_seconds) {
			long ncores = sysconf(_SC_NPROCESSORS_ONLN);
			if (ncores < 1) ncores = 1;
			double pct = 100.0 * (cpu_seconds - (double)m_last_cpu_ticks / 1e6) /
				((wall_seconds - m_last_wall_seconds) * ncores);
			if (pct < 0.0) pct = 0.0;
			if (pct > 100.0) pct = 100.0;
			m_cpu_percent.store((float)pct, std::memory_order_relaxed);
		}
		m_last_cpu_ticks = (long)(cpu_seconds * 1e6);
	}
	m_last_wall_seconds = wall_seconds;
}

void PerfMonitor::sampleGpu()
{
	// Per-process GPU usage has no portable POSIX API -- it's NVML on
	// Nvidia, a different vendor SDK on AMD/Intel, and none of them are
	// linked by this build. Left at -1 (see getGpuPercent() in the
	// header); Hud::drawConsumptionHud() in hud.cpp only shows the GPU
	// segment when this is >= 0, so it just quietly doesn't appear here
	// instead of showing a fake number.
}

#endif
