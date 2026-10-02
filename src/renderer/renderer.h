#pragma once

constexpr const char *DEFAULT_FONT = "Consolas";
constexpr float DEFAULT_FONT_SIZE = 14.0f;

constexpr uint32_t DEFAULT_COLOR = 0x46464646;
enum HighlightAttributeFlags : uint16_t {
	HL_ATTRIB_REVERSE			= 1 << 0,
	HL_ATTRIB_ITALIC			= 1 << 1,
	HL_ATTRIB_BOLD				= 1 << 2,
	HL_ATTRIB_STRIKETHROUGH		= 1 << 3,
	HL_ATTRIB_UNDERLINE			= 1 << 4,
	HL_ATTRIB_UNDERCURL			= 1 << 5
};
struct HighlightAttributes {
	uint32_t foreground;
	uint32_t background;
	uint32_t special;
	uint16_t flags;
};

enum class CursorShape {
	None,
	Block,
	Vertical,
	Horizontal
};

struct GridPoint {
	int row;
	int col;
};
struct GridSize {
	int rows;
	int cols;
};
struct PixelSize {
	int width;
	int height;
};

struct CursorModeInfo {
	CursorShape shape;
	uint16_t hl_attrib_id;
	// In ms, only used with the animations
	int blinkwait;
	int blinkon;
	int blinkoff;
};
struct Cursor {
	CursorModeInfo *mode_info;
	int row;
	int col;
};

struct CellProperty {
	uint16_t hl_attrib_id;
	bool is_wide_char;
};

// Open addressing hash map from a grid cell to its measured text width
struct CharWidthEntry {
	uint64_t key;
	float width;
};
struct CharWidthCache {
	CharWidthEntry *entries;
	uint32_t capacity;
	uint32_t count;
};
enum class GlyphState : uint8_t {
	Unknown,
	Present,
	Missing
};

enum class CompositionLineStyle : uint8_t {
	None,
	Solid,
	Dot,
	Dash,
	Squiggle
};
// A range of the IME composition string, styled as declared by the
// IME's display attribute. Colors are 0xRRGGBB.
struct CompositionClause {
	uint32_t start;
	uint32_t end;
	CompositionLineStyle line_style;
	bool bold_line;
	bool has_text_color;
	bool has_background_color;
	bool has_line_color;
	uint32_t text_color;
	uint32_t background_color;
	uint32_t line_color;
};

constexpr int MAX_HIGHLIGHT_ATTRIBS = 0xFFFF;
constexpr int MAX_CURSOR_MODE_INFOS = 64;
constexpr int MAX_FONT_LENGTH = 128;
constexpr int MAX_COMPOSITION_CLAUSES = 64;
constexpr int MAX_GUIFONT_FONTS = 8;
constexpr float DEFAULT_DPI = 96.0f;
constexpr float POINTS_PER_INCH = 72.0f;
struct GlyphDrawingEffect;
struct GlyphRenderer;
struct CursorAnimation;
struct ScrollAnimation;
struct Renderer {
	CursorModeInfo cursor_mode_infos[MAX_CURSOR_MODE_INFOS];
	Vec<HighlightAttributes> hl_attribs;
	Cursor cursor;
	bool in_insert_mode;
	bool in_cmdline_mode;

	// With the cursor or scroll animation, the grid is drawn into d2d_grid_bitmap and every
	// frame is composed of it, the scrolling regions and the animated cursor on top
	CursorAnimation *cursor_animation;
	ScrollAnimation *scroll_animation;
	bool animation_active;
	bool cursor_animating;
	bool scroll_animating;
	bool cursor_animation_was_in_cmdline;
	LARGE_INTEGER animation_last_frame;
	LARGE_INTEGER performance_frequency;
	ID2D1Bitmap1 *d2d_grid_bitmap;
	bool window_focused;
	// Signaled when the next animation frame is due
	HANDLE animation_timer;
	// Drawing the frame scheduled by animation_timer
	bool drawing_animation_frame;

	GlyphRenderer *glyph_renderer;

	D3D_FEATURE_LEVEL d3d_feature_level;
	ID3D11Device2 *d3d_device;
	ID3D11DeviceContext2 *d3d_context;
	IDXGISwapChain2 *dxgi_swapchain;
	HANDLE swapchain_wait_handle;
	ID2D1Factory5 *d2d_factory;
	ID2D1Device4 *d2d_device;
	ID2D1DeviceContext4 *d2d_context;
	ID2D1Bitmap1 *d2d_target_bitmap;
	ID2D1SolidColorBrush *d2d_background_rect_brush;

