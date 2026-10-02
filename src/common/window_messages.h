#pragma once

// WPARAM: mpack_tree_t *, LPARAM: none
#define WM_NVIM_MESSAGE WM_USER

// WPARAM: none, LPARAM: none
#define WM_RENDERER_FONT_UPDATE (WM_USER + 1)

// WPARAM: none, LPARAM: none
#define WM_TSF_PROCESS_PENDING (WM_USER + 2)

// Does nothing, wakes up the message loop to schedule the animations
// WPARAM: none, LPARAM: none
#define WM_RENDERER_ANIMATE (WM_USER + 3)