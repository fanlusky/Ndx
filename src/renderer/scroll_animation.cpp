#include "scroll_animation.h"
#include "renderer/renderer.h"

constexpr ScrollAnimationSettings DEFAULT_SCROLL_SETTINGS {
	.enabled = false,
	.animation_length = 0.3f,
	.far_lines = 1
};

void ScrollAnimationInitialize(ScrollAnimation *scroll) {
	scroll->settings = DEFAULT_SCROLL_SETTINGS;
	scroll->region_count = 0;
	scroll->d2d_snapshot = nullptr;
	scroll->d2d_snapshot_temp = nullptr;
	scroll->pending_scroll_count = 0;
	scroll->pending_grid_shift_count = 0;
}

bool ScrollAnimationSetOption(ScrollAnimation *scroll, const char *name, size_t length, mpack_node_t value) {
	ScrollAnimationSettings *settings = &scroll->settings;
	const auto Matches = [&](const char *option) {
		return strlen(option) == length && !strncmp(option, name, length);
	};

	if (Matches("animation")) {
		settings->enabled = NodeToBool(value, DEFAULT_SCROLL_SETTINGS.enabled);
		// Queued grid shifts are still needed, the grid's rows were moved already
		scroll->region_count = 0;
		scroll->pending_scroll_count = 0;
	}
	else if (Matches("animation_length")) {
		settings->animation_length = max(0.0f, NodeToFloat(value, DEFAULT_SCROLL_SETTINGS.animation_length));
	}
	else if (Matches("animation_far_lines")) {
		settings->far_lines = max(0, static_cast<int>(NodeToFloat(value, static_cast<float>(DEFAULT_SCROLL_SETTINGS.far_lines))));
	}
	else {
		return false;
	}
	return true;
}

void ScrollAnimationReset(ScrollAnimation *scroll) {
	scroll->region_count = 0;
	scroll->pending_scroll_count = 0;
	// The grid is cleared or drawn again from scratch
	scroll->pending_grid_shift_count = 0;
}

void ScrollAnimationReleaseResources(ScrollAnimation *scroll) {
	ScrollAnimationReset(scroll);
	SafeRelease(&scroll->d2d_snapshot);
	SafeRelease(&scroll->d2d_snapshot_temp);
}

D2D1_RECT_F RegionRect(Renderer *renderer, const ScrollRegionAnimation *region) {
	return D2D1_RECT_F {
		.left = region->left * renderer->font_width,
		.top = region->top * renderer->font_height,
		.right = region->right * renderer->font_width,
		.bottom = region->bottom * renderer->font_height
	};
}

D2D1_RECT_U RegionPixelRect(Renderer *renderer, const ScrollRegionAnimation *region) {
	D2D1_RECT_F rect = RegionRect(renderer, region);
	return D2D1_RECT_U {
		.left = static_cast<uint32_t>(max(0.0f, floorf(rect.left))),
		.top = static_cast<uint32_t>(max(0.0f, floorf(rect.top))),
		.right = min(static_cast<uint32_t>(ceilf(rect.right)), renderer->pixel_size.width),
		.bottom = min(static_cast<uint32_t>(ceilf(rect.bottom)), renderer->pixel_size.height)
	};
}

