#include <win32.hpp>
#include <gui/gui.hpp>
#include <vk/vulkan.h>
#include <device.hpp>
#include <layer.hpp>
#include <platform.hpp>
#include <util/util.hpp>
#include <swa/key.h>

#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <hidusage.h>
#include <windowsx.h>

namespace vil {

constexpr auto toggleKey = swa_key_equals;
constexpr auto focusKey = swa_key_rightbrace;

static enum swa_key winapi_to_key(unsigned vkcode);
static unsigned key_to_winapi(enum swa_key key);

// TODO: rather create one thread for all platform objects.
// TODO: we probably just want to use WH_GETMESSAGE.
//  Since that is the only real way to access WM_TEXT.
//  Simply set msg->msg = WM_NULL when we want to block them.
// TODO: we might be able to block raw input using the low-level hooks.
//  investigate!

struct Win32Platform : Platform {
	enum class State {
		// overlay and input window are hidden
		hidden,
		// overlay is shown but input window is hidden, i.e. no input
		// can be passed to window
		shown,
		// overlay is shown and input window is active, i.e. input
		// is redirected
		focused,
	};

	HWND surfaceWindow {};
	HWND overlayWindow {};

	HHOOK mouseHook {};

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
	
	bool moveResizing {};
	bool rawInput {};

	~Win32Platform();

	void init(Device& dev, unsigned width, unsigned height) override;
	void resize(unsigned, unsigned) override {}
	bool update(Gui& gui) override;

	bool checkPressed(u32 key) const;
	void uiThread(Device& dev, u32 width, u32 height);
	void initWindow();
	bool doUpdate();
	bool doUpdate2();
	void updateWindowRect();
};

Win32Platform* globalPlatform = nullptr;

Win32Platform::~Win32Platform() {
	if(thread.joinable()) {
		doStuff.store(1);
		cv.notify_one();
		thread.join();
	}
}

bool Win32Platform::checkPressed(u32 key) const {
	auto keycode = key_to_winapi(swa_key(key));
	return GetAsyncKeyState(keycode) < 0;
}

// from platform.cpp
bool updateEdge(bool& val, bool pressed);

constexpr auto windowClassName = L"VIL";

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
	auto keycode = winapi_to_key((unsigned)(wparam));
	if(keycode < 512) {
		platform->gui->imguiIO().KeysDown[keycode] = pressed;
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

static LRESULT CALLBACK winProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	Win32Platform* platform = (Win32Platform*) GetWindowLongPtr(hwnd, GWLP_USERDATA);

	switch(msg) {
		case WM_CREATE: {
			return 0;
		} case WM_KEYDOWN: {
			handleKey(platform, true, wparam, lparam);
			break;
		} case WM_KEYUP: {
			handleKey(platform, false, wparam, lparam);
			break;
		} case WM_CHAR: {
			if(wparam > 0 && wparam < 0x10000) {
				platform->gui->imguiIO().AddInputCharacter(wparam);
			}

			break;
		} case WM_INPUT: {
			UINT dwSize = sizeof(RAWINPUT);
			static std::aligned_storage_t<sizeof(RAWINPUT), 8> lpb {};
			GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &lpb, &dwSize, sizeof(RAWINPUTHEADER));
			RAWINPUT* raw = (RAWINPUT*)&lpb;
			if (raw->header.dwType == RIM_TYPEMOUSE) {
				platform->gui->imguiIO().MousePos.x += raw->data.mouse.lLastX;
				platform->gui->imguiIO().MousePos.y += raw->data.mouse.lLastY;
				// dlg_trace("mouse pos: {} {}", platform->gui->imguiIO().MousePos.x, platform->gui->imguiIO().MousePos.y);
			}

			break;
		} case WM_ERASEBKGND:
			// return 1; // prevent the background erase
			break;
		case WM_LBUTTONDOWN:
			dlg_trace("button0 pressed");
			platform->gui->imguiIO().MouseDown[0] = true;
			break;
		case WM_LBUTTONUP:
			dlg_trace("button0 release");
			platform->gui->imguiIO().MouseDown[0] = false;
			break;
		case WM_RBUTTONDOWN:
			platform->gui->imguiIO().MouseDown[1] = true;
			break;
		case WM_RBUTTONUP:
			platform->gui->imguiIO().MouseDown[1] = false;
			break;
		case WM_MBUTTONDOWN:
			platform->gui->imguiIO().MouseDown[2] = true;
			break;
		case WM_MBUTTONUP:
			platform->gui->imguiIO().MouseDown[2] = false;
			break;
		case WM_XBUTTONDOWN:
			platform->gui->imguiIO().MouseDown[HIWORD(wparam) == 1 ? 3 : 4] = true;
			break;
		case WM_XBUTTONUP:
			platform->gui->imguiIO().MouseDown[HIWORD(wparam) == 1 ? 3 : 4] = false;
			break;
		default:
			break;
	}

