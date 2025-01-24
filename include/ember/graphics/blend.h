#pragma once

#include "color.h"

namespace Ember
{
	enum class BlendOp
	{
		Add,
		Subtract,
		ReverseSubtract,
		Min,
		Max
	};

	enum class BlendFactor
	{
		Zero,
		One,
		SrcColor,
		OneMinusSrcColor,
		DstColor,
		OneMinusDstColor,
		SrcAlpha,
		OneMinusSrcAlpha,
		DstAlpha,
		OneMinusDstAlpha,
		ConstantColor,
		OneMinusConstantColor,
		ConstantAlpha,
		OneMinusConstantAlpha,
		SrcAlphaSaturate,
		Src1Color,
		OneMinusSrc1Color,
		Src1Alpha,
		OneMinusSrc1Alpha
	};

	enum class BlendMask
	{
		None  = 0,
		Red   = 1,
		Green = 2,
		Blue  = 4,
		Alpha = 8,
		RGB   = Red | Green | Blue,
		RGBA  = Red | Green | Blue | Alpha,
	};

	struct BlendMode
	{
		BlendOp color_op;
		BlendFactor color_src;
		BlendFactor color_dst;
		BlendOp alpha_op;
		BlendFactor alpha_src;
		BlendFactor alpha_dst;
		BlendMask mask = BlendMask::RGBA;
		Color color = Color::White;

		static const BlendMode premultiply;
        static const BlendMode no_premultiply;
        static const BlendMode add;
        static const BlendMode subtract;
        static const BlendMode multiply;
        static const BlendMode screen;


	};
}
