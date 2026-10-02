#include "cursor_animation.h"
#include "renderer/renderer.h"

constexpr float PI = 3.14159265358979f;

constexpr CursorAnimationSettings DEFAULT_SETTINGS {
	.enabled = false,
	.animation_length = 0.150f,
	.short_animation_length = 0.04f,
	.trail_size = 1.0f,
	.animate_in_insert_mode = true,
	.animate_command_line = true,
	.antialiasing = true,
	.unfocused_outline_width = 1.0f / 8.0f,
	.vfx_modes = {},
	.vfx_mode_count = 0,
	.vfx_opacity = 200.0f,
	.vfx_particle_lifetime = 0.5f,
	.vfx_particle_highlight_lifetime = 0.2f,
	.vfx_particle_density = 0.7f,
	.vfx_particle_speed = 10.0f,
	.vfx_particle_phase = 1.5f,
	.vfx_particle_curl = 1.0f
};

// Corners of a block cursor relative to its center, in clockwise order from the top left
constexpr float STANDARD_CORNERS[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };

float Lerp(float start, float end, float t) {
	return start + (end - start) * t;
}

void SpringReset(SpringAnimation *spring) {
	spring->position = 0.0f;
	spring->velocity = 0.0f;
}

// Analytical solution of a critically damped spring, the destination is reached
// within a 2% tolerance in animation_length seconds. See
// https://gdcvault.com/play/1027059/Math-In-Game-Development-Summit
bool SpringUpdate(SpringAnimation *spring, float dt, float animation_length) {
	if (animation_length <= dt) {
		SpringReset(spring);
		return false;
	}
	if (spring->position == 0.0f) {
		return false;
	}

	float omega = 4.0f / animation_length;
	float a = spring->position;
	float b = spring->position * omega + spring->velocity;
	float c = expf(-omega * dt);

	spring->position = (a + b * dt) * c;
	spring->velocity = c * (-a * omega - b * dt * omega + b);

	if (fabsf(spring->position) < 0.01f) {
		SpringReset(spring);
		return false;
	}
	return true;
}

// PCG random number generator, see http://www.pcg-random.org/
constexpr uint64_t RNG_INITIAL_STATE = 0x853C49E6748FEA9Bull;
constexpr uint64_t RNG_INCREMENT = (0xDA3E39CB94B95BDBull << 1) | 1;

uint32_t RngNext(uint64_t *state) {
	uint64_t old_state = *state;
	*state = old_state * 6364136223846793005ull + RNG_INCREMENT;

	uint32_t rotation = static_cast<uint32_t>(old_state >> 59);
	uint32_t xor_shifted = static_cast<uint32_t>(((old_state >> 18) ^ old_state) >> 27);
	return (xor_shifted >> rotation) | (xor_shifted << ((32 - rotation) & 31));
}

// In the [0, 1) range
float RngNextFloat(uint64_t *state) {
	return static_cast<float>(ldexp(static_cast<double>(RngNext(state)), -32));
}

void Normalize(float *x, float *y) {
	float length = sqrtf(*x * *x + *y * *y);
	if (length > 0.0f) {
		*x /= length;
		*y /= length;
	}
}

// Random direction of length 1
void RngNextDirection(uint64_t *state, float *x, float *y) {
	*x = RngNextFloat(state) * 2.0f - 1.0f;
	*y = RngNextFloat(state) * 2.0f - 1.0f;
	Normalize(x, y);
}

bool IsHighlight(CursorVfxMode mode) {
	return mode == CursorVfxMode::SonicBoom || mode == CursorVfxMode::Ripple || mode == CursorVfxMode::Wireframe;
}

void CreateVfxs(CursorAnimation *animation) {
	animation->vfx_count = animation->settings.vfx_mode_count;
	for (int i = 0; i < animation->vfx_count; ++i) {
		CursorVfx *vfx = &animation->vfxs[i];
		vfx->mode = animation->settings.vfx_modes[i];
		vfx->t = 1.0f;
		vfx->previous_destination_x = animation->destination_x;
		vfx->previous_destination_y = animation->destination_y;
		vfx->count_remainder = 0.0f;
		vfx->rng_state = RNG_INITIAL_STATE;
		vfx->particle_count = 0;
	}
}

void CursorAnimationInitialize(CursorAnimation *animation) {
	animation->settings = DEFAULT_SETTINGS;
	animation->vfx_count = 0;
	animation->initialized = false;
}