	return DefWindowProc(hwnd, msg, wparam, lparam);
}

void Win32Platform::initWindow() {
	// register window class
	WNDCLASSEXW wcx = {0};
	wcx.cbSize = sizeof(wcx);
	wcx.style = CS_VREDRAW | CS_HREDRAW;
	wcx.lpfnWndProc = winProc;
	wcx.cbClsExtra = 0;
	wcx.cbWndExtra = 0;
	wcx.hInstance = GetModuleHandle(NULL);
	wcx.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wcx.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
	wcx.hCursor = LoadCursor(NULL, IDC_ARROW);
	wcx.hbrBackground = (HBRUSH)CreateSolidBrush(RGB(30, 20, 30));
	wcx.lpszMenuName = NULL;
	wcx.lpszClassName = windowClassName;

	if(!::RegisterClassExW(&wcx)) {
		print_winapi_error("RegisterClassEx");
		// NOTE: we can probably just assume it was already registered by us
	}

	// create window
	auto style = WS_POPUP;
	auto exstyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST; // | */WS_EX_LAYERED | WS_EX_TOOLWINDOW;
	auto x = CW_USEDEFAULT;
	auto y = CW_USEDEFAULT;
	auto width = 5;
	auto height = 5;
	auto hinstance = GetModuleHandle(NULL);

	overlayWindow = CreateWindowEx(exstyle, windowClassName, L"VIL", style,
		x, y, width, height, NULL, NULL, hinstance, NULL);
	if(!overlayWindow) {
		print_winapi_error("CreateWindowEx");
		return;
	}

	ShowWindowAsync(overlayWindow, SW_HIDE);
	SetWindowLongPtr(overlayWindow, GWLP_USERDATA, (uintptr_t) this);

	// This will simply cause windows to respect the alpha bits in the content of the window
	// and not actually blur anything.
	// DWM_BLURBEHIND bb = {0};
	// bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
	// bb.hRgnBlur = CreateRectRgn(0, 0, -1, -1);  // makes the window transparent
	// bb.fEnable = TRUE;
	// DwmEnableBlurBehindWindow(overlayWindow, &bb);

	// make transparent
	// SetLayeredWindowAttributes(overlayWindow, 0, 0, LWA_ALPHA);
}

void Win32Platform::updateWindowRect() {
	/*
	RECT topWndRect;
	GetWindowRect(surfaceWindow, &topWndRect);
	auto w = topWndRect.right - topWndRect.left;
	auto h = topWndRect.bottom - topWndRect.top;
	// dlg_info("window rect, pos {} {}, size {} {}", topWndRect.left, topWndRect.top, w, h);
	MoveWindow(overlayWindow, topWndRect.left, topWndRect.top, w, h, false);
	*/
}

