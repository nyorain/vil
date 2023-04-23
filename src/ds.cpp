#include <ds.hpp>
#include <device.hpp>
#include <wrap.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <accelStruct.hpp>
#include <threadContext.hpp>
#include <util/util.hpp>
#include <util/profiling.hpp>

namespace vil {

// Whether descriptor sets increase the refCount of the handles
// written into the descriptors, effectively taking shared ownership of them.
// Doing this has a huge performance impact (especially for applications
// with many huge and dynamic descriptorSets). But not doing this means
// that descriptor sets might contain invalid bindings. That is mainly
// a problem for the resource viewer for descriptor sets at the moment
// as we are able to filter them out like 99% of the times (the other times
// we get valid new handles that were never bound but just happen
// to be at the address now, also not UB at least), and we can increase
// this via Device::keepAliveCount
// TODO: better documentation, make it a meson_option
// TODO: we could try to explicitly detect the invalid bindings, as we
//   do with descriptor sets already. See notes in ds3.hpp for details.
constexpr auto refBindings = false;

// Whether we allow pool fragmentation. Setting this to false means we
// might explicitly return an error from ds allocation (as valid per spec)
// even though the driver could do it.
constexpr auto enableDsFragmentationPath = false;

// util
size_t descriptorSize(VkDescriptorType dsType) {
	switch(category(dsType)) {
		case DescriptorCategory::buffer: return sizeof(BufferDescriptor);
		case DescriptorCategory::image: return sizeof(ImageDescriptor);
		case DescriptorCategory::bufferView: return sizeof(BufferViewDescriptor);
		case DescriptorCategory::accelStruct: return sizeof(AccelStructDescriptor);
		case DescriptorCategory::inlineUniformBlock: return 1u;
		case DescriptorCategory::none:
			dlg_error("unreachable: Invalid descriptor category");
			return 0u;
	}

	dlg_error("unreachable");
	return 0u;
}

std::byte* bindingData(const DescriptorSet& ds) {
	static_assert(sizeof(ds) % sizeof(void*) == 0u);
	auto ptr = reinterpret_cast<const std::byte*>(&ds);
	return const_cast<std::byte*>(ptr) + sizeof(ds);
}

DescriptorStateRef::DescriptorStateRef(const DescriptorSet& ds) :
	layout(ds.layout.get()), data(bindingData(ds)), variableDescriptorCount(ds.variableDescriptorCount) {
}

DescriptorStateRef::DescriptorStateRef(DescriptorStateCopy& ds) :
	layout(ds.layout.get()),
	data(reinterpret_cast<std::byte*>(&ds) + sizeof(DescriptorStateCopy)),
	variableDescriptorCount(ds.variableDescriptorCount) {
}

template<typename T, typename O>
void debugStatAdd(std::atomic<T>& dst, const O& val) {
#ifdef VIL_DEBUG_STATS
	dst.fetch_add(val, std::memory_order_relaxed);
#else // VIL_DEBUG_STATS
	(void) dst;
	(void) val;
#endif // VIL_DEBUG_STATS
}

template<typename T, typename O>
void debugStatSub(std::atomic<T>& dst, const O& val) {
#ifdef VIL_DEBUG_STATS
	auto before = dst.fetch_sub(val, std::memory_order_relaxed);
	dlg_assert(before >= val);
#else // VIL_DEBUG_STATS
	(void) dst;
	(void) val;
#endif // VIL_DEBUG_STATS
}

// Returns the total raw memory size needed by descriptor state of
// the given layout, with the given variable descriptor count.
size_t totalDescriptorMemSize(const DescriptorSetLayout& layout, u32 variableDescriptorCount) {
	if(layout.bindings.empty()) {
		return 0;
	}

	auto& last = layout.bindings.back();
	size_t ret = last.offset;
	auto lastCount = last.descriptorCount;

	if(last.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		lastCount = variableDescriptorCount;
	}

	if(lastCount) {
		dlg_assert(last.descriptorType != VK_DESCRIPTOR_TYPE_MAX_ENUM);
		ret += lastCount * descriptorSize(last.descriptorType);
	}

	return ret;
}

bool conflicting(const DescriptorSetLayout& da, const DescriptorSetLayout& db,
		bool checkImmutableSamplers) {
	if(&da == &db) {
		return false;
	}

	auto count = std::min(da.bindings.size(), db.bindings.size());
	for(auto b = 0u; b < count; ++b) {
		auto& ba = da.bindings[b];
		auto& bb = db.bindings[b];

		if(ba.descriptorCount == 0u || bb.descriptorCount == 0u) {
			continue;
		}

		if(ba.descriptorType != bb.descriptorType) {
			return true;
		}

		// immutable samplers
		auto samplersRelevant = checkImmutableSamplers &&
			(ba.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
			 ba.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		if(samplersRelevant) {
			if(bool(ba.immutableSamplers) != bool(bb.immutableSamplers)) {
				return true;
			}

			if(ba.immutableSamplers) {
				auto count = std::min(ba.descriptorCount, bb.descriptorCount);
				for(auto e = 0u; e < count; ++e) {
					// TODO: consider compatible (instead of just same)
					// samplers as well?
					if(ba.immutableSamplers[e] != bb.immutableSamplers[e]) {
						return true;
					}
				}
			}
		}
	}

	return false;
}

bool compatible(const DescriptorSetLayout& da, const DescriptorSetLayout& db,
		bool checkImmutableSamplers) {
	if(&da == &db) {
		return true;
	}

	if(da.bindings.size() != db.bindings.size()) {
		return false;
	}

	// bindings are sorted by binding number so we can simply compare
	// them in order
	for(auto b = 0u; b < da.bindings.size(); ++b) {
		auto& ba = da.bindings[b];
		auto& bb = db.bindings[b];

		if(ba.descriptorCount != bb.descriptorCount ||
				ba.descriptorType != bb.descriptorType ||
				ba.stageFlags != bb.stageFlags) {
			return false;
		}

		// immutable samplers
		auto samplersRelevant = checkImmutableSamplers &&
			(ba.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
			 ba.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		if(samplersRelevant) {
			if(bool(ba.immutableSamplers) != bool(bb.immutableSamplers)) {
				return false;
			}

			if(ba.immutableSamplers) {
				dlg_assert(ba.descriptorCount == bb.descriptorCount);
				for(auto e = 0u; e < ba.descriptorCount; ++e) {
					// TODO: consider compatible (instead of just same)
					// samplers as well?
					if(ba.immutableSamplers[e] != bb.immutableSamplers[e]) {
						return false;
					}
				}
			}
		}
	}

	return true;
}

void initImmutableSamplers(DescriptorStateRef state) {
	ZoneScoped;

	for(auto b = 0u; b < state.layout->bindings.size(); ++b) {
		// If the binding holds immutable samplers, fill them in.
		// We do this so we don't have to check for immutable samplers
		// every time we read a binding. Also needed for correct
		// invalidation tracking.
		if(state.layout->bindings[b].immutableSamplers.get()) {
			dlg_assert(needsSampler(state.layout->bindings[b].descriptorType));
			auto binds = images(state, b);

			for(auto e = 0u; e < binds.size(); ++e) {
				auto* sampler = state.layout->bindings[b].immutableSamplers[e].get();
				dlg_assert(sampler);

				// NOTE: this can happen e.g. when initializing the samplers
				// of a ds cow. It means the sampler itself was already
				// destroyed but we keep our object alive for the ui.
				// dlg_assert(sampler->handle);

				binds[e].sampler = sampler;
				if(refBindings) {
					incRefCount(*sampler);
				}
			}
		}
	}
}

void initDescriptorState(std::byte* data,
		const DescriptorSetLayout& layout, u32 variableDescriptorCount) {
	// Possibly faster path but strictly speaking UB I guess?
	// Compbilers should probably optimize it to this tho
	auto bindingSize = totalDescriptorMemSize(layout, variableDescriptorCount);
	std::memset(data, 0x0, bindingSize);
}

void copy(DescriptorStateRef dst, unsigned dstBindID, unsigned dstElemID,
		DescriptorStateRef src, unsigned srcBindID, unsigned srcElemID) {
	auto& srcLayout = src.layout->bindings[srcBindID];
	auto& dstLayout = dst.layout->bindings[dstBindID];
	dlg_assert(srcLayout.descriptorType == dstLayout.descriptorType);

	switch(category(dstLayout.descriptorType)) {
		case DescriptorCategory::image: {
			auto immutSampler = !!dstLayout.immutableSamplers.get();
			auto srcCopy = images(src, srcBindID)[srcElemID];
			auto& dstBind = images(dst, dstBindID)[dstElemID];

			if(refBindings) {
				if(dstBind.sampler && !immutSampler) decRefCount(*dstBind.sampler);
				if(srcCopy.sampler && !immutSampler) incRefCount(*srcCopy.sampler);
				if(dstBind.imageView) decRefCount(*dstBind.imageView);
				if(srcCopy.imageView) incRefCount(*srcCopy.imageView);
			}

			dstBind.imageView = std::move(srcCopy.imageView);
			dstBind.layout = srcCopy.layout;

			if(!immutSampler) {
				dstBind.sampler = std::move(srcCopy.sampler);
			}

			break;
		} case DescriptorCategory::buffer: {
			auto& srcBuf = buffers(src, srcBindID)[srcElemID];
			auto& dstBuf = buffers(dst, dstBindID)[dstElemID];

			if(refBindings) {
				if(dstBuf.buffer) decRefCount(*dstBuf.buffer);
				if(srcBuf.buffer) incRefCount(*srcBuf.buffer);
			}

			dstBuf = srcBuf;
			break;
		} case DescriptorCategory::bufferView: {
			auto& srcBuf = bufferViews(src, srcBindID)[srcElemID];;
			auto& dstBuf = bufferViews(dst, dstBindID)[dstElemID];

			if(refBindings) {
				if(dstBuf.bufferView) decRefCount(*dstBuf.bufferView);
				if(srcBuf.bufferView) incRefCount(*srcBuf.bufferView);
			}

			dstBuf = srcBuf;
			break;
		} case DescriptorCategory::inlineUniformBlock: {
			// NOTE: we copy byte-by-byte here which is inefficient. Would
			// have to rework the entire code structure of 'copy'. Shouldn't
			// be a huge problem tho, inline uniform blocks should be very
			// small anyways.
			auto srcBuf = inlineUniformBlock(src, srcBindID);
			auto dstBuf = inlineUniformBlock(src, dstBindID);
			dlg_assert(srcElemID < srcBuf.size());
			dlg_assert(dstElemID < dstBuf.size());
			dstBuf[dstElemID] = srcBuf[srcElemID];
			break;
		} case DescriptorCategory::accelStruct: {
			auto& srcAS = accelStructs(src, srcBindID)[srcElemID];
			auto& dstAS = accelStructs(dst, dstBindID)[dstElemID];

			if(refBindings) {
				if(dstAS.accelStruct) decRefCount(*dstAS.accelStruct);
				if(srcAS.accelStruct) incRefCount(*srcAS.accelStruct);
			}

			dstAS = srcAS;
			break;
		} case DescriptorCategory::none:
			dlg_error("unreachable: Invalid descriptor type");
			break;
	}
}

template<typename Set, typename Handle>
bool validateIncRef(Device& dev, Set& set, Handle*& handle, bool checkReplace) {
	if(!handle) {
		return false;
	}

	if(!checkReplace) {
		incRefCount(*handle);
		return true;
	}

	(void) dev;

	// TODO: not fully exception safe
	// Remove with c++20s better container lookup
	IntrusivePtr<Handle> dummy(acquireOwnership, handle);
	auto it = set.inner.find(dummy);
	(void) dummy.release();

	if(it == set.inner.end()) {
		dlg_debug("Detected destroyed handle in descriptorSet");
		handle = nullptr;
		return false;
	}

	incRefCount(*handle);
	return true;
}

// NOTE: regarding checkIfValid
// When we create a CoW in addCow and refBindings == false, the descriptorSet
// might actually contain bindings that are invalid. With the descriptor_indexing
// features, it's valid to submit records using descriptor sets with
// destroyed bindings.
// In that case, we need to check for each binding if it's still valid. We
// can know whether the pointers are dangling by just looking them up in the
// respective device data structures. Yep, this can go wrong if a handle
// is destroyed and then another handle (of the same type) recreated at the
// same address, giving us a false positive here. We won't crash in that case
// but simply assume a wrong handle being written to the descriptor.
// To minimize the chance for that case, we have the keepAliveXXX buffers
// in Device, actually keeping handles alive for a time. But it might still
// happen. Users that want to make absolutely sure false positives
// can't happen should simply run with refBindings = true.
// We *really* don't want refBindings = true since it's expensive, making
// descriptor set updates and destruction a lot slower.
static void doRefBindings(Device& dev, DescriptorStateRef state, bool checkIfValid) {
	ZoneScopedN("refBindings");

	// NOTE: might seem like bad design but we need the device mutex locked
	// to validate handles when checkIfValid = true.
	// We can't lock it locally since it must be locked *before* the
	// ds/pool mutex is locked.
	assertOwned(dev.mutex);

	for(auto b = 0u; b < state.layout->bindings.size(); ++b) {
		auto& binding = state.layout->bindings[b];
		if(!descriptorCount(state, b)) {
			continue;
		}

		switch(category(binding.descriptorType)) {
			case DescriptorCategory::buffer: {
				for(auto& b : buffers(state, b)) {
					validateIncRef(dev, dev.buffers, b.buffer, checkIfValid);
				}
				break;
			} case DescriptorCategory::bufferView: {
				for(auto& b : bufferViews(state, b)) {
					validateIncRef(dev, dev.bufferViews, b.bufferView, checkIfValid);
				}
				break;
			} case DescriptorCategory::image: {
				for(auto& b : images(state, b)) {
					validateIncRef(dev, dev.imageViews, b.imageView, checkIfValid);
					validateIncRef(dev, dev.samplers, b.sampler, checkIfValid);
				}
				break;
			} case DescriptorCategory::accelStruct: {
				for(auto& b : accelStructs(state, b)) {
					validateIncRef(dev, dev.accelStructs, b.accelStruct, checkIfValid);
				}
				break;
			} case DescriptorCategory::inlineUniformBlock: {
				// no-op, we just have raw bytes here
				break;
			} case DescriptorCategory::none:
				dlg_error("unreachable: invalid descriptor type");
				break;
		}
	}
}

void unrefBindings(DescriptorStateRef state) {
	ZoneScopedN("destroyDsState");

	dlg_assert(state.layout);
	assertNotOwned(state.layout->dev->mutex);

	for(auto b = 0u; b < state.layout->bindings.size(); ++b) {
		auto& binding = state.layout->bindings[b];
		if(!descriptorCount(state, b)) {
			continue;
		}

		switch(category(binding.descriptorType)) {
			case DescriptorCategory::buffer: {
				for(auto& b : buffers(state, b)) {
					if(b.buffer) {
						decRefCount(*b.buffer);
					}
				}
				break;
			} case DescriptorCategory::bufferView: {
				for(auto& b : bufferViews(state, b)) {
					if(b.bufferView) {
						decRefCount(*b.bufferView);
					}
				}
				break;
			} case DescriptorCategory::image: {
				for(auto& b : images(state, b)) {
					if(b.imageView) {
						decRefCount(*b.imageView);
					}
					if(b.sampler) {
						decRefCount(*b.sampler);
					}
				}
				break;
			} case DescriptorCategory::accelStruct: {
				for(auto& b : accelStructs(state, b)) {
					if(b.accelStruct) {
						decRefCount(*b.accelStruct);
					}
				}
				break;
			} case DescriptorCategory::inlineUniformBlock: {
				// no-op, we just have raw bytes here
				break;
			} case DescriptorCategory::none:
				dlg_error("unreachable: invalid descriptor type");
				break;
		}
	}
}

void DescriptorStateCopy::Deleter::operator()(DescriptorStateCopy* copy) const {
	// we have a reference on the bindings in any case
	unrefBindings(DescriptorStateRef(*copy));

	auto memSize = sizeof(DescriptorStateCopy);
	memSize += totalDescriptorMemSize(*copy->layout, copy->variableDescriptorCount);
	debugStatSub(DebugStats::get().descriptorCopyMem, u32(memSize));
	debugStatSub(DebugStats::get().aliveDescriptorCopies, 1u);

	copy->~DescriptorStateCopy();

	// we allocated the memory as std::byte array, so we have to free
	// it like that. We don't have to call any other destructors of
	// Binding elements since they are all trivial
	auto ptr = reinterpret_cast<std::byte*>(copy);
	TracyFreeS(ptr, 8);
	delete[] ptr;
}

DescriptorStateCopyPtr DescriptorSet::copyLockedState() {
	ZoneScoped;

	// NOTE: when this assert fails somewhere, we have to adjust the code (storing stuff
	// that is up-to-pointer-aligned directly behind the state object in memory).
	static_assert(sizeof(DescriptorStateCopy) % alignof(void*) == 0u);
	assertOwned(pool->mutex);

	auto bindingSize = totalDescriptorMemSize(*this->layout, this->variableDescriptorCount);
	auto memSize = sizeof(DescriptorStateCopy) + bindingSize;

	auto* mem = new std::byte[memSize]();
	TracyAllocS(mem, memSize, 8);

	debugStatAdd(DebugStats::get().descriptorCopyMem, u32(memSize));
	debugStatAdd(DebugStats::get().aliveDescriptorCopies, 1u);

	auto* copy = new(mem) DescriptorStateCopy();
	dlg_assert(reinterpret_cast<std::byte*>(copy) == mem);

	copy->variableDescriptorCount = this->variableDescriptorCount;
	copy->layout = this->layout;

	DescriptorStateRef srcRef(*this);
	auto dstRef = srcRef;
	dstRef.data = mem + sizeof(DescriptorStateCopy);

	initDescriptorState(dstRef.data, *this->layout, this->variableDescriptorCount);
	initImmutableSamplers(dstRef);

	// copy descriptors
	for(auto b = 0u; b < this->layout->bindings.size(); ++b) {
		for(auto e = 0u; e < descriptorCount(srcRef, b); ++e) {
			// with !refBindings, we "take ownership" of the increased
			// reference count here
			vil::copy(dstRef, b, e, srcRef, b, e);
		}
	}

	return DescriptorStateCopyPtr(copy);
}

DescriptorStateCopyPtr DescriptorSet::validateAndCopyLocked() {
	assertOwned(dev().mutex);
	assertOwned(pool->mutex);

	// We need to reference all bindings when they aren't referenced
	// at the moment. This will also validate them (i.e. set the ones
	// we detect as destroyed to null)
	if(!refBindings) {
		doRefBindings(dev(), *this, true);
	}

	return copyLockedState();
}

u32 descriptorCount(DescriptorStateRef state, unsigned binding) {
	dlg_assert(state.layout);
	dlg_assert(binding < state.layout->bindings.size());
	auto& layout = state.layout->bindings[binding];
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT) {
		return state.variableDescriptorCount;
	}

	return layout.descriptorCount;
}

u32 totalDescriptorCount(DescriptorStateRef state) {
	auto ret = 0u;
	for(auto i = 0u; i < state.layout->bindings.size(); ++i) {
		ret += descriptorCount(state, i);
	}

	return ret;
}

IntrusivePtr<DescriptorSetCow> DescriptorSet::addCowLocked() {
	// NOTE: might seem like bad design but on the doRefBindings codepath,
	// we need the device mutex locked to validate handles.
	// We can't lock it locally since it must be locked *before* the
	// ds/pool mutex is locked.
	assertOwned(dev().mutex);
	assertOwned(pool->mutex);

	if(!cow_) {
		// TODO PERF: get from a pool or something
		// (low prio since only relevant for gui stuff)
		cow_.reset(new DescriptorSetCow());
		cow_->ds = this;

		// we need to reference all bindings when they aren't referenced
		// at the moment.
		if(!refBindings) {
			doRefBindings(dev(), *this, true);
		}
	}

	// increase reference count via new intrusive ptr
	return IntrusivePtr<DescriptorSetCow>(cow_);
}

std::unique_lock<LockableBase(DebugMutex)> DescriptorSet::checkResolveCow() {
	std::unique_lock objLock(pool->mutex);
	if(!cow_) {
		return objLock;
	}

	dlg_assert(cow_->ds == this);
	dlg_assert(!cow_->copy);

	// Check if there is anybody interested in the cow.
	// This isn't a race, nobody is able to access cow_ from the outside
	// without holding mutex_. So if the refCount is 1 we know that
	// we keep the only reference.
	// We just didn't destroy it before because that's not possible
	// to do safely without deadlock. Destroying it here now instead.
	if(cow_->refCount.load() == 1u) {
		// With !refBindings, we referenced all bindings when a cow was
		// added so we have to unref them here since we will never
		// create the cow-owned copy that takes ownership of the increased
		// counts.
		if(!refBindings) {
			unrefBindings(*this);
		}

		cow_->ds = nullptr; // just as a debug marker, see ~DescriptorSetCow
		cow_.reset();
	} else {
		// here we have to resolve the cow
		std::unique_lock cowLock(cow_->mutex);
		cow_->copy = this->copyLockedState();
		cow_->ds = nullptr; // just as a debug marker, see ~DescriptorSetCow
	}

	cow_.reset();
	return objLock;
}

void destroy(DescriptorSet& ds, bool unlink) {
	ZoneScoped;

	dlg_assert(ds.pool);
	if(!HandleDesc<VkDescriptorSet>::wrap) {
		ds.dev().descriptorSets.mustErase(ds.handle);
	}

	// no need to keep lock here, ds can't be accessed anymore
	ds.checkResolveCow();

	if(refBindings) {
		unrefBindings(ds);
	}

	debugStatSub(DebugStats::get().aliveDescriptorSets, 1u);

	auto& pool = *ds.pool;
	auto* setEntry = ds.setEntry;

	{
		// The descriptor is placed in a raw memory block so we have to
		// call the dtor manually here
		ExtZoneScopedN("dtor");
		ds.~DescriptorSet();
	}

	// Return data to pool. We don't have to lock the pool mutex
	// for this, external sync guaranteed by spec and it's not
	// accessed by us.

	if constexpr(enableDsFragmentationPath) {
		auto* raw = reinterpret_cast<std::byte*>(&ds);
		if(raw < pool.data.get() || pool.data.get() + pool.dataSize <= raw) {
			// See AllocateDescriptorSets. We had to choose a slow path due
			// to fragmentation
			dlg_trace("free independent DS data slot");
			dlg_assert(setEntry->offset == u32(-1));
			delete[] raw;
		}
	}

	if(unlink) {
		auto lock = std::scoped_lock(pool.mutex);
		dlg_assert(pool.flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);

		// unlink setEntry
		dlg_assert(!setEntry->next == (setEntry == pool.highestEntry));
		dlg_assert(!setEntry->prev == (setEntry == pool.usedEntries));

		if(setEntry->next) {
			setEntry->next->prev = setEntry->prev;
		} else {
			pool.highestEntry = setEntry->prev;
		}

		if(setEntry->prev) {
			setEntry->prev->next = setEntry->next;
		} else {
			pool.usedEntries = setEntry->next;
		}

		if(setEntry == pool.lastEntry) {
			pool.lastEntry = setEntry->prev;
		}

		// return to free list
		setEntry->next = pool.freeEntries;
		setEntry->prev = nullptr;
		setEntry->set = nullptr;
		pool.freeEntries = setEntry;
	}
}

span<BufferDescriptor> buffers(DescriptorStateRef state, unsigned binding) {
	dlg_assert(binding < state.layout->bindings.size());

	auto& layout = state.layout->bindings[binding];
	dlg_assert(category(layout.descriptorType) == DescriptorCategory::buffer);

	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		count = state.variableDescriptorCount;
	}

	auto ptr = state.data + layout.offset;
	auto d = std::launder(reinterpret_cast<BufferDescriptor*>(ptr));
	return {d, count};
}
span<ImageDescriptor> images(DescriptorStateRef state, unsigned binding) {
	dlg_assert(binding < state.layout->bindings.size());

	auto& layout = state.layout->bindings[binding];
	dlg_assert(category(layout.descriptorType) == DescriptorCategory::image);

	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		count = state.variableDescriptorCount;
	}

