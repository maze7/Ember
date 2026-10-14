#pragma once

// The general heap, for what has no lifetime: a subsystem that lives from initialization to
// shutdown, middleware that mallocs through a callback, the rare thing that grows and shrinks
// with no frame or level to belong to. Everything with a lifetime goes through an Arena instead,
// so the heap is never on the frame's critical path.
//
// Underneath is rpmalloc, which caches per thread and needs no lock on its common path. There
// is one Heap per Category, all fronting the same rpmalloc, so that a PMR container carries its
// category by pointer alone and every byte is attributed to the subsystem that owns it. The
// views are distinct resources: a block is freed through the view that allocated it, which the
// containers guarantee and which new_object and delete_object callers must keep to.

#include <ember/core/enum.h>
#include <ember/core/macros.h>
#include <ember/core/types.h>
#include <ember/memory/alignment.h>
#include <ember/memory/allocator.h>

#include <cstddef>

namespace ember
{
	// Who owns a heap allocation. A category is chosen once, where a subsystem is wired up, and
	// never repeated at the allocation sites inside it.
	enum class Category : u8
	{
		Unknown = 0,
		Engine,
		Graphics,
		Audio,
		Physics,
		Gameplay,
		Assets,
		Scripting,
		Network,
		Platform,
		Input,
		Tools,
		Count
	};

	EMBER_ENUM_NAMES(Category, "Unknown", "Engine", "Graphics", "Audio", "Physics", "Gameplay",
	    "Assets", "Scripting", "Network", "Platform", "Input", "Tools");

	// How much heap each category is expected to hold. Crossing a budget is reported once, as an
	// early warning rather than a limit; the tagged heap's capacity is the hard one.
	inline constexpr std::array<u64, static_cast<size_t>(Category::Count)> CATEGORY_BUDGETS = {
	    256_mb, // Unknown
	    64_mb,  // Engine
	    512_mb, // Graphics
	    64_mb,  // Audio
	    128_mb, // Physics
	    128_mb, // Gameplay
	    512_mb, // Assets
	    64_mb,  // Scripting
	    64_mb,  // Network
	    64_mb,  // Platform
	    64_mb,  // Input
	    64_mb,  // Tools
	};

	class Heap final : public Allocator
	{
	public:
		const Category category;

		explicit Heap(Category category) : category(category)
		{
		}

		Heap(const Heap&) = delete;
		Heap& operator=(const Heap&) = delete;

		// Malloc shaped extras for C libraries. Their callbacks rarely give a size on free and
		// expect calloc and realloc, which memory_resource does not model. Engine code has no
		// business with these.
		[[nodiscard]] void* allocate_zeroed(size_t size, size_t alignment = DEFAULT_ALIGNMENT);
		[[nodiscard]] void* reallocate(void* ptr, size_t new_size);
		void deallocate_unsized(void* ptr);

		// The size the heap actually set aside for ptr, which is what the tracker counts.
		static size_t usable_size(void* ptr);

	private:
		void* do_allocate(size_t bytes, size_t alignment) override;
		void do_deallocate(void* ptr, size_t bytes, size_t alignment) override;
		bool do_is_equal(const Allocator& other) const noexcept override;

		Category m_category;
	};

	namespace internal
	{
		// The allocator underneath, brought up by Memory::initialize() and on each thread by
		// Memory::initialize_thread(). It also brings itself up on first use, so a static object
		// may allocate before either call.
		void heap_initialize();
		void heap_initialize_thread();
		void heap_shutdown_thread();
	}
}
