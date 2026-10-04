#include "config/config.h"
#include "nvim/nvim.h"
#include "renderer/renderer.h"
#include "renderer/cursor_animation.h"
#include "tsf/tsf.h"

struct Context {
	bool start_maximized;
	bool start_fullscreen;
	// See KeepCentered
	bool keep_centered;
	// Set while Ndx moves or resizes the window itself
	bool positioning_window;
	int64_t start_rows;
	int64_t start_cols;
	bool disable_fullscreen;
	HWND hwnd;
	Nvim *nvim;
	Renderer *renderer;
	Tsf *tsf;
	bool dead_char_pending;
	bool xbuttons[2];
	float buffered_scroll_amount;
	GridPoint cached_cursor_grid_pos;
	WINDOWPLACEMENT saved_window_placement;
	UINT saved_dpi_scaling;
	uint32_t saved_window_width;
	uint32_t saved_window_height;
	bool enable_cursor_timeout;
	uint32_t cursor_timer_id;
	uint32_t cursor_timeout_in_ms;
	HKL hkl;
};

void ToggleFullscreen(HWND hwnd, Context *context) {
	DWORD style = GetWindowLong(hwnd, GWL_STYLE);
	MONITORINFO mi { .cbSize = sizeof(MONITORINFO) };
	if (style & WS_OVERLAPPEDWINDOW) {
		if (GetWindowPlacement(hwnd, &context->saved_window_placement) &&
			GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
			SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
			SetWindowPos(hwnd, HWND_TOP,
				mi.rcMonitor.left, mi.rcMonitor.top,
				mi.rcMonitor.right - mi.rcMonitor.left,
				mi.rcMonitor.bottom - mi.rcMonitor.top,
				SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
	}
	else {
		SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
		SetWindowPlacement(hwnd, &context->saved_window_placement);
		SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
			SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	}
}

// Set by g:ndx_fullscreen, which is kept in sync with --fullscreen at VimEnter
void SetFullscreen(HWND hwnd, Context *context, bool fullscreen) {
	if (context->disable_fullscreen) return;
	bool is_fullscreen = !(GetWindowLong(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW);
	if (fullscreen != is_fullscreen) {
		ToggleFullscreen(hwnd, context);
	}
}

// A borderless window keeps its WS_OVERLAPPEDWINDOW style, so snapping, the minimize and
// maximize animations and the DWM shadow and rounded corners keep working. WM_NCCALCSIZE
// makes the whole window rect the client area and WM_NCHITTEST adds the resize borders.
bool IsBorderlessFrame(HWND hwnd, Context *context) {
	// Fullscreen removes WS_OVERLAPPEDWINDOW, the window has no frame then anyway
	return context && context->renderer->borderless && (GetWindowLong(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW);
}

void SetBorderless(HWND hwnd, Context *context, bool borderless) {
	Renderer *renderer = context->renderer;
	if (borderless == renderer->borderless) return;

	// The text stays in place, the title bar and borders are added or removed around it
	RECT client_rect;
	GetClientRect(hwnd, &client_rect);
	MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT *>(&client_rect), 2);
	renderer->borderless = borderless;

	// Extending the frame 1px into the client area brings back the DWM shadow
	MARGINS margins = borderless ? MARGINS { 1, 1, 1, 1 } : MARGINS {};
	DwmExtendFrameIntoClientArea(hwnd, &margins);

	// Maximized and fullscreen windows keep filling their monitor
	if (IsZoomed(hwnd) || !(GetWindowLong(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW)) {
		SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		return;
	}

	RECT window_rect = client_rect;
	if (!borderless) {
		AdjustWindowRectExForDpi(&window_rect, WS_OVERLAPPEDWINDOW, false,
			GetWindowLong(hwnd, GWL_EXSTYLE), GetDpiForWindow(hwnd));
		// Keep the title bar on screen
		MONITORINFO mi { .cbSize = sizeof(MONITORINFO) };
		if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) &&
			window_rect.top < mi.rcWork.top) {
			OffsetRect(&window_rect, 0, mi.rcWork.top - window_rect.top);
		}
	}
	SetWindowPos(hwnd, nullptr, window_rect.left, window_rect.top,
		window_rect.right - window_rect.left, window_rect.bottom - window_rect.top,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

LRESULT BorderlessHitTest(HWND hwnd, LPARAM lparam) {
	// A maximized window can't be resized from its edges
	if (IsZoomed(hwnd)) return HTCLIENT;

	POINT cursor { static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)) };
	RECT rect;
	if (!GetWindowRect(hwnd, &rect)) return HTNOWHERE;
	UINT dpi = GetDpiForWindow(hwnd);
	LONG border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

	bool left = cursor.x < rect.left + border;
	bool right = cursor.x >= rect.right - border;
	bool top = cursor.y < rect.top + border;
	bool bottom = cursor.y >= rect.bottom - border;
	if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
	if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
	if (left) return HTLEFT;
	if (right) return HTRIGHT;
	return HTCLIENT;
}

// Centers the window in the work area of its monitor. The visible frame is centered, the
// window rect also includes the invisible resize borders on the left, right and bottom.
// DWM only reports the visible frame once the window is shown, before that the borders
// are estimated from the system metrics.
void CenterWindow(HWND hwnd) {
	// Maximized and fullscreen windows already fill their monitor
	if (IsZoomed(hwnd) || !(GetWindowLong(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW)) return;

	RECT window_rect, frame_rect;
	MONITORINFO mi { .cbSize = sizeof(MONITORINFO) };
	if (!GetWindowRect(hwnd, &window_rect) ||
		!GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
		return;
	}
	if (!IsWindowVisible(hwnd) ||
		FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame_rect, sizeof(RECT)))) {
		UINT dpi = GetDpiForWindow(hwnd);
		LONG border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
		frame_rect = RECT {
			.left = window_rect.left + border,
			.top = window_rect.top,
			.right = window_rect.right - border,
			.bottom = window_rect.bottom - border
		};
	}

	LONG frame_width = frame_rect.right - frame_rect.left;
	LONG frame_height = frame_rect.bottom - frame_rect.top;
	LONG frame_x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - frame_width) / 2;
	LONG frame_y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - frame_height) / 2;
	// Keep the title bar on screen when the window is larger than the work area
	frame_x = max(frame_x, mi.rcWork.left);
	frame_y = max(frame_y, mi.rcWork.top);
	SetWindowPos(hwnd, nullptr,
		frame_x - (frame_rect.left - window_rect.left),
		frame_y - (frame_rect.top - window_rect.top),
		0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// With --position=center, the window is centered again whenever Ndx resizes it, as the
// font and grid size settle during startup. That stops once the window is moved or
// resized from outside, or the user starts typing or clicking.
void KeepCentered(Context *context) {
	if (!context->keep_centered) return;
	bool positioning = context->positioning_window;
	context->positioning_window = true;
	CenterWindow(context->hwnd);
	context->positioning_window = positioning;
}

void ProcessMPackMessage(Context *context, mpack_tree_t *tree) {
	MPackMessageResult result = MPackExtractMessageResult(tree);

	switch (result.type) {
	case MPackMessageType::Response: {
		assert(result.response.msg_id <= context->nvim->next_msg_id);
		switch (context->nvim->msg_id_to_method[result.response.msg_id]) {
		case NvimRequest::nvim_get_option_value: {
			Vec<char> guifont_buffer;
			NvimParseOptionValueStr(context->nvim, result.params, &guifont_buffer);
			if (!guifont_buffer.empty()) {
				RendererUpdateGuiFont(context->renderer, guifont_buffer.data(), strlen(guifont_buffer.data()));

				if (context->start_rows != 0 && context->start_cols != 0) {
					// after user config is read, process --geometry resize for the current font.
					// if user config also sets lines or columns, --geometry takes precedence.
					PixelSize start_size = RendererGridToPixelSize(context->renderer, context->start_rows, context->start_cols);
					SetWindowPos(context->hwnd, HWND_TOP, 0, 0, 
						start_size.width, start_size.height, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
				}
			}
		} break;
		case NvimRequest::vim_get_api_info:
		case NvimRequest::nvim_command: {
		} break;
		}
	} break;
	case MPackMessageType::Notification: {
		if (MPackMatchString(result.notification.name, "redraw")) {
			Cursor previous_cursor = context->renderer->cursor;
			float previous_font_width = context->renderer->font_width;
			float previous_font_height = context->renderer->font_height;
			RendererRedraw(context->renderer, result.params, context->start_maximized);

			// Let the IME reposition its candidate window
			if (previous_cursor.row != context->renderer->cursor.row ||
				previous_cursor.col != context->renderer->cursor.col ||
				previous_font_width != context->renderer->font_width ||
				previous_font_height != context->renderer->font_height) {
				TsfNotifyLayoutChange(context->tsf);
			}
		}
		else if (MPackMatchString(result.notification.name, "ndx_scroll")) {
			// Sent by the WinScrolled autocmd, see NvimInitialize
			RendererScrollWindows(context->renderer, mpack_node_array_at(result.params, 0));
		}
		else if (MPackMatchString(result.notification.name, "ndx_option")) {
			// Sent by the g:ndx_* watcher, see NvimInitialize
			mpack_node_t name = mpack_node_array_at(result.params, 0);
			mpack_node_t value = mpack_node_array_at(result.params, 1);
			if (mpack_node_type(name) == mpack_type_str) {
				const char *name_str = mpack_node_str(name);
				size_t name_length = mpack_node_strlen(name);
				if (name_length == strlen("ndx_borderless") && !strncmp(name_str, "ndx_borderless", name_length)) {
					SetBorderless(context->hwnd, context, NodeToBool(value, false));
				}
				else if (name_length == strlen("ndx_fullscreen") && !strncmp(name_str, "ndx_fullscreen", name_length)) {
					SetFullscreen(context->hwnd, context, NodeToBool(value, false));
				}
				else {
					RendererSetOption(context->renderer, name_str, name_length, value);
				}
			}
		}
	} break;
	case MPackMessageType::Request: {
		if (MPackMatchString(result.request.method, "vimenter")) {
			// nvim has read user init file, we can now request info if we want
			// like additional startup settings or something else
			NvimSendResponse(context->nvim, result.request.msg_id);
			NvimGetOptionValue(context->nvim, "guifont");
			NvimSendOptions(context->nvim);
			// So that toggling g:ndx_fullscreen leaves the fullscreen window --fullscreen opened
			if (!(GetWindowLong(context->hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW)) {
				NvimSendCommand(context->nvim, "let g:ndx_fullscreen = v:true");
			}
		}
	} break;
	}
}

bool SendResizeIfNecessary(Context *context, int rows, int cols) {
	if (!context->renderer->grid_initialized) return false;

	if (rows != context->renderer->grid_rows || cols != context->renderer->grid_cols) {
		NvimSendResize(context->nvim, rows, cols);
		return true;
	}
	return false;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	Context *context = reinterpret_cast<Context *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (msg == WM_CREATE) {
		LPCREATESTRUCT createStruct = reinterpret_cast<LPCREATESTRUCT>(lparam);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
		return 0;
	}

	switch (msg) {
	case WM_NCCALCSIZE: {
		if (wparam == TRUE && IsBorderlessFrame(hwnd, context)) {
			// A maximized window extends past its monitor by the frame size, keep the client
			// area within the work area
			RECT &rect = reinterpret_cast<NCCALCSIZE_PARAMS *>(lparam)->rgrc[0];
			MONITORINFO mi { .cbSize = sizeof(MONITORINFO) };
			if (IsZoomed(hwnd) && GetMonitorInfo(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &mi)) {
				rect = mi.rcWork;
			}
			return 0;
		}
	} break;
	case WM_NCHITTEST: {
		if (IsBorderlessFrame(hwnd, context)) {
			return BorderlessHitTest(hwnd, lparam);
		}
	} break;
	case WM_SIZE: {
		// Moved or resized by the user or the system, it then stays where it was put
		if (!context->positioning_window) {
			context->keep_centered = false;
		}
		if (wparam != SIZE_MINIMIZED) {
			uint32_t new_width = LOWORD(lparam);
			uint32_t new_height = HIWORD(lparam);
			context->saved_window_height = new_height;
			context->saved_window_width = new_width;
			TsfNotifyLayoutChange(context->tsf);
		}
	} return 0;
	case WM_MOVE: {
		if (!context->positioning_window) {
			context->keep_centered = false;
		}
		TsfNotifyLayoutChange(context->tsf);
	} return 0;
	case WM_DPICHANGED: {
		UINT current_dpi = HIWORD(wparam);
		RECT* const prcNewWindow = (RECT*)lparam;

		context->renderer->dpi_scale = current_dpi / 96.0f;
		context->saved_window_width = prcNewWindow->right - prcNewWindow->left;
		context->saved_window_height = prcNewWindow->bottom - prcNewWindow->top;
		context->saved_dpi_scaling = current_dpi;
		RendererUpdateFont(context->renderer, context->renderer->last_requested_font_size);

		SetWindowPos(hwnd, NULL,
				prcNewWindow->left, prcNewWindow->top,
				prcNewWindow->right - prcNewWindow->left, prcNewWindow->bottom - prcNewWindow->top,
				SWP_NOZORDER | SWP_NOACTIVATE);

		RendererResize(context->renderer, context->saved_window_width, context->saved_window_height);
		auto [rows, cols] = RendererPixelsToGridSize(context->renderer, context->saved_window_width, context->saved_window_height);
		SendResizeIfNecessary(context, rows, cols);
		TsfNotifyLayoutChange(context->tsf);
	} return 0;
	case WM_DESTROY: {
		PostQuitMessage(0);
	} return 0;
	case WM_NVIM_MESSAGE: {
		mpack_tree_t *tree = reinterpret_cast<mpack_tree_t *>(wparam);
		// Ndx shows and resizes the window itself here, to fit nvim's grid and --geometry
		RECT previous_rect {};
		GetWindowRect(hwnd, &previous_rect);
		bool was_visible = IsWindowVisible(hwnd);
		context->positioning_window = true;
		ProcessMPackMessage(context, tree);
		context->positioning_window = false;

		RECT rect {};
		GetWindowRect(hwnd, &rect);
		bool visible = IsWindowVisible(hwnd);
		if (visible != was_visible ||
			rect.right - rect.left != previous_rect.right - previous_rect.left ||
			rect.bottom - rect.top != previous_rect.bottom - previous_rect.top) {
			KeepCentered(context);
		}

		// This message is sent from the nvim thread and handled within GetMessage, which keeps
		// waiting afterwards. Wake up the message loop in case an animation or blinking started.
		if (context->renderer->animation_active) {
			PostMessage(hwnd, WM_RENDERER_ANIMATE, 0, 0);
		}
	} return 0;
	case WM_RENDERER_ANIMATE: {
	} return 0;
	case WM_RENDERER_VSYNC: {
		RendererOnVsync(context->renderer);
	} return 0;
	case WM_RENDERER_FONT_UPDATE: {
		auto [rows, cols] = RendererPixelsToGridSize(context->renderer,
			context->renderer->pixel_size.width, context->renderer->pixel_size.height);
		SendResizeIfNecessary(context, rows, cols);
		TsfNotifyLayoutChange(context->tsf);
	} return 0;
	case WM_TSF_PROCESS_PENDING: {
		TsfProcessPending(context->tsf);
	} return 0;
	case WM_INPUTLANGCHANGE: {
		HKL hkl = (HKL)lparam;
		context->hkl = hkl;
		return DefWindowProc(hwnd, msg, wparam, lparam);
	}
	case WM_DEADCHAR:
	case WM_SYSDEADCHAR: {
		context->dead_char_pending = true;
	} return 0;
	case WM_CHAR: {
		context->dead_char_pending = false;
		// Special case for <LT>
		if (wparam == 0x3C) {
			NvimSendInput(context->nvim, "<LT>");
			return 0;
		}
		if (wparam == 0x00) {
			NvimSendInput(context->nvim, "<Nul>");
			return 0;
		}
		NvimSendChar(context->nvim, static_cast<wchar_t>(wparam));
	} return 0;
	case WM_SYSCHAR: {
		if (static_cast<int>(wparam) == VK_SPACE) {
			return DefWindowProc(hwnd, msg, wparam, lparam);
		}
		else {
			context->dead_char_pending = false;
			NvimSendSysChar(context->nvim, static_cast<wchar_t>(wparam));
		}
	} return 0;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN: {
		// The key has already been handled by the IME through TSF
		if (wparam == VK_PROCESSKEY) {
			return 0;
		}

		// Fullscreen and the borderless window are toggled from nvim, see g:ndx_fullscreen and
		// g:ndx_borderless, so Alt+Enter reaches nvim as <M-CR>
		if (((GetKeyState(VK_LMENU) & 0x80) != 0) && wparam == VK_F4) {
			NvimQuit(context->nvim);
		}
		else {
			LONG msg_pos = GetMessagePos();
			POINTS pt = MAKEPOINTS(msg_pos);
			MSG current_msg {
				.hwnd = hwnd,
				.message = msg,
				.wParam = wparam,
				.lParam = lparam,
				.time = static_cast<DWORD>(GetMessageTime()),
				.pt = POINT { pt.x, pt.y }
			};

			if(context->dead_char_pending) {
				if(static_cast<int>(wparam) == VK_SPACE ||
				   static_cast<int>(wparam) == VK_BACK  ||
				   static_cast<int>(wparam) == VK_ESCAPE) {
					context->dead_char_pending = false;
					TranslateMessage(&current_msg);
					return 0;
				}
			}

			bool altgr_down = (GetKeyState(VK_RMENU) & 0x80) != 0;
			bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x80) != 0;
			wchar_t wchar = static_cast<wchar_t>(MapVirtualKeyEx(wparam, MAPVK_VK_TO_CHAR, context->hkl));
			if (!altgr_down && ctrl_down && wchar) {
				NvimSendSysChar(context->nvim, wchar);
				return 0;
			}

			// If none of the special keys were hit, process in WM_CHAR
			if(!NvimProcessKeyDown(context->nvim, static_cast<int>(wparam))) {
				TranslateMessage(&current_msg);
			}
		}
	} return 0;
	case WM_MOUSEMOVE: {
		if (context->enable_cursor_timeout) {
			HCURSOR hCursor = LoadCursor(NULL, IDC_ARROW);
			SetCursor(hCursor);
			SetTimer(hwnd, context->cursor_timer_id, context->cursor_timeout_in_ms, NULL);
		}
		POINTS cursor_pos = MAKEPOINTS(lparam);
		GridPoint grid_pos = RendererCursorToGridPoint(context->renderer, cursor_pos.x, cursor_pos.y);
		if (context->cached_cursor_grid_pos.col != grid_pos.col || context->cached_cursor_grid_pos.row != grid_pos.row) {
			switch (wparam) {
			case MK_LBUTTON: {
				NvimSendMouseInput(context->nvim, MouseButton::Left, MouseAction::Drag, grid_pos.row, grid_pos.col);
			} break;
			case MK_MBUTTON: {
				NvimSendMouseInput(context->nvim, MouseButton::Middle, MouseAction::Drag, grid_pos.row, grid_pos.col);
			} break;
			case MK_RBUTTON: {
				NvimSendMouseInput(context->nvim, MouseButton::Right, MouseAction::Drag, grid_pos.row, grid_pos.col);
			} break;
			}
			context->cached_cursor_grid_pos = grid_pos;
		}
	} return 0;
	case WM_TIMER: {
		if (context->enable_cursor_timeout && wparam == 1) {
			SetCursor(NULL);
		}
	} return 0;
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	case WM_MBUTTONUP: {
		// Commit any pending composition before nvim moves the cursor
		if (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN) {
			TsfTerminateComposition(context->tsf);
		}

		POINTS cursor_pos = MAKEPOINTS(lparam);
		auto [row, col] = RendererCursorToGridPoint(context->renderer, cursor_pos.x, cursor_pos.y);
		if (msg == WM_LBUTTONDOWN) {
			NvimSendMouseInput(context->nvim, MouseButton::Left, MouseAction::Press, row, col);
		}
		else if (msg == WM_MBUTTONDOWN) {
			NvimSendMouseInput(context->nvim, MouseButton::Middle, MouseAction::Press, row, col);
		}
		else if (msg == WM_RBUTTONDOWN) {
			NvimSendMouseInput(context->nvim, MouseButton::Right, MouseAction::Press, row, col);
		}
		else if (msg == WM_LBUTTONUP) {
			NvimSendMouseInput(context->nvim, MouseButton::Left, MouseAction::Release, row, col);
		}
		else if (msg == WM_MBUTTONUP) {
			NvimSendMouseInput(context->nvim, MouseButton::Middle, MouseAction::Release, row, col);
		}
		else if (msg == WM_RBUTTONUP) {
			NvimSendMouseInput(context->nvim, MouseButton::Right, MouseAction::Release, row, col);
		}
	} return 0;
	case WM_XBUTTONDOWN: {
		int button = GET_XBUTTON_WPARAM(wparam);
		if(button == XBUTTON1 && !context->xbuttons[0]) {
			NvimSendInput(context->nvim, "<C-o>");
			context->xbuttons[0] = true;
		}
		else if(button == XBUTTON2 && !context->xbuttons[1]) {
			NvimSendInput(context->nvim, "<C-i>");
			context->xbuttons[1] = true;
		}
	} return 0;
	case WM_XBUTTONUP: {
		int button = GET_XBUTTON_WPARAM(wparam);
		if(button == XBUTTON1) {
			context->xbuttons[0] = false;
		}
		else if(button == XBUTTON2) {
			context->xbuttons[1] = false;
		}
	} return 0;
	case WM_MOUSEWHEEL: {
		bool should_resize_font = (GetKeyState(VK_CONTROL) & 0x80) != 0;

		POINTS screen_point = MAKEPOINTS(lparam);
		POINT client_point {
			.x = static_cast<LONG>(screen_point.x),
			.y = static_cast<LONG>(screen_point.y),
		};
		ScreenToClient(hwnd, &client_point);

		float scroll_amount = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
		context->buffered_scroll_amount += scroll_amount;

		auto [row, col] = RendererCursorToGridPoint(context->renderer, client_point.x, client_point.y);

		MouseAction action;
		if (context->buffered_scroll_amount > 0.0) {
			scroll_amount = 1.0f;
			action = MouseAction::MouseWheelUp;
		} else {
			scroll_amount = -1.0f;
			action = MouseAction::MouseWheelDown;
		}

		while (abs(context->buffered_scroll_amount) >= 1.0f) {
			if (should_resize_font) {
				RendererUpdateFont(context->renderer, context->renderer->last_requested_font_size + (scroll_amount * 2.0f));
				auto [rows, cols] = RendererPixelsToGridSize(context->renderer,
					context->renderer->pixel_size.width, context->renderer->pixel_size.height);
				SendResizeIfNecessary(context, rows, cols);
			}
			else {
				for (int i = 0; i < abs(scroll_amount); ++i) {
					NvimSendMouseInput(context->nvim, MouseButton::Wheel, action, row, col);
				}
			}

			context->buffered_scroll_amount -= scroll_amount;
		}
	} return 0;
	case WM_DROPFILES: {
		wchar_t file_to_open[MAX_PATH];
		uint32_t num_files = DragQueryFileW(reinterpret_cast<HDROP>(wparam), 0xFFFFFFFF, file_to_open, MAX_PATH);
		for(int i = 0; i < num_files; ++i) {
			DragQueryFileW(reinterpret_cast<HDROP>(wparam), i, file_to_open, MAX_PATH);

			// Click left mouse button to ensure file is opened in the appropriate neovim split
			POINT screen_point;
			GetCursorPos(&screen_point);
			POINT client_point {
				.x = static_cast<LONG>(screen_point.x),
				.y = static_cast<LONG>(screen_point.y),
			};
			ScreenToClient(hwnd, &client_point);
			auto [row, col] = RendererCursorToGridPoint(context->renderer, client_point.x, client_point.y);
			NvimSendMouseInput(context->nvim, MouseButton::Left, MouseAction::Press, row, col);
			NvimSendMouseInput(context->nvim, MouseButton::Left, MouseAction::Release, row, col);

			// Not the most elegant solution, but must wait for mouseclick to be registered with nvim
			Sleep(10);

      NvimOpenFile(context->nvim, file_to_open, (GetKeyState(VK_CONTROL) & 0x80) != 0);
		}
	} return 0;
	case WM_SETFOCUS: {
		NvimSetFocus(context->nvim);
		RendererSetFocus(context->renderer, true);
	} return 0;
	case WM_KILLFOCUS: {
		NvimKillFocus(context->nvim);
		RendererSetFocus(context->renderer, false);
	} return 0;
	case WM_CLOSE: { 
		NvimQuit(context->nvim);
	} return 0;
	}

	return DefWindowProc(hwnd, msg, wparam, lparam);
}

BOOL ShouldUseDarkMode()
{
	constexpr const LPCWSTR key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
	constexpr const LPCWSTR value = L"AppsUseLightTheme";

	DWORD type;
	DWORD data;
	DWORD size = sizeof(DWORD);
	LSTATUS st = RegGetValue(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, &type, &data, &size);

	if (st == ERROR_SUCCESS && type == REG_DWORD) return data == 0;
	return false;
}

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev_instance, _In_ LPWSTR p_cmd_line, _In_ int n_cmd_show) {
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);

	int n_system_args;
	LPWSTR *system_args = CommandLineToArgvW(GetCommandLineW(), &n_system_args);

	// The options from config.toml come first, so the actual arguments take precedence
	ConfigArgs config {};
	ConfigLoad(&config);
	int n_args = n_system_args + config.count;
	LPWSTR *cmd_line_args = static_cast<LPWSTR *>(malloc(n_args * sizeof(LPWSTR)));
	cmd_line_args[0] = system_args[0];
	for (int i = 0; i < config.count; ++i) {
		cmd_line_args[1 + i] = config.args[i];
	}
	for (int i = 1; i < n_system_args; ++i) {
		cmd_line_args[config.count + i] = system_args[i];
	}

	bool start_maximized = false;
	bool start_fullscreen = false;
	bool start_centered = false;
	bool disable_ligatures = false;
  bool disable_fullscreen = false;
	float linespace_factor = 1.0f;
	int64_t start_rows = 0;
	int64_t start_cols = 0;
	int64_t start_pos_x = CW_USEDEFAULT;
	int64_t start_pos_y = CW_USEDEFAULT;
	bool enable_cursor_timeout = false;
	uint32_t cursor_timeout_in_ms = 0;

	static constexpr const wchar_t *NVIM_CMD = L"nvim --embed";
	size_t nvim_cmd_len = wcslen(NVIM_CMD);
	wchar_t *nvim_cmd = static_cast<wchar_t *>(calloc(nvim_cmd_len + 1, sizeof(wchar_t)));
	wmemcpy_s(nvim_cmd, nvim_cmd_len + 1, NVIM_CMD, nvim_cmd_len + 1);

	// Find explicitly provided nvim bin in case its given in arguments
	for (int i = 1; i < n_args; ++i) {
		if(!wcsncmp(cmd_line_args[i], L"--neovim-bin=", wcslen(L"--neovim-bin="))) {
			free(nvim_cmd);

			wchar_t *nvim_bin = &cmd_line_args[i][13];
			size_t nvim_bin_len = wcslen(&cmd_line_args[i][13]);
      nvim_cmd = static_cast<wchar_t *>(malloc(sizeof(wchar_t) * (1 + nvim_bin_len + wcslen(L"\" --embed") + 1)));
			wcscpy_s(nvim_cmd, 1 + nvim_bin_len + wcslen(L"\" --embed") + 1, L"\"");
			wcscat_s(nvim_cmd, 1 + nvim_bin_len + wcslen(L"\" --embed") + 1, nvim_bin);
      wcscat_s(nvim_cmd, 1 + nvim_bin_len + wcslen(L"\" --embed") + 1, L"\" --embed");
      nvim_cmd_len = wcslen(nvim_cmd);
		}
	}

	// Skip argv[0]
	for(int i = 1; i < n_args; ++i) {
		if(!wcscmp(cmd_line_args[i], L"--maximize")) {
			start_maximized = true;
		}
		else if(!wcscmp(cmd_line_args[i], L"--fullscreen")) {
			start_fullscreen = true;
		}
		else if(!wcscmp(cmd_line_args[i], L"--disable-ligatures")) {
			disable_ligatures = true;
		}
		else if(!wcscmp(cmd_line_args[i], L"--disable-fullscreen")) {
			disable_fullscreen = true;
		}
		else if(!wcsncmp(cmd_line_args[i], L"--geometry=", wcslen(L"--geometry="))) {
			wchar_t *end_ptr;
			start_cols = wcstol(&cmd_line_args[i][11], &end_ptr, 10);
			start_rows = wcstol(end_ptr + 1, nullptr, 10);
		}
		// The last position wins, so the command line overrides config.toml
		else if(!wcscmp(cmd_line_args[i], L"--position=center")) {
			start_centered = true;
			start_pos_x = CW_USEDEFAULT;
			start_pos_y = CW_USEDEFAULT;
		}
		else if(!wcsncmp(cmd_line_args[i], L"--position=", wcslen(L"--position="))) {
			wchar_t *end_ptr;
			start_centered = false;
			start_pos_x = wcstol(&cmd_line_args[i][11], &end_ptr, 10);
			start_pos_y = wcstol(end_ptr + 1, nullptr, 10);
		}
		else if(!wcsncmp(cmd_line_args[i], L"--linespace-factor=", wcslen(L"--linespace-factor="))) {
			wchar_t *end_ptr;
			float factor = wcstof(&cmd_line_args[i][19], &end_ptr);
			if(factor > 0.0f && factor < 20.0f) {
				linespace_factor = factor;
			}
		}
		else if (!wcsncmp(cmd_line_args[i], L"--cursor-timeout=", wcslen(L"--cursor-timeout="))) {
			enable_cursor_timeout = true;
			wchar_t* end_ptr;
			cursor_timeout_in_ms = wcstol(&cmd_line_args[i][17], &end_ptr, 10);
		}
		// Already processed
		else if (!wcsncmp(cmd_line_args[i], L"--neovim-bin=", wcslen(L"--neovim-bin="))) {}
		// Otherwise assume the argument is a filename to open
		else {
			// Otherwise assume switch is for nvim initialization
			const size_t arg_len = wcslen(cmd_line_args[i]);
			// Number of wchars including ' "', '"' and the terminator. The wcscat_s
			// sizes must be in elements, not bytes, otherwise the debug CRT fills
			// past the end of the buffer and corrupts the heap
			const size_t new_cmd_size = nvim_cmd_len + arg_len + 4;
			if (new_cmd_size >= 32767) {
				MessageBoxA(NULL, "ERROR: File path too long", "Ndx", MB_OK | MB_ICONERROR);
				return 1;
			}
			wchar_t *tmp = static_cast<wchar_t *>(realloc(nvim_cmd, new_cmd_size * sizeof(wchar_t)));
			if (tmp) {
				nvim_cmd = tmp;
				wcscat_s(nvim_cmd, new_cmd_size, L" \"");
				wcscat_s(nvim_cmd, new_cmd_size, cmd_line_args[i]);
				wcscat_s(nvim_cmd, new_cmd_size, L"\"");
				nvim_cmd_len = wcslen(nvim_cmd);
			} else {
				break; // not enough memory to continue
			}
		}
	}
	free(cmd_line_args);
	ConfigFree(&config);
	LocalFree(system_args);

	const wchar_t *window_class_name = L"Ndx_Class";
	const wchar_t *window_title = L"Ndx";
	WNDCLASSEX window_class {
		.cbSize = sizeof(WNDCLASSEX),
		.style = CS_HREDRAW | CS_VREDRAW,
		.lpfnWndProc = WndProc,
		.hInstance = instance,
		.hIcon = static_cast<HICON>(LoadImage(GetModuleHandle(NULL), L"NVIM_ICON", IMAGE_ICON, LR_DEFAULTSIZE, LR_DEFAULTSIZE, 0)),
		.hCursor = LoadCursor(NULL, IDC_ARROW),
		.lpszClassName = window_class_name,
		.hIconSm = static_cast<HICON>(LoadImage(GetModuleHandle(NULL), L"NVIM_ICON", IMAGE_ICON, LR_DEFAULTSIZE, LR_DEFAULTSIZE, 0))
	};

	if (!RegisterClassEx(&window_class)) {
		return 1;
	}

	// TSF requires a single threaded apartment
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	Nvim nvim {};
	Renderer renderer {};
	Tsf tsf {};
	constexpr uint32_t cursor_timer_id = 1;
	Context context {
		.start_maximized = start_maximized,
		.start_fullscreen = start_fullscreen,
		.keep_centered = start_centered,
		// Until the message loop starts, the window is set up by Ndx
		.positioning_window = true,
		.start_rows = start_rows,
		.start_cols = start_cols,
        .disable_fullscreen = disable_fullscreen,
		.nvim = &nvim,
		.renderer = &renderer,
		.tsf = &tsf,
		.saved_window_placement = WINDOWPLACEMENT { .length = sizeof(WINDOWPLACEMENT) },
		.enable_cursor_timeout = enable_cursor_timeout,
		.cursor_timer_id = cursor_timer_id,
		.cursor_timeout_in_ms = cursor_timeout_in_ms
	};

	HWND hwnd = CreateWindowEx(
		WS_EX_ACCEPTFILES | WS_EX_NOREDIRECTIONBITMAP,
		window_class_name,
		window_title,
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		nullptr,
		nullptr,
		instance,
		&context
	);
	if (hwnd == NULL) return 1;
	context.hwnd = hwnd;
	context.hkl = GetKeyboardLayout(0);

	// The system default size is rather wide, shrink it to 2/3 of the width
	// unless an explicit size is requested with --geometry
	if (start_rows == 0 && start_cols == 0) {
		RECT default_rect;
		GetWindowRect(hwnd, &default_rect);
		SetWindowPos(hwnd, nullptr, 0, 0, (default_rect.right - default_rect.left) * 2 / 3,
			default_rect.bottom - default_rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	RECT window_rect;
	DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &window_rect, sizeof(RECT));
	HMONITOR monitor = MonitorFromPoint({window_rect.left, window_rect.top}, MONITOR_DEFAULTTONEAREST);
	GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &(context.saved_dpi_scaling), &(context.saved_dpi_scaling));
	constexpr int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
	BOOL should_use_dark_mode = ShouldUseDarkMode();
	DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &should_use_dark_mode, sizeof(BOOL));
	RendererInitialize(&renderer, hwnd, disable_ligatures, linespace_factor, context.saved_dpi_scaling);

	NvimInitialize(&nvim, nvim_cmd, hwnd);
	free(nvim_cmd);

	TsfInitialize(&tsf, hwnd, &renderer, &nvim);

	// Forceably update the window to prevent any frames where the window is blank. Windows API docs
	// specify that SetWindowPos should be called with these arguments after SetWindowLong is called.
	UINT window_flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED;
	if (start_pos_x != CW_USEDEFAULT || start_pos_y != CW_USEDEFAULT) {
		window_flags = window_flags & ~SWP_NOMOVE;
	}
	SetWindowPos(hwnd, HWND_TOP, start_pos_x, start_pos_y, 0, 0, window_flags);
	// Before going fullscreen, so leaving it restores the centered window
	KeepCentered(&context);

	if (start_fullscreen) {
		ToggleFullscreen(context.hwnd, &context);
	}

	// Attach the renderer now that the window size is determined
	RendererAttach(context.renderer);
	auto [rows, cols] = RendererPixelsToGridSize(context.renderer,
		context.renderer->pixel_size.width, context.renderer->pixel_size.height);
	NvimSendUIAttach(context.nvim, rows, cols);

	context.positioning_window = false;

	MSG msg;
	uint32_t previous_width = 0, previous_height = 0;
	while (true) {
		if (DWORD blink_timeout = renderer.draw_active ? INFINITE : RendererGetBlinkTimeout(&renderer);
			blink_timeout != INFINITE) {
			// Wake up to show or hide the blinking cursor
			if (MsgWaitForMultipleObjectsEx(0, nullptr, blink_timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_TIMEOUT) {
				RendererFlush(&renderer);
				continue;
			}
			if (!PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
				continue;
			}
			if (msg.message == WM_QUIT) {
				break;
			}
		}
		else if (GetMessage(&msg, 0, 0, 0) <= 0) {
			break;
		}

		// Once the user starts working, later resizes such as font size changes keep the position
		switch (msg.message) {
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_MBUTTONDOWN:
		case WM_XBUTTONDOWN:
		case WM_MOUSEWHEEL:
		case WM_MOUSEHWHEEL: {
			context.keep_centered = false;
		} break;
		}

		// TranslateMessage(&msg);
		DispatchMessage(&msg);

		if (renderer.draw_active) continue;

		if (previous_width != context.saved_window_width || previous_height != context.saved_window_height) {
			previous_width = context.saved_window_width;
			previous_height = context.saved_window_height;
			auto [rows, cols] = RendererPixelsToGridSize(context.renderer, context.saved_window_width, context.saved_window_height);
			RendererResize(context.renderer, context.saved_window_width, context.saved_window_height);
			if (!SendResizeIfNecessary(&context, rows, cols))
				RendererFlush(context.renderer);
		}
	}

	TsfShutdown(&tsf);
	RendererShutdown(&renderer);
	NvimShutdown(&nvim);

	if (nvim.exit_code != EXIT_SUCCESS) {
		// We'll generate a message from the error stdout
		size_t cap = 512;
		DWORD len = 0; 
		char *msg = static_cast<char *>(calloc(cap, 1));
		while (true) {
			// nvim outputs directly in double byte on error on windows
			char buffer[1024 * 4];
			DWORD read = 0;
			if (!ReadFile(nvim.stderr_read, buffer, sizeof(buffer) - 1, &read, NULL)) {
				break;
			}
			if (!read) { continue; }
			buffer[read] = 0;
			char *tmp = static_cast<char *>(realloc(msg, size_t(read) + len + 1));
			if (!tmp) { break; } // no more memory... bail out
			msg = tmp;
			memcpy_s(msg + len, read, buffer, read);
			len += read;
			msg[len] = 0;
		}
		if (len > 0) {
			MessageBoxA(NULL, msg, "Ndx", MB_OK | MB_ICONERROR);
			free(msg);
		}

	}

	UnregisterClass(window_class_name, instance);
	DestroyWindow(hwnd);
	CoUninitialize();

	return nvim.exit_code;
}