	auto ptr = state.data + layout.offset;
	auto d = std::launder(reinterpret_cast<ImageDescriptor*>(ptr));
	return {d, count};
}
span<BufferViewDescriptor> bufferViews(DescriptorStateRef state, unsigned binding) {
	dlg_assert(binding < state.layout->bindings.size());

	auto& layout = state.layout->bindings[binding];
	dlg_assert(category(layout.descriptorType) == DescriptorCategory::bufferView);

	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		count = state.variableDescriptorCount;
	}

	auto ptr = state.data + layout.offset;
	auto d = std::launder(reinterpret_cast<BufferViewDescriptor*>(ptr));
	return {d, count};
}
span<std::byte> inlineUniformBlock(DescriptorStateRef state, unsigned binding) {
	dlg_assert(binding < state.layout->bindings.size());

	auto& layout = state.layout->bindings[binding];
	dlg_assert(layout.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT);

	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		count = state.variableDescriptorCount;
	}

	auto ptr = state.data + layout.offset;
	return {ptr, count};
}
span<AccelStructDescriptor> accelStructs(DescriptorStateRef state, unsigned binding) {
	dlg_assert(binding < state.layout->bindings.size());

	auto& layout = state.layout->bindings[binding];
	dlg_assert(category(layout.descriptorType) == DescriptorCategory::accelStruct);

	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		count = state.variableDescriptorCount;
	}

	auto ptr = state.data + layout.offset;
	auto d = std::launder(reinterpret_cast<AccelStructDescriptor*>(ptr));
	return {d, count};
}