LRESULT CALLBACK msgHookFunc(int nCode, WPARAM wParam, LPARAM lParam) {
	MSG* msg = (MSG*) lParam;

	if (msg->message == WM_ENTERSIZEMOVE)
	{
		dlg_trace("enter size move {}", nCode);
	}

	if (globalPlatform->moveResizing || nCode < 0)
	{
		return CallNextHookEx(nullptr, nCode, wParam, lParam);
	}

	auto doHitTest = [](u32 x, u32 y) {
		u64 l = (u64(x) << 32) | u64(y);
		auto res = SendMessage(globalPlatform->surfaceWindow, WM_NCHITTEST, 0, l);
		return res == HTCLIENT;
	};

	switch(msg->message) {
		case WM_ENTERSIZEMOVE: {
			dlg_trace("enter move resize");
			globalPlatform->moveResizing = true;
			break;
		} case WM_EXITSIZEMOVE: {
			dlg_trace("exit move resize");
			globalPlatform->moveResizing = false;
			break;
		} case WM_MOUSEMOVE: {
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			// if(x < 0 || y < 0) {
			if(!doHitTest(x, y)) {
				break;
			}

			if (!globalPlatform->rawInput) {
				globalPlatform->gui->imguiIO().MousePos.x = x;
				globalPlatform->gui->imguiIO().MousePos.y = y;
			}

			msg->message = WM_NULL;
			return 0;
		} case WM_LBUTTONDOWN: {
			// dlg_trace("lbuttondown {} {} {}", data->pt.x, data->pt.y, data->wHitTestCode);
			globalPlatform->gui->imguiIO().MouseDown[0] = true;
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			// if(x < 0 || y < 0) {
			if(!doHitTest(x, y)) {
				break;
			}

			msg->message = WM_NULL;
			return 0;
		} case WM_LBUTTONUP: {
			// dlg_trace("lbuttonup {} {} {} | {} {}", data->pt.x, data->pt.y, data->wHitTestCode, winPos.x, winPos.y);
			globalPlatform->gui->imguiIO().MouseDown[0] = false;
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			// if(x < 0 || y < 0) {
			if(!doHitTest(x, y)) {
				break;
			}

			msg->message = WM_NULL;
			return 0;
		} case WM_RBUTTONDOWN: {
			globalPlatform->gui->imguiIO().MouseDown[1] = true;
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			// if(x < 0 || y < 0) {
			if(!doHitTest(x, y)) {
				break;
			}

			msg->message = WM_NULL;
			return 0;
		} case WM_RBUTTONUP: {
			globalPlatform->gui->imguiIO().MouseDown[1] = false;
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			// if(x < 0 || y < 0) {
			if(!doHitTest(x, y)) {
				break;
			}

			msg->message = WM_NULL;
			return 0;
		} case WM_MBUTTONDOWN: {
			globalPlatform->gui->imguiIO().MouseDown[2] = true;
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			// if(x < 0 || y < 0) {
			if(!doHitTest(x, y)) {
				break;
			}

			msg->message = WM_NULL;
			return 0;
		} case WM_MBUTTONUP: {
			globalPlatform->gui->imguiIO().MouseDown[2] = false;
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			// if(x < 0 || y < 0) {
			if(!doHitTest(x, y)) {
				break;
			}

			msg->message = WM_NULL;
			return 0;
		} case WM_XBUTTONDOWN: {
			// globalPlatform->gui->imguiIO().MouseClicked[HIWORD(wparam) == 1 ? 3 : 4] = true;
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			// if(x < 0 || y < 0) {
			if(!doHitTest(x, y)) {
				break;
			}

			msg->message = WM_NULL;
			return 0;
		} case WM_XBUTTONUP: {
			// globalPlatform->gui->imguiIO().MouseClicked[HIWORD(wparam) == 1 ? 3 : 4] = false;
			auto x = GET_X_LPARAM(msg->lParam);
			auto y = GET_Y_LPARAM(msg->lParam);
			// if(x < 0 || y < 0) {
			if(!doHitTest(x, y)) {
				break;
			}

			msg->message = WM_NULL;
			return 0;
		} case WM_KEYDOWN: {
			dlg_trace("wm keydown");

			auto key = winapi_to_key(unsigned(msg->wParam));
			if (key < 512 && key != 0) {
				globalPlatform->gui->imguiIO().KeysDown[unsigned(key)] = true;
			}

			// oh no, this is terrible
			TranslateMessage(msg);

			msg->message = WM_NULL;
			return 0;
		} case WM_KEYUP: {
			dlg_trace("wm keyup");

			auto key = winapi_to_key(unsigned(msg->wParam));
			if (key < 512 && key != 0) {
				globalPlatform->gui->imguiIO().KeysDown[unsigned(key)] = false;
			}

			// oh no, this is terrible
			TranslateMessage(msg);

			msg->message = WM_NULL;
			return 0;
		} case WM_CHAR: {
			dlg_trace("wm char");

			if (msg->wParam > 0 && msg->wParam < 0x10000) {
				globalPlatform->gui->imguiIO().AddInputCharacterUTF16((unsigned short)msg->wParam);
			}

			msg->message = WM_NULL;
			return 0;
		} case WM_INPUT: {
			// Shouldn't happen
			dlg_error("wm_input");

			UINT dwSize = sizeof(RAWINPUT);
			static std::aligned_storage_t<sizeof(RAWINPUT), 8> lpb {};
			auto ret = GetRawInputData((HRAWINPUT)msg->lParam, RID_INPUT, &lpb, &dwSize, sizeof(RAWINPUTHEADER));
			if (ret == UINT(-1)) {
				print_winapi_error("GetRawInputData");
				break;
			}

			RAWINPUT* raw = (RAWINPUT*) &lpb;
			if (raw->header.dwType == RIM_TYPEMOUSE) {
				globalPlatform->gui->imguiIO().MousePos.x += raw->data.mouse.lLastX;
				globalPlatform->gui->imguiIO().MousePos.y += raw->data.mouse.lLastY;

				// dlg_trace("mouse pos: {} {}", platform->gui->imguiIO().MousePos.x, platform->gui->imguiIO().MousePos.y);
			}

			// msg->message = WM_NULL;
			return 0;
		} case WM_MOUSEWHEEL: {
			// TODO: accumulate?
			globalPlatform->gui->imguiIO().MouseWheel = GET_WHEEL_DELTA_WPARAM(msg->wParam) / 120.f;
			msg->message = WM_NULL;
			return 0;
		} case WM_MOUSEHWHEEL: {
			// TODO: accumulate?
			globalPlatform->gui->imguiIO().MouseWheelH = GET_WHEEL_DELTA_WPARAM(msg->wParam) / 120.f;
			msg->message = WM_NULL;
			return 0;
		} default:
			break;
	}

	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK mouseHookFunc(int nCode, WPARAM wParam, LPARAM lParam) {
	MOUSEHOOKSTRUCTEX* data = (MOUSEHOOKSTRUCTEX*) lParam;

	POINT winPos = data->pt;
	ScreenToClient(globalPlatform->surfaceWindow, &winPos);

	if (nCode >= 0 && wParam == WM_MOUSEMOVE) {
		/*
		dlg_assert(globalPlatform);

		auto* data = (MOUSEHOOKSTRUCT*)lParam;
		// auto* data = (MSLLHOOKSTRUCT*)lParam;
		auto xPos = data->pt.x;
		auto yPos = data->pt.y;

		if (abs(xPos - globalPlatform->lastX) < 10 && abs(yPos - globalPlatform->lastY) < 10) {
			globalPlatform->gui->imguiIO().MousePos.x += xPos - globalPlatform->lastX;
			globalPlatform->gui->imguiIO().MousePos.y += yPos - globalPlatform->lastY;
		}

		globalPlatform->lastX = xPos;
		globalPlatform->lastY = yPos;

		*/

		// dlg_trace("move {} {} {}", winPos.x, winPos.y, data->wHitTestCode);
		// if(winPos.y < 0 || winPos.x < 0) {
			return CallNextHookEx(nullptr, nCode, wParam, lParam);
		// }

		// return 1;
	} else if(nCode >= 0) {
		switch(wParam) {
			case WM_LBUTTONDOWN:
				// dlg_trace("lbuttondown {} {} {}", data->pt.x, data->pt.y, data->wHitTestCode);
				globalPlatform->gui->imguiIO().MouseDown[0] = true;
				if(winPos.y < 0 || winPos.x < 0) {
					break;
				}
				return 1;
			case WM_LBUTTONUP:
				// dlg_trace("lbuttonup {} {} {} | {} {}", data->pt.x, data->pt.y, data->wHitTestCode, winPos.x, winPos.y);
				globalPlatform->gui->imguiIO().MouseDown[0] = false;
				if(winPos.y < 0 || winPos.x < 0) {
					break;
				}
				return 1;
			case WM_RBUTTONDOWN:
				globalPlatform->gui->imguiIO().MouseDown[1] = true;
				if(winPos.y < 0 || winPos.x < 0) {
					break;
				}
				return 1;
			case WM_RBUTTONUP:
				globalPlatform->gui->imguiIO().MouseDown[1] = false;
				if(winPos.y < 0 || winPos.x < 0) {
					break;
				}
				return 1;
			case WM_MBUTTONDOWN:
				globalPlatform->gui->imguiIO().MouseDown[2] = true;
				if(winPos.y < 0 || winPos.x < 0) {
					break;
				}
				return 1;
			case WM_MBUTTONUP:
				globalPlatform->gui->imguiIO().MouseDown[2] = false;
				if(winPos.y < 0 || winPos.x < 0) {
					break;
				}
				return 1;
			case WM_XBUTTONDOWN:
				// globalPlatform->gui->imguiIO().MouseClicked[HIWORD(wparam) == 1 ? 3 : 4] = true;
				if(winPos.y < 0 || winPos.x < 0) {
					break;
				}
				return 1;
			case WM_XBUTTONUP:
				// globalPlatform->gui->imguiIO().MouseClicked[HIWORD(wparam) == 1 ? 3 : 4] = false;
				if(winPos.y < 0 || winPos.x < 0) {
					break;
				}
				return 1;
			case WM_MOUSEWHEEL:
				// TODO: accumulate?
				globalPlatform->gui->imguiIO().MouseWheel = GET_WHEEL_DELTA_WPARAM(data->mouseData) / 120.f;
				return 1;
			case WM_MOUSEHWHEEL:
				// TODO: accumulate?
				globalPlatform->gui->imguiIO().MouseWheelH = GET_WHEEL_DELTA_WPARAM(data->mouseData) / 120.f;
				return 1;
			default:
				break;
		}
	}

	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool Win32Platform::doUpdate() {
	// update overlay window position
	if(state != State::focused) {
		if(updateEdge(togglePressed, this->checkPressed(toggleKey))) {
			dlg_trace("showing overlay; grabbing input");
			dlg_assert(!mouseHook);

			state = State::focused;

			globalPlatform = this;
			rawInput = false;

			// register for raw input on mouse
			if (!cursorShown()) {
				RAWINPUTDEVICE Rid[1];

				// unregister application
				Rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
				Rid[0].usUsage = HID_USAGE_GENERIC_MOUSE;
				Rid[0].dwFlags = RIDEV_REMOVE;
				Rid[0].hwndTarget = nullptr;
				auto r = RegisterRawInputDevices(Rid, 1, sizeof(Rid[0]));
				if(!r) { print_winapi_error("RegisterRawInput"); }

				Rid[0].usUsage = HID_USAGE_GENERIC_KEYBOARD;
				r = RegisterRawInputDevices(Rid, 1, sizeof(Rid[0]));
				if(!r) { print_winapi_error("RegisterRawInput"); }

				// register layer
				Rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
				Rid[0].usUsage = HID_USAGE_GENERIC_MOUSE;
				Rid[0].dwFlags = 0u;
				Rid[0].hwndTarget = overlayWindow;
				RegisterRawInputDevices(Rid, 1, sizeof(Rid[0]));
				if(!r) { print_winapi_error("RegisterRawInput"); }

				rawInput = true;
			}

			/*
			auto threadID = GetWindowThreadProcessId(surfaceWindow, nullptr);
			mouseHook = SetWindowsHookEx(WH_MOUSE, mouseHookFunc, nullptr, threadID);
			// mouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouseHookFunc, nullptr, 0);
			if (!mouseHook) {
				print_winapi_error("SetWindowsHookEx");
			}
			*/

			auto threadID = GetWindowThreadProcessId(surfaceWindow, nullptr);
			mouseHook = SetWindowsHookEx(WH_GETMESSAGE, msgHookFunc, nullptr, threadID);
			// mouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouseHookFunc, nullptr, 0);
			if (!mouseHook) {
				print_winapi_error("SetWindowsHookEx");
			}


			// updateWindowRect();
			// ShowWindow(overlayWindow, SW_SHOW);

			// TODO
			// SetFocus(overlayWindow);
			// SetActiveWindow(overlayWindow);
			// SetWindowPos(surfaceWindow, overlayWindow, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_ASYNCWINDOWPOS);

			POINT point;
			GetCursorPos(&point);
			gui->imguiIO().MousePos.x = point.x;
			gui->imguiIO().MousePos.y = point.y;
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
		if(updateEdge(togglePressed, this->checkPressed(toggleKey))) {
			dlg_trace("hiding overlay window, ungrabbing input");
			// ShowWindowAsync(overlayWindow, SW_HIDE);
			UnhookWindowsHookEx(mouseHook);

			// TODO: probably have to destroy window, this does no seem to work
			if (rawInput)
			{
				RAWINPUTDEVICE Rid[1];
				Rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
				Rid[0].usUsage = HID_USAGE_GENERIC_MOUSE;
				Rid[0].dwFlags = RIDEV_REMOVE;
				Rid[0].hwndTarget = nullptr;
				auto r = RegisterRawInputDevices(Rid, 1, sizeof(Rid[0]));
				if(!r) { print_winapi_error("RegisterRawInput"); }

				// re-register for application
				// shouldn't be a problem if not used
				Rid[0].usUsage = HID_USAGE_GENERIC_MOUSE;
				Rid[0].dwFlags = 0;
				r = RegisterRawInputDevices(Rid, 1, sizeof(Rid[0]));
				if(!r) { print_winapi_error("RegisterRawInput"); }

				Rid[0].usUsage = HID_USAGE_GENERIC_KEYBOARD;
				Rid[0].dwFlags = 0;
				r = RegisterRawInputDevices(Rid, 1, sizeof(Rid[0]));
				if(!r) { print_winapi_error("RegisterRawInput"); }
			}

			mouseHook = nullptr;
			state = State::hidden;
		} else if(updateEdge(focusPressed, this->checkPressed(focusKey))) {
			dlg_trace("ungrabbing input (overlay still shown)");
			// ShowWindowAsync(overlayWindow, SW_HIDE);
			// TODO: remove input device as well!
			UnhookWindowsHookEx(mouseHook);
			mouseHook = nullptr;
			state = State::shown;
		}

		// check if cursor is shown. If not, we draw our own.
	}

	if(state != State::hidden) {
		auto drawCursor = false;

		// when the overlay is focused and the application does not show a
		// cursor, we show our own software cursor.
		// TODO: don't need raw input if application is showing cursor...
		if(state == State::focused) {
			drawCursor = !cursorShown();
		}

		gui->imguiIO().MouseDrawCursor = drawCursor;

		if(!drawCursor) {
			// TODO: error handling
			// TODO: could use the mouse hook instead (but make sure to
			//  only enable that when the cursor isn't shown as otherwise we must
			//  rely on raw input and it will mess with that).
			POINT pos;
			GetCursorPos(&pos);
			ScreenToClient(surfaceWindow, &pos);

			gui->imguiIO().MousePos.x = pos.x;
			gui->imguiIO().MousePos.y = pos.y;
		}

		// Read keyboard modifiers inputs
		gui->imguiIO().KeyCtrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
		gui->imguiIO().KeyShift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
		gui->imguiIO().KeyAlt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
		gui->imguiIO().KeySuper = false;
	}

	return state != State::hidden;
}

LRESULT CALLBACK mouseLLHookFunc(int nCode, WPARAM wParam, LPARAM lParam) {
	auto* data = (MSLLHOOKSTRUCT*) lParam;
	(void) data;

	if (nCode == 0 && globalPlatform->state == Win32Platform::State::focused) {
		switch (wParam) {
			case WM_LBUTTONDOWN:
				dlg_trace("buttondown");
				break;
				// return 1;
			case WM_MOUSEMOVE:
				dlg_trace("mousemove");
				break;
				// return 1;
			default:
				break;
		}
	}

	if (nCode == 0 && wParam == WM_LBUTTONDOWN)
	{
		PostThreadMessage(GetCurrentThreadId(), WM_USER, 0, 0);
	}

	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

bool Win32Platform::doUpdate2() {
	if(state != State::focused) {
		if(updateEdge(togglePressed, this->checkPressed(toggleKey))) {
			dlg_trace("showing overlay; grabbing input");
			dlg_assert(!mouseHook);

			state = State::focused;

			globalPlatform = this;

		}
	}

	MSG msg;
	if(GetMessage(&msg, 0, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	if(state == State::focused) {
		if(updateEdge(togglePressed, this->checkPressed(toggleKey))) {
			dlg_trace("hiding overlay window, ungrabbing input");
			// UnhookWindowsHookEx(mouseHook);
			mouseHook = nullptr;
			state = State::hidden;
		}
	}

	return state != State::hidden;
}

void Win32Platform::uiThread(Device& dev, u32, u32) {
	std::unique_lock lock(mutex);
	(void) dev;

	// init hook
	/*
	globalPlatform = this;
	mouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouseLLHookFunc, nullptr, 0);
	if (!mouseHook) {
		print_winapi_error("SetWindowsHookEx");
	}
	*/

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
}

bool Win32Platform::update(Gui& gui) {
	// this->gui = &gui;
	// return ret.load();

	std::unique_lock lock(mutex);
	this->gui = &gui;
	doStuff.store(2);
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

	if(!checkEnvBinary("VIL_HOOK_OVERLAY", false)) {
		return res;
	}

	auto& platform = createData<Win32Platform>(*pSurface);
	platform.surfaceWindow = pCreateInfo->hwnd;

	return res;
}

// from swa
static const struct {
	enum swa_key key;
	unsigned vkcode;
} key_map [] = {
	{swa_key_none, 0x0},
	{swa_key_a, 'A'},
	{swa_key_b, 'B'},
	{swa_key_c, 'C'},
	{swa_key_d, 'D'},
	{swa_key_e, 'E'},
	{swa_key_f, 'F'},
	{swa_key_g, 'G'},
	{swa_key_h, 'H'},
	{swa_key_i, 'I'},
	{swa_key_j, 'J'},
	{swa_key_k, 'K'},
	{swa_key_l, 'L'},
	{swa_key_m, 'M'},
	{swa_key_n, 'N'},
	{swa_key_o, 'O'},
	{swa_key_p, 'P'},
	{swa_key_q, 'Q'},
	{swa_key_r, 'R'},
	{swa_key_s, 'S'},
	{swa_key_t, 'T'},
	{swa_key_u, 'U'},
	{swa_key_v, 'V'},
	{swa_key_w, 'W'},
	{swa_key_x, 'X'},
	{swa_key_y, 'Y'},
	{swa_key_z, 'Z'},
	{swa_key_k0, '0'},
	{swa_key_k1, '1'},
	{swa_key_k2, '2'},
	{swa_key_k3, '3'},
	{swa_key_k4, '4'},
	{swa_key_k5, '5'},
	{swa_key_k6, '6'},
	{swa_key_k7, '7'},
	{swa_key_k8, '8'},
	{swa_key_k9, '9'},
	{swa_key_backspace, VK_BACK},
	{swa_key_tab, VK_TAB},
	{swa_key_clear, VK_CLEAR},
	{swa_key_enter, VK_RETURN},
	{swa_key_leftshift, VK_SHIFT},
	{swa_key_leftctrl, VK_CONTROL},
	{swa_key_leftalt, VK_MENU},
	{swa_key_capslock, VK_CAPITAL},
	{swa_key_katakana, VK_KANA},
	{swa_key_hanguel, VK_HANGUL},
	{swa_key_hanja, VK_HANJA},
	{swa_key_escape, VK_ESCAPE},
	{swa_key_space, VK_SPACE},
	{swa_key_pageup, VK_PRIOR},
	{swa_key_pagedown, VK_NEXT},
	{swa_key_end, VK_END},
	{swa_key_home, VK_HOME},
	{swa_key_left, VK_LEFT},
	{swa_key_right, VK_RIGHT},
	{swa_key_up, VK_UP},
	{swa_key_down, VK_DOWN},
	{swa_key_select, VK_SELECT},
	{swa_key_print, VK_PRINT},
	{swa_key_insert, VK_INSERT},
	{swa_key_del, VK_DELETE},
	{swa_key_help, VK_HELP},
	{swa_key_leftmeta, VK_LWIN},
	{swa_key_rightmeta, VK_RWIN},
	{swa_key_sleep, VK_SLEEP},
	{swa_key_kp0, VK_NUMPAD0},
	{swa_key_kp1, VK_NUMPAD1},
	{swa_key_kp2, VK_NUMPAD2},
	{swa_key_kp3, VK_NUMPAD3},
	{swa_key_kp4, VK_NUMPAD4},
	{swa_key_kp5, VK_NUMPAD5},
	{swa_key_kp6, VK_NUMPAD6},
	{swa_key_kp7, VK_NUMPAD7},
	{swa_key_kp8, VK_NUMPAD8},
	{swa_key_kp9, VK_NUMPAD9},
	{swa_key_kpmultiply, VK_MULTIPLY},
	{swa_key_kpplus, VK_ADD},
	{swa_key_kpminus, VK_SUBTRACT},
	{swa_key_kpdivide, VK_DIVIDE},
	{swa_key_kpperiod, VK_SEPARATOR}, //XXX not sure
	{swa_key_f1, VK_F1},
	{swa_key_f2, VK_F2},
	{swa_key_f3, VK_F3},
	{swa_key_f4, VK_F4},
	{swa_key_f5, VK_F5},
	{swa_key_f6, VK_F6},
	{swa_key_f7, VK_F7},
	{swa_key_f8, VK_F8},
	{swa_key_f9, VK_F9},
	{swa_key_f10, VK_F10},
	{swa_key_f11, VK_F11},
	{swa_key_f12, VK_F12},
	{swa_key_f13, VK_F13},
	{swa_key_f14, VK_F14},
	{swa_key_f15, VK_F15},
	{swa_key_f16, VK_F16},
	{swa_key_f17, VK_F17},
	{swa_key_f18, VK_F18},
	{swa_key_f19, VK_F19},
	{swa_key_f20, VK_F20},
	{swa_key_f21, VK_F21},
	{swa_key_f22, VK_F22},
	{swa_key_f23, VK_F23},
	{swa_key_f24, VK_F24},
	{swa_key_numlock, VK_NUMLOCK},
	{swa_key_scrollock, VK_SCROLL},
	{swa_key_leftshift, VK_LSHIFT},
	{swa_key_rightshift, VK_RSHIFT},
	{swa_key_leftctrl, VK_LCONTROL},
	{swa_key_rightctrl, VK_RCONTROL},
	{swa_key_leftalt, VK_LMENU},
	{swa_key_rightalt, VK_RMENU},
	// XXX: some browser keys after this. not sure about it
	{swa_key_mute, VK_VOLUME_MUTE},
	{swa_key_volumedown, VK_VOLUME_DOWN},
	{swa_key_volumeup, VK_VOLUME_UP},
	{swa_key_nextsong, VK_MEDIA_NEXT_TRACK},
	{swa_key_previoussong, VK_MEDIA_PREV_TRACK},
	{swa_key_stopcd, VK_MEDIA_STOP}, // XXX: or keycode::stop?
	{swa_key_playpause, VK_MEDIA_PLAY_PAUSE},
	{swa_key_mail, VK_LAUNCH_MAIL},

	{swa_key_period, VK_OEM_PERIOD},
	{swa_key_comma, VK_OEM_COMMA},
	{swa_key_equals, VK_OEM_PLUS},
	{swa_key_minus, VK_OEM_MINUS},
	{swa_key_102nd, VK_OEM_102},

	{swa_key_semicolon, VK_OEM_1},
	{swa_key_slash, VK_OEM_2},
	{swa_key_grave, VK_OEM_3},
	{swa_key_leftbrace, VK_OEM_4},
	{swa_key_backslash, VK_OEM_5},
	{swa_key_rightbrace, VK_OEM_6},
	{swa_key_apostrophe, VK_OEM_7},

	{swa_key_play, VK_PLAY},
	{swa_key_zoom, VK_ZOOM},
};

static enum swa_key winapi_to_key(unsigned vkcode) {
	unsigned len = sizeof(key_map) / sizeof(key_map[0]);
	for(unsigned i = 0u; i < len; ++i) {
		if(key_map[i].vkcode == vkcode) {
			return key_map[i].key;
		}
	}

	return swa_key_none;
}

static unsigned key_to_winapi(enum swa_key key) {
	unsigned len = sizeof(key_map) / sizeof(key_map[0]);
	for(unsigned i = 0u; i < len; ++i) {
		if(key_map[i].key == key) {
			return key_map[i].vkcode;
		}
	}

	return 0x0;
}

} // namespace vil
