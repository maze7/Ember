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

		template <class T>
		static VertexFormat create(std::span<Element> elements) {
			return {elements, sizeof(T)};
		}

		template <class T>
		static auto create(std::initializer_list<Element> list) {
			std::vector<Element> elems(list);
			return VertexFormat {
				std::span<Element>(elems.data(), elems.size()),
				sizeof(T)
			};
		}

		VertexFormat() = default;
		VertexFormat(std::span<Element> elements_, int stride_ = 0) {
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
