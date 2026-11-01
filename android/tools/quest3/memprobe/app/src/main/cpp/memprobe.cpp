// memprobe: measure what memory an app can really hold on a Horizon OS headset, and what
// file-backed memory costs there. See ../../../../README.md. Plain POSIX, no Android log: the
// Kotlin side writes every result to a file as it comes, since the interesting end of a run is
// a SIGKILL.

#include <jni.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	constexpr size_t k_page = 4096;
	constexpr size_t k_mb = 1024 * 1024;

	enum mode : int
	{
		mode_anonymous = 0, // MAP_PRIVATE|MAP_ANONYMOUS: swappable only through zram
		mode_file = 1,      // a file in the app's storage, MAP_SHARED: the page cache can evict and write back
		mode_memfd = 2,     // memfd (shmem), MAP_SHARED: what RPCS3 uses for guest memory
	};

	struct region
	{
		int mode = mode_anonymous;
		int fd = -1;
		void* base = MAP_FAILED;
		size_t size = 0;
		std::string path;
		std::string error;

		~region()
		{
			if (base != MAP_FAILED)
			{
				::munmap(base, size);
			}
			if (fd >= 0)
			{
				::close(fd);
			}
			if (!path.empty())
			{
				::unlink(path.c_str());
			}
		}
	};

	std::mutex g_mutex;
	std::vector<std::unique_ptr<region>> g_regions;
	std::string g_last_error;

	int64_t now_us()
	{
		timespec ts{};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return static_cast<int64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1000;
	}

	uint64_t xorshift(uint64_t& s)
	{
		s ^= s << 13;
		s ^= s >> 7;
		s ^= s << 17;
		return s;
	}

	region* get(jlong handle)
	{
		std::lock_guard lock(g_mutex);
		if (handle < 0 || static_cast<size_t>(handle) >= g_regions.size())
		{
			return nullptr;
		}
		return g_regions[static_cast<size_t>(handle)].get();
	}

	std::string jstr(JNIEnv* env, jstring s)
	{
		if (!s) return {};
		const char* c = env->GetStringUTFChars(s, nullptr);
		std::string out = c ? c : "";
		if (c) env->ReleaseStringUTFChars(s, c);
		return out;
	}

	// "Key:   123 kB" lines from a /proc file, as "Key=123MB " for the keys asked.
	void grab(std::string& out, const char* path, std::initializer_list<const char*> keys, bool pages)
	{
		FILE* f = std::fopen(path, "r");
		if (!f)
		{
			out += std::string(path) + "=unreadable ";
			return;
		}
		char line[256];
		while (std::fgets(line, sizeof(line), f))
		{
			for (const char* key : keys)
			{
				const size_t len = std::strlen(key);
				if (std::strncmp(line, key, len) != 0) continue;
				const char* p = line + len;
				if (*p == ':') ++p;
				else if (*p != ' ') continue;
				const unsigned long long v = std::strtoull(p, nullptr, 10);
				// /proc/vmstat counts pages; status and meminfo count kB.
				const unsigned long long mb = pages ? v * k_page / k_mb : v / 1024;
				char buf[96];
				std::snprintf(buf, sizeof(buf), "%s=%lluMB ", key, mb);
				out += buf;
			}
		}
		std::fclose(f);
	}
}

