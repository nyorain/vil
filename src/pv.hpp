#pragma once

#include <cstdlib> // size_t
#include <cassert> // assert
#include <type_traits> // std::aligned_storage_t
#include <utility> // std::forward/std::move
#include <stdexcept> // std::invalid_argument
#include <cmath> // std::floor
#include <cstring> // std::memcpy
#include <memory> // std::allocator_traits

namespace fuen {

// Returns ceil(num / denom), efficiently, only using integer division.
inline constexpr std::size_t ceilDivide(std::size_t num, std::size_t denom) {
	return (num + denom - 1) / denom;
}

// Returns log2(x). Throws an exception if x isn't a power of two (which
// means a compile-time error if evaluated as constexpr).
inline constexpr unsigned exactLog2(std::size_t x) {
	if(x == 0) {
		throw std::invalid_argument("log2(0) not defined");
	}

    if((x & 1) == 1) {
		if(x != 1) {
			throw std::invalid_argument("Given number is not power of two");
		}

		return 0;
	}

	return 1 + exactLog2(x >> 1);
}

// Mixture of std::deque and std::vector. Mainly needed here since most
// std::deque implementations are shitty. This one if optimized for large
// allocations and it does not deal with the ugly bits.
// NOTE: could optimize for smaller allocations but not allocating a whole
// block at a time (would remove some of the theoretical advantages gained
// by the deque-like design tho)
// TODO: likely not exception-safe (when allocator or constructor throws).
template<typename T, typename Allocator = std::allocator<T>>
class PageVector {
public:
	static constexpr auto blockSize = 16 * 1024; // in bytes
	static constexpr auto elemsPerBlock = blockSize / sizeof(T); // floor

	struct Block;
	struct MutIterator;
	struct ConstIterator;

	using AllocTraits = std::allocator_traits<Allocator>;
	using BlockAllocTraits = typename std::allocator_traits<Allocator>::template rebind_traits<Block>;
	using BlockPtr = typename BlockAllocTraits::pointer;

	using value_type = T;
	using allocator_type = Allocator;
	using size_type = std::size_t;
	using difference_type = std::ptrdiff_t;
	using pointer = typename AllocTraits::pointer;
	using const_pointer = typename AllocTraits::const_pointer;
	using reference = value_type&;
	using const_reference = const value_type&;
	using iterator = MutIterator;
	using const_iterator = ConstIterator;

	struct Block {
		pointer values; // [elemsPerBlock]
	};

	// NOTE: can be more efficient
	// TODO: just iterating through the container can be made more efficient,
	//   could write a `range(container)` adaptor that uses a sentinal
	//   to not check in every operator++ whether the end is reached.
	struct Iterator {
		// TODO; iterator is invalidated when a new block is allocated.
		// We could do better, not sure if worth it tho
		Block* block_ {};
		unsigned elemID_ {};

		Iterator& operator++() {
			++elemID_;
			if(elemID_ == elemsPerBlock) {
				++block_;
				elemID_ = 0u;
			}

			return *this;
		}

		Iterator& operator++(int) {
			auto ret = *this;
			++(*this);
			return ret;
		}

		friend bool operator==(const Iterator& a, const Iterator& b) {
			return a.block_ == b.block_ && a.elemID_ == b.elemID_;
		}
		friend bool operator!=(const Iterator& a, const Iterator& b) {
			return a.block_ != b.block_ || a.elemID_ != b.elemID_;
		}

		friend Iterator operator+(Iterator a, std::ptrdiff_t diff) {
			auto elem = std::ptrdiff_t(a.elemID_) + diff;
			// TODO: inefficient
			a.block_ += std::floor(float(elem) / elemsPerBlock);
			a.elemID = elem % elemsPerBlock;
			return a;
		}

		friend Iterator operator-(Iterator a, std::ptrdiff_t diff) {
			auto elem = std::ptrdiff_t(a.elemID_) - diff;
			// TODO: inefficient
			a.block_ += std::floor(float(elem) / elemsPerBlock);
			a.elemID = elem % elemsPerBlock;
			return a;
		}