bool CreateSnapshotBitmap(Renderer *renderer, ID2D1Bitmap1 **bitmap) {
	if (*bitmap) {
		D2D1_SIZE_U size = (*bitmap)->GetPixelSize();
		if (size.width == renderer->pixel_size.width && size.height == renderer->pixel_size.height) {
			return true;
		}
		SafeRelease(bitmap);
	}

	constexpr D2D1_BITMAP_PROPERTIES1 snapshot_properties {
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
	return SUCCEEDED(renderer->d2d_context->CreateBitmap(size, nullptr, 0, &snapshot_properties, bitmap));
}

void DrawBackgroundRect(Renderer *renderer, D2D1_RECT_F rect, HighlightAttributes *hl_attribs);

void DrawDefaultBackground(Renderer *renderer, D2D1_RECT_F rect) {
	DrawBackgroundRect(renderer, rect, &renderer->hl_attribs[0]);
}

// Draws the region as it is shown: the grid moved by the scroll position, the
// rows it uncovers from the snapshot
void DrawScrollRegion(Renderer *renderer, ScrollAnimation *scroll, const ScrollRegionAnimation *region) {
	ID2D1DeviceContext4 *context = renderer->d2d_context;
	D2D1_RECT_F rect = RegionRect(renderer, region);
	float offset = roundf(region->spring.position * renderer->font_height);
	float snapshot_offset = offset - roundf(region->snapshot_position * renderer->font_height);

	context->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
	D2D1_POINT_2F grid_origin { .x = rect.left, .y = rect.top + offset };
	context->DrawImage(renderer->d2d_grid_bitmap, &grid_origin, &rect,
		D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);

	if (offset != 0.0f && scroll->d2d_snapshot) {
		D2D1_RECT_F uncovered = rect;
		if (offset > 0.0f) {
			uncovered.bottom = min(rect.bottom, rect.top + offset);
		}
		else {
			uncovered.top = max(rect.top, rect.bottom + offset);
		}
		if (region->far_jump) {
			DrawDefaultBackground(renderer, uncovered);
		}
		else {
			D2D1_POINT_2F snapshot_origin { .x = rect.left, .y = rect.top + snapshot_offset };
			context->PushAxisAlignedClip(uncovered, D2D1_ANTIALIAS_MODE_ALIASED);
			context->DrawImage(scroll->d2d_snapshot, &snapshot_origin, &rect,
				D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
			context->PopAxisAlignedClip();
		}
	}
	context->PopAxisAlignedClip();
}

// Saves what the region shows right now, before nvim's changes are drawn
bool SaveSnapshot(Renderer *renderer, ScrollAnimation *scroll, ScrollRegionAnimation *region) {
	if (!renderer->d2d_grid_bitmap || !CreateSnapshotBitmap(renderer, &scroll->d2d_snapshot)) return false;

	D2D1_RECT_U pixel_rect = RegionPixelRect(renderer, region);
	if (pixel_rect.right <= pixel_rect.left || pixel_rect.bottom <= pixel_rect.top) return false;
	ID2D1DeviceContext4 *context = renderer->d2d_context;
	ID2D1Bitmap1 *source = renderer->d2d_grid_bitmap;
	if (region->spring.position != 0.0f) {
		// Mid scroll the region is a mix of the grid and the previous snapshot
		if (!CreateSnapshotBitmap(renderer, &scroll->d2d_snapshot_temp)) return false;

		ID2D1Image *target;
		context->GetTarget(&target);
		context->SetTarget(scroll->d2d_snapshot_temp);
		DrawScrollRegion(renderer, scroll, region);
		context->SetTarget(target);
		SafeRelease(&target);
		source = scroll->d2d_snapshot_temp;
	}

	ID2D1Image *target;
	context->GetTarget(&target);
	context->SetTarget(scroll->d2d_snapshot);
	D2D1_RECT_F rect = RegionRect(renderer, region);
	D2D1_POINT_2F origin { .x = rect.left, .y = rect.top };
	context->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
	context->DrawImage(source, &origin, &rect,
		D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
	context->PopAxisAlignedClip();
	context->SetTarget(target);
	SafeRelease(&target);
	return true;
}

void RemoveRegion(ScrollAnimation *scroll, int index) {
	scroll->regions[index] = scroll->regions[--scroll->region_count];
}

bool RegionsOverlap(const ScrollRegionAnimation *region, int top, int bottom, int left, int right) {
	return region->left < right && left < region->right && region->top < bottom && top < region->bottom;
}

void StartScroll(Renderer *renderer, int top, int bottom, int left, int right, int rows) {
	ScrollAnimation *scroll = renderer->scroll_animation;

	ScrollRegionAnimation *region = nullptr;
	for (int i = 0; i < scroll->region_count; ++i) {
		ScrollRegionAnimation *existing = &scroll->regions[i];
		if (existing->top == top && existing->bottom == bottom && existing->left == left && existing->right == right) {
			region = existing;
		}
		else if (RegionsOverlap(existing, top, bottom, left, right)) {
			// The same window keeps scrolling when only its height changed, e.g. when a
			// float covering its top grew. A window that was resized or moved starts over.
			if (!region && existing->left == left && existing->right == right) {
				existing->top = top;
				existing->bottom = bottom;
				existing->snapshot_taken = false;
				region = existing;
			}
			else {
				RemoveRegion(scroll, i--);
			}
		}
	}
	if (!region) {
		if (scroll->region_count == MAX_SCROLL_REGIONS) return;
		region = &scroll->regions[scroll->region_count++];
		*region = ScrollRegionAnimation {
			.top = top,
			.bottom = bottom,
			.left = left,
			.right = right
		};
	}

	if (!region->snapshot_taken) {
		if (!SaveSnapshot(renderer, scroll, region)) {
			RemoveRegion(scroll, static_cast<int>(region - scroll->regions));
			return;
		}
		region->snapshot_taken = true;
		region->snapshot_position = region->spring.position;
		// The empty rows of a far jump are part of the snapshot now
		region->far_jump = false;
	}

	float region_rows = static_cast<float>(bottom - top);
	if (static_cast<float>(abs(rows)) > region_rows) {
		// Like neovide, a jump further than the window only moves by a few rows to show its direction
		float far_rows = min(static_cast<float>(scroll->settings.far_lines), region_rows);
		region->spring.position = rows > 0 ? far_rows : -far_rows;
		region->snapshot_position = region->spring.position;
		region->far_jump = true;
		return;
	}

	// The content is drawn where it was and slides to its new place, scrolling
	// down moves it up. The snapshot keeps its place relative to the content.
	float position = max(-region_rows, min(region->spring.position + rows, region_rows));
	region->snapshot_position += position - region->spring.position;
	region->spring.position = position;
}

void ScrollAnimationQueueScroll(Renderer *renderer, GridScroll grid_scroll) {
	ScrollAnimation *scroll = renderer->scroll_animation;
	if (!renderer->animation_active || !scroll->settings.enabled || grid_scroll.rows == 0 ||
		grid_scroll.bottom <= grid_scroll.top || grid_scroll.right <= grid_scroll.left) {
		return;
	}

	for (int i = 0; i < scroll->pending_scroll_count; ++i) {
		GridScroll *pending = &scroll->pending_scrolls[i];
		if (pending->top == grid_scroll.top && pending->bottom == grid_scroll.bottom &&
			pending->left == grid_scroll.left && pending->right == grid_scroll.right) {
			pending->rows += grid_scroll.rows;
			return;
		}
	}
	if (scroll->pending_scroll_count < MAX_SCROLL_REGIONS) {
		scroll->pending_scrolls[scroll->pending_scroll_count++] = grid_scroll;
	}
}

bool ScrollAnimationQueueGridShift(Renderer *renderer, GridScroll grid_scroll) {
	ScrollAnimation *scroll = renderer->scroll_animation;
	int kept_rows = grid_scroll.bottom - grid_scroll.top - abs(grid_scroll.rows);
	if (!renderer->d2d_grid_bitmap || kept_rows <= 0 || grid_scroll.right <= grid_scroll.left ||
		renderer->font_height != floorf(renderer->font_height) ||
		scroll->pending_grid_shift_count == MAX_PENDING_GRID_SHIFTS) {
		return false;
	}
	scroll->pending_grid_shifts[scroll->pending_grid_shift_count++] = grid_scroll;
	return true;
}

void ShiftGrid(Renderer *renderer, int top, int bottom, int left, int right, int rows) {
	ScrollAnimation *scroll = renderer->scroll_animation;
	if (!CreateSnapshotBitmap(renderer, &scroll->d2d_snapshot_temp)) {
		// The moved rows show what was there before until they're drawn again
		renderer->draws_invalidated = true;
		return;
	}

	// The rows staying in the region, scrolling down moves them up
	D2D1_RECT_F source {
		.left = left * renderer->font_width,
		.top = (rows > 0 ? top + rows : top) * renderer->font_height,
		.right = right * renderer->font_width,
		.bottom = (rows > 0 ? bottom : bottom + rows) * renderer->font_height
	};
	D2D1_RECT_F destination = source;
	destination.top -= rows * renderer->font_height;
	destination.bottom -= rows * renderer->font_height;

	// A bitmap can't be drawn onto itself, go through the temporary one
	ID2D1DeviceContext4 *context = renderer->d2d_context;
	ID2D1Image *target;
	context->GetTarget(&target);
	context->SetTarget(scroll->d2d_snapshot_temp);
	D2D1_POINT_2F source_origin { .x = source.left, .y = source.top };
	context->DrawImage(renderer->d2d_grid_bitmap, &source_origin, &source,
		D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
	context->SetTarget(target);
	SafeRelease(&target);

	D2D1_POINT_2F destination_origin { .x = destination.left, .y = destination.top };
	context->PushAxisAlignedClip(destination, D2D1_ANTIALIAS_MODE_ALIASED);
	context->DrawImage(scroll->d2d_snapshot_temp, &destination_origin, &source,
		D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
	context->PopAxisAlignedClip();
}

void ScrollAnimationApplyPending(Renderer *renderer, bool start_scrolls) {
	ScrollAnimation *scroll = renderer->scroll_animation;

	// The snapshots are taken from the grid as it was shown so far, before it is changed
	if (start_scrolls) {
		for (int i = 0; i < scroll->pending_scroll_count; ++i) {
			const GridScroll *pending = &scroll->pending_scrolls[i];
			if (pending->rows != 0) {
				StartScroll(renderer, pending->top, pending->bottom, pending->left, pending->right, pending->rows);
			}
		}
		scroll->pending_scroll_count = 0;
	}

	for (int i = 0; i < scroll->pending_grid_shift_count; ++i) {
		const GridScroll *shift = &scroll->pending_grid_shifts[i];
		ShiftGrid(renderer, shift->top, shift->bottom, shift->left, shift->right, shift->rows);
	}
	scroll->pending_grid_shift_count = 0;
}

void ScrollAnimationOnFlush(ScrollAnimation *scroll) {
	for (int i = 0; i < scroll->region_count; ++i) {
		scroll->regions[i].snapshot_taken = false;
	}
}

bool ScrollAnimationUpdate(ScrollAnimation *scroll, float dt, float font_height) {
	bool animating = false;
	for (int i = 0; i < scroll->region_count; ++i) {
		ScrollRegionAnimation *region = &scroll->regions[i];
		// Regions which arrived last update are dropped now, after the
		// cursor moved along with them for the last time
		if (region->spring.position == 0.0f && !region->snapshot_taken) {
			RemoveRegion(scroll, i--);
			continue;
		}

		float offset_before = roundf(region->spring.position * font_height);
		SpringUpdate(&region->spring, dt, scroll->settings.animation_length);
		region->frame_delta = roundf(region->spring.position * font_height) - offset_before;
		animating |= region->spring.position != 0.0f || region->frame_delta != 0.0f;
	}
	return animating;
}

void ScrollAnimationDraw(Renderer *renderer) {
	ScrollAnimation *scroll = renderer->scroll_animation;
	for (int i = 0; i < scroll->region_count; ++i) {
		if (scroll->regions[i].spring.position != 0.0f) {
			DrawScrollRegion(renderer, scroll, &scroll->regions[i]);
		}
	}
}

float ScrollAnimationCellOffset(Renderer *renderer, int row, int col, float *frame_delta) {
	ScrollAnimation *scroll = renderer->scroll_animation;
	for (int i = 0; i < scroll->region_count; ++i) {
		const ScrollRegionAnimation *region = &scroll->regions[i];
		if (row >= region->top && row < region->bottom && col >= region->left && col < region->right) {
			*frame_delta = region->frame_delta;
			return roundf(region->spring.position * renderer->font_height);
		}
	}
	*frame_delta = 0.0f;
	return 0.0f;
}
