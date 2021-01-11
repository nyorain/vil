#include <win32.hpp>
#include <swa/winapi.h>
#include <vk/vulkan.h>
#include <device.hpp>
#include <layer.hpp>
#include <platform.hpp>

#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>

namespace fuen {

struct Win32Platform : SwaPlatform {
	HWND surfaceWindow {};

	std::thread thread;
	mutable std::mutex mutex;
	mutable std::condition_variable cv;
	mutable std::atomic<i64> doStuff;
	mutable Gui* gui;
	std::atomic<bool> ret;

	void init(Device& dev, unsigned width, unsigned height) override;
	bool pressed(u32 key) const override;
	bool update(Gui& gui) override;
	~Win32Platform();

	bool checkPressed(u32 key) const;
	void uiThread(Device& dev, u32 width, u32 height);
};

Win32Platform::~Win32Platform() {
	if(thread.joinable()) {
		doStuff.store(-1);
		cv.notify_one();
		thread.join();
	}
}

bool Win32Platform::checkPressed(u32 key) const {
	auto keycode = [&]() -> unsigned {
		switch(key) {
			case swa_key_backslash:	return VK_OEM_5;
			case swa_key_f: return 'F';
			default: dlg_error("Unexpected key"); return 0;
		}
	}();

	return GetAsyncKeyState(keycode) < 0;
}

void Win32Platform::uiThread(Device& dev, u32 width, u32 height) {
	std::unique_lock lock(mutex);

	dpy = swa_display_winapi_create("fuencaliente");
	if(!dpy) {
		throw std::runtime_error("Can't create window");
	}

	SwaPlatform::initWindow(dev, this->surfaceWindow, width, height);
	cv.notify_one();

	while(true) {
		while(doStuff.load() == 0) {
			cv.wait(lock);
		}

		auto stuff = doStuff.load();
		doStuff.store(0);

		if(stuff > 0) {
			u32 key = u32(stuff);
			cv.notify_one();
			ret.store(checkPressed(key));
		} else if(stuff == -1) {
			break;
		} else {
			if(status == Status::focused) {
				SetCapture(NULL);
				ClipCursor(NULL);
				// while(ShowCursor(TRUE) <= 0);
			}

			// POINT point;
			// ::GetCursorPos(&point);
			// dlg_trace("cursor pos {} {}", point.x, point.y);

			ret.store(SwaPlatform::update(*this->gui));
			cv.notify_one();
		}
	}
}

void Win32Platform::init(Device& dev, unsigned width, unsigned height) {
	std::unique_lock lock(mutex);
	thread = std::thread([&]{ uiThread(dev, width, height); });
	cv.wait(lock);
}

bool Win32Platform::pressed(u32 key) const {
	if(std::this_thread::get_id() == this->thread.get_id()) {
		return checkPressed(key);
	}

	std::unique_lock lock(mutex);
	doStuff.store(i64(key));
	cv.notify_one();
	cv.wait(lock);
	return ret.load();
}

bool Win32Platform::update(Gui& gui) {
	std::unique_lock lock(mutex);
	this->gui = &gui;
	doStuff.store(-2);
	cv.notify_one();
	cv.wait(lock);
	return ret.load();
}

VKAPI_ATTR VkResult VKAPI_CALL CreateWin32SurfaceKHR(
		VkInstance                                  instance,
		const VkWin32SurfaceCreateInfoKHR*          pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSurfaceKHR*                               pSurface) {
	auto& ini = getData<Instance>(instance);
	auto res = ini.dispatch.CreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& platform = createData<Win32Platform>(*pSurface);
	platform.surfaceWindow = pCreateInfo->hwnd;

	return res;
}

} // namespace fuen