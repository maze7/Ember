#pragma once

// Containers can use the heap or an arena without changing type. Each request
// carries a tag for subsystem accounting.
//
// TagAllocator adapts this interface for standard containers. Acquiring or
// releasing storage makes a virtual call.

#include <ember/core/types.h>
#include <ember/memory/tag.h>

#include <cstddef>
#include <new>
#include <source_location>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ember::memory
{
	class Allocator
	{
	public:
		virtual ~Allocator() = default;

		// Alignment must be a power of two. Failure reports the request and stops
		// the process; this function never returns null.
		//
		// Zero-size requests still get distinct live addresses. Containers pass an
		// empty source location; direct memory::allocate calls capture the caller.
		virtual void* allocate(memory::Tag tag, size_t size, size_t alignment,
		    std::source_location where) = 0;

		// Use the allocator, size, and alignment of the original request. Arena
		// storage is reclaimed by lifetime rather than by this call.
		virtual void deallocate(void* ptr, size_t size, size_t alignment) = 0;
	};

	// Declared here so TagAllocator can default to the general heap without
	// including the rest of its public API.
	Allocator* heap();

	namespace internal
	{
		template <typename T>
		inline constexpr bool IsPair = false;

		template <typename A, typename B>
		inline constexpr bool IsPair<std::pair<A, B>> = true;

		template <typename T, template <typename> typename Alloc>
		concept TakesAllocator = requires { typename T::allocator_type; } &&
		                         std::is_constructible_v<typename T::allocator_type, Alloc<T>>;
	}

	// Carries an allocator and tag into a standard container. If neither is
	// supplied, allocations use the heap under Tag::Unknown.
	//
	// Move assignment and swap carry both with the storage. Copy assignment keeps
	// the destination's allocator; copy construction inherits the source's.
	template <typename T>
	struct TagAllocator
	{
		using value_type = T;
		using propagate_on_container_copy_assignment = std::false_type;
		using propagate_on_container_move_assignment = std::true_type;
		using propagate_on_container_swap = std::true_type;
		using is_always_equal = std::false_type;

		Allocator* allocator = nullptr;
		Tag tag = Tag::Unknown;

		TagAllocator() : allocator(heap())
		{
		}

		TagAllocator(Tag tag) : allocator(heap()), tag(tag)
		{
		}

		TagAllocator(Allocator* allocator, Tag tag = Tag::Unknown) : allocator(allocator), tag(tag)
		{
		}

		template <typename U>
		TagAllocator(const TagAllocator<U>& other) : allocator(other.allocator), tag(other.tag)
		{
		}

		T* allocate(size_t count)
		{
			return (
			    T*)allocator->allocate(tag, count * sizeof(T), alignof(T), std::source_location{});
		}

		void deallocate(T* ptr, size_t count)
		{
			allocator->deallocate(ptr, count * sizeof(T), alignof(T));
		}

		// Pass the allocator as the final argument to allocator-aware elements,
		// including each half of a pair, to keep their allocations in the same
		// allocator and tag as the container.
		template <typename U, typename... Args>
		void construct(U* object, Args&&... args)
		{
			if constexpr (internal::IsPair<U>)
			{
				construct_pair(object, std::forward<Args>(args)...);
			}
			else if constexpr (internal::TakesAllocator<U, TagAllocator>)
			{
				::new ((void*)object) U(std::forward<Args>(args)..., *this);
			}
			else
			{
				::new ((void*)object) U(std::forward<Args>(args)...);
			}
		}

		bool operator==(const TagAllocator&) const = default;

	private:
		// Construct each half in place with its own arguments and allocator.
		template <typename A, typename B, typename... As, typename... Bs>
		void construct_pair(std::pair<A, B>* object, std::piecewise_construct_t,
		    std::tuple<As...> a, std::tuple<Bs...> b)
		{
			::new ((void*)object) std::pair<A, B>(std::piecewise_construct,
			    with_allocator<A>(std::move(a)), with_allocator<B>(std::move(b)));
		}

		template <typename A, typename B>
		void construct_pair(std::pair<A, B>* object)
		{
			construct_pair(object, std::piecewise_construct, std::tuple<>(), std::tuple<>());
		}

		template <typename A, typename B, typename X, typename Y>
		void construct_pair(std::pair<A, B>* object, X&& x, Y&& y)
		{
			construct_pair(object, std::piecewise_construct,
			    std::forward_as_tuple(std::forward<X>(x)),
			    std::forward_as_tuple(std::forward<Y>(y)));
		}

		template <typename A, typename B, typename P>
		    requires(internal::IsPair<std::remove_cvref_t<P>>)
		void construct_pair(std::pair<A, B>* object, P&& pair)
		{
			construct_pair(object, std::piecewise_construct,
			    std::forward_as_tuple(std::forward<P>(pair).first),
			    std::forward_as_tuple(std::forward<P>(pair).second));
		}

		template <typename Half, typename... Args>
		auto with_allocator(std::tuple<Args...>&& args)
		{
			if constexpr (internal::TakesAllocator<Half, TagAllocator>)
			{
				return std::tuple_cat(std::move(args), std::tuple<TagAllocator<Half>>(*this));
			}
			else
			{
				return std::move(args);
			}
		}
	};
}
