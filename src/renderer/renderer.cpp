#include "renderer.h"
#include "renderer/cursor_animation.h"
#include "renderer/glyph_renderer.h"
#include "renderer/scroll_animation.h"

constexpr float DEFAULT_NEON_RADIUS = 4.0f;
constexpr float DEFAULT_NEON_INTENSITY = 1.0f;

void ResetLineLayouts(Renderer *renderer) {
	for (LineLayoutEntry &entry : renderer->line_layouts) {
		SafeRelease(&entry.layout);
		free(entry.chars);
		free(entry.properties);
		entry = {};
	}
}

void InitializeD2D(Renderer *renderer) {
	D2D1_FACTORY_OPTIONS options {};
#ifndef NDEBUG
	options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

	WIN_CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options, &renderer->d2d_factory));
}

void InitializeD3D(Renderer *renderer) {
	uint32_t flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	// Force DirectX 11.1
	ID3D11Device *temp_device;
	ID3D11DeviceContext *temp_context;
	D3D_FEATURE_LEVEL feature_levels[] = { 
		D3D_FEATURE_LEVEL_11_1,         
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3,
		D3D_FEATURE_LEVEL_9_2,
		D3D_FEATURE_LEVEL_9_1 
	};
	WIN_CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, feature_levels,
		ARRAYSIZE(feature_levels), D3D11_SDK_VERSION, &temp_device, &renderer->d3d_feature_level, &temp_context));
	WIN_CHECK(temp_device->QueryInterface(__uuidof(ID3D11Device2), reinterpret_cast<void **>(&renderer->d3d_device)));
	WIN_CHECK(temp_context->QueryInterface(__uuidof(ID3D11DeviceContext2), reinterpret_cast<void **>(&renderer->d3d_context)));

	IDXGIDevice3 *dxgi_device;
	WIN_CHECK(renderer->d3d_device->QueryInterface(__uuidof(IDXGIDevice3), reinterpret_cast<void **>(&dxgi_device)));
	WIN_CHECK(renderer->d2d_factory->CreateDevice(dxgi_device, &renderer->d2d_device));
	WIN_CHECK(renderer->d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS, &renderer->d2d_context));
	WIN_CHECK(renderer->d2d_context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &renderer->d2d_background_rect_brush));

	SafeRelease(&dxgi_device);
}

void InitializeDWrite(Renderer *renderer) {
	WIN_CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory4), reinterpret_cast<IUnknown **>(&renderer->dwrite_factory)));
	if(renderer->disable_ligatures) {
		WIN_CHECK(renderer->dwrite_factory->CreateTypography(&renderer->dwrite_typography));
		WIN_CHECK(renderer->dwrite_typography->AddFontFeature(DWRITE_FONT_FEATURE {
			.nameTag = DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES,
			.parameter = 0		
		}));
	}
}

void HandleDeviceLost(Renderer *renderer);
void InitializeWindowDependentResources(Renderer *renderer, uint32_t width, uint32_t height) {
	// Initializing window resources invalidates previous draws to the window,
	// so all lines need to be redrawn to make sure they are preserved.
	// Otherwise lines will appear entirely black until updated.
	renderer->draws_invalidated = true;

	renderer->pixel_size.width = width;
	renderer->pixel_size.height = height;

	ID3D11RenderTargetView *null_views[] = { nullptr };
	renderer->d3d_context->OMSetRenderTargets(ARRAYSIZE(null_views), null_views, nullptr);
	renderer->d2d_context->SetTarget(nullptr);
	renderer->d3d_context->Flush();
	// Recreated with the new size on the next draw
	SafeRelease(&renderer->d2d_grid_bitmap);
	SafeRelease(&renderer->d2d_neon_bitmap);
	SafeRelease(&renderer->d2d_neon_gain);
	SafeRelease(&renderer->d2d_neon_blur_near);
	SafeRelease(&renderer->d2d_neon_blur_far);
	ScrollAnimationReset(renderer->scroll_animation);

	if (renderer->dxgi_swapchain) {
		renderer->d2d_target_bitmap->Release();

		HRESULT hr = renderer->dxgi_swapchain->ResizeBuffers(
			2,
			width,
			height,
			DXGI_FORMAT_B8G8R8A8_UNORM,
			DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
		);

		if (hr == DXGI_ERROR_DEVICE_REMOVED) {
			HandleDeviceLost(renderer);
		}
	}
	else {
		DXGI_SWAP_CHAIN_DESC1 swapchain_desc {
			.Width = width,
			.Height = height,
			.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
			.SampleDesc = {
				.Count = 1,
				.Quality = 0
			},
			.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
			.BufferCount = 2,
			.Scaling = DXGI_SCALING_NONE,
			.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
			.AlphaMode = DXGI_ALPHA_MODE_IGNORE,
			.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
		};

		IDXGIDevice3 *dxgi_device;
		WIN_CHECK(renderer->d3d_device->QueryInterface(__uuidof(IDXGIDevice3), reinterpret_cast<void **>(&dxgi_device)));
		IDXGIAdapter *dxgi_adapter;
		WIN_CHECK(dxgi_device->GetAdapter(&dxgi_adapter));
		IDXGIFactory2 *dxgi_factory;
		WIN_CHECK(dxgi_adapter->GetParent(IID_PPV_ARGS(&dxgi_factory)));

		IDXGISwapChain1 *dxgi_swapchain_temp;
		WIN_CHECK(dxgi_factory->CreateSwapChainForHwnd(renderer->d3d_device,
			renderer->hwnd, &swapchain_desc, nullptr, nullptr, &dxgi_swapchain_temp));
		WIN_CHECK(dxgi_factory->MakeWindowAssociation(renderer->hwnd, DXGI_MWA_NO_ALT_ENTER));
		WIN_CHECK(dxgi_swapchain_temp->QueryInterface(__uuidof(IDXGISwapChain2), 
					reinterpret_cast<void **>(&renderer->dxgi_swapchain)));

		WIN_CHECK(renderer->dxgi_swapchain->SetMaximumFrameLatency(1));
		renderer->swapchain_wait_handle = renderer->dxgi_swapchain->GetFrameLatencyWaitableObject();
		renderer->swapchain_frame_acquired = false;

		SafeRelease(&dxgi_swapchain_temp);
		SafeRelease(&dxgi_device);
		SafeRelease(&dxgi_adapter);
		SafeRelease(&dxgi_factory);
	}

	constexpr D2D1_BITMAP_PROPERTIES1 target_bitmap_properties {
		.pixelFormat = D2D1_PIXEL_FORMAT {
			.format = DXGI_FORMAT_B8G8R8A8_UNORM,
			.alphaMode = D2D1_ALPHA_MODE_IGNORE
		},
		.dpiX = DEFAULT_DPI,
		.dpiY = DEFAULT_DPI,
		.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW
	};
	IDXGISurface2 *dxgi_backbuffer;
	WIN_CHECK(renderer->dxgi_swapchain->GetBuffer(0, IID_PPV_ARGS(&dxgi_backbuffer)));
	WIN_CHECK(renderer->d2d_context->CreateBitmapFromDxgiSurface(
		dxgi_backbuffer,
		&target_bitmap_properties,
		&renderer->d2d_target_bitmap
	));
	renderer->d2d_context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

	SafeRelease(&dxgi_backbuffer);
}

void HandleDeviceLost(Renderer *renderer) {
	ResetLineLayouts(renderer);
	SafeRelease(&renderer->d3d_device);
	SafeRelease(&renderer->d3d_context);
	SafeRelease(&renderer->dxgi_swapchain);
	SafeRelease(&renderer->d2d_factory);
	SafeRelease(&renderer->d2d_device);
	SafeRelease(&renderer->d2d_context);
	SafeRelease(&renderer->d2d_target_bitmap);
	SafeRelease(&renderer->d2d_grid_bitmap);
	SafeRelease(&renderer->d2d_neon_bitmap);
	SafeRelease(&renderer->d2d_neon_gain);
	SafeRelease(&renderer->d2d_neon_blur_near);
	SafeRelease(&renderer->d2d_neon_blur_far);
	ScrollAnimationReleaseResources(renderer->scroll_animation);
	SafeRelease(&renderer->d2d_background_rect_brush);
	SafeRelease(&renderer->dwrite_factory);
	SafeRelease(&renderer->dwrite_text_format);
	SafeRelease(&renderer->dwrite_font_fallback);
	delete renderer->glyph_renderer;

	InitializeD2D(renderer);
	InitializeD3D(renderer);
	InitializeDWrite(renderer);
	RECT client_rect;
	GetClientRect(renderer->hwnd, &client_rect);
	InitializeWindowDependentResources(
		renderer,
		static_cast<uint32_t>(client_rect.right - client_rect.left),
		static_cast<uint32_t>(client_rect.bottom - client_rect.top)
	);
}

bool IsCjkLanguage(LANGID language) {
	WORD primary_language = PRIMARYLANGID(language);
	return primary_language == LANG_CHINESE || primary_language == LANG_JAPANESE || primary_language == LANG_KOREAN;
}

// Han characters are shared between Chinese, Japanese and Korean, the locale decides
// which fonts the system fallback picks for them and which glyph forms are used.
// Use the user locale if it is a CJK one, otherwise the language of a CJK input
// method the user has installed (e.g. an English locale with a Chinese IME).
void InitializeLocale(Renderer *renderer) {
	LANGID language = LANGIDFROMLCID(GetUserDefaultLCID());
	if (!IsCjkLanguage(language)) {
		LANGID active_language = LOWORD(reinterpret_cast<uintptr_t>(GetKeyboardLayout(0)));
		if (IsCjkLanguage(active_language)) {
			language = active_language;
		}
		else {
			HKL layouts[64];
			int layout_count = GetKeyboardLayoutList(ARRAYSIZE(layouts), layouts);
			for (int i = 0; i < layout_count; ++i) {
				LANGID layout_language = LOWORD(reinterpret_cast<uintptr_t>(layouts[i]));
				if (IsCjkLanguage(layout_language)) {
					language = layout_language;
					break;
				}
			}
		}
	}

	if (!LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), renderer->locale_name, LOCALE_NAME_MAX_LENGTH, 0)) {
		wcscpy_s(renderer->locale_name, LOCALE_NAME_MAX_LENGTH, L"en-us");
	}
}

void RendererInitialize(Renderer *renderer, HWND hwnd, bool disable_ligatures, float linespace_factor, float monitor_dpi) {
	renderer->hwnd = hwnd;
	renderer->neon_radius = DEFAULT_NEON_RADIUS;
	renderer->neon_intensity = DEFAULT_NEON_INTENSITY;
	renderer->disable_ligatures = disable_ligatures;
	renderer->linespace_factor = linespace_factor;

	renderer->dpi_scale = monitor_dpi / 96.0f;
	renderer->hl_attribs.resize(MAX_HIGHLIGHT_ATTRIBS);

	wcscpy_s(renderer->fallback_font, MAX_FONT_LENGTH, L"Consolas");

	// Too big for the stack, the renderer lives in wWinMain
	renderer->cursor_animation = static_cast<CursorAnimation *>(malloc(sizeof(CursorAnimation)));
	CursorAnimationInitialize(renderer->cursor_animation);
	renderer->scroll_animation = static_cast<ScrollAnimation *>(malloc(sizeof(ScrollAnimation)));
	ScrollAnimationInitialize(renderer->scroll_animation);
	renderer->window_focused = true;
	QueryPerformanceFrequency(&renderer->performance_frequency);

	InitializeLocale(renderer);

	InitializeD2D(renderer);
	InitializeD3D(renderer);
	InitializeDWrite(renderer);
	renderer->glyph_renderer = new GlyphRenderer(renderer);
	RendererUpdateFont(renderer, DEFAULT_FONT_SIZE, DEFAULT_FONT, static_cast<int>(strlen(DEFAULT_FONT)));
}

void RendererAttach(Renderer *renderer) {
	RECT client_rect;
	GetClientRect(renderer->hwnd, &client_rect);
	InitializeWindowDependentResources(
		renderer,
		static_cast<uint32_t>(client_rect.right - client_rect.left),
		static_cast<uint32_t>(client_rect.bottom - client_rect.top)
	);
}

void RendererShutdown(Renderer *renderer) {
	ResetLineLayouts(renderer);
	if (renderer->vsync_thread) {
		renderer->vsync_thread_exit = true;
		SetEvent(renderer->vsync_request_event);
		// It may still be waiting for the swapchain, for at most a second
		WaitForSingleObject(renderer->vsync_thread, 1500);
		CloseHandle(renderer->vsync_thread);
		CloseHandle(renderer->vsync_request_event);
	}

	SafeRelease(&renderer->d3d_device);
	SafeRelease(&renderer->d3d_context);
	SafeRelease(&renderer->dxgi_swapchain);
	SafeRelease(&renderer->d2d_factory);
	SafeRelease(&renderer->d2d_device);
	SafeRelease(&renderer->d2d_context);
	SafeRelease(&renderer->d2d_target_bitmap);
	SafeRelease(&renderer->d2d_grid_bitmap);
	SafeRelease(&renderer->d2d_neon_bitmap);
	SafeRelease(&renderer->d2d_neon_gain);
	SafeRelease(&renderer->d2d_neon_blur_near);
	SafeRelease(&renderer->d2d_neon_blur_far);
	SafeRelease(&renderer->d2d_background_rect_brush);
	SafeRelease(&renderer->dwrite_factory);
	SafeRelease(&renderer->dwrite_text_format);
	SafeRelease(&renderer->dwrite_font_fallback);
	SafeRelease(&renderer->font_face);
	delete renderer->glyph_renderer;
	free(renderer->cursor_animation);
	ScrollAnimationReleaseResources(renderer->scroll_animation);
	free(renderer->scroll_animation);

	free(renderer->grid_chars);
	free(renderer->wchar_buffer);
	free(renderer->grid_cell_properties);
	free(renderer->dirty_rows);
	free(renderer->composition_text);
	free(renderer->char_widths.entries);
}

