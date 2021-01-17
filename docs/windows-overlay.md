There are just so many ways to do inputs and overlays on windows.
Rendering:

1. We can directly render our overlay onto the application swapchain.
    - Problems with applications that don't render continuously.
      Many applications stop rendering when not focused, meaning this can
      create conflicts when we use an overlay system window for other things (see input below).
2. We can render in our own (transparent) overlay window.
   There are various ways to set this window up. Transparency is quite messy on windows as well.
   Could be either toplevel or child (to the applications main window).
   Creating a transparent window (with transparent swapchain) might not work in all cases.

Input:
1. We can create an (insvisible) overlay window and try to get that focused
   when we show and focus our overlay. Trying to take input away from the application.
    - problems with SetCursor GetCursor applications
    - problems with applications using something like raw input
2. We can just use raw input on our own to get input. Does not block input 
   from the application though.
    - This works even with SetCursor/GetCursor applications. But rendering a software
      mouse cursor via raw input might feel weird, does not follow system cursor settings.
3. We can use the low-level windows system hooks to prevent input from being processed
   elsewhere when the overlay is active. We have to take care to properly release
   the hooks when the overlay is closed and have to be careful to never create a
   situation where the user is stuck in the overlay.
   The hooks can also be used to directly process the input.
   We need a separate thread procesing them though.
    - Even this has problem for input processing with GetCursor SetCursor appliations.
      Since somehow some games on windows think this is a good idea, we don't

Ugh the sources for this gave me various illnesses

- https://www.unknowncheats.me/forum/c-and-c-/36849-getting-control-mouse.html
- https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexa
- https://github.com/TsudaKageyu/minhook/blob/master/include/MinHook.h
- https://docs.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass
- https://github.com/concatstr/External-Overlay-Base-w-Working-ImGui-Impl/blob/4322a9a1c233b64f403823f749ac93de558f6535/External%20Overlay%20Base%20with%20ImGui%20-%20DREAM666/External%20Overlay%20Base%20ImGui/main.cpp
- https://guidedhacking.com/threads/imgui-directx9-overlay.11264/
- https://www.codeproject.com/Articles/297312/Minimal-Key-Logger-using-RAWINPUT
- https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexa?redirectedfrom=MSDN