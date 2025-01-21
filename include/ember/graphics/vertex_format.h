#pragma once

#include <span>
#include <vector>
#include "vertex_type.h"

namespace Ember
{
	struct VertexFormat
	{
		struct Element
		{
			int index		= 0;
			VertexType type = VertexType::None;
			bool normalized = true;
		};

		VertexFormat() = default;

		template <class T>
		static VertexFormat create(std::span<Element> elements) {
			return VertexFormat{
				.elements = elements,
				.stride = sizeof(T)
			};
		}

		explicit VertexFormat(std::span<Element> elements_, int stride_ = 0) {
			for (auto& el : elements_) {
				elements.push_back(el);
				stride += VertexTypeExt::size(el.type);
			}

			if (stride_ != 0)
				stride = stride_;
		}

		std::vector<Element> elements;
		u32 stride = 0;
	};
}