void RendererResize(Renderer *renderer, uint32_t width, uint32_t height) {
	InitializeWindowDependentResources(renderer, width, height);
}

bool ContainsSurrogatePair(uint32_t cell) {
	return cell > 0xFFFF;
}

void ConvertToWide(Renderer *renderer, uint32_t *text, uint32_t length) {
	size_t wchar_i = 0;
	for (size_t i = 0; i < length; i++) {
		// Unpack surrogate pairs into two sequential wchars.
		if (ContainsSurrogatePair(text[i])) {
			renderer->wchar_buffer[wchar_i] = static_cast<wchar_t>(text[i] >> 16);
			renderer->wchar_buffer[wchar_i + 1] = static_cast<wchar_t>(text[i] & 0xFFFF);
			wchar_i += 2;
			continue;
		}

		renderer->wchar_buffer[wchar_i] = static_cast<wchar_t>(text[i]);
		wchar_i += 1;
	}

	renderer->wchar_buffer_length = wchar_i;
}

float GetTextWidth(Renderer *renderer, uint32_t *text, uint32_t length) {
	ConvertToWide(renderer, text, length);

	// Create dummy text format to hit test the width of the font
	IDWriteTextLayout *test_text_layout = nullptr;
	WIN_CHECK(renderer->dwrite_factory->CreateTextLayout(
		renderer->wchar_buffer,
		renderer->wchar_buffer_length,
		renderer->dwrite_text_format,
		0.0f,
		0.0f,
		&test_text_layout
	));

	DWRITE_HIT_TEST_METRICS metrics;
	float _;
	WIN_CHECK(test_text_layout->HitTestTextPosition(0, 0, &_, &_, &metrics));
	test_text_layout->Release();

	return metrics.width;
}

constexpr uint64_t CHAR_WIDTH_EMPTY_KEY = UINT64_MAX;
constexpr uint32_t CHAR_WIDTH_INITIAL_CAPACITY = 1024;

void ResetCharWidths(Renderer *renderer) {
	CharWidthCache *cache = &renderer->char_widths;
	for (uint32_t i = 0; i < cache->capacity; ++i) {
		cache->entries[i].key = CHAR_WIDTH_EMPTY_KEY;
	}
	cache->count = 0;
	memset(renderer->latin1_glyphs, 0, sizeof(renderer->latin1_glyphs));
}

CharWidthEntry *FindCharWidthEntry(CharWidthCache *cache, uint64_t key) {
	// Fibonacci hashing, the capacity is always a power of two
	uint32_t mask = cache->capacity - 1;
	uint32_t i = static_cast<uint32_t>((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;
	while (cache->entries[i].key != key && cache->entries[i].key != CHAR_WIDTH_EMPTY_KEY) {
		i = (i + 1) & mask;
	}
	return &cache->entries[i];
}

void GrowCharWidths(CharWidthCache *cache) {
	CharWidthEntry *old_entries = cache->entries;
	uint32_t old_capacity = cache->capacity;

	cache->capacity = old_capacity ? old_capacity * 2 : CHAR_WIDTH_INITIAL_CAPACITY;
	cache->entries = static_cast<CharWidthEntry *>(malloc(cache->capacity * sizeof(CharWidthEntry)));
	for (uint32_t i = 0; i < cache->capacity; ++i) {
		cache->entries[i].key = CHAR_WIDTH_EMPTY_KEY;
	}
	for (uint32_t i = 0; i < old_capacity; ++i) {
		if (old_entries[i].key != CHAR_WIDTH_EMPTY_KEY) {
			*FindCharWidthEntry(cache, old_entries[i].key) = old_entries[i];
		}
	}
	free(old_entries);
}

// Width of the text in a grid cell, a wide char is measured together with the
// cell holding its right half. Creating a text layout for every character of
// every redrawn line is slow, so widths are cached until the font changes.
float GetCellTextWidth(Renderer *renderer, uint32_t *cell, bool is_wide_char) {
	uint32_t length = is_wide_char ? 2 : 1;
	// The right half of a wide char is normally empty, only that case is cached
	if (is_wide_char && cell[1] != 0) {
		return GetTextWidth(renderer, cell, length);
	}

	CharWidthCache *cache = &renderer->char_widths;
	if ((cache->count + 1) * 2 > cache->capacity) {
		GrowCharWidths(cache);
	}

	uint64_t key = static_cast<uint64_t>(cell[0]) | (is_wide_char ? (1ull << 32) : 0);
	CharWidthEntry *entry = FindCharWidthEntry(cache, key);
	if (entry->key == CHAR_WIDTH_EMPTY_KEY) {
		entry->key = key;
		entry->width = GetTextWidth(renderer, cell, length);
		++cache->count;
	}
	return entry->width;
}

bool IsGlyphMissing(Renderer *renderer, uint32_t codepoint) {
	assert(codepoint < ARRAYSIZE(renderer->latin1_glyphs));
	GlyphState *state = &renderer->latin1_glyphs[codepoint];
	if (*state == GlyphState::Unknown) {
		uint16_t glyph_index;
		WIN_CHECK(renderer->font_face->GetGlyphIndicesW(&codepoint, 1, &glyph_index));
		*state = glyph_index == 0 ? GlyphState::Missing : GlyphState::Present;
	}
	return *state == GlyphState::Missing;
}

bool UpdateFontMetrics(Renderer *renderer, float font_size, const char* font_string, int strlen) {
	font_size = max(5.0f, min(font_size, 150.0f));
	renderer->last_requested_font_size = font_size;

	IDWriteFontCollection *font_collection;
	WIN_CHECK(renderer->dwrite_factory->GetSystemFontCollection(&font_collection));

	int wstrlen = MultiByteToWideChar(CP_UTF8, 0, font_string, strlen, 0, 0);
	if (wstrlen != 0 && wstrlen < MAX_FONT_LENGTH) {
		MultiByteToWideChar(CP_UTF8, 0, font_string, strlen, renderer->font, MAX_FONT_LENGTH - 1);
		renderer->font[wstrlen] = L'\0';
	}

	uint32_t index;
	BOOL exists;
	font_collection->FindFamilyName(renderer->font, &index, &exists);

	bool guifont_exists = true;
	if (!exists) {
		guifont_exists = false;
		font_collection->FindFamilyName(renderer->fallback_font, &index, &exists);
		// Reset fallback font if it doesn't exist
		if (!exists) {
			wcscpy_s(renderer->fallback_font, MAX_FONT_LENGTH, L"Consolas");
			font_collection->FindFamilyName(renderer->fallback_font, &index, &exists);
		}
		memcpy(renderer->font, renderer->fallback_font, (wcslen(renderer->fallback_font) + 1) * sizeof(wchar_t));
	}

	IDWriteFontFamily *font_family;
	WIN_CHECK(font_collection->GetFontFamily(index, &font_family));

	IDWriteFont *write_font;
	WIN_CHECK(font_family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &write_font));

	IDWriteFontFace *font_face;
	WIN_CHECK(write_font->CreateFontFace(&font_face));
	SafeRelease(&renderer->font_face);
	WIN_CHECK(font_face->QueryInterface<IDWriteFontFace1>(&renderer->font_face));
	ResetCharWidths(renderer);

	renderer->font_face->GetMetrics(&renderer->font_metrics);

	uint16_t glyph_index;
	constexpr uint32_t codepoint = L'A';
	WIN_CHECK(renderer->font_face->GetGlyphIndicesW(&codepoint, 1, &glyph_index));

	int32_t glyph_advance_in_em;
	WIN_CHECK(renderer->font_face->GetDesignGlyphAdvances(1, &glyph_index, &glyph_advance_in_em));

	IDWriteFont* write_font_bold;
	WIN_CHECK(font_family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &write_font_bold));

	IDWriteFontFace* font_face_bold;
	WIN_CHECK(write_font_bold->CreateFontFace(&font_face_bold));
	IDWriteFontFace1* font_size_scale_bold1;
	WIN_CHECK(font_face_bold->QueryInterface<IDWriteFontFace1>(&font_size_scale_bold1));
	DWRITE_FONT_METRICS1 font_metrics_bold;
	font_size_scale_bold1->GetMetrics(&font_metrics_bold);

	int32_t glyph_advance_in_em_bold;
	WIN_CHECK(font_size_scale_bold1->GetDesignGlyphAdvances(1, &glyph_index, &glyph_advance_in_em_bold));

	float desired_height = font_size * renderer->dpi_scale * (DEFAULT_DPI / POINTS_PER_INCH);
	float width_advance = static_cast<float>(glyph_advance_in_em) / renderer->font_metrics.designUnitsPerEm;
	float desired_width = desired_height * width_advance;

	float width_advance_bold = static_cast<float>(glyph_advance_in_em_bold) / font_metrics_bold.designUnitsPerEm;
	float desired_width_bold = desired_height * width_advance_bold;

	float bold_scale = desired_width / desired_width_bold;
	// We need the width to be aligned on a per-pixel boundary, thus we will
	// roundf the desired_width and calculate the font size given the new exact width
	renderer->font_width = roundf(desired_width);
	renderer->font_size = renderer->font_width / width_advance;

	renderer->font_size_scale_bold = renderer->font_size * bold_scale;

	float frac_font_ascent = (renderer->font_size * renderer->font_metrics.ascent) / renderer->font_metrics.designUnitsPerEm;
	float frac_font_descent = (renderer->font_size * renderer->font_metrics.descent) / renderer->font_metrics.designUnitsPerEm;
	float linegap = (renderer->font_size * renderer->font_metrics.lineGap) / renderer->font_metrics.designUnitsPerEm;
	float half_linegap = linegap / 2.0f;
	renderer->font_ascent = ceilf(frac_font_ascent + half_linegap);
	renderer->font_descent = ceilf(frac_font_descent + half_linegap);
	renderer->font_height = renderer->font_ascent + renderer->font_descent;
	renderer->font_height *= renderer->linespace_factor;

	WIN_CHECK(renderer->dwrite_factory->CreateTextFormat(
		renderer->font,
		nullptr,
		DWRITE_FONT_WEIGHT_NORMAL,
		DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL,
		renderer->font_size,
		renderer->locale_name,
		&renderer->dwrite_text_format
	));

	WIN_CHECK(renderer->dwrite_text_format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, renderer->font_height, renderer->font_ascent * renderer->linespace_factor));
	WIN_CHECK(renderer->dwrite_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR));
	WIN_CHECK(renderer->dwrite_text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));

	// Glyphs missing from the main font are looked up in the other guifont fonts
	if (renderer->dwrite_font_fallback) {
		IDWriteTextFormat1 *text_format;
		if (SUCCEEDED(renderer->dwrite_text_format->QueryInterface<IDWriteTextFormat1>(&text_format))) {
			text_format->SetFontFallback(renderer->dwrite_font_fallback);
			text_format->Release();
		}
	}

	SafeRelease(&font_face);
	SafeRelease(&font_face_bold);
	SafeRelease(&font_size_scale_bold1);
	SafeRelease(&write_font);
	SafeRelease(&write_font_bold);

	SafeRelease(&font_family);
	SafeRelease(&font_collection);

	return guifont_exists;
}

bool RendererUpdateFont(Renderer *renderer, float font_size, const char *font_string, int strlen) {
	ResetLineLayouts(renderer);
	if (renderer->dwrite_text_format) {
		renderer->dwrite_text_format->Release();
	}

	renderer->draws_invalidated = true;
	return UpdateFontMetrics(renderer, font_size, font_string, strlen);
}

void UpdateDefaultColors(Renderer *renderer, mpack_node_t default_colors) {
	ResetLineLayouts(renderer);
	size_t default_colors_arr_length = mpack_node_array_length(default_colors);

	for (size_t i = 1; i < default_colors_arr_length; ++i) {
		mpack_node_t color_arr = mpack_node_array_at(default_colors, i);

		// Default colors occupy the first index of the highlight attribs array
		renderer->hl_attribs[0].foreground = static_cast<uint32_t>(mpack_node_array_at(color_arr, 0).data->value.u);
		renderer->hl_attribs[0].background = static_cast<uint32_t>(mpack_node_array_at(color_arr, 1).data->value.u);
		renderer->hl_attribs[0].special = static_cast<uint32_t>(mpack_node_array_at(color_arr, 2).data->value.u);
		renderer->hl_attribs[0].flags = 0;
	}
}

void UpdateHighlightAttributes(Renderer *renderer, mpack_node_t highlight_attribs) {
	ResetLineLayouts(renderer);
	uint64_t attrib_count = mpack_node_array_length(highlight_attribs);
	for (uint64_t i = 1; i < attrib_count; ++i) {
		int64_t attrib_index = mpack_node_array_at(mpack_node_array_at(highlight_attribs, i), 0).data->value.i;
		assert(attrib_index <= MAX_HIGHLIGHT_ATTRIBS);

		mpack_node_t attrib_map = mpack_node_array_at(mpack_node_array_at(highlight_attribs, i), 1);

		const auto SetColor = [&](const char *name, uint32_t *color) {
			mpack_node_t color_node = mpack_node_map_cstr_optional(attrib_map, name);
			if (!mpack_node_is_missing(color_node)) {
				*color = static_cast<uint32_t>(color_node.data->value.u);
			}
			else {
				*color = DEFAULT_COLOR;
			}
		};
		SetColor("foreground", &renderer->hl_attribs[attrib_index].foreground);
		SetColor("background", &renderer->hl_attribs[attrib_index].background);
		SetColor("special", &renderer->hl_attribs[attrib_index].special);

		const auto SetFlag = [&](const char *flag_name, HighlightAttributeFlags flag) {
			mpack_node_t flag_node = mpack_node_map_cstr_optional(attrib_map, flag_name);
			if (!mpack_node_is_missing(flag_node)) {
				if (flag_node.data->value.b) {
					renderer->hl_attribs[attrib_index].flags |= flag;
				}
				else {
					renderer->hl_attribs[attrib_index].flags &= ~flag;
				}
			}
		};
		SetFlag("reverse", HL_ATTRIB_REVERSE);
		SetFlag("italic", HL_ATTRIB_ITALIC);
		SetFlag("bold", HL_ATTRIB_BOLD);
		SetFlag("strikethrough", HL_ATTRIB_STRIKETHROUGH);
		SetFlag("underline", HL_ATTRIB_UNDERLINE);
		SetFlag("undercurl", HL_ATTRIB_UNDERCURL);
	}
}