void CursorAnimationReset(CursorAnimation *animation) {
	animation->initialized = false;
}

float NodeToFloat(mpack_node_t node, float default_value) {
	switch (mpack_node_type(node)) {
	case mpack_type_int: return static_cast<float>(node.data->value.i);
	case mpack_type_uint: return static_cast<float>(node.data->value.u);
	case mpack_type_float: return node.data->value.f;
	case mpack_type_double: return static_cast<float>(node.data->value.d);
	case mpack_type_bool: return node.data->value.b ? 1.0f : 0.0f;
	default: return default_value;
	}
}

bool NodeToBool(mpack_node_t node, bool default_value) {
	switch (mpack_node_type(node)) {
	case mpack_type_bool: return node.data->value.b;
	case mpack_type_int:
	case mpack_type_uint:
	case mpack_type_float:
	case mpack_type_double: return NodeToFloat(node, 0.0f) != 0.0f;
	default: return default_value;
	}
}

bool ParseVfxMode(mpack_node_t node, CursorVfxMode *mode) {
	if (mpack_node_type(node) != mpack_type_str) return false;

	constexpr struct {
		const char *name;
		CursorVfxMode mode;
	} VFX_MODE_NAMES[] = {
		{ "sonicboom", CursorVfxMode::SonicBoom },
		{ "ripple", CursorVfxMode::Ripple },
		{ "wireframe", CursorVfxMode::Wireframe },
		{ "railgun", CursorVfxMode::Railgun },
		{ "torpedo", CursorVfxMode::Torpedo },
		{ "pixiedust", CursorVfxMode::PixieDust }
	};
	const char *str = mpack_node_str(node);
	size_t length = mpack_node_strlen(node);
	for (const auto &vfx_mode : VFX_MODE_NAMES) {
		if (strlen(vfx_mode.name) == length && !strncmp(vfx_mode.name, str, length)) {
			*mode = vfx_mode.mode;
			return true;
		}
	}
	return false;
}

// Accepts a single mode name or a list of them, "" disables the effects
void ParseVfxModes(CursorAnimationSettings *settings, mpack_node_t node) {
	settings->vfx_mode_count = 0;
	if (mpack_node_type(node) == mpack_type_array) {
		size_t count = mpack_node_array_length(node);
		for (size_t i = 0; i < count && settings->vfx_mode_count < MAX_CURSOR_VFX_MODES; ++i) {
			if (ParseVfxMode(mpack_node_array_at(node, i), &settings->vfx_modes[settings->vfx_mode_count])) {
				++settings->vfx_mode_count;
			}
		}
	}
	else if (ParseVfxMode(node, &settings->vfx_modes[0])) {
		settings->vfx_mode_count = 1;
	}
}

bool CursorAnimationSetOption(CursorAnimation *animation, const char *name, size_t length, mpack_node_t value) {
	CursorAnimationSettings *settings = &animation->settings;
	const auto Matches = [&](const char *option) {
		return strlen(option) == length && !strncmp(option, name, length);
	};

	if (Matches("animation")) {
		settings->enabled = NodeToBool(value, DEFAULT_SETTINGS.enabled);
		// Start from where the cursor is when turned on
		animation->initialized = false;
	}
	else if (Matches("animation_length")) {
		settings->animation_length = max(0.0f, NodeToFloat(value, DEFAULT_SETTINGS.animation_length));
	}
	else if (Matches("short_animation_length")) {
		settings->short_animation_length = max(0.0f, NodeToFloat(value, DEFAULT_SETTINGS.short_animation_length));
	}
	else if (Matches("trail_size")) {
		settings->trail_size = NodeToFloat(value, DEFAULT_SETTINGS.trail_size);
	}
	else if (Matches("animate_in_insert_mode")) {
		settings->animate_in_insert_mode = NodeToBool(value, DEFAULT_SETTINGS.animate_in_insert_mode);
	}
	else if (Matches("animate_command_line")) {
		settings->animate_command_line = NodeToBool(value, DEFAULT_SETTINGS.animate_command_line);
	}
	else if (Matches("antialiasing")) {
		settings->antialiasing = NodeToBool(value, DEFAULT_SETTINGS.antialiasing);
	}
	else if (Matches("unfocused_outline_width")) {
		settings->unfocused_outline_width = max(0.0f, NodeToFloat(value, DEFAULT_SETTINGS.unfocused_outline_width));
	}
	else if (Matches("vfx_mode")) {
		ParseVfxModes(settings, value);
		CreateVfxs(animation);
	}
	else if (Matches("vfx_opacity")) {
		settings->vfx_opacity = max(0.0f, min(NodeToFloat(value, DEFAULT_SETTINGS.vfx_opacity), 255.0f));
	}
	else if (Matches("vfx_particle_lifetime")) {
		settings->vfx_particle_lifetime = max(0.0f, NodeToFloat(value, DEFAULT_SETTINGS.vfx_particle_lifetime));
	}
	else if (Matches("vfx_particle_highlight_lifetime")) {
		settings->vfx_particle_highlight_lifetime = max(0.0f, NodeToFloat(value, DEFAULT_SETTINGS.vfx_particle_highlight_lifetime));
	}
	else if (Matches("vfx_particle_density")) {
		settings->vfx_particle_density = max(0.0f, NodeToFloat(value, DEFAULT_SETTINGS.vfx_particle_density));
	}
	else if (Matches("vfx_particle_speed")) {
		settings->vfx_particle_speed = NodeToFloat(value, DEFAULT_SETTINGS.vfx_particle_speed);
	}
	else if (Matches("vfx_particle_phase")) {
		settings->vfx_particle_phase = NodeToFloat(value, DEFAULT_SETTINGS.vfx_particle_phase);
	}
	else if (Matches("vfx_particle_curl")) {
		settings->vfx_particle_curl = NodeToFloat(value, DEFAULT_SETTINGS.vfx_particle_curl);
	}
	else {
		return false;
	}
	return true;
}