		reference get() const {
			assert(elemID_ < elemsPerBlock);
			return block_->values[elemID_];
		}
	};

	struct MutIterator : Iterator {
		pointer operator->() const { return &Iterator::get(); }
		reference operator*() const { return Iterator::get(); }
	};

	struct ConstIterator : Iterator {
		ConstIterator(Block* block, unsigned elem) : Iterator{block, elem} {}
		ConstIterator(MutIterator it) : Iterator(it) {}

		const_pointer operator->() const { return &Iterator::get(); }
		const_reference operator*() const { return Iterator::get(); }
	};


public:
	PageVector(Allocator&& alloc = {}) : alloc_(std::move(alloc)) {
		static_assert(blockSize > sizeof(T));
		static_assert(elemsPerBlock > 0);
		static_assert(sizeof(Block) <= blockSize);
	}

	PageVector(PageVector&& rhs) noexcept : alloc_(rhs.alloc_) { swap(*this, rhs); }
	PageVector& operator=(PageVector rhs) noexcept {
		swap(*this, rhs);
		return *this;
	}

	// TODO: could be implemented, deep-copy
	PageVector(const PageVector&) = delete;
	PageVector& operator=(const PageVector&) = delete;

	~PageVector() {
		release();
	}

	void reserve(std::size_t newCapacity) {
		if(newCapacity > numBlocks_ * elemsPerBlock) {
			doReserve(newCapacity);
		}
	}

	void resize(std::size_t newSize) {
		if(newSize > size_) {
			resizeGrow(newSize);
		} else if(newSize < size_) {
			resizeShrink(newSize);
		}
	}

	template<typename... Args>
	reference emplace_back(Args&&... args) {
		auto [blockID, elemID] = offsets(size_);
		if(blockID >= numBlocks_) {
			doReserve(size_ + 1);
		}

		++size_;
		auto& block = blockat(blockID);
		auto& elem = block.values[elemID];
		return *(new(&elem) T(std::forward<Args>(args)...));
	}

	value_type pop_back() {
		assert(!empty());
		auto& elem = get(size_ - 1);
		auto ret = std::move(elem);
		std::destroy_at(&elem);
		--size_;
		return ret;
	}

	/*
	void shrink_to_fit() {
		auto newNumBlocks = ceilDivide(size_, elemsPerBlock);
		assert(newNumBlocks > numBlocks_);
		auto newBlocksMem = blockAlloc().allocate(newNumBlocks);
		auto newBlocks = new(&*newBlocksMem) Block[newNumBlocks];

		std::memcpy(&*newBlocks, &*blocks_, newNumBlocks * sizeof(Block));

		// deallocate storage for new blocks
		for(auto i = newNumBlocks; i < numBlocks_; ++i) {
			alloc_.deallocate(blocks_[i].values, elemsPerBlock);
		}

		blockAlloc().deallocate(blocks_, numBlocks_);

		blocks_ = newBlocks;
		numBlocks_ = newNumBlocks;
	}
	*/

	// Removes all elements and releases all memory.
	// Basically like clear + shrink_to_fit.
	void release() {
		for(auto i = 0u; i < numBlocks_ && i * elemsPerBlock < size_; ++i) {
			auto& block = blockat(i);
			auto count = std::min(elemsPerBlock, size_ - i * elemsPerBlock);
			std::destroy(block.values, block.values + count);
			alloc_.deallocate(block.values, elemsPerBlock);
		}

		blockAlloc().deallocate(blocks_, numBlocks_);
		blocks_ = nullptr;
		numBlocks_ = 0u;
		size_ = 0u;
	}

	std::size_t size() const { return size_; }
	std::size_t empty() const { return size_ == 0; }
	std::size_t capacity() const { return numBlocks_ * elemsPerBlock; }
	void clear() { resizeShrink(0); }
	reference push_back(T&& value) { return emplace_back(std::move(value)); }
	reference operator[](std::size_t pos) { return get(pos); }
	const_reference operator[](std::size_t pos) const { return get(pos); }