// DescriptorPool impl
DescriptorPool::~DescriptorPool() {
	if(!dev) {
		return;
	}

	for(auto it = usedEntries; it; it = it->next) {
		dlg_assert(it->set);
		destroy(*it->set, false);
	}

	debugStatSub(DebugStats::get().descriptorPoolMem, dataSize);
	TracyFree(data.get());
}

DescriptorSetLayout::~DescriptorSetLayout() {
	if(!dev) {
		return;
	}

	// ds layouts are never used directly by command buffers
	dlg_assert(handle);
	dev->dispatch.DestroyDescriptorSetLayout(dev->handle, handle, nullptr);
}

DescriptorUpdateTemplate::~DescriptorUpdateTemplate() {
	if(!dev) {
		return;
	}

	// never used directly by command buffers
	dlg_assert(handle);
	dev->dispatch.DestroyDescriptorUpdateTemplate(dev->handle, handle, nullptr);
}

// util
DescriptorCategory category(VkDescriptorType type) {
	switch(type) {
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			return DescriptorCategory::image;
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
			return DescriptorCategory::buffer;
		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			return DescriptorCategory::bufferView;
		case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT:
			return DescriptorCategory::inlineUniformBlock;
		case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
			return DescriptorCategory::accelStruct;
		default:
			dlg_trace("Unsupported descriptor type: {}", type);
			return DescriptorCategory::none;
	}
}

