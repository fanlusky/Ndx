#pragma once
#include "renderer/cursor_animation.h"

// Smooth scrolling of nvim's windows, modeled after neovide. nvim reports the windows it
// scrolled through the WinScrolled autocmd set up in NvimInitialize, before redrawing them.
// The new content slides into place while the rows it uncovers keep showing what
// was there before, taken from a snapshot of the region saved when it scrolled.

constexpr int MAX_SCROLL_REGIONS = 16;

struct ScrollRegionAnimation {
	int top;
	int bottom;
	int left;
	int right;
	// Rows the content is still drawn away from its place
	SpringAnimation spring;
	// Rows d2d_scroll_snapshot has to be moved by to be drawn where it was saved
	float snapshot_position;
	// The snapshot shows the region as of the last flush, later scrolls
	// before the next flush have to be applied on top of it
	bool snapshot_taken;
	// After a jump further than the region the old content doesn't connect to the
	// new one, the uncovered rows are left empty instead
	bool far_jump;
	// Pixels the content moved during the last update
	float frame_delta;
};

struct ScrollAnimationSettings {
	bool enabled;
	float animation_length;
	// Rows a scroll further than the window height is animated by
	int far_lines;
};

// A scroll of a part of the grid, rows being positive when scrolling down
struct GridScroll {
	int top;
	int bottom;
	int left;
	int right;
	int rows;
};

constexpr int MAX_PENDING_GRID_SHIFTS = 64;

struct ScrollAnimation {
	ScrollAnimationSettings settings;
	ScrollRegionAnimation regions[MAX_SCROLL_REGIONS];
	int region_count;
	ID2D1Bitmap1 *d2d_snapshot;
	ID2D1Bitmap1 *d2d_snapshot_temp;

	// Scrolls and grid shifts are applied on nvim's flush, together with the content they
	// belong to. Until then frames keep showing the previous content where it was.
	GridScroll pending_scrolls[MAX_SCROLL_REGIONS];
	int pending_scroll_count;
	GridScroll pending_grid_shifts[MAX_PENDING_GRID_SHIFTS];
	int pending_grid_shift_count;
};

struct Renderer;

void ScrollAnimationInitialize(ScrollAnimation *scroll);
// Applies a g:ndx_scroll_<name> variable, a nil value restores the default.
// Returns false if the name is unknown.
bool ScrollAnimationSetOption(ScrollAnimation *scroll, const char *name, size_t length, mpack_node_t value);
// Stops all scrolling, e.g. when the grid is cleared or resized
void ScrollAnimationReset(ScrollAnimation *scroll);
void ScrollAnimationReleaseResources(ScrollAnimation *scroll);

// Called when nvim scrolled a window by a number of screen rows, before it is redrawn. The
// region is the part of the window on the grid not covered by floating windows.
void ScrollAnimationQueueScroll(Renderer *renderer, GridScroll scroll);
// Moves the drawn rows of a region nvim scrolled with grid_scroll within the grid bitmap on
// the next flush, so only the rows the scroll uncovers have to be laid out again. Returns false
// if the rows can't be moved by whole pixels, they have to be drawn again then.
bool ScrollAnimationQueueGridShift(Renderer *renderer, GridScroll scroll);
// Called before the dirty grid lines are drawn, moves the rows of the grid bitmap. The
// queued scrolls are started on nvim's flush, when the content they belong to is drawn.
void ScrollAnimationApplyPending(Renderer *renderer, bool start_scrolls);
// Called after the dirty grid lines are drawn
void ScrollAnimationOnFlush(ScrollAnimation *scroll);
// Returns whether a region is still scrolling
bool ScrollAnimationUpdate(ScrollAnimation *scroll, float dt, float font_height);
// Draws the scrolling regions over the grid
void ScrollAnimationDraw(Renderer *renderer);
// Pixels the content of a grid cell is currently moved by, and by how much that changed
// during the last update
float ScrollAnimationCellOffset(Renderer *renderer, int row, int col, float *frame_delta);