uint32_t CreateForegroundColor(Renderer *renderer, HighlightAttributes *hl_attribs) {
	if (hl_attribs->flags & HL_ATTRIB_REVERSE) {
		return hl_attribs->background == DEFAULT_COLOR ? renderer->hl_attribs[0].background : hl_attribs->background;
	}
	else {
		return hl_attribs->foreground == DEFAULT_COLOR ? renderer->hl_attribs[0].foreground : hl_attribs->foreground;
	}
}

uint32_t CreateBackgroundColor(Renderer *renderer, HighlightAttributes *hl_attribs) {
	if (hl_attribs->flags & HL_ATTRIB_REVERSE) {
		return hl_attribs->foreground == DEFAULT_COLOR ? renderer->hl_attribs[0].foreground : hl_attribs->foreground;
	}
	else {
		return hl_attribs->background == DEFAULT_COLOR ? renderer->hl_attribs[0].background : hl_attribs->background;
	}
}

uint32_t CreateSpecialColor(Renderer *renderer, HighlightAttributes *hl_attribs) {
	return hl_attribs->special == DEFAULT_COLOR ? renderer->hl_attribs[0].special : hl_attribs->special;
}

void ApplyHighlightAttributes(Renderer *renderer, HighlightAttributes *hl_attribs,
	IDWriteTextLayout *text_layout, int start, int end) {
	GlyphDrawingEffect *drawing_effect = new GlyphDrawingEffect(
			CreateForegroundColor(renderer, hl_attribs),
			CreateSpecialColor(renderer, hl_attribs)
	);
	DWRITE_TEXT_RANGE range {
		.startPosition = static_cast<uint32_t>(start),
		.length = static_cast<uint32_t>(end - start)
	};
	if (hl_attribs->flags & HL_ATTRIB_ITALIC) {
		text_layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
	}
	if (hl_attribs->flags & HL_ATTRIB_BOLD) {
		text_layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
		if(renderer->font_size != renderer->font_size_scale_bold)
			text_layout->SetFontSize(renderer->font_size_scale_bold, range);
	}
	if (hl_attribs->flags & HL_ATTRIB_STRIKETHROUGH) {
		text_layout->SetStrikethrough(true, range);
	}
	if (hl_attribs->flags & HL_ATTRIB_UNDERLINE) {
		text_layout->SetUnderline(true, range);
	}
	if (hl_attribs->flags & HL_ATTRIB_UNDERCURL) {
		text_layout->SetUnderline(true, range);
	}
	text_layout->SetDrawingEffect(drawing_effect, range);
}

void DrawBackgroundRect(Renderer *renderer, D2D1_RECT_F rect, HighlightAttributes *hl_attribs) {
	uint32_t color = CreateBackgroundColor(renderer, hl_attribs);
	renderer->d2d_background_rect_brush->SetColor(D2D1::ColorF(color));

	renderer->d2d_context->FillRectangle(rect, renderer->d2d_background_rect_brush);
}

D2D1_RECT_F GetCursorForegroundRect(Renderer *renderer, D2D1_RECT_F cursor_bg_rect) {
	if (renderer->cursor.mode_info) {
		switch (renderer->cursor.mode_info->shape) {
		case CursorShape::None: {
		} return cursor_bg_rect;
		case CursorShape::Block: {
		} return cursor_bg_rect;
		case CursorShape::Vertical: {
			cursor_bg_rect.right = cursor_bg_rect.left + 2;
		} return cursor_bg_rect;
		case CursorShape::Horizontal: {
			cursor_bg_rect.top = cursor_bg_rect.bottom - 2;
		} return cursor_bg_rect;
		}
	}
	return cursor_bg_rect;
}

void DrawHighlightedText(Renderer *renderer, D2D1_RECT_F rect, uint32_t *text, uint32_t length, HighlightAttributes *hl_attribs) {
	ConvertToWide(renderer, text, length);

	IDWriteTextLayout *text_layout = nullptr;
	WIN_CHECK(renderer->dwrite_factory->CreateTextLayout(
		renderer->wchar_buffer,
		renderer->wchar_buffer_length,
		renderer->dwrite_text_format,
		rect.right - rect.left,
		rect.bottom - rect.top,
		&text_layout
	));
	ApplyHighlightAttributes(renderer, hl_attribs, text_layout, 0, 1);

	renderer->d2d_context->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
	text_layout->Draw(renderer, renderer->glyph_renderer, rect.left, rect.top);
	text_layout->Release();
	renderer->d2d_context->PopAxisAlignedClip();
}

LineLayoutEntry *GetLineLayoutEntry(Renderer *renderer, int row) {
	int columns = renderer->grid_cols;
	uint32_t *chars = &renderer->grid_chars[row * columns];
	CellProperty *properties = &renderer->grid_cell_properties[row * columns];
	uint64_t key = 0xCBF29CE484222325ull;
	for (int column = 0; column < columns; ++column) {
		uint64_t cell = chars[column] |
			(static_cast<uint64_t>(properties[column].hl_attrib_id) << 32) |
			(static_cast<uint64_t>(properties[column].is_wide_char) << 48);
		key = (key ^ cell) * 0x100000001B3ull;
	}
	LineLayoutEntry *entry = &renderer->line_layouts[(key ^ (key >> 32)) % MAX_LINE_LAYOUTS];
	size_t chars_size = columns * sizeof(uint32_t);
	size_t properties_size = columns * sizeof(CellProperty);
	if (entry->layout && entry->key == key && entry->columns == columns &&
		!memcmp(entry->chars, chars, chars_size) && !memcmp(entry->properties, properties, properties_size)) {
		return entry;
	}
	SafeRelease(&entry->layout);
	entry->chars = static_cast<uint32_t *>(realloc(entry->chars, chars_size));
	entry->properties = static_cast<CellProperty *>(realloc(entry->properties, properties_size));
	memcpy(entry->chars, chars, chars_size);
	memcpy(entry->properties, properties, properties_size);
	entry->columns = columns;
	entry->key = key;
	return entry;
}

void DrawGridLine(Renderer *renderer, int row) {
	int base = row * renderer->grid_cols;

	D2D1_RECT_F rect {
		.left = 0.0f,
		.top = row * renderer->font_height,
		.right = renderer->grid_cols * renderer->font_width,
		.bottom = (row * renderer->font_height) + renderer->font_height
	};

	LineLayoutEntry *entry = GetLineLayoutEntry(renderer, row);
	bool layout_cached = entry->layout != nullptr;
	if (!layout_cached) {
		IDWriteTextLayout *temp_text_layout = nullptr;
		ConvertToWide(renderer, &renderer->grid_chars[base], renderer->grid_cols);
		WIN_CHECK(renderer->dwrite_factory->CreateTextLayout(
			renderer->wchar_buffer,
			renderer->wchar_buffer_length,
			renderer->dwrite_text_format,
			rect.right - rect.left,
			rect.bottom - rect.top,
			&temp_text_layout
		));
		entry->text_length = static_cast<uint32_t>(renderer->wchar_buffer_length);
		WIN_CHECK(temp_text_layout->QueryInterface<IDWriteTextLayout1>(&entry->layout));
		temp_text_layout->Release();
	}
	size_t grid_chars_length = entry->text_length;
	IDWriteTextLayout1 *text_layout = entry->layout;

	uint16_t hl_attrib_id = renderer->grid_cell_properties[base].hl_attrib_id;
	int col_offset = 0;
	int col_offset_wchars = 0;
	for (int i = 0, i_wchars = 0; i < renderer->grid_cols;
		i_wchars += ContainsSurrogatePair(renderer->grid_chars[base + i]) ? 2 : 1, ++i) {

		// Add spacing for wide chars
		if (!layout_cached && renderer->grid_cell_properties[base + i].is_wide_char && i + 1 < renderer->grid_cols) {
			float char_width = GetCellTextWidth(renderer, &renderer->grid_chars[base + i], true);
			DWRITE_TEXT_RANGE range { .startPosition = static_cast<uint32_t>(i_wchars), .length = 1 };
			text_layout->SetCharacterSpacing(0, (renderer->font_width * 2) - char_width, 0, range);
		}

		// Add spacing for unicode chars. These characters are still single char width,
		// but some of them by default will take up a bit more or less, leading to issues.
		// So we realign them here.
		else if (!layout_cached && renderer->grid_chars[base + i] > 0xFF) {
			float char_width = GetCellTextWidth(renderer, &renderer->grid_chars[base + i], false);
			if(abs(char_width - renderer->font_width) > 0.01f) {
				DWRITE_TEXT_RANGE range { .startPosition = static_cast<uint32_t>(i_wchars), .length = 1 };
				text_layout->SetCharacterSpacing(0, renderer->font_width - char_width, 0, range);
			}
		}
		else if (!layout_cached) {
			// Add spacing for character not existing in this font
			if (IsGlyphMissing(renderer, renderer->grid_chars[base + i]))
			{
				float char_width = GetCellTextWidth(renderer, &renderer->grid_chars[base + i], false);
				float d_width = renderer->font_width - char_width;
				if (d_width > 0)
				{
					DWRITE_TEXT_RANGE range{ .startPosition = static_cast<uint32_t>(i_wchars), .length = 1 };
					text_layout->SetCharacterSpacing(d_width / 2, d_width / 2, 0, range);
				}
			}
		}

		// Check if the attributes change, 
		// if so draw until this point and continue with the new attributes
		if (renderer->grid_cell_properties[base + i].hl_attrib_id != hl_attrib_id) {
			D2D1_RECT_F bg_rect {
				.left = col_offset * renderer->font_width,
				.top = row * renderer->font_height,
				.right = col_offset * renderer->font_width + renderer->font_width * (i - col_offset),
				.bottom = (row * renderer->font_height) + renderer->font_height
			};
			DrawBackgroundRect(renderer, bg_rect, &renderer->hl_attribs[hl_attrib_id]);
			if (!layout_cached) {
				ApplyHighlightAttributes(renderer, &renderer->hl_attribs[hl_attrib_id], text_layout, col_offset_wchars, i_wchars);
			}

			hl_attrib_id = renderer->grid_cell_properties[base + i].hl_attrib_id;
			col_offset = i;
			col_offset_wchars = i_wchars;
		}
	}
	
	// Draw the remaining columns, there is always atleast the last column to draw,
	// but potentially more in case the last X columns share the same hl_attrib
	D2D1_RECT_F last_rect = rect;
	last_rect.left = col_offset * renderer->font_width;
	DrawBackgroundRect(renderer, last_rect, &renderer->hl_attribs[hl_attrib_id]);
	if (!layout_cached) {
		ApplyHighlightAttributes(renderer, &renderer->hl_attribs[hl_attrib_id], text_layout, col_offset_wchars, grid_chars_length);
	}

	renderer->d2d_context->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
	if (!layout_cached && renderer->disable_ligatures) {
		text_layout->SetTypography(renderer->dwrite_typography, DWRITE_TEXT_RANGE { 
			.startPosition = 0, 
			.length = static_cast<uint32_t>(grid_chars_length)
		});
	}
	text_layout->Draw(renderer, renderer->glyph_renderer, 0.0f, rect.top);
	renderer->d2d_context->PopAxisAlignedClip();
	if (renderer->neon_text && renderer->d2d_neon_bitmap) {
		ID2D1Bitmap1 *target = renderer->animation_active ? renderer->d2d_grid_bitmap : renderer->d2d_target_bitmap;
		renderer->d2d_context->SetTarget(renderer->d2d_neon_bitmap);
		renderer->d2d_context->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
		text_layout->Draw(renderer, renderer->glyph_renderer, 0.0f, rect.top);
		renderer->d2d_context->PopAxisAlignedClip();
		renderer->d2d_context->SetTarget(target);
	}
}

void MarkRowDirty(Renderer *renderer, int row) {
	if (renderer->dirty_rows && row >= 0 && row < renderer->grid_rows) {
		renderer->dirty_rows[row] = true;
	}
}

// The cursor is drawn into the grid without the animations, so its row has to be
// drawn again when it moves away
void MarkCursorRowDirty(Renderer *renderer) {
	if (!renderer->animation_active) {
		MarkRowDirty(renderer, renderer->cursor.row);
	}
}

void DrawDirtyGridLines(Renderer *renderer) {
	if (!renderer->dirty_rows) return;

	for (int i = 0; i < renderer->grid_rows; ++i) {
		if (renderer->draws_invalidated || renderer->dirty_rows[i]) {
			renderer->dirty_rows[i] = false;
			DrawGridLine(renderer, i);
		}
	}
	renderer->draws_invalidated = false;
}

void UpdateNeonEffects(Renderer *renderer) {
	if (!renderer->d2d_neon_gain) return;
	float intensity = renderer->neon_intensity;
	D2D1_MATRIX_5X4_F gain {
		intensity, 0, 0, 0,
		0, intensity, 0, 0,
		0, 0, intensity, 0,
		0, 0, 0, intensity,
		0, 0, 0, 0
	};
	WIN_CHECK(renderer->d2d_neon_gain->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, gain));
	WIN_CHECK(renderer->d2d_neon_blur_near->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
		renderer->neon_radius * 0.5f));
	WIN_CHECK(renderer->d2d_neon_blur_far->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
		renderer->neon_radius));
}