extern "C"
{
	// Create a region of sizeMb in the given mode; dir is where the file goes in mode_file.
	// Returns a handle, or -1 with the reason in lastError().
	JNIEXPORT jlong JNICALL Java_com_armsx3_memprobe_Native_create(JNIEnv* env, jclass, jint mode, jlong sizeMb, jstring dir)
	{
		auto r = std::make_unique<region>();
		r->mode = mode;
		r->size = static_cast<size_t>(sizeMb) * k_mb;

		int flags = MAP_SHARED;
		if (mode == mode_anonymous)
		{
			flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
		}
		else if (mode == mode_file)
		{
			r->path = jstr(env, dir) + "/memprobe-" + std::to_string(::getpid()) + "-" + std::to_string(g_regions.size()) + ".bin";
			r->fd = ::open(r->path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
			if (r->fd < 0)
			{
				g_last_error = std::string("open: ") + std::strerror(errno);
				return -1;
			}
			if (::ftruncate(r->fd, static_cast<off_t>(r->size)) != 0)
			{
				g_last_error = std::string("ftruncate: ") + std::strerror(errno);
				return -1;
			}
		}
		else
		{
			r->fd = static_cast<int>(::syscall(__NR_memfd_create, "memprobe", 0u));
			if (r->fd < 0)
			{
				g_last_error = std::string("memfd_create: ") + std::strerror(errno);
				return -1;
			}
			if (::ftruncate(r->fd, static_cast<off_t>(r->size)) != 0)
			{
				g_last_error = std::string("ftruncate(memfd): ") + std::strerror(errno);
				return -1;
			}
		}

		r->base = ::mmap(nullptr, r->size, PROT_READ | PROT_WRITE, flags, r->fd, 0);
		if (r->base == MAP_FAILED)
		{
			g_last_error = std::string("mmap: ") + std::strerror(errno);
			return -1;
		}

		std::lock_guard lock(g_mutex);
		g_regions.push_back(std::move(r));
		return static_cast<jlong>(g_regions.size() - 1);
	}

	// Fill [fromMb, toMb) with incompressible data (every byte of every page), so zram cannot
	// make anonymous memory look free. Returns microseconds taken.
	JNIEXPORT jlong JNICALL Java_com_armsx3_memprobe_Native_touch(JNIEnv*, jclass, jlong handle, jlong fromMb, jlong toMb)
	{
		region* r = get(handle);
		if (!r) return -1;

		const size_t from = std::min(static_cast<size_t>(fromMb) * k_mb, r->size);
		const size_t to = std::min(static_cast<size_t>(toMb) * k_mb, r->size);
		if (to <= from) return 0;

		const int64_t t0 = now_us();
		uint64_t seed = 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(from);
		auto* p = static_cast<uint64_t*>(r->base) + from / sizeof(uint64_t);
		const size_t words = (to - from) / sizeof(uint64_t);
		for (size_t i = 0; i < words; ++i)
		{
			p[i] = xorshift(seed);
		}
		return now_us() - t0;
	}

	// Random single-page reads over the whole region: latency percentiles in microseconds.
	// After a fill that outgrew RAM, this is the cost of coming back from flash (or zram).
	JNIEXPORT jstring JNICALL Java_com_armsx3_memprobe_Native_randomRead(JNIEnv* env, jclass, jlong handle, jint samples)
	{
		region* r = get(handle);
		if (!r || r->size < k_page) return env->NewStringUTF("no region");

		const size_t pages = r->size / k_page;
		std::vector<int64_t> lat;
		lat.reserve(static_cast<size_t>(samples));
		uint64_t seed = 0xD1B54A32D192ED03ull;
		volatile uint64_t sink = 0;

		const int64_t t_all = now_us();
		for (int i = 0; i < samples; ++i)
		{
			const size_t page = xorshift(seed) % pages;
			const auto* p = static_cast<const volatile uint64_t*>(r->base) + page * (k_page / sizeof(uint64_t));
			const int64_t t0 = now_us();
			sink += *p;
			lat.push_back(now_us() - t0);
		}
		const int64_t total = now_us() - t_all;
		(void)sink;

		std::sort(lat.begin(), lat.end());
		const auto pct = [&](double q) { return lat.empty() ? 0 : lat[std::min(lat.size() - 1, static_cast<size_t>(q * lat.size()))]; };
		char buf[256];
		std::snprintf(buf, sizeof(buf), "random reads n=%d p50=%lldus p90=%lldus p99=%lldus max=%lldus total=%lldms",
			samples, static_cast<long long>(pct(0.50)), static_cast<long long>(pct(0.90)), static_cast<long long>(pct(0.99)),
			static_cast<long long>(lat.empty() ? 0 : lat.back()), static_cast<long long>(total / 1000));
		return env->NewStringUTF(buf);
	}

	// Rewrite the first hotMb of the region over and over for `seconds`: what a game does to its
	// working set. Returns the number of full passes. The caller reads /proc/vmstat around it to
	// see how much of that the kernel wrote to flash.
	JNIEXPORT jlong JNICALL Java_com_armsx3_memprobe_Native_rewrite(JNIEnv*, jclass, jlong handle, jlong hotMb, jint seconds)
	{
		region* r = get(handle);
		if (!r) return -1;

		const size_t hot = std::min(static_cast<size_t>(hotMb) * k_mb, r->size);
		const size_t words = hot / sizeof(uint64_t);
		auto* p = static_cast<uint64_t*>(r->base);
		const int64_t deadline = now_us() + static_cast<int64_t>(seconds) * 1'000'000;
		uint64_t seed = 0xA0761D6478BD642Full;
		jlong passes = 0;

		while (now_us() < deadline)
		{
			// One word per 64-byte line is enough to dirty every page; the full fill above
			// already made the data incompressible.
			for (size_t i = 0; i < words; i += 8)
			{
				p[i] = xorshift(seed);
			}
			++passes;
		}
		return passes;
	}

	JNIEXPORT void JNICALL Java_com_armsx3_memprobe_Native_releaseAll(JNIEnv*, jclass)
	{
		std::lock_guard lock(g_mutex);
		g_regions.clear();
	}

	JNIEXPORT jstring JNICALL Java_com_armsx3_memprobe_Native_lastError(JNIEnv* env, jclass)
	{
		return env->NewStringUTF(g_last_error.c_str());
	}

	// The kernel's view, in one line: this process (status), the system (meminfo) and the
	// counters that reveal writeback and swapping (vmstat, cumulative).
	JNIEXPORT jstring JNICALL Java_com_armsx3_memprobe_Native_procStats(JNIEnv* env, jclass)
	{
		std::string out;
		grab(out, "/proc/self/status", { "VmRSS", "RssAnon", "RssFile", "RssShmem", "VmSwap" }, false);
		out += "| ";
		grab(out, "/proc/meminfo", { "MemAvailable", "Dirty", "Writeback", "SwapTotal", "SwapFree" }, false);
		out += "| ";
		grab(out, "/proc/vmstat", { "nr_written", "pswpout", "pswpin", "pgmajfault" }, true);
		return env->NewStringUTF(out.c_str());
	}
}
