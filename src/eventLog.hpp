#pragma once

#include <fwd.hpp>
#include <vk/vulkan.h>
#include <frame.hpp>
#include <vector>
#include <chrono>
#include <mutex>
#include <array>

#ifdef VIL_WITH_CALLSTACKS
	#define VIL_EVENT_CALLSTACKS
#endif // VIL_WITH_CALLSTACKS

namespace vil {

enum class EventType {
	resourceCreated,
	resourceDestroyed,
	memoryBind,
	queueSubmit,
	queuePresent,
	queueBindSparse,
	queueWait,
	deviceWait,
	fenceWait,
};

struct LoggedEvent {
	using Clock = std::chrono::steady_clock;

	EventType type;
	Clock::time_point time;

#ifdef VIL_EVENT_CALLSTACKS
	std::array<void*, 16> stacktrace {};
#endif // VIL_COMMAND_CALLSTACKS
};

struct CreationEventBase : LoggedEvent {
	CreationEventBase() { this->type = EventType::resourceCreated; }
	VkObjectType objectType;
};

struct DestructionEventBase : LoggedEvent {
	DestructionEventBase() { this->type = EventType::resourceDestroyed; }
	VkObjectType objectType;
};

template<typename T>
struct CreationEvent : CreationEventBase {
	CreationEvent() { this->objectType = T::objectType; }
	IntrusivePtr<T> handle;
};

template<typename T>
struct DestructionEvent : DestructionEventBase {
	DestructionEvent() { this->objectType = T::objectType; }
	IntrusivePtr<T> handle;
};

struct QueueSubmitEvent : LoggedEvent {
	QueueSubmitEvent() { this->type = EventType::queueSubmit; }
	FrameSubmission submission;
};

struct QueueWaitEvent : LoggedEvent {
	QueueWaitEvent() { this->type = EventType::queueWait; }
};

struct QueuePresentEvent : LoggedEvent {
	QueuePresentEvent() { this->type = EventType::queuePresent; }
};

struct QueueBindSparseEvent : LoggedEvent {
	QueueBindSparseEvent() { this->type = EventType::queueBindSparse; }
	// IntrusivePtr<SparseBind> ?
};

struct DeviceWaitEvent : LoggedEvent {
	DeviceWaitEvent() { this->type = EventType::deviceWait; }
};

struct FenceWaitEvent : LoggedEvent {
	FenceWaitEvent() { this->type = EventType::fenceWait; }
};

struct MemoryBindEvent : LoggedEvent {
	// TODO
};

struct EventLog {
	using Clock = LoggedEvent::Clock;

	// TODO: performance! Don't allocate per event.
	std::vector<std::unique_ptr<LoggedEvent>> events;
	std::mutex mutex;

	template<typename T, typename... Args>
	T& construct(Args&... args) {
		auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
		auto& ret = *ptr;

		ret.time = Clock::now();

		// TODO: don't call every time. Performance.
		// Only in gui isn't enough either though (think of e.g.
		// gui not shown). Not sure when it is proper.
		// maybe only in expensive cases like queueSubmit?
		if(std::is_same_v<T, QueueSubmitEvent>) {
			// clear();
		}

		// TODO: set stacktrace
		// but move that to cpp part.

		{
			std::lock_guard lock(mutex);
			events.push_back(std::move(ptr));
		}

		return ret;
	}

	void clear() {
		auto now = Clock::now();
		auto lifetime = std::chrono::seconds(50);
		auto start = now - lifetime;

		// make sure to destruct objects outside lock
		std::vector<std::unique_ptr<LoggedEvent>> keepAlive;

		{
			std::lock_guard lock(mutex);

			auto it = events.begin();
			while(it != events.end() && it->get()->time < start) {
				++it;
			}

			if(it != events.begin()) {
				keepAlive.insert(keepAlive.begin(),
					std::make_move_iterator(events.begin()),
					std::make_move_iterator(it));
				events.erase(events.begin(), it);
			}
		}
	}
};

} // namespace vil
