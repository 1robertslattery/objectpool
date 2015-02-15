#include "memory_pool.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <memory>

namespace {

	const size_t MIN_BLOCK_ALIGN = 64;

	inline uint8_t * malloc_block(size_t block_size, size_t alignment)
	{
#if defined( _WIN32 )
		return reinterpret_cast<uint8_t*>(
				_aligned_malloc(block_size, alignment));
#else
		void * ptr;
		int result = posix_memalign(&ptr, alignment, block_size);
		return result == 0 ? reinterpret_cast<uint8_t *>(ptr) : nullptr;
#endif
	}

	inline void free_block(uint8_t * ptr)
	{
#if defined( _WIN32 )
		_aligned_free(ptr);
#else
		std::free(ptr);
#endif
	}

	/// Returns the next unused bit index
	inline uint32_t find_slot(uint32_t n)
	{
		// create a word with a single 1-bit at the position of the rightmost
		// 0-bit in x, producing 0 if none, then count the trailing zeros.
		uint32_t m = ~n & (n + 1);
#if _MSC_VER
		unsigned long i;
		_BitScanForward(&i, m);
		return i;
#else
		return __builtin_ctz(m);
#endif
	}


	/// Returns the number of allocations in the given mask
	inline uint32_t allocation_count(uint32_t n)
	{
#if _MSC_VER
		return __popcnt(n);
#else
		return __builtin_popcount(n);
#endif
	}

	/// Returns true if the pointer is of the given alignment
	inline bool is_aligned(const void * ptr, size_t align)
	{
		return (reinterpret_cast<uintptr_t>(ptr) & (align - 1)) == 0;
	}
}

MemoryPoolBase::MemoryPoolBase(uint_t entry_size, uint_t max_entries) :
	max_entries_(max_entries),
	entry_size_(entry_size),
	num_free_entries_(max_entries),
	pool_mem_(malloc_block(max_entries * entry_size, MIN_BLOCK_ALIGN)),
	next_free_(pool_mem_)
{
	// intialise the free list
	for (uint_t i = 0; i < max_entries_; ++i)
	{
		uint_t * ptr = reinterpret_cast<uint_t *>(element_at(i));
		*ptr = i + 1;
	}
}

MemoryPoolBase::~MemoryPoolBase()
{
	assert(num_free_entries_ == max_entries_);
	free_block(pool_mem_);
}

MemoryPoolStats MemoryPoolBase::get_stats() const
{
	MemoryPoolStats stats;
	stats.block_count = 1;
	stats.allocation_count = max_entries_ - num_free_entries_;
	return stats;
}

void * MemoryPoolBase::allocate()
{
	//increment_generation();

	if (num_free_entries_ > 1)
	{
		void * ptr = static_cast<void *>(next_free_);
		--num_free_entries_;
		next_free_ = element_at(*reinterpret_cast<uint_t *>(next_free_));
		return ptr;
	}
	else if (num_free_entries_ == 1)
	{
		void * ptr = static_cast<void *>(next_free_);
		num_free_entries_ = 0;
		next_free_ = nullptr;
		return ptr;
	}
	else
	{
		return nullptr;
	}
}

void MemoryPoolBase::deallocate(void * ptr)
{
	//increment_generation();
	assert(ptr >= pool_mem_ && ptr < (pool_mem_ + (entry_size_ * max_entries_)));
	// store index of next free entry in this pointer
	uint_t index = next_free_ != nullptr ? index_of(next_free_) : max_entries_;
	*reinterpret_cast<uint_t *>(ptr) = index;
	// set next free to this pointer
	next_free_ = reinterpret_cast<uint8_t *>(ptr);
	++num_free_entries_;
}


uint8_t * MemoryPoolBase::element_at(uint_t index)
{
	return pool_mem_ + (index * entry_size_);
}

const uint8_t * MemoryPoolBase::element_at(uint_t index) const
{
	return const_cast<MemoryPoolBase *>(this)->element_at(index);
}

MemoryPoolBase::uint_t MemoryPoolBase::index_of(const uint8_t * ptr) const
{
	return (ptr - pool_mem_) / entry_size_;
}

