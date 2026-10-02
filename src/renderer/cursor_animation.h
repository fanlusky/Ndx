#pragma once

// Animated cursor and cursor particle effects, modeled after neovide's cursor renderer.
// This file only simulates the animation, drawing is done by the renderer.

enum class CursorShape;

enum class CursorVfxMode : uint8_t {
	SonicBoom,
	Ripple,
	Wireframe,
	Railgun,
	Torpedo,
	PixieDust
};

constexpr int MAX_CURSOR_VFX_MODES = 6;
constexpr int MAX_CURSOR_PARTICLES = 1024;

// Set from nvim through the g:ndx_cursor_* variables
struct CursorAnimationSettings {
	bool enabled;
	float animation_length;
	float short_animation_length;
	float trail_size;
	bool animate_in_insert_mode;
	bool animate_command_line;
	bool antialiasing;
	float unfocused_outline_width;
	bool smooth_blink;

	CursorVfxMode vfx_modes[MAX_CURSOR_VFX_MODES];
	int vfx_mode_count;
	float vfx_opacity;
	float vfx_particle_lifetime;
	float vfx_particle_highlight_lifetime;
	float vfx_particle_density;
	float vfx_particle_speed;
	float vfx_particle_phase;
	float vfx_particle_curl;
};

// Critically damped spring, position is the remaining distance to the destination
struct SpringAnimation {
	float position;
	float velocity;
};

void SpringReset(SpringAnimation *spring);
// Moves position towards 0, returns whether it hasn't arrived yet
bool SpringUpdate(SpringAnimation *spring, float dt, float animation_length);

enum class BlinkState : uint8_t {
	Waiting,
	On,
	Off
};

// Cursor blinking as set by blinkwait, blinkon and blinkoff in guicursor, times are in seconds
struct CursorBlink {
	BlinkState state;
	double transition_time;
	bool initialized;
	int row;
	int col;
	const void *mode_info;
};

struct CursorBlinkTimes {
	int blinkwait;
	int blinkon;
	int blinkoff;
};

// Restarts blinking when the cursor moves or changes mode
void CursorBlinkUpdate(CursorBlink *blink, const CursorBlinkTimes *times, int row, int col, const void *mode_info, double now);
bool CursorBlinkIsStatic(const CursorBlinkTimes *times);
// The next time the blink state changes, INFINITY for a static cursor
double CursorBlinkDeadline(const CursorBlink *blink, const CursorBlinkTimes *times);
bool CursorBlinkVisible(const CursorBlink *blink);
// Opacity fading in and out with smooth blinking, from 0 to 1
float CursorBlinkOpacity(const CursorBlink *blink, const CursorBlinkTimes *times, double now);

struct CursorCorner {
	float x;
	float y;
	// Position relative to the cursor center, in cursor cell sizes
	float relative_x;
	float relative_y;
	float previous_destination_x;
	float previous_destination_y;
	SpringAnimation animation_x;
	SpringAnimation animation_y;
	float animation_length;
};

struct CursorParticle {
	float x;
	float y;
	float speed_x;
	float speed_y;
	float rotation_speed;
	float lifetime;
	uint32_t color;
};

struct CursorVfx {
	CursorVfxMode mode;

	// Highlight modes
	float t;
	float center_x;
	float center_y;

	// Trail modes
	float previous_destination_x;
	float previous_destination_y;
	float count_remainder;
	uint64_t rng_state;
	CursorParticle particles[MAX_CURSOR_PARTICLES];
	int particle_count;
};

// Where the cursor should end up, in pixels
struct CursorAnimationTarget {
	float x;
	float y;
	float width;
	float height;
	CursorShape shape;
	// Fraction of the cell covered by a vertical or horizontal cursor
	float cell_percentage;
	bool immediate;
	// How far the text under the cursor scrolled since the last update, the
	// cursor moves along with it instead of animating towards it
	float scroll_delta_y;
	// Cursor background color, used for the particles
	uint32_t color;
};

struct CursorAnimation {
	CursorAnimationSettings settings;

	CursorCorner corners[4];
	CursorVfx vfxs[MAX_CURSOR_VFX_MODES];
	int vfx_count;
	CursorBlink blink;

	bool initialized;
	float destination_x;
	float destination_y;
	CursorShape shape;
};

// Option values from nvim, the default is used for nil and other unexpected types
float NodeToFloat(mpack_node_t node, float default_value);
bool NodeToBool(mpack_node_t node, bool default_value);

void CursorAnimationInitialize(CursorAnimation *animation);
// Applies a g:ndx_cursor_<name> variable, a nil value restores the default.
// Returns false if the name is unknown.
bool CursorAnimationSetOption(CursorAnimation *animation, const char *name, size_t length, mpack_node_t value);
// Snaps the cursor to its next destination instead of animating from the old one
void CursorAnimationReset(CursorAnimation *animation);
// Advances the animation by dt seconds, returns whether it's still animating
bool CursorAnimationUpdate(CursorAnimation *animation, const CursorAnimationTarget *target, float dt);
