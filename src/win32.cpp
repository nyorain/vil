#include <win32.hpp>
#include <swaPlatform.hpp>
#include <gui/gui.hpp>
#include <vk/vulkan.h>
#include <device.hpp>
#include <layer.hpp>
#include <platform.hpp>
#include <util/util.hpp>
#include <vil_api.h>
#include <swa/key.h>
#include <swa/winapi.h>
#include <minhook/MinHook.h>

#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <hidusage.h>
#include <windowsx.h>

namespace vil {

// TODO: rather create one thread for all platform objects.
// TODO: we probably just want to use WH_GETMESSAGE.
//  Since that is the only real way to access WM_TEXT.
//  Simply set msg->msg = WM_NULL when we want to block them.
// TODO: we might be able to block raw input using the low-level hooks.
//  investigate!


// NOTES:
// - grabbing input for all applications without hooking winapi functions isn't possible:
//   We can't intercept raw input or applications that use some GetCursorPos magic e.g. for
//   movement (yes, it's terrible and yes, games do that, e.g. vkQuake).
// - activating/deactivating hooks via minhook can be expensive, we therefore leave the hooks
//   in place the whole time and just forward if the overlay is currently not visible.
// - SetWindowsHookEx isn't expensive but calling it on every show (and UnhookWindowsHookEx on every hide)
//   will eventually lead to an 'all windows api handles used' error (internal windows leak? idk).
//   So we just ignore it and install the hook just once forever, just skipping it when not needed

// NOTE: we can't know event calls always happen in the same thread as rendering
// so we need to use gui-internal even queue

template<typename Fn>
struct Hook;

template<typename Ret, typename... Args>
struct Hook<Ret(Args...)> {
	using Fn = Ret(*)(Args...);

	Fn original_ {};
	void* target_ {};

	Hook() = default;

	void init(void* target, void* dst) {
		dlg_assert(!target_);
		dlg_assert(target);
		dlg_assert(dst);

		target_ = target;
		auto res = MH_CreateHook(target_, dst, (void**) &original_);
		dlg_assertm(res == MH_OK, "{}: {}", res, MH_StatusToString(res));
	}

	~Hook() {
		MH_RemoveHook(target_);
	}

	void activate(bool active) {
		dlg_assert_or(target_, return);

		if(active) {
			auto res = MH_EnableHook(target_);
			dlg_assertm(res == MH_OK, "[{}] {}: {}", target_, res, MH_StatusToString(res));
		} else {
			auto res = MH_DisableHook(target_);
			dlg_assertm(res == MH_OK, "[{}] {}: {}", target_, res, MH_StatusToString(res));
		}
	}

	Ret forward(Args... args) const {
		return original_(std::forward<Args>(args)...);
	}
};

struct SavedState {
	std::atomic<bool> inputStateSaved = false;
    std::atomic<int> cursorCount = 0;
    std::atomic<bool> cursorVisible = false;
    std::atomic<POINT> cursorPos = POINT{ 0, 0 };
    std::atomic<HCURSOR> cursor = nullptr;
};

struct InputHooks {
	using PFN_GetAsyncKeyState = SHORT(WINAPI)(int vKey);
	using PFN_GetKeyState = SHORT(WINAPI)(int vKey);
	using PFN_GetKeyboardState = BOOL(WINAPI)(__out_ecount(256) PBYTE lpKeyState);
	using PFN_GetRawInputData = UINT(WINAPI)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
	using PFN_GetRawInputBuffer = UINT(WINAPI)(PRAWINPUT pData, PUINT pcbSize, UINT cbSizeHeader);
	using PFN_ShowCursor = int(WINAPI)(__in BOOL bShow);
	using PFN_GetCursorPos = BOOL(WINAPI)(LPPOINT lpPoint);
	using PFN_SetCursorPos = BOOL(WINAPI)(int X, int Y);
	using PFN_SetCursor = HCURSOR(WINAPI)(HCURSOR hCursor);
	using PFN_GetCursor = HCURSOR(WINAPI)();

