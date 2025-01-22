#pragma once

#include <span>
#include <vector>
#include "vertex_type.h"

namespace Ember
{
struct VertexFormat {
	struct Element
	{
		int index		= 0;
		VertexType type = VertexType::None;
		bool normalized = true;
	};

    std::vector<Element> elements;
    u32 stride = 0;

    VertexFormat() = default;

    VertexFormat(std::vector<Element> elements_, int stride_ = 0)
        : elements(std::move(elements_)) {
        calculate_stride();
        if (stride_ != 0) {
            stride = stride_;
        }
    }

    VertexFormat(const VertexFormat&) = default;
    VertexFormat& operator=(const VertexFormat&) = default;

    VertexFormat(VertexFormat&& other) noexcept
        : elements(std::move(other.elements)), stride(other.stride) {
        other.stride = 0;
    }

    VertexFormat& operator=(VertexFormat&& other) noexcept {
        if (this != &other) {
            elements = std::move(other.elements);
            stride = other.stride;
            other.stride = 0;
        }
        return *this;
    }

    ~VertexFormat() = default;

private:
    void calculate_stride() {
        stride = 0;
        for (auto& el : elements) {
            stride += VertexTypeExt::size(el.type);
        }
    }
};
}