void SetCornerShape(CursorAnimation *animation, CursorShape shape, float cell_percentage) {
	for (int i = 0; i < 4; ++i) {
		float x = STANDARD_CORNERS[i][0];
		float y = STANDARD_CORNERS[i][1];
		CursorCorner *corner = &animation->corners[i];
		switch (shape) {
		case CursorShape::Vertical: {
			// Move the right side over to the bar width
			corner->relative_x = (x + 0.5f) * cell_percentage - 0.5f;
			corner->relative_y = y;
		} break;
		case CursorShape::Horizontal: {
			// Same as above, but for the bottom of the cell
			corner->relative_x = x;
			corner->relative_y = -((-y + 0.5f) * cell_percentage - 0.5f);
		} break;
		case CursorShape::None:
		case CursorShape::Block: {
			corner->relative_x = x;
			corner->relative_y = y;
		} break;
		}
	}
}

void CornerDestination(const CursorCorner *corner, const CursorAnimationTarget *target,
	float center_x, float center_y, float *x, float *y) {
	*x = center_x + corner->relative_x * target->width;
	*y = center_y + corner->relative_y * target->height;
}

// How much the corner is aligned with the direction of travel. Corners in
// front move faster than the ones in the back, which creates the trail.
float CornerDirectionAlignment(const CursorCorner *corner, const CursorAnimationTarget *target,
	float center_x, float center_y) {
	float destination_x, destination_y;
	CornerDestination(corner, target, center_x, center_y, &destination_x, &destination_y);

	float corner_direction_x = corner->relative_x;
	float corner_direction_y = corner->relative_y;
	Normalize(&corner_direction_x, &corner_direction_y);
	float travel_x = destination_x - corner->previous_destination_x;
	float travel_y = destination_y - corner->previous_destination_y;
	Normalize(&travel_x, &travel_y);
	return travel_x * corner_direction_x + travel_y * corner_direction_y;
}

void CornerJump(CursorCorner *corner, const CursorAnimationSettings *settings, const CursorAnimationTarget *target,
	float center_x, float center_y, float alignment) {
	float destination_x, destination_y;
	CornerDestination(corner, target, center_x, center_y, &destination_x, &destination_y);
	float jump_x = (destination_x - corner->previous_destination_x) / target->width;
	float jump_y = (destination_y - corner->previous_destination_y) / target->height;

	if (fabsf(jump_x) <= 2.001f && fabsf(jump_y) <= 0.001f) {
		// Short jumps of up to two characters, typically when typing in insert mode
		corner->animation_length = min(settings->animation_length, settings->short_animation_length);
	}
	else {
		float leading = settings->animation_length * max(0.0f, min(1.0f - settings->trail_size, 1.0f));
		float trailing = settings->animation_length;
		corner->animation_length = Lerp(trailing, leading, alignment);
	}
}

