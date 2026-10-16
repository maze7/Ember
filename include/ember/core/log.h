#pragma once

// Logging, from any thread or job, without a lock and without allocating. A call formats its
// message into a fixed record on its own stack and pushes the record into a ring the logger
// allocated at initialize(). One thread drains the ring to the sinks, the IO thread once the job
// system runs and main until then, so a job never waits on the console, a faile, or another job's
// message. A ring that is full drops the record and counts it. An Error is pushed then flushed on
// the spot, so the process said what failed before it dies.

#include <ember/core/types.h>

#include <fmt/base.h>

#include <cstddef>
#include <source_location>

#ifndef EMBER_LOG_LEVEL
	#define EMBER_LOG_LEVEL 0
#endif

#if EMBER_LOG_LEVEL <= 0
	#define EMBER_TRACE(...) ::ember::log::trace(::std::source_location::current(), __VA_ARGS__)
#else
	#define EMBER_TRACE(...) ((void)0)
#endif

#if EMBER_LOG_LEVEL <= 1
	#define EMBER_INFO(...) ::ember::log::info(::std::source_location::current(), __VA_ARGS__)
#else
	#define EMBER_INFO(...) ((void)0)
#endif

#if EMBER_LOG_LEVEL <= 2
	#define EMBER_WARN(...) ::ember::log::warn(::std::source_location::current(), __VA_ARGS__)
#else
	#define EMBER_WARN(...) ((void)0)
#endif

#if EMBER_LOG_LEVEL <= 3
	#define EMBER_ERROR(...) ::ember::log::error(::std::source_location::current(), __VA_ARGS__)
#else
	#define EMBER_ERROR(...) ((void)0)
#endif

namespace ember
{
	enum class LogLevel : u8
	{
		Trace = 0,
		Info,
		Warn,
		Error,
		Silent,
	};

	struct LogRecord
	{
		static constexpr size_t TEXT_CAPACITY = 232;

		// Nanoseconds on a steady clock. Absolute, so a sink shows whatever origin it likes; the
		// console shows seconds since the first record it saw.
		u64 time;

		// The call site. The file name is the compiler's string for it, which lives as long as the
		// process does.
		const char* file;
		u32 line;

		// Characters in [text], which is always NULL terminated.
		u16 length;
		LogLevel level;

		// The calling thread's arena slot, 255 for a thread that has none.
		u8 thread;

		char text[TEXT_CAPACITY];
	};

	static_assert(sizeof(LogRecord) == 256, "a record is a quarter of a kilobyte by design");

	struct LogDef
	{
		// Records the ring holds between flushes, a power of two. Taken from the heap under
		// Tag::Engine, so a quarter of a megabyte by default.
		u32 ring_capacity = 1024;

		// ANSI color in the console sink. Off for a terminal that cannot show it.
		bool color = true;
	};

	namespace log
	{
		// A sink sees every record the flushing thread drains, in order, and never two at once.
		using Sink = void (*)(const LogRecord& record, void* user);

		// Takes the ring. Until this runs, and after shutdown(), messages go straight to the sinks.
		// Must be called after memory::initialize(), since the ring is heap memory. False for a
		// capacity that is not a power of two of at least two.
		bool initialize(const LogDef& def = {});

		// Flushes what the ring holds and returns it to the heap. Before memory::shutdown().
		void shutdown();

		// The run time filter among the levels the build kept: a record below it is not even
		// formatted. Defaults to Trace, so nothing is missed by default.
		void set_level(LogLevel level);
		LogLevel level();

		// Adds a sink beside the console. Up to four; false when they ar etaken. Sinks are
		// registered at boot and never removed.
		bool add_sink(Sink sink, void* user);

		// Drains the ring to the sinks, usually called on the IO thread. It takes the sink lock.
		void flush();

		// Records lost to a full ring since initialize(). flush() also says so on the consle.
		u64 dropped();

		// The one entry point the macros reach. Formats the record and submits it.
		void write(LogLevel level, std::source_location where, fmt::string_view format,
		    fmt::format_args args);

		template <typename... Args>
		void trace(std::source_location where, fmt::format_string<Args...> format, Args&&... args)
		{
			if (level() <= LogLevel::Trace)
			{
				write(LogLevel::Trace, where, format, fmt::make_format_args(args...));
			}
		}

		template <typename... Args>
		void info(std::source_location where, fmt::format_string<Args...> format, Args&&... args)
		{
			if (level() <= LogLevel::Info)
			{
				write(LogLevel::Info, where, format, fmt::make_format_args(args...));
			}
		}

		template <typename... Args>
		void warn(std::source_location where, fmt::format_string<Args...> format, Args&&... args)
		{
			if (level() <= LogLevel::Warn)
			{
				write(LogLevel::Warn, where, format, fmt::make_format_args(args...));
			}
		}

		template <typename... Args>
		void error(std::source_location where, fmt::format_string<Args...> format, Args&&... args)
		{
			if (level() <= LogLevel::Error)
			{
				write(LogLevel::Error, where, format, fmt::make_format_args(args...));
			}
		}
	}
}
