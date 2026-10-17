#include <ember/containers/mpmc_queue.h>
#include <ember/core/log.h>
#include <ember/core/macros.h>
#include <ember/core/spin_mutex.h>
#include <ember/core/time.h>
#include <ember/memory/arena.h>
#include <ember/memory/heap.h>

#include <fmt/format.h>

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>

using ember::memory::Arena;

namespace ember::log
{
	namespace
	{
		constexpr u32 MAX_SINKS = 4;
		constexpr u8 NO_THREAD = 255;

		// The ring, and whether writes go through it. s_ready flips after the ring exists and
		// before it goes, so a write that sees it set finds a ring to push to.
		constinit MpmcQueue<LogRecord> s_ring;
		constinit std::atomic<bool> s_ready{false};
		constinit std::atomic<u8> s_level{(u8)LogLevel::Trace};
		constinit std::atomic<u64> s_dropped{0};

		// Everything a sink touches is under this lock: the sinks themselves, the console, and
		// the count of drops already reported.
		constinit SpinMutex s_sink_lock;
		constinit Sink s_sinks[MAX_SINKS] = {};
		constinit void* s_users[MAX_SINKS] = {};
		constinit u32 s_sink_count = 0;
		constinit u64 s_reported_dropped = 0;
		constinit bool s_color = true;

		const char* level_name(LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Trace:
					return "trace";
				case LogLevel::Info:
					return "info";
				case LogLevel::Warn:
					return "warn";
				case LogLevel::Error:
					return "error";
				default:
					return "?";
			}
		}

		const char* level_color(LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Trace:
					return "90";
				case LogLevel::Info:
					return "32";
				case LogLevel::Warn:
					return "33";
				case LogLevel::Error:
					return "31";
				default:
					return "0";
			}
		}

		// The file name without its directories: the compiler's path is the build machine's,
		// and the name is what a reader wants.
		const char* short_name(const char* path)
		{
			const char* name = path;
			for (const char* p = path; *p != '\0'; ++p)
			{
				if (*p == '/' || *p == '\\')
				{
					name = p + 1;
				}
			}

			return name;
		}

		// The console: stderr, one line per record, seconds since the first record it saw.
		void console(const LogRecord& record)
		{
			static constinit u64 s_origin = 0;
			if (s_origin == 0)
			{
				s_origin = record.time;
			}

			const double seconds = (double)(record.time - s_origin) * 1e-9;
			if (s_color)
			{
				std::fprintf(stderr,
				    "\x1b[90m%9.3f\x1b[0m \x1b[%sm%-5s\x1b[0m %s \x1b[90m%s:%u t%u\x1b[0m\n",
				    seconds, level_color(record.level), level_name(record.level), record.text,
				    short_name(record.file), record.line, record.thread);
			}
			else
			{
				std::fprintf(stderr, "%9.3f %-5s %s %s:%u t%u\n", seconds, level_name(record.level),
				    record.text, short_name(record.file), record.line, record.thread);
			}
		}

		// Hands a record to the console and every sink, in order. Under the lock.
		void emit(const LogRecord& record)
		{
			console(record);
			for (u32 index = 0; index < s_sink_count; ++index)
			{
				s_sinks[index](record, s_users[index]);
			}
		}

		// Builds a record on the caller's stack: the call site, the time, the thread, and the
		// message formatted straight into the text with fmt's bounded vformat, cut with an
		// ellipsis when it does not fit. Nothing here allocates.
		LogRecord make_record(LogLevel level, std::source_location where, fmt::string_view format,
		    fmt::format_args args)
		{
			constexpr size_t LIMIT = LogRecord::TEXT_CAPACITY - 1;

			LogRecord record;
			record.time = now_ns();
			record.file = where.file_name();
			record.line = where.line();
			record.level = level;

			const u32 slot = Arena::thread_slot();
			record.thread = slot == Arena::NO_SLOT ? NO_THREAD : (u8)slot;

			const auto result = fmt::vformat_to_n(record.text, LIMIT, format, args);
			size_t length = result.size < LIMIT ? result.size : LIMIT;
			if (result.size > LIMIT)
			{
				std::memcpy(record.text + LIMIT - 3, "...", 3);
			}

			record.text[length] = '\0';
			record.length = (u16)length;
			return record;
		}

		// Says, once per batch, how many records the ring lost since the last time. Under the
		// lock, after a drain, so the line lands where the gap was.
		void report_dropped()
		{
			const u64 dropped = s_dropped.load(std::memory_order_relaxed);
			if (dropped == s_reported_dropped)
			{
				return;
			}

			const unsigned long long lost = dropped - s_reported_dropped;
			s_reported_dropped = dropped;
			emit(make_record(LogLevel::Warn, std::source_location::current(),
			    "{} log records dropped: the ring was full", fmt::make_format_args(lost)));
		}
	}

	bool initialize(const LogDef& def)
	{
		EMBER_ASSERT_MSG(!s_ready.load(std::memory_order_relaxed),
		    "the logger is initialized once");

		if (def.ring_capacity < 2 || !std::has_single_bit(def.ring_capacity))
		{
			return false;
		}

		s_ring.initialize(memory::heap(), memory::Tag::Engine, def.ring_capacity);
		s_color = def.color;
		s_dropped.store(0, std::memory_order_relaxed);
		s_reported_dropped = 0;
		s_ready.store(true, std::memory_order_release);
		return true;
	}

	void shutdown()
	{
		if (!s_ready.load(std::memory_order_relaxed))
		{
			return;
		}

		// Drain, close the ring to new writes, and drain again for anything that slipped in
		// between; from then on a write goes straight to the sinks.
		flush();
		s_ready.store(false, std::memory_order_release);
		flush();
		s_ring.shutdown();
	}

	void set_level(LogLevel level)
	{
		s_level.store((u8)level, std::memory_order_relaxed);
	}

	LogLevel level()
	{
		return (LogLevel)s_level.load(std::memory_order_relaxed);
	}

	bool add_sink(Sink sink, void* user)
	{
		EMBER_ASSERT(sink != nullptr);

		bool added = false;

		s_sink_lock.lock();
		if (s_sink_count < MAX_SINKS)
		{
			s_sinks[s_sink_count] = sink;
			s_users[s_sink_count] = user;
			s_sink_count += 1;
			added = true;
		}
		s_sink_lock.unlock();

		return added;
	}

	void flush()
	{
		s_sink_lock.lock();

		if (s_ring.capacity() != 0)
		{
			LogRecord record;
			while (s_ring.try_pop(record))
			{
				emit(record);
			}
		}

		report_dropped();
		s_sink_lock.unlock();
	}

	u64 dropped()
	{
		return s_dropped.load(std::memory_order_relaxed);
	}

	void write(LogLevel level, std::source_location where, fmt::string_view format,
	    fmt::format_args args)
	{
		const LogRecord record = make_record(level, where, format, args);

		if (s_ready.load(std::memory_order_acquire))
		{
			if (!s_ring.try_push(record))
			{
				s_dropped.fetch_add(1, std::memory_order_relaxed);
			}

			if (level == LogLevel::Error)
			{
				flush();
			}

			return;
		}

		s_sink_lock.lock();
		emit(record);
		s_sink_lock.unlock();
	}
}