    IDWriteFontFace1 *font_face;

	IDWriteFactory4 *dwrite_factory;
	IDWriteTextFormat *dwrite_text_format;

	bool disable_ligatures;
	IDWriteTypography *dwrite_typography;

	float linespace_factor;

    float last_requested_font_size;
	wchar_t font[MAX_FONT_LENGTH];
	wchar_t fallback_font[MAX_FONT_LENGTH];
	wchar_t locale_name[LOCALE_NAME_MAX_LENGTH];
	// The other fonts listed in guifont, used for glyphs missing from the main font
	wchar_t guifont_fallbacks[MAX_GUIFONT_FONTS][MAX_FONT_LENGTH];
	int guifont_fallback_count;
	IDWriteFontFallback *dwrite_font_fallback;
	DWRITE_FONT_METRICS1 font_metrics;
	float font_size_scale_bold;
	float dpi_scale;
    float font_size;
	float font_height;
	float font_width;
	float font_ascent;
    float font_descent;

	// Measuring characters is expensive, so the results are kept until the font changes
	CharWidthCache char_widths;
	GlyphState latin1_glyphs[256];

	D2D1_SIZE_U pixel_size;
	bool grid_initialized;
	int grid_rows;
	int grid_cols;
	uint32_t *grid_chars;
	wchar_t *wchar_buffer;
	size_t wchar_buffer_length;
	CellProperty *grid_cell_properties;
	// Lines are only laid out on flush, so a line changed several times is drawn once
	bool *dirty_rows;

	wchar_t *composition_text;
	uint32_t composition_length;
	uint32_t composition_capacity;
	uint32_t composition_caret;
	CompositionClause composition_clauses[MAX_COMPOSITION_CLAUSES];
	uint32_t composition_clause_count;
	bool composition_drawn;
	int composition_drawn_row;

	HWND hwnd;
	bool draw_active;
	bool ui_busy;
	bool has_drawn;
	bool draws_invalidated;
};

void RendererInitialize(Renderer *renderer, HWND hwnd, bool disable_ligatures, float linespace_factor, float monitor_dpi);
void RendererAttach(Renderer *renderer);
void RendererShutdown(Renderer *renderer);

void RendererResize(Renderer *renderer, uint32_t width, uint32_t height);
bool RendererUpdateGuiFont(Renderer *renderer, const char *guifont, size_t strlen);
bool RendererUpdateFont(Renderer *renderer, float font_size, const char *font_string = "", int strlen = 0);
void RendererRedraw(Renderer *renderer, mpack_node_t params, bool start_maximized);
void RendererFlush(Renderer* renderer);

// Draws the IME composition string inline at the cursor, pass length 0 to clear it
void RendererSetComposition(Renderer *renderer, const wchar_t *text, uint32_t length, uint32_t caret,
	const CompositionClause *clauses, uint32_t clause_count);
// Client rect of [start, end) of a composition string as it would be drawn at the cursor
RECT RendererGetCompositionTextRect(Renderer *renderer, const wchar_t *text, uint32_t length, uint32_t start, uint32_t end);

// Applies a g:ndx_cursor_* or g:ndx_scroll_* variable, see CursorAnimationSetOption
// and ScrollAnimationSetOption
void RendererSetOption(Renderer *renderer, const char *name, size_t length, mpack_node_t value);
void RendererSetFocus(Renderer *renderer, bool focused);
// Starts scrolling the windows nvim reports with an ndx_scroll notification, before it redraws
// them. Each scroll is [top, bottom, left, right, rows], rows being positive when scrolling down.
void RendererScrollWindows(Renderer *renderer, mpack_node_t scrolls);
// Whether the animations need more frames. They are drawn with RendererAnimate
// once the handle returned by RendererScheduleAnimationFrame is signaled.
bool RendererIsAnimating(Renderer *renderer);
HANDLE RendererScheduleAnimationFrame(Renderer *renderer);
void RendererAnimate(Renderer *renderer);
// Milliseconds until the cursor blinks next and has to be drawn with
// RendererFlush, INFINITE if it doesn't blink
DWORD RendererGetBlinkTimeout(Renderer *renderer);

PixelSize RendererGridToPixelSize(Renderer *renderer, int rows, int cols);
GridSize RendererPixelsToGridSize(Renderer *renderer, int width, int height);
GridPoint RendererCursorToGridPoint(Renderer *renderer, int x, int y);