bool CornerUpdate(CursorCorner *corner, const CursorAnimationTarget *target,
	float center_x, float center_y, float dt, bool immediate) {
	float destination_x, destination_y;
	CornerDestination(corner, target, center_x, center_y, &destination_x, &destination_y);
	if (destination_x != corner->previous_destination_x || destination_y != corner->previous_destination_y) {
		corner->animation_x.position = destination_x - corner->x;
		corner->animation_y.position = destination_y - corner->y;
		corner->previous_destination_x = destination_x;
		corner->previous_destination_y = destination_y;
	}

	if (immediate) {
		SpringReset(&corner->animation_x);
		SpringReset(&corner->animation_y);
		corner->x = destination_x;
		corner->y = destination_y;
		return false;
	}

	bool animating = SpringUpdate(&corner->animation_x, dt, corner->animation_length);
	animating |= SpringUpdate(&corner->animation_y, dt, corner->animation_length);
	corner->x = destination_x - corner->animation_x.position;
	corner->y = destination_y - corner->animation_y.position;
	return animating;
}

void RotateVector(float *x, float *y, float rotation) {
	float s = sinf(rotation);
	float c = cosf(rotation);
	float rotated_x = *x * c - *y * s;
	float rotated_y = *x * s + *y * c;
	*x = rotated_x;
	*y = rotated_y;
}

bool HighlightUpdate(CursorVfx *vfx, const CursorAnimationSettings *settings, float center_x, float center_y, float dt) {
	vfx->center_x = center_x;
	vfx->center_y = center_y;
	if (settings->vfx_particle_highlight_lifetime > 0.0f) {
		vfx->t = min(vfx->t + dt / settings->vfx_particle_highlight_lifetime, 1.0f);
	}
	else {
		vfx->t = 1.0f;
	}
	return vfx->t < 1.0f;
}

bool TrailUpdate(CursorVfx *vfx, const CursorAnimationSettings *settings, const CursorAnimationTarget *target,
	float center_x, float center_y, float dt) {
	// Age the particles, removing the dead ones without keeping their order
	for (int i = 0; i < vfx->particle_count;) {
		CursorParticle *particle = &vfx->particles[i];
		particle->lifetime -= dt;
		if (particle->lifetime <= 0.0f) {
			*particle = vfx->particles[--vfx->particle_count];
		}
		else {
			++i;
		}
	}

	for (int i = 0; i < vfx->particle_count; ++i) {
		CursorParticle *particle = &vfx->particles[i];
		particle->x += particle->speed_x * dt;
		particle->y += particle->speed_y * dt;
		RotateVector(&particle->speed_x, &particle->speed_y, dt * particle->rotation_speed);
	}

	if (center_x != vfx->previous_destination_x || center_y != vfx->previous_destination_y) {
		if (!target->immediate) {
			float travel_x = center_x - vfx->previous_destination_x;
			float travel_y = center_y - vfx->previous_destination_y;
			float travel_distance = sqrtf(travel_x * travel_x + travel_y * travel_y);
			float travel_cells = travel_distance / target->height;

			// More particles the further the cursor travels
			float particle_count_f = travel_cells * settings->vfx_particle_density + vfx->count_remainder;
			int particle_count = static_cast<int>(particle_count_f);
			vfx->count_remainder = particle_count_f - particle_count;

			float travel_direction_x = travel_x;
			float travel_direction_y = travel_y;
			Normalize(&travel_direction_x, &travel_direction_y);

			for (int i = 0; i < particle_count; ++i) {
				float t = static_cast<float>(i + 1) / particle_count;

				CursorParticle particle {
					.lifetime = t * settings->vfx_particle_lifetime,
					.color = target->color
				};
				switch (vfx->mode) {
				case CursorVfxMode::Railgun: {
					float phase = t / PI * settings->vfx_particle_phase * travel_cells;
					particle.speed_x = sinf(phase) * 2.0f * settings->vfx_particle_speed;
					particle.speed_y = cosf(phase) * 2.0f * settings->vfx_particle_speed;
					particle.x = vfx->previous_destination_x + travel_x * t;
					particle.y = vfx->previous_destination_y + travel_y * t;
					particle.rotation_speed = PI * settings->vfx_particle_curl;
				} break;
				case CursorVfxMode::Torpedo:
				case CursorVfxMode::PixieDust: {
					float direction_x, direction_y;
					RngNextDirection(&vfx->rng_state, &direction_x, &direction_y);
					if (vfx->mode == CursorVfxMode::Torpedo) {
						direction_x -= travel_direction_x * 1.5f;
						direction_y -= travel_direction_y * 1.5f;
						Normalize(&direction_x, &direction_y);
						particle.speed_x = direction_x * settings->vfx_particle_speed;
						particle.speed_y = direction_y * settings->vfx_particle_speed;
					}
					else {
						particle.speed_x = direction_x * 0.5f * 3.0f * settings->vfx_particle_speed;
						particle.speed_y = (0.4f + fabsf(direction_y)) * 3.0f * settings->vfx_particle_speed;
					}

					float position = RngNextFloat(&vfx->rng_state);
					particle.x = vfx->previous_destination_x + travel_x * position;
					particle.y = vfx->previous_destination_y + travel_y * position + target->height * 0.5f;
					particle.rotation_speed = (RngNextFloat(&vfx->rng_state) - 0.5f) * PI * 0.5f * settings->vfx_particle_curl;
				} break;
				default: {
				} break;
				}

				if (vfx->particle_count < MAX_CURSOR_PARTICLES) {
					vfx->particles[vfx->particle_count++] = particle;
				}
			}
		}

		vfx->previous_destination_x = center_x;
		vfx->previous_destination_y = center_y;
	}

	return vfx->particle_count > 0;
}

