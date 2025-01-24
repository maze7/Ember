#include "graphics/blend.h"

using namespace Ember;

const BlendMode BlendMode::premultiply = {
	.color_op = BlendOp::Add,
	.color_src = BlendFactor::One,
	.color_dst = BlendFactor::OneMinusSrcAlpha,
	.alpha_op = BlendOp::Add,
	.alpha_src = BlendFactor::One,
	.alpha_dst = BlendFactor::OneMinusSrcAlpha,
	.mask = BlendMask::RGBA,
	.color = Color::White,
};

const BlendMode BlendMode::no_premultiply = {
	.color_op = BlendOp::Add,
	.color_src = BlendFactor::SrcAlpha,
	.color_dst = BlendFactor::OneMinusSrcAlpha,
	.alpha_op = BlendOp::Add,
	.alpha_src = BlendFactor::SrcAlpha,
	.alpha_dst = BlendFactor::OneMinusSrcAlpha,
	.mask = BlendMask::RGBA,
	.color = Color::White,
};

const BlendMode BlendMode::add = {
	.color_op = BlendOp::Add,
	.color_src = BlendFactor::One,
	.color_dst = BlendFactor::DstAlpha,
	.alpha_op = BlendOp::Add,
	.alpha_src = BlendFactor::One,
	.alpha_dst = BlendFactor::DstAlpha,
	.mask = BlendMask::RGBA,
	.color = Color::White,
};

const BlendMode BlendMode::subtract = {
	.color_op = BlendOp::ReverseSubtract,
	.color_src = BlendFactor::One,
	.color_dst = BlendFactor::One,
	.alpha_op = BlendOp::ReverseSubtract,
	.alpha_src = BlendFactor::One,
	.alpha_dst = BlendFactor::One,
	.mask = BlendMask::RGBA,
	.color = Color::White,
};

const BlendMode BlendMode::multiply = {
	.color_op = BlendOp::Add,
	.color_src = BlendFactor::DstColor,
	.color_dst = BlendFactor::OneMinusSrcAlpha,
	.alpha_op = BlendOp::Add,
	.alpha_src = BlendFactor::DstAlpha,
	.alpha_dst = BlendFactor::OneMinusSrcAlpha,
	.mask = BlendMask::RGBA,
	.color = Color::White,
};

const BlendMode BlendMode::screen = {
	.color_op = BlendOp::Add,
	.color_src = BlendFactor::One,
	.color_dst = BlendFactor::OneMinusSrcColor,
	.alpha_op = BlendOp::Add,
	.alpha_src = BlendFactor::One,
	.alpha_dst = BlendFactor::OneMinusSrcColor,
	.mask = BlendMask::RGBA,
	.color = Color::White,
};