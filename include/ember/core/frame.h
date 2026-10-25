#pragma once

// A frame is a piece of data, not a length of time. Its number, the delta time every stage of it
// uses, the memory everything built for it lives in, and what the game built in one stage for the
// next. Ember follows Naughty Dog's FrameParams design from Christian Gyrling's 2015 GDC talk
// "Parallelizing the Naughty Dog Engine using Fibers".
//
// One Frame instance is passed through every stage, game logic, render, and GPU, so that stages
// can work on different frames at once through the job system without requiring a lock.
//
// A frame's memory is an arena begun under block_tag(Lifetime::Frame, frame_index): per-thread bump
// allocation with no frees, returned to the block heap all at once when the frame is retired. The
// loop retires a frame once its last render has finished, so up to [in_flight] frames hold memory
// at once, each on its own arena, and game logic for frame N+1 may allocate while render logic for
// frame N still reads. Worst case a live frame holds one block per thread that touches it, so the
// block partition has to cover in_flight *  num_threads * block_size.

#include <ember/core/types.h>

namespace ember
{
	namespace memory
	{
		class Arena;
	}

	// Logical stages a frame passes through on its way to the screen, in order. Gpu ends when the
	// device has finished the frame's work; Present ends when the frame's images are handed to the
	// display, which can come first, since the display waits on the GPU itself.
	enum class Stage : u8
	{
		Update,
		Render,
		Gpu,
		Present,
		Count
	};

	inline constexpr u32 STAGE_COUNT = static_cast<u32>(Stage::Count);

	struct Frame
	{
		// Frames count from 1. Zero is boot, which no frame is.
		u64 index = 0;

		// Which of the in_flight storages this frame uses: index % in_flight, set by begin().
		// Anything versioned per frame in flight picks by it: the device's slots, a readback
		// buffer, an Upload buffer the CPU rewrites each frame.
		u32 slot = 0;

		// Since the previous frame began, on the engine's clock, clamped to FramesDef::max_dt.
		u64 dt_ns = 0;
		f32 dt = 0.0f;

		// When the frame began.
		u64 time_ns = 0;

		// The frame's memory, from begin() to retire(). Null once retired.
		memory::Arena* memory = nullptr;

		// The game's own per-frame state, built in one stage for the next, usually in [memory].
		// The engine never reads it.
		void* data = nullptr;

		// When each stage began and ended. Zero for a stage that has not.
		u64 stage_begin_ns[STAGE_COUNT] = {};
		u64 stage_end_ns[STAGE_COUNT] = {};
	};

	struct FramesDef
	{
		// Frames that hold memory at once. Three lets game logic, render logic and the GPU each
		// work on a frame at the same time. A loop that retires each frame before beginning the
		// next needs one.
		u32 in_flight = 3;

		// Frames remembered for find() and completed(), at least in_flight.
		u32 history = 16;

		// A delta time above this is a hitch or a breakpoint, not a step the game should take.
		u64 max_dt_ns = 100'000'000;

		// The first frame has no previous one to measure from. At most max_dt_ns.
		u64 first_dt_ns = 16'666'667;
	};

	namespace frames
	{
		// Takes the arena and the history from the block heap. Call after memory::initialize().
		void initialize(const FramesDef& def = {});

		// Returns them. Every frame must have been retired.
		void shutdown();

		// Begins the next frame: numbers it, measures its delta time and begins its arena. Fewer
		// than in_flight frames may be live, so the loop retires before  it begins.
		Frame& begin();

		// Stamps the stage. end_stage() is what completed() answers from.
		void begin_stage(Frame& frame, Stage stage);
		void end_stage(Frame& frame, Stage stage);

		// Returns the frame's memory to the block heap, once every stage that needs it has ended.
		void retire(Frame& frame);

		// The record of a frame in the history, or null for one not begun yet or too old.
		const Frame* find(u64 index);

		// Whether a frame has ended a stage. A frame too old for the history is long gone, so it
		// is considered complete.
		bool completed(u64 index, Stage stage);

		// The number of the latest frame begun, 0 before the first.
		u64 current();

		// Frames begun and not retured.
		u32 live();

		// Frames that may be live at once, FramesDef::in_flight. The GPU sizes its slots by it.
		u32 in_flight();
	}
}