bool needsSampler(VkDescriptorType type) {
	switch(type) {
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_SAMPLER:
			return true;
		default:
			return false;
	}
}

bool needsBoundSampler(const DescriptorSetLayout& dsl, unsigned binding) {
	auto& bind = dsl.bindings[binding];
	return needsSampler(bind.descriptorType) && !bind.immutableSamplers;
}

bool needsImageView(VkDescriptorType type) {
	switch(type) {
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			return true;
		default:
			return false;
	}
}

bool needsImageLayout(VkDescriptorType type) {
	switch(type) {
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			return true;
		default:
			return false;
	}
}

bool needsDynamicOffset(VkDescriptorType type) {
	switch(type) {
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
			return true;
		default:
			return false;
	}
}

// dsLayout
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(
		VkDevice                                    device,
		const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDescriptorSetLayout*                      pSetLayout) {
	// NOTE: we don't use host allocators here since this handle is potentially
	// kept alive inside the layer, preventing us from passing an application
	// allocator to the destruction function
	// See design.md on allocators.
	(void) pAllocator;

	auto& dev = getDevice(device);

	// unwrap immutable sampler handles
	auto nci = *pCreateInfo;

	ThreadMemScope memScope;
	auto nbindings = memScope.copy(nci.pBindings, nci.bindingCount);
	nci.pBindings = nbindings.data();

	auto maxBindingID = 0u;
	for(auto& bind : nbindings) {
		maxBindingID = std::max(maxBindingID, bind.binding);
		if(!needsSampler(bind.descriptorType) || bind.descriptorCount == 0 ||
				!bind.pImmutableSamplers) {
			continue;
		}

		auto handles = memScope.alloc<VkSampler>(bind.descriptorCount);
		for(auto i = 0u; i < bind.descriptorCount; ++i) {
			auto& sampler = get(dev, bind.pImmutableSamplers[i]);
			handles[i] = sampler.handle;
		}

		bind.pImmutableSamplers = handles.data();
	}

	// PERF: our bindings vector is always the size of the largest bindingID + 1 to
	//   allow fast direct access per bindingID.
	//   Per spec, there is not limit on bindingIDs, so large IDs with
	//   large holes in between are really inefficient.
	//   Since accessing bindings is on a path that must be fast, just storing
	//   the used slots or using a mapping by default here is not feasible.
	//   Maybe use a fallback path when extremely large bindings are detected?
	if(maxBindingID >= 10'000 && (maxBindingID / float(pCreateInfo->bindingCount)) < 0.1) {
		dlg_warn("DesriptorSetLayout: Large bindingID and large holes detected, "
			"vil will be inefficient");
	}

	auto res = dev.dispatch.CreateDescriptorSetLayout(dev.handle, &nci, nullptr, pSetLayout);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto dsLayoutPtr = IntrusivePtr<DescriptorSetLayout>(new DescriptorSetLayout());
	auto& dsLayout = *dsLayoutPtr;
	dsLayout.dev = &dev;
	dsLayout.handle = *pSetLayout;
	dsLayout.flags = nci.flags;

	auto* flagsInfo = findChainInfo<VkDescriptorSetLayoutBindingFlagsCreateInfo,
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO>(*pCreateInfo);
	flagsInfo = (flagsInfo && flagsInfo->bindingCount == 0u) ? nullptr : flagsInfo;
	dlg_assert(!flagsInfo || flagsInfo->bindingCount == pCreateInfo->bindingCount);


	ensureSize(dsLayout.bindings, maxBindingID + 1);
	for(auto i = 0u; i < pCreateInfo->bindingCount; ++i) {
		const auto& bind = pCreateInfo->pBindings[i];

		auto& dst = dsLayout.bindings[bind.binding];
		dst.descriptorCount = bind.descriptorCount;
		dst.descriptorType = bind.descriptorType;
		dst.stageFlags = bind.stageFlags;
		dst.flags = flagsInfo ? flagsInfo->pBindingFlags[i] : 0u;

		if(needsSampler(bind.descriptorType) && dst.descriptorCount > 0 &&
				bind.pImmutableSamplers) {
			// Couldn't find in the spec whether this is allowed or not.
			// But it seems incorrect to me, we might not handle it correctly
			// everywhere.
			dlg_assert(!(dst.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT));
			dst.immutableSamplers = std::make_unique<IntrusivePtr<Sampler>[]>(dst.descriptorCount);
			for(auto e = 0u; e < dst.descriptorCount; ++e) {
				dst.immutableSamplers[e] = getPtr(dev, bind.pImmutableSamplers[e]);
			}

			dsLayout.immutableSamplers = true;
		}
	}

	// number offsets
	auto off = 0u;
	for(auto b = 0u; b < dsLayout.bindings.size(); ++b) {
		auto& bind = dsLayout.bindings[b];
		bind.offset = off;

		if(bind.descriptorCount == 0u) {
			continue;
		}

		off += unsigned(bind.descriptorCount * descriptorSize(bind.descriptorType));

		auto varCount = !!(bind.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT);
		dlg_assert(b + 1 == dsLayout.bindings.size() || !varCount);

		if(needsDynamicOffset(bind.descriptorType)) {
			// VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-pBindingFlags-03015
			dlg_assert(!varCount);
			bind.dynOffset = dsLayout.numDynamicBuffers;
			dsLayout.numDynamicBuffers += bind.descriptorCount;
		}
	}

	*pSetLayout = castDispatch<VkDescriptorSetLayout>(dsLayout);
	dev.dsLayouts.mustEmplace(*pSetLayout, std::move(dsLayoutPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(
		VkDevice                                    device,
		VkDescriptorSetLayout                       descriptorSetLayout,
		const VkAllocationCallbacks*                pAllocator) {
	if(!descriptorSetLayout) {
		return;
	}

	auto& dev = getDevice(device);
	dev.dsLayouts.mustErase(descriptorSetLayout);

	// NOTE: We intenntionally don't destruct the handle here, handle might
	// need to be kept alive, they have shared ownership. Destroyed
	// in handle destructor.
	// dev.dispatch.DestroyDescriptorSetLayout(dev.handle, dsl.handle, pAllocator);
	(void) pAllocator;
}

// dsPool
void initResetPoolEntries(DescriptorPool& dsPool) {
	auto lock = std::scoped_lock(dsPool.mutex);

	dsPool.entries[0] = {};
	dsPool.entries[dsPool.maxSets - 1] = {};

	for(auto i = 1u; i + 1 < dsPool.maxSets; ++i) {
		dsPool.entries[i] = {};
		dsPool.entries[i].prev = &dsPool.entries[i - 1];
		dsPool.entries[i].next = &dsPool.entries[i + 1];
	}

	if(dsPool.maxSets > 1) {
		dsPool.entries[0].next = &dsPool.entries[1];
		dsPool.entries[dsPool.maxSets - 1].prev = &dsPool.entries[dsPool.maxSets - 2];
	}

	dsPool.freeEntries = &dsPool.entries[0];

	dsPool.usedEntries = nullptr;
	dsPool.lastEntry = nullptr;
	dsPool.highestEntry = nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorPool(
		VkDevice                                    device,
		const VkDescriptorPoolCreateInfo*           pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDescriptorPool*                           pDescriptorPool) {
	auto& dev = getDevice(device);

	auto res = dev.dispatch.CreateDescriptorPool(dev.handle, pCreateInfo, pAllocator, pDescriptorPool);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto dsPoolPtr = IntrusivePtr<DescriptorPool>(new DescriptorPool());
	auto& dsPool = *dsPoolPtr;
	dsPool.dev = &dev;
	dsPool.handle = *pDescriptorPool;
	dsPool.maxSets = pCreateInfo->maxSets;
	dsPool.poolSizes = {pCreateInfo->pPoolSizes, pCreateInfo->pPoolSizes + pCreateInfo->poolSizeCount};
	dsPool.flags = pCreateInfo->flags;

	// init descriptor entries
	dsPool.entries = std::make_unique<DescriptorPool::SetEntry[]>(dsPool.maxSets);
	initResetPoolEntries(dsPool);

	// init descriptor data
	dsPool.dataSize = dsPool.maxSets * sizeof(DescriptorSet);
	for(auto& pool : dsPool.poolSizes) {
		dsPool.dataSize += descriptorSize(pool.type) * pool.descriptorCount;
	}

	// NOTE: no idea why this was needed for the nvidia rtxgi samples.
	// Maybe we don't calculate the size correctly in some edge cases?
	// constexpr auto overAllocFac = 4u;
	// dsPool.dataSize *= overAllocFac;
	// dsPool.dataSize = std::max<u32>(dsPool.dataSize, 1024 * 1024 * 64);

	dsPool.data = std::make_unique<std::byte[]>(dsPool.dataSize);
	debugStatAdd(DebugStats::get().descriptorPoolMem, dsPool.dataSize);
	TracyAlloc(dsPool.data.get(), dsPool.dataSize);

	*pDescriptorPool = castDispatch<VkDescriptorPool>(dsPool);
	dev.dsPools.mustEmplace(*pDescriptorPool, std::move(dsPoolPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool(
		VkDevice                                    device,
		VkDescriptorPool                            descriptorPool,
		const VkAllocationCallbacks*                pAllocator) {
	if(!descriptorPool) {
		return;
	}

	auto& dev = *mustMoveUnset(device, descriptorPool)->dev;
	dev.dispatch.DestroyDescriptorPool(dev.handle, descriptorPool, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetDescriptorPool(
		VkDevice                                    device,
		VkDescriptorPool                            descriptorPool,
		VkDescriptorPoolResetFlags                  flags) {
	ZoneScoped;

	auto& dsPool = get(device, descriptorPool);
	auto& dev = *dsPool.dev;

	for(auto it = dsPool.usedEntries; it; it = it->next) {
		dlg_assert(it->set);
		destroy(*it->set, false);
	}

	initResetPoolEntries(dsPool);

	{
		ZoneScopedN("dispatch");
		return dev.dispatch.ResetDescriptorPool(dev.handle, dsPool.handle, flags);
	}
}

VkResult findEntry(DescriptorPool& pool, u32 memSize,
		std::byte*& data, DescriptorPool::SetEntry*& setEntry) {
	// note that this mutex is only important to sync with other threads
	// that access entries, e.g. to check whether a descriptor set reference
	// in a descriptor snapshot is still valid.
	// Applications can't never actually allocate/free from multiple threads
	// at the same time.
	auto lock = std::scoped_lock(pool.mutex);

	if(!pool.freeEntries) {
		// It's valid for applications to "just try" for newer api versions.
		// Seems to be what dota does. When an application uses api 1.0 this
		// is still valid behavior for us to do as behavior of overallocation was
		// undefined back then.
		return VK_ERROR_OUT_OF_POOL_MEMORY;
	}

	auto highestOffset = 0u;
	if(pool.highestEntry) {
		highestOffset = pool.highestEntry->offset + pool.highestEntry->size;
	}

	if(highestOffset + memSize <= pool.dataSize) {
		dlg_assert(pool.freeEntries);
		auto& entry = *pool.freeEntries;
		pool.freeEntries = entry.next;

		if(pool.highestEntry) {
			pool.highestEntry->next = &entry;
		} else {
			pool.usedEntries = &entry;
		}

		entry.offset = highestOffset;
		entry.size = memSize;
		entry.next = nullptr;
		entry.prev = pool.highestEntry;

		pool.highestEntry = &entry;
		setEntry = &entry;
		data = &pool.data[entry.offset];
	} else {
		ZoneScopedN("findData - fragmented");

		// otherwise we can't get fragmentation at all
		dlg_assert(pool.flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);

		auto offset = 0u;
		auto it = pool.lastEntry;

		// Try to use position of last allocation
		if(it) {
			auto nextOff = pool.dataSize;
			if(it->next) {
				nextOff = it->next->offset;
			}

			offset = it->offset + it->size;
			auto fits = (offset + memSize <= nextOff);
			it = (fits) ? it->next : nullptr;
		}

		// Start search at 0
		if(!it) {
			offset = 0u;
			it = pool.usedEntries;

			// skip special fragmented entries
			// PERF: this makes fragmented entries extra hurtful.
			// We could probably append them at the end instead with
			// some additional trickery so we don't have to do this here.
			while(it && it->offset == u32(-1)) {
				it = it->next;
			}

			while(it) {
				auto& entry = *it;
				if(offset + memSize <= it->offset) {
					break;
				}
				offset = entry.offset + entry.size;
				it = it->next;
			}
		}

		if(offset + memSize > pool.dataSize) {
			if constexpr(enableDsFragmentationPath) {
				dlg_assert(pool.freeEntries);
				auto& entry = *pool.freeEntries;
				pool.freeEntries = entry.next;
				setEntry = &entry;

				dlg_assert(!it);
				dlg_warn("Fragmentation of descriptor pool detected. Slow path");
				data = new std::byte[memSize];

				// dummy setEntry so we can put it into our linked list
				entry.offset = u32(-1);
				entry.size = u32(-1);
				entry.next = pool.usedEntries;
				entry.prev = nullptr;
				pool.usedEntries = &entry;
			} else {
				dlg_trace("returning fragmented pool");
				return VK_ERROR_FRAGMENTED_POOL;
			}
		} else {
			// reserve entry
			dlg_assert(pool.freeEntries);
			auto& entry = *pool.freeEntries;
			pool.freeEntries = entry.next;
			setEntry = &entry;

			// it == null can't happen, we should have landed
			// in some earlier branch.
			dlg_assert(it);
			dlg_assert(pool.usedEntries && pool.highestEntry);

			entry.offset = offset;
			entry.size = memSize;

			// insert entry before 'it'
			entry.prev = it->prev;
			entry.next = it;

			dlg_assert(!it->prev == (it == pool.usedEntries));
			if(it->prev) {
				it->prev->next = &entry;
			} else {
				pool.usedEntries = &entry;
			}

			it->prev = &entry;

			pool.lastEntry = &entry;
			data = &pool.data[offset];
		}
	}

	return VK_SUCCESS;
}

VkResult initDescriptorSet(Device& dev, DescriptorPool& pool, VkDescriptorSet& handle,
		IntrusivePtr<DescriptorSetLayout> layoutPtr, u32 varCount,
		DescriptorSet*& out) {
	ZoneScopedN("initDescriptorSet");

	// find data
	auto memSize = sizeof(DescriptorSet);
	memSize += align(totalDescriptorMemSize(*layoutPtr, varCount), sizeof(void*));

	// try to find a free setEntry object and space in the memory block
	DescriptorPool::SetEntry* setEntry {};
	std::byte* data {};
	auto res = findEntry(pool, memSize, data, setEntry);
	if(res != VK_SUCCESS) {
		dlg_trace("could not find free entry");
		return res;
	}

	dlg_assert(data);
	dlg_assert(setEntry);
	dlg_assert(std::uintptr_t(data) % sizeof(void*) == 0u);

	auto& ds = *new(data) DescriptorSet();
	ds.setEntry = setEntry;
	ds.handle = handle;
	ds.layout = std::move(layoutPtr);
	ds.variableDescriptorCount = varCount;
	ds.pool = &pool;
	ds.id = ++pool.lastID;
	setEntry->set = &ds;

	initDescriptorState(bindingData(ds), *ds.layout, ds.variableDescriptorCount);
	handle = castDispatch<VkDescriptorSet>(ds);

	if(!HandleDesc<VkDescriptorSet>::wrap) {
		dev.descriptorSets.mustEmplace(handle, &ds);
	}

	if(ds.layout->immutableSamplers) {
		initImmutableSamplers(ds);
	}

	out = &ds;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(
		VkDevice                                    device,
		const VkDescriptorSetAllocateInfo*          pAllocateInfo,
		VkDescriptorSet*                            pDescriptorSets) {
	ZoneScoped;

	auto& pool = get(device, pAllocateInfo->descriptorPool);
	auto& dev = *pool.dev;
	auto count = pAllocateInfo->descriptorSetCount;

	auto nci = *pAllocateInfo;
	nci.descriptorPool = pool.handle;

	ThreadMemScope memScope;
	auto dsLayouts = memScope.allocUndef<VkDescriptorSetLayout>(count);
	for(auto i = 0u; i < count; ++i) {
		dsLayouts[i] = get(dev, pAllocateInfo->pSetLayouts[i]).handle;
	}

	nci.pSetLayouts = dsLayouts.data();

	{
		ZoneScopedN("dispatch");
		auto res = dev.dispatch.AllocateDescriptorSets(dev.handle, &nci, pDescriptorSets);
		if(res != VK_SUCCESS) {
			return res;
		}
	}

	auto* variableCountInfo = findChainInfo<VkDescriptorSetVariableDescriptorCountAllocateInfo,
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO>(*pAllocateInfo);
	if (variableCountInfo && variableCountInfo->descriptorSetCount == 0u) {
		variableCountInfo = nullptr;
	}
	dlg_assert(!variableCountInfo ||
		variableCountInfo->descriptorSetCount == pAllocateInfo->descriptorSetCount);

	auto dss = memScope.alloc<DescriptorSet*>(count);
	VkResult res = VK_SUCCESS;

	auto i = 0u;
	for(i = 0u; i < count; ++i) {
		auto layoutPtr = getPtr(dev, pAllocateInfo->pSetLayouts[i]);
		auto& layout = *layoutPtr;

		// per spec variable counts are zero by default, if no other value is provided
		auto varCount = u32(0);
		if(variableCountInfo && !layout.bindings.empty() &&
				layout.bindings.back().flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
			varCount = variableCountInfo->pDescriptorCounts[i];
		}

		res = initDescriptorSet(dev, pool, pDescriptorSets[i], std::move(layoutPtr), varCount, dss[i]);
		if(res != VK_SUCCESS) {
			break;
		}
	}

	if(res != VK_SUCCESS) {
		for(auto j = 0u; j < i; ++j) {
			destroy(*dss[i], true);
		}
		dev.dispatch.FreeDescriptorSets(dev.handle, pool.handle,
			count, pDescriptorSets);
		memset(pDescriptorSets, 0x0, sizeof(pDescriptorSets[0]) * count);
		return res;
	}

	debugStatAdd(DebugStats::get().aliveDescriptorSets, count);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL FreeDescriptorSets(
		VkDevice                                    device,
		VkDescriptorPool                            descriptorPool,
		uint32_t                                    descriptorSetCount,
		const VkDescriptorSet*                      pDescriptorSets) {
	ZoneScoped;

	auto& pool = get(device, descriptorPool);
	auto& dev = *pool.dev;

	ThreadMemScope memScope;
	auto handles = memScope.alloc<VkDescriptorSet>(descriptorSetCount);

	for(auto i = 0u; i < descriptorSetCount; ++i) {
		if(!pDescriptorSets[i]) {
			continue;
		}

		auto& ds = get(dev, pDescriptorSets[i]);
		handles[i] = ds.handle;
		destroy(ds, true);
	}

	{
		ZoneScopedN("dispatch");
		return dev.dispatch.FreeDescriptorSets(dev.handle, pool.handle,
			u32(handles.size()), handles.data());
	}
}

void update(DescriptorSet& state, unsigned bind, unsigned elem,
		VkBufferView& handle) {
	auto& binding = bufferViews(state, bind)[elem];

	BufferView* newView {};
	if(handle) VIL_LIKELY {
		newView = &get(*state.layout->dev, handle);
		handle = newView->handle;
	}

	if(refBindings) {
		if(binding.bufferView) {
			decRefCount(*binding.bufferView);
		}
		if(newView) {
			incRefCount(*newView);
		}
	}

	binding.bufferView = newView;
}

void update(DescriptorSet& state, unsigned bind, unsigned elem,
		VkDescriptorImageInfo& img) {
	auto& dev = *state.layout->dev;

	auto& binding = images(state, bind)[elem];
	binding.layout = img.imageLayout;

	auto& layout = state.layout->bindings[bind];

	// update imageView, if needed
	if(needsImageView(layout.descriptorType)) {
		ImageView* newView {};
		if(img.imageView) VIL_LIKELY { // can be VK_NULL_HANDLE
			newView = &get(dev, img.imageView);
			img.imageView = newView->handle;
		}

		if(refBindings) {
			if(binding.imageView) {
				decRefCount(*binding.imageView);
			}
			if(newView) {
				incRefCount(*newView);
			}
		}

		binding.imageView = newView;
	}

	// update sampler, if needed
	if(needsSampler(layout.descriptorType)) {
		if(layout.immutableSamplers) {
			// immutable samplers are initialized at the beginning and
			// never unset.
			dlg_assert(binding.sampler);
			dlg_assert(binding.sampler == layout.immutableSamplers[elem].get());
		} else {
			Sampler* newSampler {};
			if(img.sampler) VIL_LIKELY { // can be VK_NULL_HANDLE
				newSampler = &get(dev, img.sampler);
				img.sampler = newSampler->handle;
			}

			if(refBindings) {
				if(binding.sampler) {
					decRefCount(*binding.sampler);
				}
				if(newSampler) {
					incRefCount(*newSampler);
				}
			}

			binding.sampler = newSampler;
		}
	}
}

void update(DescriptorSet& state, unsigned bind, unsigned elem,
		VkDescriptorBufferInfo& info) {
	auto& binding = buffers(state, bind)[elem];
	Buffer* newBuffer {};

	if(info.buffer) VIL_LIKELY { // can be VK_NULL_HANDLE
		newBuffer = &get(*state.layout->dev, info.buffer);
		info.buffer = newBuffer->handle;
	}

	if(refBindings) {
		if(binding.buffer) {
			decRefCount(*binding.buffer);
		}
		if(newBuffer) {
			incRefCount(*newBuffer);
		}
	}

	binding.buffer = newBuffer;
	binding.offset = info.offset;

	if(binding.buffer) {
		binding.range = evalRange(binding.buffer->ci.size, info.offset, info.range);
	} else {
		binding.range = info.range;
	}
}

void update(DescriptorSet& state, unsigned bind, unsigned elem,
		VkAccelerationStructureKHR& handle) {
	auto& binding = accelStructs(state, bind)[elem];
	AccelStruct* newAS {};

	if(handle) VIL_LIKELY { // can be VK_NULL_HANDLE
		newAS = &get(*state.layout->dev, handle);
		handle = newAS->handle;
	}

	if(refBindings) {
		if(binding.accelStruct) {
			decRefCount(*binding.accelStruct);
		}
		if(newAS) {
			incRefCount(*newAS);
		}
	}

	binding.accelStruct = newAS;
}

void update(DescriptorSet& state, unsigned bind, unsigned offset,
		std::byte src) {
	auto buf = inlineUniformBlock(state, bind);
	dlg_assert(offset < buf.size());

	// std::lock_guard lock(state.mutex);
	// TODO: updating uniform inline blocks byte-by-byte is inefficient
	// but reworking this to be more efficient would be complicated.
	// Especially so since we still have to consider that additional bytes
	// will update the next descriptor.
	buf[offset] = src;
}

void advanceUntilValid(DescriptorSet& state, unsigned& binding, unsigned& elem) {
	dlg_assert(binding < state.layout->bindings.size());
	auto count = descriptorCount(state, binding);
	while(elem >= count) {
		++binding;
		elem = 0u;
		dlg_assert(binding < state.layout->bindings.size());
		count = descriptorCount(state, binding);
	}
}

// NOTE: in UpdateDescriptorSets(WithTemplate), we don't invalidate
// command records even more, even though it would be needed in most
// cases (excluding update_after_bind stuff) but we don't need that
// information and can save some work this way.
// DescriptorSets

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(
		VkDevice                                    device,
		uint32_t                                    descriptorWriteCount,
		const VkWriteDescriptorSet*                 pDescriptorWrites,
		uint32_t                                    descriptorCopyCount,
		const VkCopyDescriptorSet*                  pDescriptorCopies) {
	ZoneScoped;
	auto& dev = getDevice(device);

	// handle writes
	auto totalWriteCount = 0u;
	for(auto i = 0u; i < descriptorWriteCount; ++i) {
		totalWriteCount += pDescriptorWrites[i].descriptorCount;
	}

	ThreadMemScope memScope;

	auto writes = memScope.allocUndef<VkWriteDescriptorSet>(descriptorWriteCount);
	auto imageInfos = memScope.allocUndef<VkDescriptorImageInfo>(totalWriteCount);
	auto bufferInfos = memScope.allocUndef<VkDescriptorBufferInfo>(totalWriteCount);
	auto bufferViews = memScope.allocUndef<VkBufferView>(totalWriteCount);
	auto accelStructs = memScope.allocUndef<VkAccelerationStructureKHR>(totalWriteCount);

	auto writeOff = 0u;
	for(auto i = 0u; i < descriptorWriteCount; ++i) {
		auto& write = pDescriptorWrites[i];
		dlg_assert(write.descriptorCount > 0u); // per vulkan spec

		auto& ds = get(dev, write.dstSet);
		dlg_assert(ds.handle);
		dlg_assert(ds.layout);

		writes[i] = write;
		writes[i].dstSet = ds.handle;

		auto dstBinding = write.dstBinding;
		auto dstElem = write.dstArrayElement;

		auto* chainCopy = copyChainLocal(memScope, write.pNext);
		auto* accelStructWrite = (VkWriteDescriptorSetAccelerationStructureKHR*) findChainInfo2<
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR>(chainCopy);
		auto* inlineUniformWrite = (VkWriteDescriptorSetInlineUniformBlockEXT*) findChainInfo2<
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK_EXT>(chainCopy);

		// NOTE: we need this lock here since, technically, the ds could
		// be accessed by another thread during the update e.g. if the ds has
		// UPDATE_UNUSED_WHILE_PENDING.
		// Since we hold the lock here, we have to guarantee that we don't
		// acquire another mutex higher in the locking hierachy.
		// That's why we need all handles being written to descriptorSets
		// to be wrapped, so we don't have to lock the device mutex to
		// access the maps.
		auto lock = ds.checkResolveCow();

		for(auto j = 0u; j < write.descriptorCount; ++j, ++dstElem) {
			advanceUntilValid(ds, dstBinding, dstElem);
			dlg_assert(dstBinding < ds.layout->bindings.size());
			auto& layout = ds.layout->bindings[dstBinding];
			dlg_assert(write.descriptorType == layout.descriptorType);

			switch(category(write.descriptorType)) {
				case DescriptorCategory::image: {
					dlg_assert(write.pImageInfo);
					auto& info = imageInfos[writeOff + j];
					info = write.pImageInfo[j];
					update(ds, dstBinding, dstElem, info);
					break;
				} case DescriptorCategory::buffer: {
					dlg_assert(write.pBufferInfo);
					auto& info = bufferInfos[writeOff + j];
					info = write.pBufferInfo[j];
					update(ds, dstBinding, dstElem, info);
					break;
				} case DescriptorCategory::bufferView: {
					dlg_assert(write.pTexelBufferView);
					auto& info = bufferViews[writeOff + j];
					info = write.pTexelBufferView[j];
					update(ds, dstBinding, dstElem, info);
					break;
				} case DescriptorCategory::accelStruct: {
					dlg_assert(accelStructWrite);
					dlg_assert(j < accelStructWrite->accelerationStructureCount);
					auto& info = accelStructs[writeOff + j];
					info = accelStructWrite->pAccelerationStructures[j];
					update(ds, dstBinding, dstElem, info);
					break;
				} case DescriptorCategory::inlineUniformBlock: {
					dlg_assert(inlineUniformWrite);
					dlg_assert(j < inlineUniformWrite->dataSize);
					auto ptr = reinterpret_cast<const std::byte*>(inlineUniformWrite->pData);
					update(ds, dstBinding, dstElem, ptr[j]);
					break;
				} case DescriptorCategory::none:
					dlg_error("unreachable: Invalid descriptor type");
					break;
			}
		}

		writes[i].pImageInfo = imageInfos.data() + writeOff;
		writes[i].pBufferInfo = bufferInfos.data() + writeOff;
		writes[i].pTexelBufferView = bufferViews.data() + writeOff;

		if(accelStructWrite) {
			dlg_assert(category(write.descriptorType) == DescriptorCategory::accelStruct);
			accelStructWrite->pAccelerationStructures = accelStructs.data() + writeOff;
			writes[i].pNext = chainCopy;
		}

		writeOff += writes[i].descriptorCount;
	}

	// handle copies
	auto copies = memScope.alloc<VkCopyDescriptorSet>(descriptorCopyCount);

	for(auto i = 0u; i < descriptorCopyCount; ++i) {
		auto& copyInfo = pDescriptorCopies[i];
		auto& src = get(dev, copyInfo.srcSet);
		auto& dst = get(dev, copyInfo.dstSet);

		copies[i] = copyInfo;
		copies[i].srcSet = src.handle;
		copies[i].dstSet = dst.handle;

		auto dstBinding = copyInfo.dstBinding;
		auto dstElem = copyInfo.dstArrayElement;
		auto srcBinding = copyInfo.srcBinding;
		auto srcElem = copyInfo.srcArrayElement;

		auto lock = dst.checkResolveCow();

		for(auto j = 0u; j < copyInfo.descriptorCount; ++j, ++srcElem, ++dstElem) {
			advanceUntilValid(dst, dstBinding, dstElem);
			advanceUntilValid(src, srcBinding, srcElem);
			copy(dst, dstBinding, dstElem, src, srcBinding, srcElem);
		}
	}

	{
		ZoneScopedN("dispatch");
		return dev.dispatch.UpdateDescriptorSets(dev.handle,
			u32(writes.size()), writes.data(),
			u32(copies.size()), copies.data());
	}
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(
		VkDevice                                    device,
		const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDescriptorUpdateTemplate*                 pDescriptorUpdateTemplate) {
	// NOTE: we don't use host allocators here since this handle is potentially
	// kept alive inside the layer, preventing us from passing an application
	// allocator to the destruction function.
	// See design.md on allocators.
	(void) pAllocator;

	auto& dsLayout = get(device, pCreateInfo->descriptorSetLayout);
	auto& dev = *dsLayout.dev;
	auto& pipeLayout = get(dev, pCreateInfo->pipelineLayout);

	auto nci = *pCreateInfo;
	nci.descriptorSetLayout = dsLayout.handle;
	nci.pipelineLayout = pipeLayout.handle;

	auto res = dev.dispatch.CreateDescriptorUpdateTemplate(dev.handle, &nci,
		nullptr, pDescriptorUpdateTemplate);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto dutPtr = IntrusivePtr<DescriptorUpdateTemplate>(new DescriptorUpdateTemplate());
	auto& dut = *dutPtr;
	dut.dev = &dev;
	dut.handle = *pDescriptorUpdateTemplate;

	dut.entries = {
		pCreateInfo->pDescriptorUpdateEntries,
		pCreateInfo->pDescriptorUpdateEntries + pCreateInfo->descriptorUpdateEntryCount
	};

	*pDescriptorUpdateTemplate = castDispatch<VkDescriptorUpdateTemplate>(dut);
	dev.dsuTemplates.mustEmplace(*pDescriptorUpdateTemplate, std::move(dutPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate(
		VkDevice                                    device,
		VkDescriptorUpdateTemplate                  descriptorUpdateTemplate,
		const VkAllocationCallbacks*                pAllocator) {
	if(!descriptorUpdateTemplate) {
		return;
	}

	auto& dev = getDevice(device);
	dev.dsuTemplates.mustErase(descriptorUpdateTemplate);

	// Don't destroy it here, handle has shared ownership, see e.g.
	// the dsuTemplates hash map in Device for justification
	(void) pAllocator;
}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplate(
		VkDevice                                    device,
		VkDescriptorSet                             descriptorSet,
		VkDescriptorUpdateTemplate                  descriptorUpdateTemplate,
		const void*                                 pData) {
	ZoneScoped;

	auto& dut = get(device, descriptorUpdateTemplate);
	auto& dev = *dut.dev;
	auto& ds  = get(dev, descriptorSet);

	// NOTE: we need this lock here since, technically, the ds could
	// be accessed by another thread during the update e.g. if the ds has
	// UPDATE_UNUSED_WHILE_PENDING.
	// Since we hold the lock here, we have to guarantee that we don't
	// acquire another mutex higher in the locking hierachy.
	// That's why we need all handles being written to descriptorSets
	// to be wrapped, so we don't have to lock the device mutex to
	// access the maps.
	auto lock = ds.checkResolveCow();

	ThreadMemScope memScope;
	std::byte* ptr;

	// Our implementation has massive overhead compared to
	// the driver on most platforms. One of the reasons is probably
	// the copying here. We could (via env variable; off by default) just
	// const_cast pData and then directly write into it. Should help
	// a lot. Applications using this function likely have a whole lot of
	// data to transmit.
	// TODO: using the modify path is UB. But i'd expect it to work
	// with most applications. It crashed with dota though. Are they
	// *really* relying on pData content staying the same?
	// hard to imagine such an update logic tbh.
	constexpr auto modify = false;
	if(!modify) {
		auto totalSize = totalUpdateDataSize(dut);
		auto fwdData = memScope.allocUndef<std::byte>(totalSize);
		std::memcpy(fwdData.data(), pData, totalSize);
		ptr = fwdData.data();
	} else {
		// UNHOLY
		ptr = (std::byte*) pData;
	}

	for(auto& entry : dut.entries) {
		auto dstBinding = entry.dstBinding;
		auto dstElem = entry.dstArrayElement;
		auto dsType = ds.layout->bindings[dstBinding].descriptorType;

		for(auto j = 0u; j < entry.descriptorCount; ++j, ++dstElem) {
			// PERF Could we maybe determine this statically
			// in CreateDescriptorUpdateTemplate?
			advanceUntilValid(ds, dstBinding, dstElem);

			// TODO: such an assertion here would be nice. Track used
			// layout in update?
			// dlg_assert(write.descriptorType == type);

			auto* data = ptr + (entry.offset + j * entry.stride);

			// TODO: the reinterpret_cast here is UB in C++ I guess.
			// Assuming the caller did it correctly (really creating
			// the objects e.g. via placement new) we could probably also
			// do it correctly by using placement new (copy) into 'fwdData'
			// instead of the memcpy above.
			switch(category(dsType)) {
				case DescriptorCategory::image: {
					auto& img = *reinterpret_cast<VkDescriptorImageInfo*>(data);
					update(ds, dstBinding, dstElem, img);
					break;
				} case DescriptorCategory::buffer: {
					auto& buf = *reinterpret_cast<VkDescriptorBufferInfo*>(data);
					update(ds, dstBinding, dstElem, buf);
					break;
				} case DescriptorCategory::bufferView: {
					auto& bufView = *reinterpret_cast<VkBufferView*>(data);
					update(ds, dstBinding, dstElem, bufView);
					break;
				} case DescriptorCategory::accelStruct: {
					auto& accelStruct = *reinterpret_cast<VkAccelerationStructureKHR*>(data);
					update(ds, dstBinding, dstElem, accelStruct);
					break;
				} case DescriptorCategory::inlineUniformBlock: {
					auto ptr = reinterpret_cast<const std::byte*>(data);
					update(ds, dstBinding, dstElem, *ptr);
					break;
				} case DescriptorCategory::none:
					dlg_error("Invalid/unknown descriptor type");
					break;
			}
		}
	}

	{
		ZoneScopedN("dispatchUpdateDescriptorSetWithTemplate");
		dev.dispatch.UpdateDescriptorSetWithTemplate(dev.handle, ds.handle,
			dut.handle, static_cast<const void*>(ptr));
	}
}

u32 totalUpdateDataSize(const DescriptorUpdateTemplate& dut) {
	ZoneScopedN("totalUpdateDataSize");

	u32 ret = 0u;
	for(auto& entry : dut.entries) {
		auto stride = entry.stride;
		auto size = 0u;
		switch(category(entry.descriptorType)) {
			case DescriptorCategory::image:
				size = sizeof(VkDescriptorImageInfo);
				break;
			case DescriptorCategory::buffer:
				size = sizeof(VkDescriptorBufferInfo);
				break;
			case DescriptorCategory::bufferView:
				size = sizeof(VkBufferView);
				break;
			case DescriptorCategory::accelStruct:
				size = sizeof(VkAccelerationStructureKHR);
				break;
			case DescriptorCategory::inlineUniformBlock:
				// this is a special case defined in VK_EXT_inline_uniform_block.
				// entry.stride should be ignored and 1 used.
				stride = 1u;
				size = 1u;
				break;
			case DescriptorCategory::none:
				dlg_error("unreachable: Invalid/unknown descriptor type");
				break;
		}

		auto off = u32(entry.offset + entry.descriptorCount * stride);
		off += size;

		ret = std::max<u32>(ret, off);
	}

	return ret;
}

DescriptorSetCow::~DescriptorSetCow() {
	dlg_assert(refCount.load() == 0u);

	// Must have been disconnected from the ds since we can't unregister
	// ourselves here due to threading/deadlock issues.
	// Either this happend by being resolved (i.e. this->copy holds
	// a valid state) or by the DescriptorSet noticing in checkResolveCow
	// that nobody is interested in the cow anymore and deleting it
	// explicitly (in which case it also sets ds = nullptr)
	dlg_assert(!ds);
}

std::pair<DescriptorStateRef, std::unique_lock<DebugMutex>> access(DescriptorSetCow& cow) {
	std::unique_lock cowLock(cow.mutex);
	if(cow.copy) {
		cowLock.unlock();
		return {DescriptorStateRef(*cow.copy), std::move(cowLock)};
	}

	dlg_assert(cow.ds);
	// TODO: private now. Would be nice to have this assert tho
	// dlg_assert(cow.ds->cow == &cow);

	// NOTE how we don't have to lock cow.obj->mutex to access the object
	// state itself here. We know that while
	// cow.ds, and therefore cow.ds->cow, are set, cow.ds->state
	// is immutable. All functions that change it must first call checkResolveCow.
	return {DescriptorStateRef(*cow.ds), std::move(cowLock)};
}

bool hasBound(DescriptorStateRef state, const Handle& handle) {
	for(auto i = 0u; i < state.layout->bindings.size(); ++i) {
		switch(category(state.layout->bindings[i].descriptorType)) {
			case DescriptorCategory::accelStruct:
				for(auto& accelStruct : accelStructs(state, i)) {
					if(accelStruct.accelStruct == &handle) {
						return true;
					}
				}
				break;
			case DescriptorCategory::buffer:
				for(auto& buffer : buffers(state, i)) {
					if(buffer.buffer == &handle) {
						return true;
					}
				}
				break;
			case DescriptorCategory::image:
				for(auto& img : images(state, i)) {
					if(img.imageView == &handle || (img.imageView && img.imageView->img == &handle)) {
						return true;
					}
				}
				break;
			case DescriptorCategory::bufferView:
				for(auto& bv : bufferViews(state, i)) {
					if(bv.bufferView == &handle || (bv.bufferView && bv.bufferView->buffer == &handle)) {
						return true;
					}
				}
				break;
			case DescriptorCategory::inlineUniformBlock:
				// nope
				break;
			case DescriptorCategory::none:
				dlg_error("unreachable");
				break;
		}
	}

	return false;
}

// only implemented for sampler unwrapping
VKAPI_ATTR void VKAPI_CALL GetDescriptorSetLayoutSupport(
		VkDevice                                    device,
		const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
		VkDescriptorSetLayoutSupport*               pSupport) {
	auto& dev = getDevice(device);

	// unwrap immutable sampler handles
	auto nci = *pCreateInfo;

	ThreadMemScope memScope;
	auto nbindings = memScope.copy(nci.pBindings, nci.bindingCount);
	nci.pBindings = nbindings.data();

	for(auto& bind : nbindings) {
		if(!needsSampler(bind.descriptorType) || bind.descriptorCount == 0 ||
				!bind.pImmutableSamplers) {
			continue;
		}

		auto handles = memScope.alloc<VkSampler>(bind.descriptorCount);
		for(auto i = 0u; i < bind.descriptorCount; ++i) {
			auto& sampler = get(dev, bind.pImmutableSamplers[i]);
			handles[i] = sampler.handle;
		}

		bind.pImmutableSamplers = handles.data();
	}

	dev.dispatch.GetDescriptorSetLayoutSupport(dev.handle, &nci, pSupport);
}

} // namespace vil
