#pragma once

// Per-frame timing, stage completion, and arena lifetime. Separate frames let update, render,
// and GPU work overlap, following the FrameParams design in Christian Gyrling's 2015 GDC talk.
//
// Frame memory holds what the CPU stages build for each other and nothing the GPU reads, so a
// frame can retire once its CPU stages have ended. begin() retires the oldest live frame when
// every frame in flight is live; retire() by hand returns memory sooner. The record outlives the
// memory in the history, which is how the device stamps Stage::Gpu by frame number afterwards.
// Budget for each allocating thread in each live frame, plus refills and oversized allocations.

#include <ember/core/types.h>

namespace ember
{
	namespace memory
	{
		class Arena;
	}

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
		// Frame indices start at one; zero means no frame.
		u64 index = 0;

		// index % in_flight, shared by per-frame storage such as GPU slots and upload buffers.
		u32 slot = 0;

		// Time since the previous frame began, clamped to FramesDef::max_dt_ns.
		u64 dt_ns = 0;
		f32 dt = 0.0f;

		u64 time_ns = 0;

		// The frame's memory, from begin() to retire(). Null once retired.
		memory::Arena* memory = nullptr;

		// Game-owned state passed between stages, usually allocated from memory. Opaque to Ember.
		void* data = nullptr;

		// When each stage began and ended. Zero for a stage that has not.
		u64 stage_begin_ns[STAGE_COUNT] = {};
		u64 stage_end_ns[STAGE_COUNT] = {};
	};

	struct FramesDef
	{
		// Maximum live frames. Three allows update, render, and GPU work on separate frames.
		u32 in_flight = 3;

		// History for find() and completed(). Must exceed in_flight.
		u32 history = 16;

		// Clamp long pauses so a hitch or debugger stop does not become one large simulation step.
		u64 max_dt_ns = 100'000'000;

		// The first frame has no previous one to measure from. At most max_dt_ns.
		u64 first_dt_ns = 16'666'667;
	};

	namespace frames
	{
		// Allocates arenas and history after memory::initialize().
		void initialize(const FramesDef& def = {});

		// Releases arenas and history, retiring any frame still live.
		void shutdown();

		// Starts the next frame and its arena. At the live limit it retires the oldest frame
		// first, whose CPU stages must have ended: wait for render jobs before beginning the
		// frame that needs the arena.
		Frame& begin();

		// Record stage times by frame number; end_stage() publishes completion to completed()
		// readers. Stamps land on retired frames too while the record is in the history, which
		// is how the GPU stage ends after the memory is gone.
		void begin_stage(u64 index, Stage stage);
		void end_stage(u64 index, Stage stage);

		// Returns the frame's memory to the block heap. begin() does this at the live limit; call
		// it to return memory sooner, or from a loop without a device.
		void retire(Frame& frame);

		// The record of a frame in the history, or null for one not begun yet or too old.
		const Frame* find(u64 index);

		// Frames older than the retained history are considered complete.
		bool completed(u64 index, Stage stage);

		// The number of the latest frame begun, 0 before the first.
		u64 current();

		// Frames begun but not yet retired.
		u32 live();

		// Frames that may be live at once, FramesDef::in_flight. The GPU sizes its slots by it.
		u32 in_flight();
	}
}