	Hook<PFN_GetAsyncKeyState> GetAsyncKeyState;
	Hook<PFN_GetKeyState> GetKeyState;
	Hook<PFN_GetKeyboardState> GetKeyboardState;
	Hook<PFN_GetRawInputData> GetRawInputData;
	Hook<PFN_GetRawInputBuffer> GetRawInputBuffer;
	Hook<PFN_ShowCursor> ShowCursor;
	Hook<PFN_GetCursorPos> GetCursorPos;
	Hook<PFN_SetCursorPos> SetCursorPos;
	Hook<PFN_SetCursor> SetCursor;
	Hook<PFN_GetCursor> GetCursor;

	void init();
	void activate(bool);

	void save();
	void restore();

public:
	static std::weak_ptr<InputHooks> instance_;
	SavedState saved;
};

std::weak_ptr<InputHooks> InputHooks::instance_;

struct Win32Platform : Platform {
	HWND surfaceWindow {};

	HHOOK msgHook {};
	HHOOK wndProcHook {};

	State state {State::hidden};
	bool togglePressed {}; // for toggle key
	bool focusPressed {}; // for focus key

	std::thread thread;
	mutable std::mutex mutex;
	mutable std::condition_variable cv;
	mutable std::atomic<i64> doStuff;
	mutable Gui* gui;
	std::atomic<bool> ret;

	int lastX {};
	int lastY {};

	Vec2f guiWinPos {};
	Vec2f guiWinSize {};
	bool doGuiUnfocus {};
	bool softwareCursor {};

	bool moveResizing {};

	std::shared_ptr<InputHooks> hooks;

	~Win32Platform();

	void init(Device& dev, unsigned width, unsigned height) override;
	void resize(unsigned, unsigned) override {}
	State update(Gui& gui) override;

	bool checkPressed(u32 key) const;
	void uiThread(Device& dev, u32 width, u32 height);
	bool doUpdate();

	bool hookInput() const { return state == State::focused; }

public:
	static Win32Platform* instance_;

	static Win32Platform& get() {
		dlg_assert(instance_);
		return *instance_;
	}
};

Win32Platform* Win32Platform::instance_ = nullptr;

Win32Platform::~Win32Platform() {
	if(msgHook && wndProcHook) {
		UnhookWindowsHookEx(msgHook);
		UnhookWindowsHookEx(wndProcHook);
		msgHook = nullptr;
		wndProcHook = nullptr;
	}

	if(thread.joinable()) {
		doStuff.store(1);
		cv.notify_one();
		thread.join();
	}
}

bool Win32Platform::checkPressed(u32 key) const {
	auto keycode = swa_key_to_winapi(swa_key(key));
	return hooks->GetAsyncKeyState.forward(keycode) < 0;
}

// from platform.cpp
bool updateEdge(bool& val, bool pressed);

constexpr auto windowClassName = "VIL";

// from swa
#define print_winapi_error(func) do { \
	char* buffer; \
	int code = GetLastError(); \
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | \
		FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, \
		code, 0, (char*) &buffer, 0, NULL); \
	dlg_errort(("winapi_error"), "{}: {}", func, buffer); \
	LocalFree(buffer); \
} while(0)

void handleKey(Win32Platform* platform, bool pressed,
		WPARAM wparam, LPARAM lparam) {
	(void) lparam;
	auto keycode = swa_winapi_to_key((unsigned)(wparam));
	if(keycode < 512) {
		platform->gui->addKeyEvent(keyToImGui(keycode), pressed);
	}
}

bool cursorShown() {
	CURSORINFO ci {};
	ci.cbSize = sizeof(ci);
	if(!GetCursorInfo(&ci) || ci.flags == 0) {
		return false;
	}

	return true;
}

