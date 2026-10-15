#pragma once

// The engine's owning string, std::basic_string over a TagAllocator, and the non-owning view.
// A String carries its allocator and tag by value like a Vector does; short strings live inside
// the object and allocate nothing. StringView is std::string_view as it is: a pointer and a
// length, with nothing to allocate.

#include <ember/memory/allocator.h>

#include <string>
#include <string_view>

namespace ember
{
	using String = std::basic_string<char, std::char_traits<char>, memory::TagAllocator<char>>;
	using StringView = std::string_view;
}
