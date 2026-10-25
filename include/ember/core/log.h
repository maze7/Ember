#pragma once

// Logging through fixed-size records and a bounded ring. Normal writes queue a record or drop
// it when full; callers choose when and where to flush. Errors flush synchronously, and writes
// before initialization or after shutdown go directly to sinks, so those paths can block.
// Stop producers and flushers before shutdown. Sink calls are serialized and must not reenter
// the logger while its sink lock is held.

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

		// Steady-clock nanoseconds; the console displays time since its first record.
		u64 time;

		// The call-site file name has static storage duration.
		const char* file;
		u32 line;

		// Character count excluding the terminating NUL.
		u16 length;
		LogLevel level;

		// The calling thread's arena slot, 255 for a thread that has none.
		u8 thread;

		char text[TEXT_CAPACITY];
	};

	static_assert(sizeof(LogRecord) == 256, "a record is a quarter of a kilobyte by design");

	struct LogDef
	{
		// Power-of-two record capacity, at least two. Heap storage is charged to Tag::Engine.
		u32 ring_capacity = 1024;

		// Emit ANSI color codes in the console sink.
		bool color = true;
	};

	namespace log
	{
		// Called under the sink lock. record is borrowed for the callback only.
		using Sink = void (*)(const LogRecord& record, void* user);

		// Allocates the ring after memory::initialize(). Returns false for an invalid capacity.
		bool initialize(const LogDef& def = {});

		// Drains and releases the ring before memory::shutdown(), with no concurrent users.
		void shutdown();

		// Runtime filter applied before formatting, within the levels compiled into the build.
		void set_level(LogLevel level);
		LogLevel level();

		// Registers one of four sinks beside the console. No removal; user must outlive all calls.
		bool add_sink(Sink sink, void* user);

		// Drains under the sink lock on the calling thread. Sink I/O may block.
		void flush();

		// Records dropped since initialize(); flush() also reports new drops to the sinks.
		u64 dropped();

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