void CreateNeonResources(Renderer *renderer) {
	D2D1_BITMAP_PROPERTIES1 properties {
		.pixelFormat = D2D1_PIXEL_FORMAT {
			.format = DXGI_FORMAT_B8G8R8A8_UNORM,
			.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED
		},
		.dpiX = DEFAULT_DPI,
		.dpiY = DEFAULT_DPI,
		.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET
	};
	D2D1_SIZE_U size {
		.width = max(renderer->pixel_size.width, 1u),
		.height = max(renderer->pixel_size.height, 1u)
	};
	WIN_CHECK(renderer->d2d_context->CreateBitmap(size, nullptr, 0, &properties, &renderer->d2d_neon_bitmap));
	WIN_CHECK(renderer->d2d_context->CreateEffect(CLSID_D2D1ColorMatrix, &renderer->d2d_neon_gain));
	WIN_CHECK(renderer->d2d_context->CreateEffect(CLSID_D2D1GaussianBlur, &renderer->d2d_neon_blur_near));
	WIN_CHECK(renderer->d2d_context->CreateEffect(CLSID_D2D1GaussianBlur, &renderer->d2d_neon_blur_far));
	renderer->d2d_neon_gain->SetInput(0, renderer->d2d_neon_bitmap);
	renderer->d2d_neon_blur_near->SetInputEffect(0, renderer->d2d_neon_gain);
	renderer->d2d_neon_blur_far->SetInputEffect(0, renderer->d2d_neon_gain);
	UpdateNeonEffects(renderer);
}

void DrawNeonGlow(Renderer *renderer) {
	// Blur the text into the surrounding background, then restore the sharp
	// foreground on top so the glow cannot wash out syntax highlight colors.
	renderer->d2d_context->DrawImage(renderer->d2d_neon_blur_far, nullptr, nullptr,
		D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_PLUS);
	renderer->d2d_context->DrawImage(renderer->d2d_neon_blur_near, nullptr, nullptr,
		D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_PLUS);
	renderer->d2d_context->DrawImage(renderer->d2d_neon_bitmap, nullptr, nullptr,
		D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
}

bool IsSurrogatePair(wchar_t left, wchar_t right) {
	return (0xD800 <= left && left <= 0xDBFF) && (0xDC00 <= right && right <= 0xDFFF);
}

void DrawGridLines(Renderer *renderer, mpack_node_t grid_lines) {
	assert(renderer->grid_chars != nullptr);
	assert(renderer->grid_cell_properties != nullptr);
	
	int grid_size = renderer->grid_cols * renderer->grid_rows;
	size_t line_count = mpack_node_array_length(grid_lines);
	for (size_t i = 1; i < line_count; ++i) {
		mpack_node_t grid_line = mpack_node_array_at(grid_lines, i);

		int row = MPackIntFromArray(grid_line, 1);
		int col_start = MPackIntFromArray(grid_line, 2);

		mpack_node_t cell_array = mpack_node_array_at(grid_line, 3);
		size_t cell_array_length = mpack_node_array_length(cell_array);

		int hl_attrib_id = 0;
		int offset = row * renderer->grid_cols + col_start;
		for (size_t j = 0; j < cell_array_length; ++j) {
			mpack_node_t cell = mpack_node_array_at(cell_array, j);
			size_t cell_length = mpack_node_array_length(cell);

			mpack_node_t text = mpack_node_array_at(cell, 0);
			const char *str = mpack_node_str(text);

			if (cell_length > 1) {
				hl_attrib_id = MPackIntFromArray(cell, 1);
			}

			int repeat = 1;
			if (cell_length > 2) {
				repeat = MPackIntFromArray(cell, 2);
			}

			int strlen = static_cast<int>(mpack_node_strlen(text));
			if (strlen == 0) {
				// This is the right part of the wide char. Sadly grid_line
				// event can be splitted at the middle of wide character.

				// Be careful not to overwrite right half of surrogate pair.
				// It never happens that offset == 0, since it is the right
				// half of wide char, but add check for safety.
				if (offset == 0 || !IsSurrogatePair(renderer->grid_chars[offset - 1], renderer->grid_chars[offset])) {
					renderer->grid_chars[offset] = L'\0';
				}

				// This cell itself is not a wide character.
				renderer->grid_cell_properties[offset].is_wide_char = false;

				// Adjust properties. Again it never happens that offset == 0,
				// since it is the right half of wide char, but adding check
				// for safety.
				if (offset > 0) {
					// Set is_wide_char flag for the left cell to true.
					renderer->grid_cell_properties[offset - 1].is_wide_char = true;

					// Inherit hl_attrib_id from left half.
					renderer->grid_cell_properties[offset].hl_attrib_id = renderer->grid_cell_properties[offset - 1].hl_attrib_id;
				}

				++offset;
			} else {
				// This is single width character or left half cell of wide
				// character.

				// Left cell should not be a wide character, so reset the
				// flag. This time checking offset > 0 is mandatory.
				if (offset > 0) {
					renderer->grid_cell_properties[offset - 1].is_wide_char = false;
				}

				uint32_t grid_char;
				wchar_t buffer[2];
				int wstrlen = MultiByteToWideChar(CP_UTF8, 0, str, strlen, buffer, 2);
				if (wstrlen == 2) {
					// If the str takes two wchars, it must be a surrogate pair.
					bool is_surrogate_pair = IsSurrogatePair(buffer[0], buffer[1]);
					if (is_surrogate_pair) {
						// Pack the surrogate pair into a single grid cell.
						grid_char = (buffer[0] << 16) | buffer[1];
					} else {
						// This is an unsupported character (ie: a diacritic), draw a box here instead.
						grid_char = 0x25a1;
					}
				} else {
					grid_char = buffer[0];
				}

				// Wide character will never be repeated, so we don't have to
				// handle wide character specially.
				for (int k = 0; k < repeat; ++k) {
					renderer->grid_chars[offset] = grid_char;
					renderer->grid_cell_properties[offset].hl_attrib_id = hl_attrib_id;

					// Here we set is_wide_char to be always false. This is
					// because if it is actually a wide character, then the
					// right half of the char, empty string, should be appear
					// soon, and the flag will be set there (first branch of
					// this `if`).
					renderer->grid_cell_properties[offset].is_wide_char = false;

					++offset;
				}
			}
		}

		MarkRowDirty(renderer, row);
	}
}

struct CursorCell {
	int grid_offset;
	// 2 for wide chars
	int width;
	HighlightAttributes hl_attribs;
};

bool GetCursorCell(Renderer *renderer, CursorCell *cell) {
	if (!renderer->cursor.mode_info || !renderer->grid_initialized) return false;
	if (renderer->cursor.row < 0 || renderer->cursor.row >= renderer->grid_rows ||
		renderer->cursor.col < 0 || renderer->cursor.col >= renderer->grid_cols) {
		return false;
	}
	cell->grid_offset = renderer->cursor.row * renderer->grid_cols + renderer->cursor.col;
	cell->width = renderer->grid_cell_properties[cell->grid_offset].is_wide_char ? 2 : 1;

	cell->hl_attribs = renderer->hl_attribs[renderer->cursor.mode_info->hl_attrib_id];

	// Inherit GUI options for char under cursor (like italic)
	int hl_attrib_id_under_cursor = renderer->grid_cell_properties[cell->grid_offset].hl_attrib_id;
	HighlightAttributes under_cursor_hl_attribs = renderer->hl_attribs[hl_attrib_id_under_cursor];
	cell->hl_attribs.flags = under_cursor_hl_attribs.flags;

	if (renderer->cursor.mode_info->hl_attrib_id == 0) {
		cell->hl_attribs.flags |= HL_ATTRIB_REVERSE;
	}
	return true;
}

void DrawCursor(Renderer *renderer) {
	CursorCell cell;
	if (!GetCursorCell(renderer, &cell)) return;

	D2D1_RECT_F cursor_rect {
		.left = renderer->cursor.col * renderer->font_width,
		.top = renderer->cursor.row * renderer->font_height,
		.right = renderer->cursor.col * renderer->font_width + renderer->font_width * cell.width,
		.bottom = (renderer->cursor.row * renderer->font_height) + renderer->font_height
	};
	D2D1_RECT_F cursor_fg_rect = GetCursorForegroundRect(renderer, cursor_rect);
	DrawBackgroundRect(renderer, cursor_fg_rect, &cell.hl_attribs);

	if (renderer->cursor.mode_info->shape == CursorShape::Block) {
		DrawHighlightedText(renderer, cursor_fg_rect, &renderer->grid_chars[cell.grid_offset],
			cell.width, &cell.hl_attribs);
	}
}

ID2D1PathGeometry *CreateQuadGeometry(Renderer *renderer, const D2D1_POINT_2F points[4]) {
	ID2D1PathGeometry *geometry;
	if (FAILED(renderer->d2d_factory->CreatePathGeometry(&geometry))) return nullptr;

	ID2D1GeometrySink *sink;
	if (FAILED(geometry->Open(&sink))) {
		geometry->Release();
		return nullptr;
	}
	sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
	sink->AddLines(&points[1], 3);
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	HRESULT hr = sink->Close();
	sink->Release();
	if (FAILED(hr)) {
		geometry->Release();
		return nullptr;
	}
	return geometry;
}

// Only the outline of the cursor is drawn while the window is unfocused
ID2D1PathGeometry *CreateUnfocusedOutline(Renderer *renderer, ID2D1PathGeometry *cursor_geometry, float outline_width) {
	const CursorCorner *corners = renderer->cursor_animation->corners;
	D2D1_POINT_2F inner_points[4] = {
		{ corners[0].x + outline_width, corners[0].y + outline_width },
		{ corners[1].x - outline_width, corners[1].y + outline_width },
		{ corners[2].x - outline_width, corners[2].y - outline_width },
		{ corners[3].x + outline_width, corners[3].y - outline_width }
	};
	ID2D1PathGeometry *inner = CreateQuadGeometry(renderer, inner_points);
	if (!inner) return nullptr;

	ID2D1PathGeometry *outline = nullptr;
	ID2D1GeometrySink *sink;
	if (SUCCEEDED(renderer->d2d_factory->CreatePathGeometry(&outline)) && SUCCEEDED(outline->Open(&sink))) {
		HRESULT hr = cursor_geometry->CombineWithGeometry(inner, D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink);
		if (SUCCEEDED(hr)) {
			hr = sink->Close();
		}
		sink->Release();
		if (FAILED(hr)) {
			SafeRelease(&outline);
		}
	}
	else {
		SafeRelease(&outline);
	}
	inner->Release();
	return outline;
}

// The cell is null for the composition caret
void DrawAnimatedCursor(Renderer *renderer, CursorCell *cell, const CursorAnimationTarget *target, float opacity) {
	const CursorAnimation *animation = renderer->cursor_animation;
	ID2D1DeviceContext4 *context = renderer->d2d_context;
	if (opacity <= 0.0f) return;

	// Snap the corners to whole pixels relative to the cell, so the resting cursor is crisp.
	// The thin bar and underline cursors snap to whole device pixels instead, otherwise
	// antialiasing spreads their 2 pixels over 3 and blends their color with the background.
	bool thin = target->shape == CursorShape::Vertical || target->shape == CursorShape::Horizontal;
	float fract_x = thin ? 0.0f : target->x - floorf(target->x);
	float fract_y = thin ? 0.0f : target->y - floorf(target->y);
	D2D1_POINT_2F points[4];
	for (int i = 0; i < 4; ++i) {
		points[i] = D2D1_POINT_2F {
			.x = roundf(animation->corners[i].x - fract_x) + fract_x,
			.y = roundf(animation->corners[i].y - fract_y) + fract_y
		};
	}

	ID2D1PathGeometry *geometry = CreateQuadGeometry(renderer, points);
	if (!geometry) return;

	if (!renderer->window_focused && target->shape == CursorShape::Block && animation->settings.enabled) {
		float outline_width = animation->settings.unfocused_outline_width * renderer->font_size;
		if (ID2D1PathGeometry *outline = CreateUnfocusedOutline(renderer, geometry, outline_width)) {
			geometry->Release();
			geometry = outline;
		}
	}

	// Fading with smooth blinking
	if (opacity < 1.0f) {
		context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_ALIASED,
			D2D1::IdentityMatrix(), opacity), nullptr);
	}

	D2D1_ANTIALIAS_MODE antialias_mode = animation->settings.antialiasing ?
		D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED;
	context->SetAntialiasMode(antialias_mode);
	renderer->d2d_background_rect_brush->SetColor(D2D1::ColorF(target->color));
	context->FillGeometry(geometry, renderer->d2d_background_rect_brush);
	context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

	// The char under the cursor, in the cursor colors where the cursor covers it. The composition
	// caret has no cell, the grid char is hidden under the composition string there.
	if (cell && target->shape != CursorShape::None) {
		context->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geometry, antialias_mode), nullptr);
		D2D1_RECT_F cell_rect {
			.left = target->x,
			.top = target->y,
			.right = target->x + renderer->font_width * cell->width,
			.bottom = target->y + target->height
		};
		DrawHighlightedText(renderer, cell_rect, &renderer->grid_chars[cell->grid_offset], cell->width, &cell->hl_attribs);
		context->PopLayer();
	}

	if (opacity < 1.0f) {
		context->PopLayer();
	}
	geometry->Release();
}

