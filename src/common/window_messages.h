#pragma once

// WPARAM: mpack_tree_t *, LPARAM: none
#define WM_NVIM_MESSAGE WM_USER

// WPARAM: none, LPARAM: none
#define WM_RENDERER_FONT_UPDATE (WM_USER + 1)

// WPARAM: none, LPARAM: none
#define WM_TSF_PROCESS_PENDING (WM_USER + 2)

// Does nothing, wakes up the message loop to schedule the cursor blinking
// WPARAM: none, LPARAM: none
#define WM_RENDERER_ANIMATE (WM_USER + 3)

// Posted by the renderer's vsync thread once the swapchain is ready for a frame
// WPARAM: none, LPARAM: none
#define WM_RENDERER_VSYNC (WM_USER + 4)