/*
size_t MemoryPoolBase::next_index(size_t index) const
{
}

size_t MemoryPoolBase::end_index() const
{
	return block_masks_.size() * NUM_BLOCK_ENTRIES;
}

uint32_t MemoryPoolBase::generation() const
{
	return generation_;
}

void MemoryPoolBase::increment_generation()
{
	uint32_t generation = generation_ + 1;
	generation_ = generation == INVALID_GENERATION ? 0 : generation;
}

bool MemoryPoolBase::check_generation(uint32_t generation) const
{
	return generation_ == generation;
}
*/

//
// Tests
//

#if UNIT_TESTS

#include "catch.hpp"

TEST_CASE("Single new and delete", "[allocation]")
{
	MemoryPool<uint32_t> mp(64);
	uint32_t * p = mp.new_object(0xaabbccdd);
	REQUIRE(p != nullptr);
	CHECK(is_aligned(p, 4));
	// should be aligned to the cache line size
	CHECK(is_aligned(p, MIN_BLOCK_ALIGN));
	CHECK(*p == 0xaabbccdd);
	mp.delete_object(p);
}

TEST_CASE("Double new and delete", "[allocation]")
{
	MemoryPool<uint32_t> mp(64);
	uint32_t * p1 = mp.new_object(0x11223344);
	REQUIRE(p1 != nullptr);
	CHECK(is_aligned(p1, 4));
	uint32_t * p2 = mp.new_object(0x55667788);
	REQUIRE(p2 != nullptr);
	CHECK(is_aligned(p2, 4));
	CHECK(p2 == p1 + 1);
	CHECK(*p1 == 0x11223344);
	mp.delete_object(p1);
	CHECK(*p2 == 0x55667788);
	mp.delete_object(p2);
}

TEST_CASE("Block fill and free", "[allocation]")
{
	MemoryPool<uint32_t> mp(64);
	std::vector<uint32_t *> v;
	for (size_t i = 0; i < 64; ++i)
	{
		uint32_t * p = mp.new_object(1 << i);
		REQUIRE(p != nullptr);
		CHECK(*p == 1u << i);
		v.push_back(p);
	}
	for (auto p : v)
	{
		mp.delete_object(p);
	}
}

TEST_CASE("Iterate full blocks", "[iteration]")
{
	MemoryPool<uint32_t> mp(64);
	std::vector<uint32_t *> v;
	size_t i;
	for (i = 0; i < 64; ++i)
	{
		uint32_t * p = mp.new_object(1 << i);
		REQUIRE(p != nullptr);
		CHECK(*p == 1u << i);
		v.push_back(p);
	}

	{
		auto stats = mp.get_stats();
		CHECK(stats.allocation_count == 64u);
		CHECK(stats.block_count == 1u);
	}

	// check values
	i = 0;
	for (auto itr = v.begin(), end = v.end(); itr != end; ++itr)
	{
		CHECK(**itr == 1u << i);
		++i;
	}

	// delete every second entry
	for (i = 1; i < 64; i += 2)
	{
		uint32_t * p = v[i];
		v[i] = nullptr;
		mp.delete_object(p);
	}

	// check allocation count is reduced but block count is the same
	{
		auto stats = mp.get_stats();
		CHECK(stats.allocation_count == 32u);
		CHECK(stats.block_count == 1u);
	}

	// check remaining objects
	for (i = 0; i < 64; i += 2)
	{
		CHECK(*v[i] == 1u << i);
	}

	// allocate 16 new entries (fill first block)
    for (i = 1; i < 32; i += 2)
	{
		CHECK(v[i] == nullptr);
        v[i] = mp.new_object(1 << i);
	}

	// check allocation and block count
	{
		auto stats = mp.get_stats();
		CHECK(stats.allocation_count == 48u);
		CHECK(stats.block_count == 1u);
	}

	// delete objects in second block
	for (i = 32; i < 64; ++i)
	{
		uint32_t * p = v[i];
		v[i] = nullptr;
		mp.delete_object(p);
	}

	// check that the empty block was freed
	{
		auto stats = mp.get_stats();
		CHECK(stats.allocation_count == 32u);
		CHECK(stats.block_count == 1u);
	}

	for (auto p : v)
	{
		mp.delete_object(p);
	}

	{
		auto stats = mp.get_stats();
		CHECK(stats.allocation_count == 0u);
		CHECK(stats.block_count == 1u);
	}
}

#endif // UNIT_TESTS