void DrawCursorVfx(Renderer *renderer, uint32_t cursor_color) {
	const CursorAnimation *animation = renderer->cursor_animation;
	const CursorAnimationSettings *settings = &animation->settings;
	ID2D1DeviceContext4 *context = renderer->d2d_context;
	ID2D1SolidColorBrush *brush = renderer->d2d_background_rect_brush;
	float stroke_width = renderer->font_height * 0.2f;

	context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	for (int i = 0; i < animation->vfx_count; ++i) {
		const CursorVfx *vfx = &animation->vfxs[i];
		switch (vfx->mode) {
		case CursorVfxMode::SonicBoom:
		case CursorVfxMode::Ripple:
		case CursorVfxMode::Wireframe: {
			if (vfx->t >= 1.0f) break;

			// Grows while fading out
			float opacity = settings->vfx_opacity * (1.0f - vfx->t * vfx->t) / 255.0f;
			brush->SetColor(D2D1::ColorF(cursor_color, opacity));
			float half_size = vfx->t * renderer->font_height * 3.0f * 0.5f;
			D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(vfx->center_x, vfx->center_y), half_size, half_size);
			if (vfx->mode == CursorVfxMode::SonicBoom) {
				context->FillEllipse(ellipse, brush);
			}
			else if (vfx->mode == CursorVfxMode::Ripple) {
				context->DrawEllipse(ellipse, brush, stroke_width);
			}
			else {
				context->DrawRectangle(D2D1::RectF(vfx->center_x - half_size, vfx->center_y - half_size,
					vfx->center_x + half_size, vfx->center_y + half_size), brush, stroke_width);
			}
		} break;
		case CursorVfxMode::Railgun:
		case CursorVfxMode::Torpedo:
		case CursorVfxMode::PixieDust: {
			if (settings->vfx_particle_lifetime <= 0.0f) break;

			for (int j = 0; j < vfx->particle_count; ++j) {
				const CursorParticle *particle = &vfx->particles[j];
				float lifetime = particle->lifetime / settings->vfx_particle_lifetime;
				brush->SetColor(D2D1::ColorF(particle->color, lifetime * settings->vfx_opacity / 255.0f));

				if (vfx->mode == CursorVfxMode::PixieDust) {
					float half_size = renderer->font_width * 0.2f * 0.5f;
					context->FillRectangle(D2D1::RectF(particle->x - half_size, particle->y - half_size,
						particle->x + half_size, particle->y + half_size), brush);
				}
				else {
					// Rings shrinking as they fade out
					float radius = renderer->font_width * 0.5f * lifetime * 0.5f;
					context->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(particle->x, particle->y), radius, radius),
						brush, stroke_width);
				}
			}
		} break;
		}
	}
	context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
}

bool UpdateGridSize(Renderer *renderer, mpack_node_t grid_resize) {
	mpack_node_t grid_resize_params = mpack_node_array_at(grid_resize, 1);
	int grid_cols = MPackIntFromArray(grid_resize_params, 1);
	int grid_rows = MPackIntFromArray(grid_resize_params, 2);

	if (renderer->grid_chars == nullptr ||
		renderer->wchar_buffer == nullptr ||
		renderer->grid_cell_properties == nullptr ||
		renderer->grid_cols != grid_cols ||
		renderer->grid_rows != grid_rows) {
		
		renderer->grid_cols = grid_cols;
		renderer->grid_rows = grid_rows;

		free(renderer->grid_chars);
		renderer->grid_chars = static_cast<uint32_t *>(malloc(static_cast<size_t>(grid_cols) * grid_rows * sizeof(uint32_t)));
		// Initialize all grid character to a space. An empty
		// grid cell is equivalent to a space in a text layout
		for (int i = 0; i < grid_cols * grid_rows; ++i) {
			renderer->grid_chars[i] = L' ';
		}
		free(renderer->grid_cell_properties);
		renderer->grid_cell_properties = static_cast<CellProperty *>(calloc(static_cast<size_t>(grid_cols) * grid_rows, sizeof(CellProperty)));
		free(renderer->wchar_buffer);
		renderer->wchar_buffer = static_cast<wchar_t *>(malloc(static_cast<size_t>(grid_cols * 2) * sizeof(wchar_t)));
		free(renderer->dirty_rows);
		renderer->dirty_rows = static_cast<bool *>(calloc(grid_rows, sizeof(bool)));
		renderer->draws_invalidated = true;
		ScrollAnimationReset(renderer->scroll_animation);

		renderer->grid_initialized = true;
		return true;
	}

	return false;
}

void UpdateCursorPos(Renderer *renderer, mpack_node_t cursor_goto) {
	mpack_node_t cursor_goto_params = mpack_node_array_at(cursor_goto, 1);
	renderer->cursor.row = MPackIntFromArray(cursor_goto_params, 1);
	renderer->cursor.col = MPackIntFromArray(cursor_goto_params, 2);
}

int CompositionRow(Renderer *renderer) {
	return max(0, min(renderer->cursor.row, renderer->grid_rows - 1));
}

// Lays out the composition string at the cursor, shifting it to the left if it would
// otherwise overflow the grid
IDWriteTextLayout *CreateCompositionLayout(Renderer *renderer, const wchar_t *text, uint32_t length, float *origin_x) {
	float grid_width = renderer->grid_cols * renderer->font_width;

	IDWriteTextLayout *text_layout = nullptr;
	if (FAILED(renderer->dwrite_factory->CreateTextLayout(text, length, renderer->dwrite_text_format,
		max(grid_width, 1.0f), renderer->font_height, &text_layout))) {
		return nullptr;
	}

	DWRITE_TEXT_METRICS metrics;
	WIN_CHECK(text_layout->GetMetrics(&metrics));
	float text_width = metrics.widthIncludingTrailingWhitespace;

	*origin_x = renderer->cursor.col * renderer->font_width;
	if (*origin_x + text_width > grid_width) {
		*origin_x = max(0.0f, grid_width - text_width);
	}
	return text_layout;
}

float CompositionPositionToX(IDWriteTextLayout *text_layout, uint32_t length, uint32_t position) {
	DWRITE_HIT_TEST_METRICS metrics;
	float x, y;
	if (position == 0) {
		WIN_CHECK(text_layout->HitTestTextPosition(0, false, &x, &y, &metrics));
	}
	else {
		WIN_CHECK(text_layout->HitTestTextPosition(min(position, length) - 1, true, &x, &y, &metrics));
	}
	return x;
}

RECT RendererGetCompositionTextRect(Renderer *renderer, const wchar_t *text, uint32_t length, uint32_t start, uint32_t end) {
	float top = CompositionRow(renderer) * renderer->font_height;
	float left = renderer->cursor.col * renderer->font_width;
	float right = left + renderer->font_width;

	float origin_x;
	IDWriteTextLayout *text_layout;
	if (length > 0 && renderer->grid_initialized &&
		(text_layout = CreateCompositionLayout(renderer, text, length, &origin_x))) {
		left = origin_x + CompositionPositionToX(text_layout, length, start);
		right = origin_x + CompositionPositionToX(text_layout, length, end);
		right = max(right, left + 1.0f);
		text_layout->Release();
	}

	return RECT {
		.left = static_cast<LONG>(floorf(left)),
		.top = static_cast<LONG>(floorf(top)),
		.right = static_cast<LONG>(ceilf(right)),
		.bottom = static_cast<LONG>(ceilf(top + renderer->font_height))
	};
}

// Restores the grid line covered by the previously drawn composition string
void ClearComposition(Renderer *renderer) {
	if (!renderer->composition_drawn) return;

	renderer->composition_drawn = false;
	MarkRowDirty(renderer, renderer->composition_drawn_row);
}

void DrawComposition(Renderer *renderer) {
	if (renderer->composition_length == 0 || !renderer->grid_initialized) return;

	float origin_x;
	IDWriteTextLayout *text_layout = CreateCompositionLayout(renderer,
		renderer->composition_text, renderer->composition_length, &origin_x);
	if (!text_layout) return;

	int row = CompositionRow(renderer);
	DWRITE_TEXT_METRICS metrics;
	WIN_CHECK(text_layout->GetMetrics(&metrics));
	D2D1_RECT_F rect {
		.left = origin_x,
		.top = row * renderer->font_height,
		.right = origin_x + metrics.widthIncludingTrailingWhitespace,
		.bottom = (row * renderer->font_height) + renderer->font_height
	};
	D2D1_RECT_F clip_rect = rect;
	clip_rect.right = max(rect.right, renderer->grid_cols * renderer->font_width);

	HighlightAttributes *hl_attribs = &renderer->hl_attribs[0];
	uint32_t foreground = CreateForegroundColor(renderer, hl_attribs);
	uint32_t special = CreateSpecialColor(renderer, hl_attribs);
	DrawBackgroundRect(renderer, rect, hl_attribs);
	ApplyHighlightAttributes(renderer, hl_attribs, text_layout, 0, renderer->composition_length);

	// Without display attributes from the IME, underline the whole composition
	CompositionClause whole_text {
		.start = 0,
		.end = renderer->composition_length,
		.line_style = CompositionLineStyle::Solid
	};
	const CompositionClause *clauses = renderer->composition_clause_count ? renderer->composition_clauses : &whole_text;
	uint32_t clause_count = renderer->composition_clause_count ? renderer->composition_clause_count : 1;

	renderer->d2d_context->PushAxisAlignedClip(clip_rect, D2D1_ANTIALIAS_MODE_ALIASED);

	// Clause backgrounds and text colors as declared by the IME
	for (uint32_t i = 0; i < clause_count; ++i) {
		uint32_t start = min(clauses[i].start, renderer->composition_length);
		uint32_t end = min(clauses[i].end, renderer->composition_length);
		if (start >= end) continue;

		if (clauses[i].has_background_color) {
			D2D1_RECT_F background_rect {
				.left = origin_x + CompositionPositionToX(text_layout, renderer->composition_length, start),
				.top = rect.top,
				.right = origin_x + CompositionPositionToX(text_layout, renderer->composition_length, end),
				.bottom = rect.bottom
			};
			renderer->d2d_background_rect_brush->SetColor(D2D1::ColorF(clauses[i].background_color));
			renderer->d2d_context->FillRectangle(background_rect, renderer->d2d_background_rect_brush);
		}
		if (clauses[i].has_text_color) {
			GlyphDrawingEffect *drawing_effect = new GlyphDrawingEffect(clauses[i].text_color, special);
			text_layout->SetDrawingEffect(drawing_effect, DWRITE_TEXT_RANGE { .startPosition = start, .length = end - start });
		}
	}

	text_layout->Draw(renderer, renderer->glyph_renderer, origin_x, rect.top);

	float thin_line = max(1.0f, roundf(renderer->dpi_scale));
	for (uint32_t i = 0; i < clause_count; ++i) {
		uint32_t start = min(clauses[i].start, renderer->composition_length);
		uint32_t end = min(clauses[i].end, renderer->composition_length);
		if (start >= end || clauses[i].line_style == CompositionLineStyle::None) continue;

		// Leave a small gap between clauses so they can be told apart
		float left = origin_x + CompositionPositionToX(text_layout, renderer->composition_length, start) + thin_line;
		float right = origin_x + CompositionPositionToX(text_layout, renderer->composition_length, end) - thin_line;
		float thickness = clauses[i].bold_line ? thin_line * 2.0f : thin_line;
		float bottom = rect.bottom;
		uint32_t line_color = clauses[i].has_line_color ? clauses[i].line_color :
			(clauses[i].has_text_color ? clauses[i].text_color : foreground);
		renderer->d2d_background_rect_brush->SetColor(D2D1::ColorF(line_color));

		const auto FillSegment = [&](float x0, float x1, float y0, float y1) {
			D2D1_RECT_F segment { .left = x0, .top = y0, .right = min(x1, right), .bottom = y1 };
			if (segment.right > segment.left) {
				renderer->d2d_context->FillRectangle(segment, renderer->d2d_background_rect_brush);
			}
		};

		switch (clauses[i].line_style) {
		case CompositionLineStyle::Solid: {
			FillSegment(left, right, bottom - thickness, bottom);
		} break;
		case CompositionLineStyle::Dot: {
			for (float x = left; x < right; x += thickness * 2.0f) {
				FillSegment(x, x + thickness, bottom - thickness, bottom);
			}
		} break;
		case CompositionLineStyle::Dash: {
			for (float x = left; x < right; x += thickness * 5.0f) {
				FillSegment(x, x + thickness * 3.0f, bottom - thickness, bottom);
			}
		} break;
		case CompositionLineStyle::Squiggle: {
			// Alternate between a low and a high step to form a wave
			float step = thickness * 2.0f;
			bool high = false;
			for (float x = left; x < right; x += step, high = !high) {
				float offset = high ? thickness : 0.0f;
				FillSegment(x, x + step, bottom - thickness - offset, bottom - offset);
			}
		} break;
		case CompositionLineStyle::None: {
		} break;
		}
	}

	// The caret takes the color and the 2 pixel width of the nvim bar cursor it stands in for.
	// With the animations, the animated cursor glides to it when composing the frame instead.
	float caret_x = origin_x + CompositionPositionToX(text_layout, renderer->composition_length, renderer->composition_caret);
	renderer->composition_caret_x = caret_x;
	if (!renderer->animation_active) {
		uint32_t caret_color = foreground;
		CursorCell cursor_cell;
		if (GetCursorCell(renderer, &cursor_cell)) {
			caret_color = CreateBackgroundColor(renderer, &cursor_cell.hl_attribs);
		}
		renderer->d2d_background_rect_brush->SetColor(D2D1::ColorF(caret_color));
		D2D1_RECT_F caret_rect {
			.left = caret_x,
			.top = rect.top,
			.right = caret_x + 2.0f,
			.bottom = rect.bottom
		};
		renderer->d2d_context->FillRectangle(caret_rect, renderer->d2d_background_rect_brush);
	}

	renderer->d2d_context->PopAxisAlignedClip();
	text_layout->Release();

	renderer->composition_drawn = true;
	renderer->composition_drawn_row = row;
}