bool CursorAnimationUpdate(CursorAnimation *animation, const CursorAnimationTarget *target, float dt) {
	const CursorAnimationSettings *settings = &animation->settings;
	float center_x = target->x + target->width * 0.5f;
	float center_y = target->y + target->height * 0.5f;

	bool immediate = target->immediate;
	bool jumped = target->x != animation->destination_x || target->y != animation->destination_y;
	animation->destination_x = target->x;
	animation->destination_y = target->y;

	if (!animation->initialized) {
		// Appear right at the destination instead of flying in from somewhere
		animation->initialized = true;
		animation->shape = target->shape;
		SetCornerShape(animation, target->shape, target->cell_percentage);
		for (int i = 0; i < 4; ++i) {
			CursorCorner *corner = &animation->corners[i];
			CornerDestination(corner, target, center_x, center_y, &corner->x, &corner->y);
			corner->previous_destination_x = corner->x;
			corner->previous_destination_y = corner->y;
			corner->animation_length = 0.0f;
			SpringReset(&corner->animation_x);
			SpringReset(&corner->animation_y);
		}
		for (int i = 0; i < animation->vfx_count; ++i) {
			animation->vfxs[i].previous_destination_x = center_x;
			animation->vfxs[i].previous_destination_y = center_y;
		}
		jumped = false;
	}

	if (target->shape != animation->shape) {
		animation->shape = target->shape;
		SetCornerShape(animation, target->shape, target->cell_percentage);
		for (int i = 0; i < animation->vfx_count; ++i) {
			CursorVfx *vfx = &animation->vfxs[i];
			vfx->t = 0.0f;
			vfx->count_remainder = 0.0f;
		}
	}

	if (jumped) {
		for (int i = 0; i < animation->vfx_count; ++i) {
			if (IsHighlight(animation->vfxs[i].mode)) {
				animation->vfxs[i].t = 0.0f;
			}
		}

		float alignments[4];
		float min_alignment = INFINITY;
		float max_alignment = -INFINITY;
		for (int i = 0; i < 4; ++i) {
			alignments[i] = CornerDirectionAlignment(&animation->corners[i], target, center_x, center_y);
			min_alignment = min(min_alignment, alignments[i]);
			max_alignment = max(max_alignment, alignments[i]);
		}
		float range = max_alignment - min_alignment;
		for (int i = 0; i < 4; ++i) {
			float alignment = range > 0.0f ? max(0.0f, min((alignments[i] - min_alignment) / range, 1.0f)) : 1.0f;
			CornerJump(&animation->corners[i], settings, target, center_x, center_y, alignment);
		}
	}

	bool animating = false;
	for (int i = 0; i < 4; ++i) {
		animating |= CornerUpdate(&animation->corners[i], target, center_x, center_y, dt, immediate);
	}

	for (int i = 0; i < animation->vfx_count; ++i) {
		CursorVfx *vfx = &animation->vfxs[i];
		if (IsHighlight(vfx->mode)) {
			animating |= HighlightUpdate(vfx, settings, center_x, center_y, dt);
		}
		else {
			animating |= TrailUpdate(vfx, settings, target, center_x, center_y, dt);
		}
	}

	return animating;
}
