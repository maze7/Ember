#pragma once

#include <fmt/format.h>
#include <iostream>
#include <mutex>

namespace Ember {
    // For now this is mostly just a place-holder interface. Eventually I want to properly support multiple sink types,
    // such as a file writer, networked terminal, etc.
    class Log
    {
    public:
        // log-level labels
        static constexpr const char* k_trace_label 		= "[\033[36mtrace\033[m] ";
        static constexpr const char* k_info_label  		= "[\033[32minfo\033[m] ";
        static constexpr const char* k_warn_label  		= "[\033[33mwarning\033[m] ";
        static constexpr const char* k_error_label 		= "[\033[31merror\033[m] ";
        static constexpr const char* k_critical_label 	= "[\033[31m\033[1m\033[4mcritical\033[m] ";

        // log a trace level message
        template <typename ... Args>
        static void trace(fmt::format_string<Args...> fmt, Args&& ... args) {
            std::scoped_lock lock(m_mutex); // lock or block until mutex is available

            // log the message to console
            std::cout << Log::k_trace_label << fmt::format(fmt, std::forward<Args>(args)...) << std::endl;
        }

        // log an info level message
        template <typename ... Args>
        static void info(fmt::format_string<Args...> fmt, Args&& ... args) {
            std::scoped_lock lock(m_mutex); // lock or block until mutex is available

            // log the message to console
            std::cout << Log::k_info_label << fmt::format(fmt, std::forward<Args>(args)...) << std::endl;
        }

        // log a warning level message
        template <typename ... Args>
        static void warn(fmt::format_string<Args...> fmt, Args&& ... args) {
            std::scoped_lock lock(m_mutex); // lock or block until mutex is available

            // log the message to console
            std::cout << Log::k_warn_label << fmt::format(fmt, std::forward<Args>(args)...) << std::endl;
        }

        // log an error level message
        template <typename ... Args>
        static void error(fmt::format_string<Args...> fmt, Args&& ... args) {
            std::scoped_lock lock(m_mutex); // lock or block until mutex is available

            // log the message to console
            std::cout << Log::k_error_label << fmt::format(fmt, std::forward<Args>(args)...) << std::endl;
        }

        // log a critical level message
        template <typename ... Args>
        static void critical(fmt::format_string<Args...> fmt, Args&& ... args) {
            std::scoped_lock lock(m_mutex); // lock or block until mutex is available

            // log the message to console
            std::cout << Log::k_critical_label << fmt::format(fmt, std::forward<Args>(args)...) << std::endl;
        }

    private:
        static std::mutex m_mutex;
    };
}