void RendererSetComposition(Renderer *renderer, const wchar_t *text, uint32_t length, uint32_t caret,
	const CompositionClause *clauses, uint32_t clause_count) {
	if (length > renderer->composition_capacity) {
		wchar_t *new_text = static_cast<wchar_t *>(realloc(renderer->composition_text, length * sizeof(wchar_t)));
		if (!new_text) {
			length = 0;
		}
		else {
			renderer->composition_text = new_text;
			renderer->composition_capacity = length;
		}
	}

	if (length > 0) {
		memcpy(renderer->composition_text, text, length * sizeof(wchar_t));
	}
	renderer->composition_length = length;
	renderer->composition_caret = min(caret, length);

	renderer->composition_clause_count = min(clause_count, static_cast<uint32_t>(MAX_COMPOSITION_CLAUSES));
	if (renderer->composition_clause_count > 0) {
		memcpy(renderer->composition_clauses, clauses, renderer->composition_clause_count * sizeof(CompositionClause));
	}

	// Redraw right away unless nvim is in the middle of a redraw,
	// in which case the composition gets drawn on the next flush
	if (renderer->grid_initialized && renderer->dxgi_swapchain && !renderer->draw_active) {
		RendererFlush(renderer);
	}
}

void UpdateWindowTitle(Renderer *renderer, mpack_node_t set_title) {
	// Get new title
	mpack_node_t params = mpack_node_array_at(set_title, 1);
	mpack_node_t value = mpack_node_array_at(params, 0);
	const char *new_title = mpack_node_str(value);
	int len = mpack_node_strlen(value);

	// Append " - Ndx" to the title. If title is empty, do not add " - ".
	const char *append = len == 0 ? "Ndx" : " - Ndx";
	size_t add_len = strlen(append);
	size_t bytes = len + add_len; // No need for '\0'
	char *buf = static_cast<char *>(malloc(bytes));
	memcpy(buf, new_title, len);
	memcpy(buf + len, append, add_len);

	// Convert to wide string
	int wstrlen = MultiByteToWideChar(CP_UTF8, 0, buf, len + add_len, NULL, 0);
	wchar_t *wbuf = static_cast<wchar_t *>(malloc((wstrlen + 1) * sizeof(wchar_t)));
	MultiByteToWideChar(CP_UTF8, 0, buf, len + add_len, wbuf, wstrlen);
	wbuf[wstrlen] = '\0';

	// Update title bar text
	SetWindowText(renderer->hwnd, wbuf);

	free(buf);
	free(wbuf);
}

void UpdateCursorMode(Renderer *renderer, mpack_node_t mode_change) {
	mpack_node_t mode_change_params = mpack_node_array_at(mode_change, 1);
	renderer->cursor.mode_info = &renderer->cursor_mode_infos[mpack_node_array_at(mode_change_params, 1).data->value.u];

	mpack_node_t mode_name = mpack_node_array_at(mode_change_params, 0);
	const char *mode_name_str = mpack_node_str(mode_name);
	size_t mode_name_length = mpack_node_strlen(mode_name);
	renderer->in_insert_mode = mode_name_length == 6 && !strncmp(mode_name_str, "insert", 6);
	renderer->in_cmdline_mode = mode_name_length >= 7 && !strncmp(mode_name_str, "cmdline", 7);
}

void UpdateCursorModeInfos(Renderer *renderer, mpack_node_t mode_info_set_params) {
	mpack_node_t mode_info_params = mpack_node_array_at(mode_info_set_params, 1);
	mpack_node_t mode_infos = mpack_node_array_at(mode_info_params, 1);
	size_t mode_infos_length = mpack_node_array_length(mode_infos);
	assert(mode_infos_length <= MAX_CURSOR_MODE_INFOS);

	for (size_t i = 0; i < mode_infos_length; ++i) {
		mpack_node_t mode_info_map = mpack_node_array_at(mode_infos, i);

		renderer->cursor_mode_infos[i].shape = CursorShape::None;
		mpack_node_t cursor_shape = mpack_node_map_cstr_optional(mode_info_map, "cursor_shape");
		if (!mpack_node_is_missing(cursor_shape)) {
			const char *cursor_shape_str = mpack_node_str(cursor_shape);
			size_t strlen = mpack_node_strlen(cursor_shape);
			if (!strncmp(cursor_shape_str, "block", strlen)) {
				renderer->cursor_mode_infos[i].shape = CursorShape::Block;
			}
			else if (!strncmp(cursor_shape_str, "vertical", strlen)) {
				renderer->cursor_mode_infos[i].shape = CursorShape::Vertical;
			}
			else if (!strncmp(cursor_shape_str, "horizontal", strlen)) {
				renderer->cursor_mode_infos[i].shape = CursorShape::Horizontal;
			}
		}

		renderer->cursor_mode_infos[i].hl_attrib_id = 0;
		mpack_node_t hl_attrib_index = mpack_node_map_cstr_optional(mode_info_map, "attr_id");
		if (!mpack_node_is_missing(hl_attrib_index)) {
			renderer->cursor_mode_infos[i].hl_attrib_id = static_cast<int>(hl_attrib_index.data->value.i);
		}

		const auto GetBlinkTime = [&](const char *name) {
			mpack_node_t time = mpack_node_map_cstr_optional(mode_info_map, name);
			return mpack_node_is_missing(time) ? 0 : static_cast<int>(NodeToFloat(time, 0.0f));
		};
		renderer->cursor_mode_infos[i].blinkwait = GetBlinkTime("blinkwait");
		renderer->cursor_mode_infos[i].blinkon = GetBlinkTime("blinkon");
		renderer->cursor_mode_infos[i].blinkoff = GetBlinkTime("blinkoff");
	}
}

void ScrollRegion(Renderer *renderer, mpack_node_t scroll_region) {
	size_t scroll_count = mpack_node_array_length(scroll_region);

	for (size_t i = 1; i < scroll_count; ++i) {
		mpack_node_t scroll_region_params = mpack_node_array_at(scroll_region, i);

		int64_t top = mpack_node_array_at(scroll_region_params, 1).data->value.i;
		int64_t bottom = mpack_node_array_at(scroll_region_params, 2).data->value.i;
		int64_t left = mpack_node_array_at(scroll_region_params, 3).data->value.i;
		int64_t right = mpack_node_array_at(scroll_region_params, 4).data->value.i;
		int64_t rows = mpack_node_array_at(scroll_region_params, 5).data->value.i;
		int64_t cols = mpack_node_array_at(scroll_region_params, 6).data->value.i;

		// Currently nvim does not support horizontal scrolling,
		// the parameter is reserved for later use
		assert(cols == 0);

		// With the animations the grid is drawn into a bitmap of our own, where the drawn
		// rows can simply be moved. Then only rows with pending changes are drawn again.
		bool grid_shifted = renderer->animation_active && !renderer->draws_invalidated && renderer->dirty_rows &&
			ScrollAnimationQueueGridShift(renderer, GridScroll {
				.top = static_cast<int>(top),
				.bottom = static_cast<int>(bottom),
				.left = static_cast<int>(left),
				.right = static_cast<int>(right),
				.rows = static_cast<int>(rows)
			});

		// This part is slightly cryptic, basically we're just
		// iterating from top to bottom or vice versa depending on scroll direction.
		bool scrolling_down = rows > 0;
		int64_t start_row = scrolling_down ? top : bottom - 1;
		int64_t end_row = scrolling_down ? bottom - 1 : top;
		int64_t increment = scrolling_down ? 1 : -1;

		for (int64_t j = start_row; scrolling_down ? j <= end_row : j >= end_row; j += increment) {
			// Clip anything outside the scroll region
			int64_t target_row = j - rows;
			if (target_row < top || target_row >= bottom) {
				continue;
			}

			memcpy(
				&renderer->grid_chars[target_row * renderer->grid_cols + left],
				&renderer->grid_chars[j * renderer->grid_cols + left],
				(right - left) * sizeof(uint32_t)
			);

			memcpy(
				&renderer->grid_cell_properties[target_row * renderer->grid_cols + left],
				&renderer->grid_cell_properties[j * renderer->grid_cols + left],
				(right - left) * sizeof(CellProperty)
			);

			// Sadly I have given up on making use of IDXGISwapChain1::Present1
			// scroll_rects or bitmap copies. The former seems insufficient for
			// nvim since it can require multiple scrolls per frame, the latter
			// I can't seem to make work with the FLIP_SEQUENTIAL swapchain model.
			// Thus we fall back to drawing the appropriate scrolled grid lines
			if (!grid_shifted) {
				MarkRowDirty(renderer, static_cast<int>(target_row));
			}
			// Pending changes move along with the row. The source row is read before
			// it is overwritten as a target, since rows are visited in scroll order.
			else if (renderer->dirty_rows[j]) {
				MarkRowDirty(renderer, static_cast<int>(target_row));
			}
		}

		if (grid_shifted) {
			// The uncovered rows show stale pixels until nvim redraws them
			int64_t uncovered_start = rows > 0 ? bottom - rows : top;
			int64_t uncovered_end = rows > 0 ? bottom : top - rows;
			for (int64_t j = uncovered_start; j < uncovered_end; ++j) {
				MarkRowDirty(renderer, static_cast<int>(j));
			}
			// The composition string drawn into the grid moved along as well
			if (renderer->composition_drawn && renderer->composition_drawn_row >= top &&
				renderer->composition_drawn_row < bottom) {
				MarkRowDirty(renderer, static_cast<int>(renderer->composition_drawn_row - rows));
			}
		}

		// Redraw the line which the cursor has moved to, as it is no
		// longer guaranteed that the cursor is still there. The animated
		// cursor isn't drawn into the grid.
		if (!renderer->animation_active) {
			MarkRowDirty(renderer, static_cast<int>(renderer->cursor.row - rows));
		}
	}
}

void DrawBorderRectangles(Renderer *renderer) {
	float left_border = renderer->font_width * renderer->grid_cols;
	float top_border = renderer->font_height * renderer->grid_rows;

	if(left_border != static_cast<float>(renderer->pixel_size.width)) {
		D2D1_RECT_F vertical_rect {
			.left = left_border,
			.top = 0.0f,
			.right = static_cast<float>(renderer->pixel_size.width),
			.bottom = static_cast<float>(renderer->pixel_size.height)
		};
		DrawBackgroundRect(renderer, vertical_rect, &renderer->hl_attribs[0]);
	}

	if(top_border != static_cast<float>(renderer->pixel_size.height)) {
		D2D1_RECT_F horizontal_rect {
			.left = 0.0f,
			.top = top_border,
			.right = static_cast<float>(renderer->pixel_size.width),
			.bottom = static_cast<float>(renderer->pixel_size.height)
		};
		DrawBackgroundRect(renderer, horizontal_rect, &renderer->hl_attribs[0]);
	}
}

bool FontFamilyExists(Renderer *renderer, const wchar_t *family_name) {
	IDWriteFontCollection *font_collection;
	WIN_CHECK(renderer->dwrite_factory->GetSystemFontCollection(&font_collection));
	uint32_t index;
	BOOL exists = false;
	font_collection->FindFamilyName(family_name, &index, &exists);
	font_collection->Release();
	return exists;
}

// Options of a guifont entry, e.g. "h12" in "Consolas:h12", see :help guifont
bool IsGuiFontOption(const char *option, size_t length) {
	if (length == 0) return true;
	switch (option[0]) {
	case 'h':
	case 'w': return length > 1 && (isdigit(static_cast<unsigned char>(option[1])) || option[1] == '.');
	case 'b':
	case 'i':
	case 'u':
	case 's': return length == 1;
	case 'c':
	case 'q': return length > 1 && isupper(static_cast<unsigned char>(option[1]));
	}
	return false;
}

void AddGuiFontName(wchar_t (*fonts)[MAX_FONT_LENGTH], int *font_count, const char *name, size_t length) {
	while (length > 0 && name[0] == ' ') { ++name; --length; }
	while (length > 0 && name[length - 1] == ' ') { --length; }
	if (length == 0 || *font_count >= MAX_GUIFONT_FONTS) return;

	int wstrlen = MultiByteToWideChar(CP_UTF8, 0, name, static_cast<int>(length), fonts[*font_count], MAX_FONT_LENGTH - 1);
	if (wstrlen > 0) {
		fonts[*font_count][wstrlen] = L'\0';
		++(*font_count);
	}
}

void UpdateFontFallback(Renderer *renderer) {
	SafeRelease(&renderer->dwrite_font_fallback);
	if (renderer->guifont_fallback_count == 0) return;

	IDWriteFontFallbackBuilder *builder;
	if (FAILED(renderer->dwrite_factory->CreateFontFallbackBuilder(&builder))) return;

	const wchar_t *family_names[MAX_GUIFONT_FONTS];
	for (int i = 0; i < renderer->guifont_fallback_count; ++i) {
		family_names[i] = renderer->guifont_fallbacks[i];
	}

	// Try the guifont fonts in order first, then the regular system fallback
	DWRITE_UNICODE_RANGE all_characters { .first = 0, .last = 0x10FFFF };
	builder->AddMapping(&all_characters, 1, family_names, renderer->guifont_fallback_count);
	IDWriteFontFallback *system_fallback;
	if (SUCCEEDED(renderer->dwrite_factory->GetSystemFontFallback(&system_fallback))) {
		builder->AddMappings(system_fallback);
		system_fallback->Release();
	}
	builder->CreateFontFallback(&renderer->dwrite_font_fallback);
	builder->Release();
}

