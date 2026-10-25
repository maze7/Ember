#pragma once

// Owning strings with engine allocation tags, and borrowed string views. Keep the allocator
// and its storage alive through String destruction; StringView never extends that lifetime.

#include <ember/memory/allocator.h>

#include <string>
#include <string_view>

namespace ember
{
	using String = std::basic_string<char, std::char_traits<char>, memory::TagAllocator<char>>;
	using StringView = std::string_view;
}