	reference front() { return get(0); }
	const_reference front() const { return get(0); }

	reference back() { return get(size_ - 1); }
	const_reference back() const { return get(size_ - 1); }

	MutIterator begin() { return {blocks_, 0}; }
	ConstIterator begin() const { return {blocks_, 0}; }

	MutIterator end() { return endIter(); }
	ConstIterator end() const { return endIter(); }

	friend void swap(PageVector& a, PageVector& b) noexcept {
		using std::swap;
		swap(a.size_, b.size_);
		swap(a.alloc_, b.alloc_);
		swap(a.blocks_, b.blocks_);
		swap(a.numBlocks_, b.numBlocks_);
	}

private:
	struct Offsets {
		unsigned blockID;
		unsigned elemID;
	};

	Offsets offsets(std::size_t pos) const {
		// Naive implementation
		// TODO: speed this up for exact matches (which should be the
		// case usually) using bit operations. Can be done via
		// constexpr if switch and exactLog2
		// auto block = pos / elemsPerBlock;
		// auto elem = (pos % elemsPerBlock);
		auto [block, elem] = std::ldiv(long(pos), long(elemsPerBlock));
		return {unsigned(block), unsigned(elem)};
	}

	void doReserve(std::size_t newCapacity) {
		auto newNumBlocks = ceilDivide(newCapacity, elemsPerBlock);
		assert(newNumBlocks > numBlocks_);
		auto newBlocksMem = blockAlloc().allocate(newNumBlocks);
		auto newBlocks = new(&*newBlocksMem) Block[newNumBlocks];

		std::memcpy(&*newBlocks, &*blocks_, numBlocks_ * sizeof(Block));

		// allocate storage for new blocks
		for(auto i = numBlocks_; i < newNumBlocks; ++i) {
			newBlocks[i].values = alloc_.allocate(elemsPerBlock);
		}

		blockAlloc().deallocate(blocks_, numBlocks_);

		blocks_ = newBlocks;
		numBlocks_ = newNumBlocks;
	}

	auto blockAlloc() {
		using R = typename AllocTraits::template rebind_alloc<Block>;
		return R(alloc_);
	}

	Block& blockat(unsigned id) {
		assert(id < numBlocks_);
		return blocks_[id];
	}

	reference get(std::size_t pos) {
		auto [blockID, elemID] = offsets(pos);
		return blockat(blockID).values[elemID];
	}

	MutIterator endIter() const {
		auto [block, elem] = offsets(size_);
		return {blocks_ + block, elem};
	}

	void resizeGrow(std::size_t newSize) {
		assert(newSize >= size_);

		// make sure we have enough storage
		reserve(newSize);

		// fill blocks; create items in range [size_, newSize)
		auto [blockID, elemID] = offsets(size_);
		assert(elemID < elemsPerBlock);
		while(blockID < numBlocks_ && blockID * elemsPerBlock + elemID < newSize) {
			auto& block = blockat(blockID);
			auto count = std::min(elemsPerBlock, newSize - blockID * elemsPerBlock) - elemID;
			new(&*(block.values + elemID)) T[count];

			++blockID;
			elemID = 0;
		}

		size_ = newSize;
	}

	void resizeShrink(std::size_t newSize) {
		assert(newSize <= size_);

		// destroy elements in range [newSize, size_)
		auto [blockID, elemID] = offsets(newSize);
		while(blockID < numBlocks_ && blockID * elemsPerBlock < size_) {
			auto& block = blockat(blockID);
			auto end = std::min(elemsPerBlock, size_ - blockID * elemsPerBlock);
			std::destroy(block.values + elemID, block.values + end);

			++blockID;
			elemID = 0;
		}

		size_ = newSize;
	}

private:
	// TODO: use allocator traits
	Allocator alloc_;
	std::size_t size_ {};
	std::size_t numBlocks_ {};
	BlockPtr blocks_ {};
};

} // namespace fuen