// Parses a guifont value, a comma separated list of fonts with options, e.g.
// "CaskaydiaCove Nerd Font,Source Han Sans SC:h12". The first existing font is used
// as the main font, the others for glyphs it doesn't have. The legacy Nvy syntax
// "Fira Code:h24:Consolas" is still supported, Consolas being treated as another font.
bool RendererUpdateGuiFont(Renderer *renderer, const char *guifont, size_t strlen) {
	if (strlen == 0) {
		return false;
	}

	wchar_t fonts[MAX_GUIFONT_FONTS][MAX_FONT_LENGTH];
	int font_count = 0;
	float font_size = 0.0f;

	const char *guifont_end = guifont + strlen;
	const char *entry = guifont;
	while (entry < guifont_end) {
		const char *entry_end = static_cast<const char *>(memchr(entry, ',', guifont_end - entry));
		if (!entry_end) entry_end = guifont_end;

		const char *part = entry;
		bool is_font_name = true;
		while (true) {
			const char *part_end = static_cast<const char *>(memchr(part, ':', entry_end - part));
			if (!part_end) part_end = entry_end;
			size_t part_length = part_end - part;

			if (is_font_name || !IsGuiFontOption(part, part_length)) {
				AddGuiFontName(fonts, &font_count, part, part_length);
			}
			else if (part[0] == 'h' && font_size == 0.0f && part_length < 32) {
				char font_size_str[32];
				memcpy(font_size_str, part + 1, part_length - 1);
				font_size_str[part_length - 1] = '\0';
				font_size = static_cast<float>(atof(font_size_str));
			}

			is_font_name = false;
			if (part_end == entry_end) break;
			part = part_end + 1;
		}

		entry = entry_end + 1;
	}

	if (font_size <= 0.0f) {
		font_size = renderer->last_requested_font_size > 0.0f ? renderer->last_requested_font_size : DEFAULT_FONT_SIZE;
	}

	int main_font = -1;
	renderer->guifont_fallback_count = 0;
	for (int i = 0; i < font_count; ++i) {
		bool exists = FontFamilyExists(renderer, fonts[i]);
		// Like gvim, allow underscores in place of spaces
		if (!exists && wcschr(fonts[i], L'_')) {
			wchar_t font_with_spaces[MAX_FONT_LENGTH];
			wcscpy_s(font_with_spaces, MAX_FONT_LENGTH, fonts[i]);
			for (wchar_t *c = font_with_spaces; *c; ++c) {
				if (*c == L'_') *c = L' ';
			}
			if ((exists = FontFamilyExists(renderer, font_with_spaces))) {
				wcscpy_s(fonts[i], MAX_FONT_LENGTH, font_with_spaces);
			}
		}
		if (!exists) continue;

		if (main_font < 0) {
			main_font = i;
		}
		else {
			wcscpy_s(renderer->guifont_fallbacks[renderer->guifont_fallback_count++], MAX_FONT_LENGTH, fonts[i]);
		}
	}
	UpdateFontFallback(renderer);

	// No font name keeps the current font, a font which doesn't exist
	// makes UpdateFontMetrics use the fallback font
	char font_name[MAX_FONT_LENGTH * 3];
	int font_name_length = 0;
	if (font_count > 0) {
		font_name_length = WideCharToMultiByte(CP_UTF8, 0, fonts[main_font >= 0 ? main_font : 0], -1,
			font_name, sizeof(font_name), NULL, NULL);
		font_name_length = max(0, font_name_length - 1);
	}
	return RendererUpdateFont(renderer, font_size, font_name, font_name_length);
}

void SetGuiOptions(Renderer *renderer, mpack_node_t option_set) {
	uint64_t option_set_length = mpack_node_array_length(option_set);

	for (uint64_t i = 1; i < option_set_length; ++i) {
		mpack_node_t name = mpack_node_array_at(mpack_node_array_at(option_set, i), 0);
		mpack_node_t value = mpack_node_array_at(mpack_node_array_at(option_set, i), 1);
		if (MPackMatchString(name, "guifont")) {
			const char *font_str = mpack_node_str(value);
			size_t strlen = mpack_node_strlen(value);
			RendererUpdateGuiFont(renderer, font_str, strlen);

			// Send message to window in order to update nvim row/col count
			PostMessage(renderer->hwnd, WM_RENDERER_FONT_UPDATE, 0, 0);
		}
	}
}

void ClearGrid(Renderer *renderer) {
	// Initialize all grid character to a space.
	for (int i = 0; i < renderer->grid_cols * renderer->grid_rows; ++i) {
		renderer->grid_chars[i] = L' ';
	}
	memset(renderer->grid_cell_properties, 0, renderer->grid_cols * renderer->grid_rows * sizeof(CellProperty));
	D2D1_RECT_F rect {
		.left = 0.0f,
		.top = 0.0f,
		.right = renderer->grid_cols * renderer->font_width,
		.bottom = renderer->grid_rows * renderer->font_height
	};
	DrawBackgroundRect(renderer, rect, &renderer->hl_attribs[0]);
	ScrollAnimationReset(renderer->scroll_animation);
}

// Turns the animations on or off once the settings change. The grid is
// then drawn into another target, so it is drawn again from scratch.
void UpdateAnimationActive(Renderer *renderer) {
	bool enabled = renderer->cursor_animation->settings.enabled || renderer->scroll_animation->settings.enabled;
	if (enabled == renderer->animation_active) return;

	renderer->animation_active = enabled;
	renderer->cursor_animating = false;
	renderer->scroll_animating = false;
	renderer->draws_invalidated = true;
	if (!enabled) {
		SafeRelease(&renderer->d2d_grid_bitmap);
		ScrollAnimationReleaseResources(renderer->scroll_animation);
	}
}

void CreateGridBitmap(Renderer *renderer) {
	constexpr D2D1_BITMAP_PROPERTIES1 grid_bitmap_properties {
		.pixelFormat = D2D1_PIXEL_FORMAT {
			.format = DXGI_FORMAT_B8G8R8A8_UNORM,
			.alphaMode = D2D1_ALPHA_MODE_IGNORE
		},
		.dpiX = DEFAULT_DPI,
		.dpiY = DEFAULT_DPI,
		.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET
	};
	D2D1_SIZE_U size {
		.width = max(renderer->pixel_size.width, 1u),
		.height = max(renderer->pixel_size.height, 1u)
	};
	WIN_CHECK(renderer->d2d_context->CreateBitmap(size, nullptr, 0, &grid_bitmap_properties, &renderer->d2d_grid_bitmap));
	renderer->draws_invalidated = true;
}

// Waits until the swapchain is ready for the next frame, must be followed by a Present
void AcquireSwapchainFrame(Renderer *renderer) {
	if (!renderer->swapchain_frame_acquired) {
		WaitForSingleObjectEx(
			renderer->swapchain_wait_handle,
			1000,
			true
		);
		renderer->swapchain_frame_acquired = true;
	}
}

void StartDraw(Renderer *renderer) {
	if (!renderer->draw_active) {
		UpdateAnimationActive(renderer);
		// With the animations, frames are drawn once the vsync thread found the swapchain
		// ready, nvim's changes are drawn into the grid bitmap meanwhile
		if (!renderer->animation_active) {
			AcquireSwapchainFrame(renderer);
		}

		if (renderer->animation_active && !renderer->d2d_grid_bitmap) {
			CreateGridBitmap(renderer);
		}

		renderer->d2d_context->SetTarget(renderer->animation_active ?
			renderer->d2d_grid_bitmap : renderer->d2d_target_bitmap);
		renderer->d2d_context->BeginDraw();
		renderer->d2d_context->SetTransform(D2D1::IdentityMatrix());
		renderer->draw_active = true;
	}
}

void CopyFrontToBack(Renderer *renderer) {
	ID3D11Resource *front;
	ID3D11Resource *back;
	WIN_CHECK(renderer->dxgi_swapchain->GetBuffer(0, IID_PPV_ARGS(&back)));
	WIN_CHECK(renderer->dxgi_swapchain->GetBuffer(1, IID_PPV_ARGS(&front)));
	renderer->d3d_context->CopyResource(back, front);

	SafeRelease(&front);
	SafeRelease(&back);
}

double NowInSeconds(Renderer *renderer) {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return static_cast<double>(now.QuadPart) / static_cast<double>(renderer->performance_frequency.QuadPart);
}

// Refresh period of the monitor the window is on, in seconds
double GetRefreshPeriod(Renderer *renderer) {
	HMONITOR monitor = MonitorFromWindow(renderer->hwnd, MONITOR_DEFAULTTONEAREST);
	if (monitor != renderer->refresh_monitor) {
		renderer->refresh_monitor = monitor;
		renderer->refresh_period = 1.0 / 60.0;

		MONITORINFOEXW monitor_info {};
		monitor_info.cbSize = sizeof(MONITORINFOEXW);
		DEVMODEW display_mode {};
		display_mode.dmSize = sizeof(DEVMODEW);
		if (GetMonitorInfoW(monitor, &monitor_info) &&
			EnumDisplaySettingsW(monitor_info.szDevice, ENUM_CURRENT_SETTINGS, &display_mode) &&
			display_mode.dmDisplayFrequency > 1) {
			renderer->refresh_period = 1.0 / display_mode.dmDisplayFrequency;
		}
	}
	return renderer->refresh_period;
}

// The time the animations advance by in a frame. Like neovide, every frame is a refresh period
// of the monitor, so the motion is even regardless of when exactly a frame is drawn. Frames
// running behind catch up at once if a whole frame was missed, smaller drifts are corrected
// over 10 frames, also when frames come faster than the monitor refreshes.
float NextAnimationTimeStep(Renderer *renderer) {
	double now = NowInSeconds(renderer);
	if (!renderer->cursor_animating && !renderer->scroll_animating) {
		// Starting from idle, the monitor or its refresh rate may have changed meanwhile
		renderer->refresh_monitor = nullptr;
		renderer->animation_start = now;
		renderer->animation_time = 0.0;
	}
	double period = GetRefreshPeriod(renderer);

	// Positive when the animations are behind the clock
	double drift = now - renderer->animation_start - renderer->animation_time;
	if (fabs(drift) > 1.0) {
		renderer->animation_start = now;
		renderer->animation_time = 0.0;
		drift = 0.0;
	}
	double correction = drift >= period ? drift : drift / 10.0;
	double dt = max(0.0, period + correction);
	renderer->animation_time += dt;
	return static_cast<float>(min(dt, 0.1));
}

CursorBlinkTimes GetCursorBlinkTimes(Renderer *renderer) {
	const CursorModeInfo *mode_info = renderer->cursor.mode_info;
	return CursorBlinkTimes {
		.blinkwait = mode_info->blinkwait,
		.blinkon = mode_info->blinkon,
		.blinkoff = mode_info->blinkoff
	};
}

// Draws the grid with the scrolling regions, the animated cursor and its particles on top to the back buffer
void ComposeFrame(Renderer *renderer) {
	ID2D1DeviceContext4 *context = renderer->d2d_context;
	context->SetTarget(renderer->d2d_target_bitmap);
	context->DrawImage(renderer->d2d_grid_bitmap, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);

	float dt = NextAnimationTimeStep(renderer);
	renderer->scroll_animating = ScrollAnimationUpdate(renderer->scroll_animation, dt, renderer->font_height);
	ScrollAnimationDraw(renderer);

	CursorCell cell;
	if (!GetCursorCell(renderer, &cell)) {
		renderer->cursor_animating = false;
		return;
	}

	CursorAnimation *animation = renderer->cursor_animation;
	const CursorAnimationSettings *settings = &animation->settings;
	bool changed_to_from_cmdline = renderer->in_cmdline_mode != renderer->cursor_animation_was_in_cmdline;
	renderer->cursor_animation_was_in_cmdline = renderer->in_cmdline_mode;

	// The cursor stays on its text while it scrolls
	float scroll_delta_y;
	float scroll_offset_y = ScrollAnimationCellOffset(renderer, renderer->cursor.row, renderer->cursor.col, &scroll_delta_y);

	// Same size as the cursor drawn by DrawCursor, the bars are 2 pixels wide. While composing,
	// the cursor becomes the bar of the composition caret, gliding along as the IME text changes.
	bool composing = renderer->composition_length > 0 && renderer->composition_drawn;
	CursorShape shape = composing ? CursorShape::Vertical : renderer->cursor.mode_info->shape;
	float width = composing ? renderer->font_width : renderer->font_width * cell.width;
	CursorAnimationTarget target {
		.x = composing ? renderer->composition_caret_x : renderer->cursor.col * renderer->font_width,
		.y = (composing ? renderer->composition_drawn_row : renderer->cursor.row) * renderer->font_height + scroll_offset_y,
		.width = width,
		.height = renderer->font_height,
		.shape = shape,
		.cell_percentage = shape == CursorShape::Vertical ? 2.0f / width : 2.0f / renderer->font_height,
		.immediate = !settings->enabled ||
			(!settings->animate_in_insert_mode && renderer->in_insert_mode) ||
			(!settings->animate_command_line && changed_to_from_cmdline),
		.insert_mode = renderer->in_insert_mode,
		.scroll_delta_y = scroll_delta_y,
		.color = CreateBackgroundColor(renderer, &cell.hl_attribs)
	};
	renderer->cursor_animating = CursorAnimationUpdate(animation, &target, dt);

	CursorBlinkTimes blink_times = GetCursorBlinkTimes(renderer);
	double now = NowInSeconds(renderer);
	CursorBlinkUpdate(&animation->blink, &blink_times, renderer->cursor.row, renderer->cursor.col,
		renderer->cursor.mode_info, now);
	bool cursor_visible = true;
	float cursor_opacity = 1.0f;
	if (settings->smooth_blink) {
		// Fading takes every frame, not only the ones where the cursor turns on or off
		cursor_opacity = CursorBlinkOpacity(&animation->blink, &blink_times, now);
		renderer->cursor_animating |= animation->blink.state != BlinkState::Waiting;
	}
	else {
		cursor_visible = CursorBlinkVisible(&animation->blink);
	}

	if (composing) {
		// The caret is always shown, like the one drawn without the animations
		DrawAnimatedCursor(renderer, nullptr, &target, 1.0f);
	}
	else if (cursor_visible && !renderer->ui_busy && renderer->composition_length == 0) {
		DrawAnimatedCursor(renderer, &cell, &target, cursor_opacity);
	}
	if (settings->enabled) {
		DrawCursorVfx(renderer, target.color);
	}
}

void RendererRequestFrame(Renderer *renderer);