LRESULT CALLBACK msgHookFunc(int nCode, WPARAM wParam, LPARAM lParam) {
	MSG* msg = (MSG*) lParam;

	dlg_assert_or(Win32Platform::instance_, return CallNextHookEx(nullptr, nCode, wParam, lParam));
	auto& wp = Win32Platform::get();

	if (wp.state == Win32Platform::State::hidden || wp.moveResizing || nCode < 0) {
		return CallNextHookEx(nullptr, nCode, wParam, lParam);
	}

	bool forward = wp.state == SwaPlatform::State::shown;
	bool handle = wp.state == SwaPlatform::State::focused;

	auto doHitTest = [&](u32 x, u32 y) {
		POINT p {LONG(x), LONG(y)};
		auto r = ClientToScreen(wp.surfaceWindow, &p);
		dlg_assert(r);

		i32 l = (u32(p.y) << 16) | (u32(p.x) & 0xFFFFu);
		auto res = SendMessage(Win32Platform::get().surfaceWindow, WM_NCHITTEST, 0, l);
		// dlg_trace("doHitTest: {}", res == HTCLIENT);
		return res == HTCLIENT;
	};

	auto handleMouseButton = [&](swa_mouse_button button, bool pressed) -> bool {
		auto x = GET_X_LPARAM(msg->lParam);
		auto y = GET_Y_LPARAM(msg->lParam);
		if(!doHitTest(x, y)) {
			return true;
		}

		auto inside =
				x > wp.guiWinPos.x &&
				y > wp.guiWinPos.y &&
				x < wp.guiWinPos.x + wp.guiWinSize.x &&
				y < wp.guiWinPos.y + wp.guiWinSize.y;
		if(wp.state == SwaPlatform::State::focused && !inside) {
			dlg_trace("state: shown (unfocsed)");
			wp.state = SwaPlatform::State::shown;
			handle = false;
			forward = true;
			wp.doGuiUnfocus = true;
		} else if(wp.state == SwaPlatform::State::shown && inside) {
			dlg_trace("state: focused");
			wp.state = SwaPlatform::State::focused;
			handle = true;
			forward = false;
		}

		if(handle) {
			wp.gui->addMouseButtonEvent(unsigned(button) - 1, pressed);
		}

		if(!forward) {
			msg->message = WM_NULL;
			return false;
		}

		return true;
	};

	switch(msg->message) {
		case WM_MOUSEMOVE: {
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			if(!doHitTest(x, y)) {
				break;
			}

			// dlg_trace("mouse move {} {}", x, y);
			wp.gui->addMousePosEvent({float(x), float(y)});

			if(!forward) {
				msg->message = WM_NULL;
				return 0;
			}

			break;
		} case WM_LBUTTONDOWN: {
			if(!handleMouseButton(swa_mouse_button_left, true)) {
				return 0;
			}
			break;
		} case WM_LBUTTONUP: {
			if(!handleMouseButton(swa_mouse_button_left, false)) {
				return 0;
			}
			break;
		} case WM_RBUTTONDOWN: {
			if(!handleMouseButton(swa_mouse_button_right, true)) {
				return 0;
			}
			break;
		} case WM_RBUTTONUP: {
			if(!handleMouseButton(swa_mouse_button_right, false)) {
				return 0;
			}
			break;
		} case WM_MBUTTONDOWN: {
			if(!handleMouseButton(swa_mouse_button_middle, true)) {
				return 0;
			}
			break;
		} case WM_MBUTTONUP: {
			if(!handleMouseButton(swa_mouse_button_middle, false)) {
				return 0;
			}
			break;
		} case WM_XBUTTONDOWN: {
			// wp.gui->imguiIO().MouseClicked[HIWORD(wparam) == 1 ? 3 : 4] = true;
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			if(!doHitTest(x, y)) {
				break;
			}

			if(!forward) {
				msg->message = WM_NULL;
				return 0;
			}

			break;
		} case WM_XBUTTONUP: {
			// wp.gui->imguiIO().MouseClicked[HIWORD(wparam) == 1 ? 3 : 4] = false;
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			if(!doHitTest(x, y)) {
				break;
			}

			if(!forward) {
				msg->message = WM_NULL;
				return 0;
			}

			break;
		} case WM_KEYDOWN: {
			dlg_trace("wm keydown");

			auto key = swa_winapi_to_key(unsigned(msg->wParam));
			if (key < 512 && key != 0) {
				wp.gui->addKeyEvent(keyToImGui(unsigned(key)), true);
			}

			// oh no, this is terrible
			TranslateMessage(msg);

			if(!forward) {
				msg->message = WM_NULL;
				return 0;
			}

			break;
		} case WM_KEYUP: {
			dlg_trace("wm keyup");

			auto key = swa_winapi_to_key(unsigned(msg->wParam));
			if (key < 512 && key != 0) {
				wp.gui->addKeyEvent(keyToImGui(unsigned(key)), false);
			}

			// oh no, this is terrible
			TranslateMessage(msg);

			if(!forward) {
				msg->message = WM_NULL;
				return 0;
			}

			break;
		} case WM_CHAR: {
			dlg_trace("wm char");

			if(handle) {
				if(msg->wParam > 0 && msg->wParam < 0x10000) {
					wp.gui->addInputEvent((unsigned short) msg->wParam);
				}
			}

			if(!forward) {
				msg->message = WM_NULL;
				return 0;
			}

			break;
		} case WM_INPUT: {
			if(handle) {
				UINT dwSize = sizeof(RAWINPUT);
				static std::aligned_storage_t<sizeof(RAWINPUT), 8> lpb {};
				auto ret = wp.hooks->GetRawInputData.forward((HRAWINPUT)msg->lParam, RID_INPUT, &lpb, &dwSize, sizeof(RAWINPUTHEADER));
				if (ret == UINT(-1)) {
					print_winapi_error("GetRawInputData");
					break;
				}

				RAWINPUT* raw = (RAWINPUT*) &lpb;
				if (raw->header.dwType == RIM_TYPEMOUSE) {
					// TODO: convert to AddMousePosEvent somehow?
					wp.gui->imguiIO().MousePos.x += raw->data.mouse.lLastX;
					wp.gui->imguiIO().MousePos.y += raw->data.mouse.lLastY;

					if (raw->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) wp.gui->addMouseButtonEvent(0, true);
					if (raw->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) wp.gui->addMouseButtonEvent(0, false);
					if (raw->data.mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) wp.gui->addMouseButtonEvent(1, true);
					if (raw->data.mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) wp.gui->addMouseButtonEvent(1, false);
					if (raw->data.mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) wp.gui->addMouseButtonEvent(2, true);
					if (raw->data.mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP) wp.gui->addMouseButtonEvent(2, false);

					// dlg_trace("mouse pos: {} {}", platform->gui->imguiIO().MousePos.x, platform->gui->imguiIO().MousePos.y);
				}
			}

			// NOTE: we always forward this message
			// important, WM_INPUT needs to be processed, otherwise the queue fills up or something.
			// The application will call GetRawInputData on it, which is hooked by us anyways
			// if(!forward) {
				// msg->message = WM_NULL;
				// return 0;
			// }

			break;
		} case WM_MOUSEWHEEL: {
			if(handle) {
				wp.gui->addMouseWheelEvent({0, GET_WHEEL_DELTA_WPARAM(msg->wParam) / 120.f});
			}

			if(!forward) {
				msg->message = WM_NULL;
				return 0;
			}

			break;
		} case WM_MOUSEHWHEEL: {
			if(handle) {
				wp.gui->addMouseWheelEvent({GET_WHEEL_DELTA_WPARAM(msg->wParam) / 120.f, 0});
			}

			if(!forward) {
				msg->message = WM_NULL;
				return 0;
			}

			break;
		} default:
			break;
	}

	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT wndProcHookFunc(_In_ int nCode, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
	dlg_assert_or(Win32Platform::instance_, return CallNextHookEx(nullptr, nCode, wParam, lParam));
	auto& wp = Win32Platform::get();

	if (wp.state != Win32Platform::State::focused || nCode < 0) {
		return CallNextHookEx(nullptr, nCode, wParam, lParam);
	}

	CWPSTRUCT* cwp = (CWPSTRUCT*)lParam;

	switch (cwp->message) {
		case WM_ENTERSIZEMOVE: {
			dlg_trace("enter move resize");
			wp.moveResizing = true;
			break;
		} case WM_EXITSIZEMOVE: {
			dlg_trace("exit move resize");
			wp.moveResizing = false;
			break;
		/*
		case WM_NCHITTEST: {
			auto res = CallNextHookEx(nullptr, nCode, cwp->wParam, cwp->lParam);

			dlg_info("nchittest: {}", res);
			if (res != HTCLIENT) {
				dlg_info("move resizing true");
				wp.moveResizing = true;
			} else {
				dlg_info("move resizing false");
				wp.moveResizing = false;
			}
			return res;
		*/
		} default:
			break;
	}

	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool Win32Platform::doUpdate() {
	// update overlay window position
	if(state != State::focused) {
		if(updateEdge(togglePressed, this->checkPressed(toggleKey_))) {
			dlg_trace("showing overlay; grabbing input");

			state = State::focused;

			Win32Platform::instance_ = this;
			hooks->save();

			POINT point;
			hooks->GetCursorPos.forward(&point);
			gui->addMousePosEvent({float(point.x), float(point.y)});
		}
	}

	// dispatch all pending messages
	MSG msg;
	while(PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// update status
	if(state == State::focused) {
		if(updateEdge(togglePressed, this->checkPressed(toggleKey_))) {
			dlg_trace("hiding overlay window, ungrabbing input");

			hooks->restore();

			state = State::hidden;
		} else if(focusKey_ != VilKeyNone && updateEdge(focusPressed, this->checkPressed(focusKey_))) {
			dlg_trace("ungrabbing input (overlay still shown)");

			hooks->restore();

			state = State::shown;
		}

		// check if cursor is shown. If not, we draw our own.
	}

	if(state != State::hidden) {
		gui->imguiIO().MouseDrawCursor = softwareCursor;

		// TODO: error handling
		// TODO: could use the mouse hook instead (but make sure to
		//  only enable that when the cursor isn't shown as otherwise we must
		//  rely on raw input and it will mess with that).
		POINT pos;
		hooks->GetCursorPos.forward(&pos);
		ScreenToClient(surfaceWindow, &pos);

		gui->addMousePosEvent({float(pos.x), float(pos.y)});

		// Read keyboard modifiers inputs
		gui->addKeyEvent(ImGuiKey_ModCtrl, (hooks->GetKeyState.forward(VK_CONTROL) & 0x8000) != 0);
		gui->addKeyEvent(ImGuiKey_ModAlt, (hooks->GetKeyState.forward(VK_MENU) & 0x8000) != 0);
		gui->addKeyEvent(ImGuiKey_ModShift, (hooks->GetKeyState.forward(VK_SHIFT) & 0x8000) != 0);
	}

	return state != State::hidden;
}

void Win32Platform::uiThread(Device& dev, u32, u32) {
	std::unique_lock lock(mutex);
	(void) dev;

	// initWindow();
	cv.notify_one();

	while(true) {
		// while(doStuff.load() >= 0) {
			// ret.store(doUpdate2());
		// }

		while(doStuff.load() == 0) {
			cv.wait(lock);
		}

		auto stuff = doStuff.load();
		doStuff.store(0);

		if(stuff < 0) {
			dlg_error("unreachable");
		} else if(stuff == 1) {
			break;
		} else {
			// if(status == Status::focused) {
				// SetCapture(NULL);
				// ClipCursor(NULL);
				// while(ShowCursor(TRUE) <= 0);
			// }

			// POINT point;
			// ::GetCursorPos(&point);
			// dlg_trace("cursor pos {} {}", point.x, point.y);

			ret.store(doUpdate());
			cv.notify_one();
		}
	}
}

void Win32Platform::init(Device& dev, unsigned width, unsigned height) {
	std::unique_lock lock(mutex);
	thread = std::thread([&]{ uiThread(dev, width, height); });
	cv.wait(lock);

	Win32Platform::instance_ = this;

	if (auto ptr = InputHooks::instance_.lock()) {
		this->hooks = std::move(ptr);
	} else {
		this->hooks = std::make_shared<InputHooks>();
		InputHooks::instance_ = this->hooks;
		hooks->init();
		hooks->activate(true);
	}

	softwareCursor = checkEnvBinary("VIL_WIN32_SOFTWARE_CURSOR", false);
	auto threadID = GetWindowThreadProcessId(surfaceWindow, nullptr);

	msgHook = SetWindowsHookEx(WH_GETMESSAGE, msgHookFunc, nullptr, threadID);
	if(!msgHook) {
		print_winapi_error("SetWindowsHookEx");
	}

	wndProcHook = SetWindowsHookEx(WH_CALLWNDPROC, wndProcHookFunc, nullptr, threadID);
	if(!wndProcHook) {
		print_winapi_error("SetWindowsHookEx");
	}
}

Platform::State Win32Platform::update(Gui& gui) {
	// this->gui = &gui;
	// return ret.load();

	std::unique_lock lock(mutex);
	this->gui = &gui;
	this->guiWinPos = gui.windowPos();
	this->guiWinSize = gui.windowSize();

	if(doGuiUnfocus) {
		gui.unfocus = true;
		doGuiUnfocus = false;
	}

	doStuff.store(2);
	cv.notify_one();
	cv.wait(lock);
	// TODO: not thread safe...
	return state;
}

#pragma warning(disable : 4100)

// hooked input functions
SHORT WINAPI hookedGetAsyncKeyState(int vKey) {
	auto& wp = Win32Platform::get();
	if (wp.hookInput()) {
		return 0u;
	}

	return wp.hooks->GetAsyncKeyState.forward(vKey);
}

SHORT WINAPI hookedGetKeyState(int vKey) {
	auto& wp = Win32Platform::get();
	if (wp.hookInput()) {
		return 0u;
	}

	return wp.hooks->GetKeyState.forward(vKey);
}

BOOL WINAPI hookedGetKeyboardState(__out_ecount(256) PBYTE lpKeyState) {
	auto& wp = Win32Platform::get();
	if (wp.hookInput()) {
		return false;
	}

	return wp.hooks->GetKeyboardState.forward(lpKeyState);
}

UINT WINAPI hookedGetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
	auto& wp = Win32Platform::get();
	if (!wp.hookInput()) {
		return wp.hooks->GetRawInputData.forward(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
	}

	if (pcbSize) {
		if(!pData) {
			wp.hooks->GetRawInputData.forward(hRawInput, uiCommand, nullptr, pcbSize, cbSizeHeader);
		}

		if(*pcbSize > 0) {
			LPBYTE lpb = new BYTE[*pcbSize];
			wp.hooks->GetRawInputData.forward(hRawInput, uiCommand, lpb, pcbSize, cbSizeHeader);

			delete[] lpb;
			*pcbSize = 0;
		}
	}

	return 0u;
}

UINT WINAPI hookedGetRawInputBuffer(PRAWINPUT pData, PUINT pcbSize, UINT cbSizeHeader) {
	auto& wp = Win32Platform::get();
	if (!wp.hookInput()) {
		return wp.hooks->GetRawInputBuffer.forward(pData, pcbSize, cbSizeHeader);
	}

	if (pcbSize) {
		if(!pData) {
			wp.hooks->GetRawInputBuffer.forward(NULL, pcbSize, sizeof(RAWINPUTHEADER));
		}

		if (*pcbSize > 0) {
			UINT AllocatedBufferByteCount = *pcbSize * 16;
			RAWINPUT* RawInputBuffer = reinterpret_cast<RAWINPUT*>(malloc(AllocatedBufferByteCount));

			UINT AllocatedBufferByteCountTwo = AllocatedBufferByteCount;
			UINT Result = wp.hooks->GetRawInputBuffer.forward(RawInputBuffer, &(AllocatedBufferByteCountTwo), sizeof(RAWINPUTHEADER));
			if (Result == -1) {
				dlg_error(GetLastError());
			} else if (Result != 0) {
				UINT RawInputCount = Result;
				DefRawInputProc(&(RawInputBuffer), RawInputCount, sizeof(RAWINPUTHEADER));
			}

			free(RawInputBuffer);
		}
		*pcbSize = 0;
	}

	return 0;
}

int WINAPI hookedShowCursor(__in BOOL bShow) {
	auto& wp = Win32Platform::get();
	if (wp.hookInput()) {
		int saveCount = wp.hooks->saved.cursorCount;
        wp.hooks->saved.cursorCount  += bShow ? 1 : -1;
        wp.hooks->saved.cursorVisible = !!bShow;
        return saveCount;
	}

	return wp.hooks->ShowCursor.forward(bShow);
}

BOOL WINAPI hookedGetCursorPos(LPPOINT lpPoint) {
	auto& wp = Win32Platform::get();
	if (wp.hookInput()) {
		if (lpPoint) {
            *lpPoint = wp.hooks->saved.cursorPos;
        }

		return true;
	}

	return wp.hooks->GetCursorPos.forward(lpPoint);
}

BOOL WINAPI hookedSetCursorPos(int X, int Y) {
	auto& wp = Win32Platform::get();
	if (wp.hookInput()) {
		wp.hooks->saved.cursorPos = POINT{X, Y};
		return true;
	}

	return wp.hooks->SetCursorPos.forward(X, Y);
}

HCURSOR WINAPI hookedSetCursor(HCURSOR hCursor) {
	auto& wp = Win32Platform::get();
	if (wp.hookInput()) {
		wp.hooks->saved.cursor = hCursor;
		return nullptr;
	}

	return wp.hooks->SetCursor.forward(hCursor);
}

HCURSOR	WINAPI hookedGetCursor() {
	auto& wp = Win32Platform::get();
	if (wp.hookInput()) {
		return wp.hooks->saved.cursor;
	}

	return wp.hooks->GetCursor.forward();
}

/////
void InputHooks::init() {
	auto res = MH_Initialize();
	dlg_assertm(res == MH_OK, "{}: {}", res, MH_StatusToString(res));

	auto lib = LoadLibraryA("user32.dll");

#define VIL_INIT_HOOK(x) { \
		auto f##x = GetProcAddress(lib, #x); \
		this->x.init(f##x, hooked##x); \
		dlg_info("hook for {}: {}", #x, this->x.target_); \
	}

	VIL_INIT_HOOK(GetAsyncKeyState);
	VIL_INIT_HOOK(GetKeyState);
	VIL_INIT_HOOK(GetKeyboardState);
	VIL_INIT_HOOK(GetRawInputData);
	VIL_INIT_HOOK(GetRawInputBuffer);
	VIL_INIT_HOOK(ShowCursor);
	VIL_INIT_HOOK(GetCursorPos);
	VIL_INIT_HOOK(SetCursorPos);
	VIL_INIT_HOOK(SetCursor);
	VIL_INIT_HOOK(GetCursor);

#undef VIL_INIT_HOOK
}

void InputHooks::activate(bool a) {
	GetAsyncKeyState.activate(a);
	GetKeyState.activate(a);
	GetKeyboardState.activate(a);
	GetRawInputData.activate(a);
	GetRawInputBuffer.activate(a);
	ShowCursor.activate(a);
	GetCursorPos.activate(a);
	SetCursorPos.activate(a);
	SetCursor.activate(a);
	GetCursor.activate(a);
}

void InputHooks::save() {
	auto& saved = this->saved;

	if(!saved.inputStateSaved) {
        saved.cursorCount = ShowCursor.forward(TRUE);
        saved.cursorCount -= 1;

        int showCursorCounter = ShowCursor.forward(TRUE);

        while (showCursorCounter  < 0) {
            auto nextCounter = ShowCursor.forward(TRUE);

            if (nextCounter == showCursorCounter) {
                dlg_trace("oops!");
                break;
            }

            showCursorCounter = nextCounter;
        }

        POINT cursorPos = { 0 };
        GetCursorPos.forward(&cursorPos);
        saved.cursorPos.store(cursorPos);
        saved.cursor = GetCursor.forward();
        saved.inputStateSaved = true;
    }
}

void InputHooks::restore() {
	auto& saved = this->saved;

    if (saved.inputStateSaved) {
        int curCursorCount = ShowCursor.forward(FALSE);

        if(saved.cursorCount != curCursorCount) {
            BOOL showOrHide = saved.cursorCount > curCursorCount ? TRUE : FALSE;
            int showCursorCounter = ShowCursor.forward(showOrHide);

            while (showCursorCounter != saved.cursorCount) {
                auto nextCounter = ShowCursor.forward(showOrHide);
                if (nextCounter == showCursorCounter) {
                    dlg_trace("oops!");
                    break;
                }

                showCursorCounter = nextCounter;
            }
        }

        SetCursor.forward(saved.cursor);

        if (saved.cursorVisible) {
            ShowCursor.forward(TRUE);
        }

        saved.inputStateSaved = false;
    }
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

	if(!checkEnvBinary("VIL_HOOK_OVERLAY", false)) {
		dlg_trace(">> no hook overlay env set");
		return res;
	}

	dlg_trace(">> CreateWin32Surface");

	auto& platform = createData<Win32Platform>(*pSurface);
	platform.surfaceWindow = pCreateInfo->hwnd;

	return res;
}

} // namespace vil
