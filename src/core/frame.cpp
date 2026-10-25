#include <ember/core/frame.h>

#include <ember/containers/ring_buffer.h>
#include <ember/core/macros.h>
#include <ember/core/time.h>
#include <ember/memory/arena.h>
#include <ember/memory/memory.h>
#include <ember/memory/new.h>

#include <atomic>

namespace ember::frames
{
	namespace
	{
		// More than a pipeline of game logic, render logic, GPU and present could hold at once.
		constexpr u32 MAX_IN_FLIGHT = 8;

		// A frame's record and the stage mask that completed() reads from other threads. A frame
		// is live while it holds its arena.
		struct Entry
		{
			Frame frame;
			std::atomic<u32> completed_stages{0};
		};

		struct Frames
		{
			FramesDef def;

			// Frame N is the ring's value N - 1, so the ring's next number is the current frame.
			RingBuffer<Entry> history;

			// One arena for each frame that may be live at once. An arena is an Allocator, so
			// they are objects of their own rather than an array walked by pointer.
			memory::Arena* arenas[MAX_IN_FLIGHT] = {};
			u64 previous_begin_ns = 0;
			u32 live = 0;
			bool initialized = false;
		};

		Frames s_frames;

		// A frame may be asked about while fewer than history - 1 frames have begun after it;
		// the slot beyond that is the one begin() takes next, while others may still be reading.
		bool visible(u64 index)
		{
			const u64 current = s_frames.history.next_sequence();
			return index != 0 && index <= current && current - index < s_frames.def.history - 1;
		}

		// Whether a record the caller holds is the history's own, for the asserts.
		bool is_ours(const Frame& frame)
		{
			const Entry* entry = s_frames.history.find(frame.index - 1);
			return entry != nullptr && &entry->frame == &frame;
		}
	}

	void initialize(const FramesDef& def)
	{
		EMBER_ASSERT_MSG(!s_frames.initialized, "initialize runs once");
		EMBER_ASSERT(def.in_flight >= 1 && def.in_flight <= MAX_IN_FLIGHT);
		EMBER_ASSERT(def.history >= def.in_flight + 1);
		EMBER_ASSERT_MSG(def.first_dt_ns <= def.max_dt_ns, "the first frame's dt is clamped too");

		memory::Allocator* heap = memory::heap();
		s_frames.def = def;
		s_frames.history.initialize(heap, memory::Tag::Engine, def.history);

		for (u32 i = 0; i < def.in_flight; ++i)
		{
			s_frames.arenas[i] = memory::new_object<memory::Arena>(heap, memory::Tag::Engine);
			s_frames.arenas[i]->initialize(memory::block_heap(), "frame");
		}

		s_frames.previous_begin_ns = 0;
		s_frames.live = 0;
		s_frames.initialized = true;
	}

	void shutdown()
	{
		if (!s_frames.initialized)
		{
			return;
		}

		EMBER_ASSERT_MSG(s_frames.live == 0, "a frame is still live at shutdown");

		memory::Allocator* heap = memory::heap();
		for (u32 i = 0; i < s_frames.def.in_flight; ++i)
		{
			s_frames.arenas[i]->shutdown();
			memory::delete_object(heap, s_frames.arenas[i]);
			s_frames.arenas[i] = nullptr;
		}

		s_frames.history.shutdown();
		s_frames.def = FramesDef{};
		s_frames.previous_begin_ns = 0;
		s_frames.initialized = false;
	}

	Frame& begin()
	{
		EMBER_ASSERT_MSG(s_frames.initialized, "begin before initialize");
		EMBER_ASSERT_MSG(s_frames.live < s_frames.def.in_flight,
		    "every in-flight frame is live: retire one before beginning another");

		const u64 index = s_frames.history.next_sequence() + 1;
		Entry& entry = s_frames.history.push();
		EMBER_ASSERT_MSG(entry.frame.memory == nullptr, "the frame this one evicts is still live");

		// An arena no live frame holds. Frames may retire out of order, so the free one is found
		// rather than taken by turn.
		memory::Arena* arena = nullptr;
		for (u32 i = 0; i < s_frames.def.in_flight; ++i)
		{
			if (!s_frames.arenas[i]->is_active())
			{
				arena = s_frames.arenas[i];
				break;
			}
		}
		EMBER_ASSERT(arena != nullptr);

		// Both readings are 64-bit and are subtracted before anything becomes a float, so the
		// clock's magnitude never reaches the delta's precision.
		const u64 now = now_ns();
		u64 dt_ns = index == 1 ? s_frames.def.first_dt_ns : now - s_frames.previous_begin_ns;
		if (dt_ns > s_frames.def.max_dt_ns)
		{
			dt_ns = s_frames.def.max_dt_ns;
		}

		s_frames.previous_begin_ns = now;
		++s_frames.live;

		Frame& frame = entry.frame;
		frame = Frame{};
		frame.index = index;
		frame.slot = (u32)(index % s_frames.def.in_flight);
		frame.dt_ns = dt_ns;
		frame.dt = (f32)dt_ns * 1.0e-9f;
		frame.time_ns = now;
		frame.memory = arena;
		arena->begin(memory::block_tag(memory::Lifetime::Frame, index));

		entry.completed_stages.store(0, std::memory_order_relaxed);
		return frame;
	}

	void begin_stage(Frame& frame, Stage stage)
	{
		EMBER_ASSERT((u32)stage < STAGE_COUNT);
		EMBER_ASSERT_MSG(frame.memory != nullptr && is_ours(frame),
		    "a stage of a frame that is not live");
		frame.stage_begin_ns[(u32)stage] = now_ns();
	}

	void end_stage(Frame& frame, Stage stage)
	{
		EMBER_ASSERT((u32)stage < STAGE_COUNT);
		EMBER_ASSERT_MSG(frame.memory != nullptr && is_ours(frame),
		    "a stage of a frame that is not live");

		frame.stage_end_ns[(u32)stage] = now_ns();
		s_frames.history.find(frame.index - 1)
		    ->completed_stages.fetch_or(u32(1) << (u32)stage, std::memory_order_release);
	}

	void retire(Frame& frame)
	{
		EMBER_ASSERT_MSG(frame.memory != nullptr && is_ours(frame),
		    "retire of a frame that is not live");

		const memory::BlockTag tag = frame.memory->end();
		(void)memory::block_heap()->free(tag);
		frame.memory = nullptr;
		--s_frames.live;
	}

	const Frame* find(u64 index)
	{
		return visible(index) ? &s_frames.history.find(index - 1)->frame : nullptr;
	}

	bool completed(u64 index, Stage stage)
	{
		EMBER_ASSERT((u32)stage < STAGE_COUNT);

		if (index == 0 || index > s_frames.history.next_sequence())
		{
			return false;
		}

		if (!visible(index))
		{
			return true;
		}

		const u32 mask =
		    s_frames.history.find(index - 1)->completed_stages.load(std::memory_order_acquire);
		return (mask >> (u32)stage & 1) != 0;
	}

	u64 current()
	{
		return s_frames.history.next_sequence();
	}

	u32 live()
	{
		return s_frames.live;
	}

	u32 in_flight()
	{
		return s_frames.def.in_flight;
	}
}