void FinishDraw(Renderer *renderer) {
	if (renderer->animation_active) {
		ComposeFrame(renderer);
	}
	renderer->d2d_context->EndDraw();

	// With the animations, frames are paced by the vsync thread waiting for the swapchain. Like
	// neovide, they're presented with a sync interval, then the swapchain is ready for the next
	// frame right after the vertical blank. Without a sync interval it would be right away, so
	// frames would be drawn far more often than shown, each showing the animations at a random
	// time. While the window isn't shown, frames wouldn't be taken from a sync interval.
	HRESULT hr;
	if (renderer->animation_active) {
		bool shown = !renderer->window_occluded && !IsIconic(renderer->hwnd) &&
			MonitorFromWindow(renderer->hwnd, MONITOR_DEFAULTTONULL);
		hr = renderer->dxgi_swapchain->Present(shown ? 1 : 0, 0);
		renderer->window_occluded = hr == DXGI_STATUS_OCCLUDED;
	}
	else {
		hr = renderer->dxgi_swapchain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
	}
	renderer->draw_active = false;
	renderer->swapchain_frame_acquired = false;

	// The frame is composed from scratch with the animation, otherwise
	// the next one is drawn on top of this one
	if (!renderer->animation_active) {
		CopyFrontToBack(renderer);
	}
	else if (renderer->cursor_animating || renderer->scroll_animating) {
		RendererRequestFrame(renderer);
	}

	if (hr == DXGI_ERROR_DEVICE_REMOVED) {
		HandleDeviceLost(renderer);
	}
}

void RendererFlush(Renderer* renderer) {
	StartDraw(renderer);
	ClearComposition(renderer);
	ScrollAnimationApplyPending(renderer, renderer->flushing_nvim_redraw);
	bool redraw_neon = renderer->neon_text && (renderer->draws_invalidated || !renderer->animation_active);
	if (renderer->neon_text && renderer->dirty_rows) {
		for (int i = 0; i < renderer->grid_rows; ++i) {
			if (renderer->dirty_rows[i]) {
				redraw_neon = true;
				break;
			}
		}
	}
	if (redraw_neon) {
		if (!renderer->d2d_neon_bitmap) CreateNeonResources(renderer);
		ID2D1Bitmap1 *target = renderer->animation_active ? renderer->d2d_grid_bitmap : renderer->d2d_target_bitmap;
		renderer->d2d_context->SetTarget(renderer->d2d_neon_bitmap);
		renderer->d2d_context->Clear(D2D1::ColorF(0, 0.0f));
		renderer->d2d_context->SetTarget(target);
		renderer->draws_invalidated = true;
	}
	DrawDirtyGridLines(renderer);
	if (redraw_neon) DrawNeonGlow(renderer);
	// The animated cursor is drawn over the grid when composing the frame
	if (!renderer->ui_busy && !renderer->animation_active) {
		DrawCursor(renderer);
	}
	DrawComposition(renderer);
	DrawBorderRectangles(renderer);
	ScrollAnimationOnFlush(renderer->scroll_animation);

	// With the animations, frames are only drawn once the swapchain is ready for them. Until
	// then nvim's changes are only drawn into the grid, they are shown with the next frame.
	if (renderer->animation_active && !renderer->swapchain_frame_acquired) {
		renderer->d2d_context->EndDraw();
		renderer->draw_active = false;
		RendererRequestFrame(renderer);
		return;
	}
	FinishDraw(renderer);
}

// Like neovide's, waits for the swapchain to be ready for a frame on its own thread, so the UI
// thread keeps handling input and nvim meanwhile. Frames are then drawn right after the vertical
// blank of the monitor the window is on.
DWORD WINAPI VsyncThread(LPVOID param) {
	Renderer *renderer = static_cast<Renderer *>(param);
	while (WaitForSingleObject(renderer->vsync_request_event, INFINITE) == WAIT_OBJECT_0 &&
		!renderer->vsync_thread_exit) {
		WaitForSingleObjectEx(renderer->swapchain_wait_handle, 1000, true);
		PostMessage(renderer->hwnd, WM_RENDERER_VSYNC, 0, 0);
	}
	return 0;
}

void RendererRequestFrame(Renderer *renderer) {
	if (renderer->frame_requested || renderer->swapchain_frame_acquired || !renderer->dxgi_swapchain) return;

	if (!renderer->vsync_thread) {
		renderer->vsync_request_event = CreateEventW(nullptr, false, false, nullptr);
		DWORD _;
		renderer->vsync_thread = CreateThread(nullptr, 0, VsyncThread, renderer, 0, &_);
	}
	renderer->frame_requested = true;
	SetEvent(renderer->vsync_request_event);
}

void RendererOnVsync(Renderer *renderer) {
	renderer->frame_requested = false;
	renderer->swapchain_frame_acquired = true;

	// In the middle of nvim's redraw the frame is drawn with its flush
	if (!renderer->draw_active && renderer->grid_initialized) {
		RendererFlush(renderer);
	}
}

DWORD RendererGetBlinkTimeout(Renderer *renderer) {
	CursorCell cell;
	if (!renderer->animation_active || !renderer->dxgi_swapchain || !GetCursorCell(renderer, &cell)) {
		return INFINITE;
	}
	// The blinking is updated with the frame which is on its way
	if (renderer->frame_requested || renderer->swapchain_frame_acquired) {
		return INFINITE;
	}

	CursorBlinkTimes blink_times = GetCursorBlinkTimes(renderer);
	double deadline = CursorBlinkDeadline(&renderer->cursor_animation->blink, &blink_times);
	if (isinf(deadline)) return INFINITE;

	double remaining = deadline - NowInSeconds(renderer);
	return remaining > 0.0 ? static_cast<DWORD>(ceil(remaining * 1000.0)) : 0;
}

void RendererScrollWindows(Renderer *renderer, mpack_node_t scrolls) {
	if (!renderer->animation_active || !renderer->grid_initialized || !renderer->dxgi_swapchain ||
		mpack_node_type(scrolls) != mpack_type_array) {
		return;
	}

	// nvim's redraw follows, the scroll starts with its flush. Frames drawn meanwhile keep
	// showing the previous content where it was, instead of waiting for nvim.
	size_t scroll_count = mpack_node_array_length(scrolls);
	for (size_t i = 0; i < scroll_count; ++i) {
		mpack_node_t scroll = mpack_node_array_at(scrolls, i);
		if (mpack_node_type(scroll) != mpack_type_array || mpack_node_array_length(scroll) < 5) continue;

		int values[5];
		for (int j = 0; j < 5; ++j) {
			values[j] = static_cast<int>(NodeToFloat(mpack_node_array_at(scroll, j), 0.0f));
		}
		ScrollAnimationQueueScroll(renderer, GridScroll {
			.top = max(values[0], 0),
			.bottom = min(values[1], renderer->grid_rows),
			.left = max(values[2], 0),
			.right = min(values[3], renderer->grid_cols),
			.rows = values[4]
		});
	}

	// Wait for the swapchain while nvim redraws, so the frame is shown right with its flush
	RendererRequestFrame(renderer);
}

bool HasPrefix(const char *name, size_t length, const char *prefix) {
	size_t prefix_length = strlen(prefix);
	return length > prefix_length && !strncmp(name, prefix, prefix_length);
}

void RendererSetOption(Renderer *renderer, const char *name, size_t length, mpack_node_t value) {
	bool known = false;
	if (length == strlen("ndx_neon_text") && !strncmp(name, "ndx_neon_text", length)) {
		bool enabled = NodeToBool(value, false);
		if (enabled != renderer->neon_text) {
			renderer->neon_text = enabled;
			renderer->draws_invalidated = true;
			if (!enabled) {
				SafeRelease(&renderer->d2d_neon_bitmap);
				SafeRelease(&renderer->d2d_neon_gain);
				SafeRelease(&renderer->d2d_neon_blur_near);
				SafeRelease(&renderer->d2d_neon_blur_far);
			}
		}
		known = true;
	}
	else if (length == strlen("ndx_neon_radius") && !strncmp(name, "ndx_neon_radius", length)) {
		float radius = NodeToFloat(value, DEFAULT_NEON_RADIUS);
		radius = std::isfinite(radius) ? max(0.5f, min(radius, 40.0f)) : DEFAULT_NEON_RADIUS;
		if (radius != renderer->neon_radius) {
			renderer->neon_radius = radius;
			renderer->draws_invalidated = true;
			UpdateNeonEffects(renderer);
		}
		known = true;
	}
	else if (length == strlen("ndx_neon_intensity") && !strncmp(name, "ndx_neon_intensity", length)) {
		float intensity = NodeToFloat(value, DEFAULT_NEON_INTENSITY);
		intensity = std::isfinite(intensity) ? max(0.0f, min(intensity, 2.0f)) : DEFAULT_NEON_INTENSITY;
		if (intensity != renderer->neon_intensity) {
			renderer->neon_intensity = intensity;
			renderer->draws_invalidated = true;
			UpdateNeonEffects(renderer);
		}
		known = true;
	}
	else if (HasPrefix(name, length, "ndx_cursor_")) {
		size_t prefix_length = strlen("ndx_cursor_");
		known = CursorAnimationSetOption(renderer->cursor_animation, name + prefix_length, length - prefix_length, value);
	}
	else if (HasPrefix(name, length, "ndx_scroll_")) {
		size_t prefix_length = strlen("ndx_scroll_");
		known = ScrollAnimationSetOption(renderer->scroll_animation, name + prefix_length, length - prefix_length, value);
	}
	if (!known) return;

	// Show the change right away unless nvim is in the middle of a redraw
	if (renderer->grid_initialized && renderer->dxgi_swapchain && !renderer->draw_active) {
		RendererFlush(renderer);
	}
}

void RendererSetFocus(Renderer *renderer, bool focused) {
	renderer->window_focused = focused;

	// The animated cursor is drawn as an outline while unfocused
	if (renderer->animation_active && renderer->grid_initialized && !renderer->draw_active) {
		RendererFlush(renderer);
	}
}

void RendererRedraw(Renderer *renderer, mpack_node_t params, bool start_maximized) {
	StartDraw(renderer);

	uint64_t redraw_commands_length = mpack_node_array_length(params);
	for (uint64_t i = 0; i < redraw_commands_length; ++i) {
		mpack_node_t redraw_command_arr = mpack_node_array_at(params, i);
		mpack_node_t redraw_command_name = mpack_node_array_at(redraw_command_arr, 0);

		if (MPackMatchString(redraw_command_name, "option_set")) {
			SetGuiOptions(renderer, redraw_command_arr);
		}
		if (MPackMatchString(redraw_command_name, "grid_resize")) {
			if (UpdateGridSize(renderer, redraw_command_arr) &&
				(GetWindowLong(renderer->hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW))
			{
				// Fullscreen size belongs to the monitor; adding window frame dimensions here
				// would make each grid resize expand the borderless window.
				PixelSize size = RendererGridToPixelSize(renderer, renderer->grid_rows, renderer->grid_cols);
				SetWindowPos(renderer->hwnd, HWND_TOP, 0, 0, size.width, size.height, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
			}
		}
		if (MPackMatchString(redraw_command_name, "grid_clear")) {
			ClearGrid(renderer);
		}
		else if (MPackMatchString(redraw_command_name, "default_colors_set")) {
			UpdateDefaultColors(renderer, redraw_command_arr);
			renderer->draws_invalidated = true;
		}
		else if (MPackMatchString(redraw_command_name, "hl_attr_define")) {
			UpdateHighlightAttributes(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "grid_line")) {
			DrawGridLines(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "grid_cursor_goto")) {
			// If the old cursor position is still within the row bounds,
			// redraw the line to get rid of the cursor
			MarkCursorRowDirty(renderer);
			UpdateCursorPos(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "mode_info_set")) {
			UpdateCursorModeInfos(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "mode_change")) {
			// Redraw cursor if its inside the bounds
			MarkCursorRowDirty(renderer);
			UpdateCursorMode(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "set_title")) {
			UpdateWindowTitle(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "busy_start")) {
			renderer->ui_busy = true;
			// Hide cursor while UI is busy
			MarkCursorRowDirty(renderer);
		}
		else if (MPackMatchString(redraw_command_name, "busy_stop")) {
			renderer->ui_busy = false;
		}
		else if (MPackMatchString(redraw_command_name, "grid_scroll")) {
			ScrollRegion(renderer, redraw_command_arr);
		}
		else if (MPackMatchString(redraw_command_name, "flush")) {
			if (!renderer->has_drawn) {
				renderer->has_drawn = true;
				ShowWindow(renderer->hwnd, start_maximized ? SW_MAXIMIZE : SW_SHOWDEFAULT);			}

			// The scrolls nvim reported start with the content they belong to
			renderer->flushing_nvim_redraw = true;
			RendererFlush(renderer);
			renderer->flushing_nvim_redraw = false;
		}
	}
}

PixelSize RendererGridToPixelSize(Renderer *renderer, int rows, int cols) {
	int requested_width = static_cast<int>(ceilf(renderer->font_width) * cols);
	int requested_height = static_cast<int>(ceilf(renderer->font_height) * rows);

	// Adjust size to include title bar
	RECT adjusted_rect = { 0, 0, requested_width, requested_height };
	AdjustWindowRect(&adjusted_rect, WS_OVERLAPPEDWINDOW, false);
	return PixelSize {
		.width = adjusted_rect.right - adjusted_rect.left,
		.height = adjusted_rect.bottom - adjusted_rect.top
	};
}

GridSize RendererPixelsToGridSize(Renderer *renderer, int width, int height) {
	return GridSize {
		.rows = static_cast<int>(height / renderer->font_height),
		.cols = static_cast<int>(width / renderer->font_width)
	};
}

GridPoint RendererCursorToGridPoint(Renderer *renderer, int x, int y) {
	return GridPoint {
		.row = static_cast<int>(y / renderer->font_height),
		.col = static_cast<int>(x / renderer->font_width)
	};
}
