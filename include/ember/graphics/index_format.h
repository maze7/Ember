#pragma once
#include "core/common.h"

namespace Ember
{
	enum class IndexFormat
	{
		Sixteen,
		ThirtyTwo,
	};

	struct IndexFormatExt
	{
		static int size_in_bytes(IndexFormat format) {
			switch (format) {
				case IndexFormat::Sixteen:
					return 2;
				case IndexFormat::ThirtyTwo:
					return 4;
				default:
					throw Exception("Unknown index format");
			}
		}
	};

	template <class TIndex>
	IndexFormat index_format_from_type();

	template <>
	inline IndexFormat index_format_from_type<u16>() {
		return IndexFormat::Sixteen;
	}

	// specialisation for 32-bit types
	template <>
	inline IndexFormat index_format_from_type<u32>() {
		return IndexFormat::ThirtyTwo;
	}
}
