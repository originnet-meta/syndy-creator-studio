/******************************************************************************
    Copyright (C) 2026 Uniflow, Inc.
    Author: Kim Taehyung <gaiaengine@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

/******************************************************************************
    Modifications Copyright (C) 2026 Uniflow, Inc.
    Author: Kim Taehyung <gaiaengine@gmail.com>
    Modified: 2026-02-16
    Notes: Changes for Syndy Creator Studio.
******************************************************************************/

#include <obs-module.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "vspace-gltf-loader.h"
#include "vspace-source.h"

#define S_MODEL_PATH "model_path"
#define S_DRACO_ENABLED "draco_enabled"
#define S_DRACO_DECODER "draco_decoder"
#define S_BACKGROUND_COLOR "background_color"
#define S_DRACO_DECODER_AUTO "auto"
#define S_DRACO_DECODER_BUILTIN "builtin"
#define S_DRACO_DECODER_EXTERNAL "external"

#define VSPACE_CAMERA_ORBIT_DEG_PER_PIXEL 0.20f
#define VSPACE_CAMERA_ZOOM_STEP 0.90f
#define VSPACE_CAMERA_ZOOM_DRAG_STEPS_PER_PIXEL 0.015f
#define VSPACE_CAMERA_DOLLY_STEPS_PER_PIXEL 0.020f

#define VSPACE_RENDER_AA_SCALE 2u
#define VSPACE_RENDER_AA_MAX_DIM 4096u

#define VSPACE_GRID_TARGET_PIXEL_STEP 96.0f
#define VSPACE_GRID_MIN_STEP 0.01f
#define VSPACE_GRID_MAX_HALF_LINES 64

struct vspace_gpu_mesh {
	gs_vertbuffer_t *vertex_buffer;
	gs_indexbuffer_t *index_buffer;
	gs_vertbuffer_t *wireframe_vertex_buffer;
	gs_indexbuffer_t *wireframe_index_buffer;
	int32_t material_index;
	size_t draw_vertex_count;
	size_t draw_index_count;
	size_t wireframe_vertex_count;
	size_t wireframe_index_count;
};

struct vspace_source {
	obs_source_t *source;
	gs_effect_t *effect;
	gs_eparam_t *effect_base_color_param;
	gs_eparam_t *effect_camera_position_param;
	gs_eparam_t *effect_light_direction_param;
	gs_eparam_t *effect_ambient_strength_param;
	gs_eparam_t *effect_diffuse_strength_param;
	gs_eparam_t *effect_specular_strength_param;
	gs_eparam_t *effect_shininess_param;
	gs_eparam_t *effect_grid_forward_param;
	gs_eparam_t *effect_grid_right_param;
	gs_eparam_t *effect_grid_up_param;
	gs_eparam_t *effect_grid_tan_half_fov_param;
	gs_eparam_t *effect_grid_aspect_param;
	gs_eparam_t *effect_grid_step_param;
	gs_eparam_t *effect_grid_origin_param;
	gs_eparam_t *effect_grid_extent_param;
	gs_eparam_t *effect_composite_image_param;
	gs_eparam_t *effect_composite_background_alpha_param;
	char *model_path;
	char *draco_decoder;
	struct vspace_gpu_mesh *gpu_meshes;
	size_t gpu_mesh_count;
	gs_vertbuffer_t *bounds_line_buffer;
	gs_vertbuffer_t *grid_triangle_buffer;
	gs_texrender_t *model_texrender;
	gs_image_file4_t base_color_image;
	size_t draw_vertex_count;
	size_t draw_index_count;
	size_t wireframe_vertex_count;
	size_t wireframe_index_count;
	bool base_color_image_valid;
	pthread_t worker_thread;
	pthread_mutex_t worker_mutex;
	pthread_mutex_t camera_mutex;
	os_event_t *worker_event;
	bool worker_thread_active;
	bool worker_mutex_valid;
	bool camera_mutex_valid;
	volatile bool worker_stop;
	struct {
		char *model_path;
		char *draco_decoder;
		bool draco_enabled;
		bool has_job;
		uint64_t token;
	} worker_job;
	struct {
		struct vspace_cpu_payload payload;
		gs_image_file4_t base_color_image;
		bool base_color_image_valid;
		bool ready;
		uint64_t token;
	} pending_upload;
	uint64_t worker_next_token;
	uint64_t worker_cancel_token;
	struct gs_device_loss device_loss_callbacks;
	bool device_loss_callbacks_registered;
	volatile bool device_loss_active;
	volatile bool device_rebuild_pending;
	bool draco_enabled;
	bool active;
	bool showing;
	bool effect_load_attempted;
	bool diagnostics_logged_upload;
	bool diagnostics_logged_draw;
	uint32_t width;
	uint32_t height;
	uint32_t background_color;
	struct vec3 model_bounds_min;
	struct vec3 model_bounds_max;
	struct vec3 camera_target;
	struct vec3 camera_up;
	struct vec3 default_camera_position;
	float camera_orbit_distance;
	float default_camera_fov_deg;
	float default_camera_znear;
	float default_camera_zfar;
	struct vec3 default_light_direction;
	float default_light_ambient_strength;
	float default_light_diffuse_strength;
	float default_light_specular_strength;
	float default_light_shininess;
	bool model_bounds_valid;
	bool default_camera_valid;
	volatile bool inspect_render_mode;
	bool camera_manual_override;
	bool camera_drag_orbit;
	bool camera_drag_pan;
	bool camera_drag_zoom;
	bool camera_drag_dolly;
	int32_t camera_last_input_x;
	int32_t camera_last_input_y;
	bool camera_last_input_valid;
	struct vec3 camera_orbit_pitch_axis;
	bool camera_orbit_pitch_axis_valid;
};

static const char *vspace_source_log_name(const struct vspace_source *context)
{
	if (context && context->source)
		return obs_source_get_name(context->source);

	return "vspace_source";
}

static inline void vspace_source_lock_camera(struct vspace_source *context)
{
	if (context && context->camera_mutex_valid)
		pthread_mutex_lock(&context->camera_mutex);
}

static inline void vspace_source_unlock_camera(struct vspace_source *context)
{
	if (context && context->camera_mutex_valid)
		pthread_mutex_unlock(&context->camera_mutex);
}

static bool vspace_source_nullable_streq(const char *a, const char *b)
{
	if (!a)
		a = "";
	if (!b)
		b = "";

	return strcmp(a, b) == 0;
}

static float vspace_source_model_extent_locked(const struct vspace_source *context)
{
	float extent_x;
	float extent_y;
	float extent_z;
	float extent_max;

	if (!context || !context->model_bounds_valid)
		return 1.0f;

	extent_x = fabsf(context->model_bounds_max.x - context->model_bounds_min.x) * 0.5f;
	extent_y = fabsf(context->model_bounds_max.y - context->model_bounds_min.y) * 0.5f;
	extent_z = fabsf(context->model_bounds_max.z - context->model_bounds_min.z) * 0.5f;
	extent_max = fmaxf(extent_x, fmaxf(extent_y, extent_z));

	return extent_max > 0.001f ? extent_max : 1.0f;
}

static void vspace_source_update_camera_clip_locked(struct vspace_source *context)
{
	float extent_max;
	float half_depth = 1.0f;
	float near_margin;
	float far_margin;
	float grid_far_plane;
	float near_plane;
	float far_plane;

	if (!context)
		return;

	extent_max = vspace_source_model_extent_locked(context);
	if (context->model_bounds_valid)
		half_depth = fabsf(context->model_bounds_max.y - context->model_bounds_min.y) * 0.5f;

	/*
	 * Keep clip planes broad enough for world-grid visibility in the interact
	 * viewport while preserving reasonable depth precision for model rendering.
	 */
	near_margin = half_depth + extent_max * 2.5f;
	far_margin = half_depth + extent_max * 2.5f;
	near_plane = context->camera_orbit_distance - near_margin;
	if (near_plane < 0.05f)
		near_plane = 0.05f;

	far_plane = context->camera_orbit_distance + far_margin;
	grid_far_plane = context->camera_orbit_distance + fmaxf(extent_max * 40.0f, 64.0f);
	if (far_plane < grid_far_plane)
		far_plane = grid_far_plane;
	if (far_plane < near_plane + 10.0f)
		far_plane = near_plane + 10.0f;

	context->default_camera_znear = near_plane;
	context->default_camera_zfar = far_plane;
}

static void vspace_source_get_camera_basis_locked(const struct vspace_source *context, struct vec3 *forward,
						    struct vec3 *right, struct vec3 *up)
{
	struct vec3 world_up;
	struct vec3 basis_forward;
	struct vec3 basis_up;
	struct vec3 basis_right;
	struct vec3 fallback_axis;
	struct vec3 pitch_axis;

	if (!context || !forward || !right || !up)
		return;

	vec3_sub(&basis_forward, &context->camera_target, &context->default_camera_position);
	if (vec3_len(&basis_forward) <= 0.0001f)
		vec3_set(&basis_forward, 0.0f, 1.0f, 0.0f);
	else
		vec3_norm(&basis_forward, &basis_forward);
	vec3_set(&fallback_axis, 1.0f, 0.0f, 0.0f);

	/* Roll lock: right is derived from world up (+Z). */
	vec3_set(&world_up, 0.0f, 0.0f, 1.0f);
	vec3_cross(&basis_right, &basis_forward, &world_up);
	if (vec3_len(&basis_right) <= 0.0001f) {
		/*
		 * At the pole forward || world_up, keep right-axis continuity from
		 * the orbit pitch axis to avoid sudden sign flips.
		 */
		if (context->camera_orbit_pitch_axis_valid && vec3_len(&context->camera_orbit_pitch_axis) > 0.0001f) {
			basis_right = context->camera_orbit_pitch_axis;
		} else {
			vec3_set(&fallback_axis, 0.0f, 1.0f, 0.0f);
			if (fabsf(vec3_dot(&basis_forward, &fallback_axis)) > 0.95f)
				vec3_set(&fallback_axis, 1.0f, 0.0f, 0.0f);
			vec3_cross(&basis_right, &basis_forward, &fallback_axis);
		}
	}
	if (vec3_len(&basis_right) <= 0.0001f) {
		vec3_cross(&basis_right, &basis_forward, &context->camera_up);
	}
	if (vec3_len(&basis_right) <= 0.0001f) {
		vec3_cross(&basis_right, &basis_forward, &fallback_axis);
	}
	if (vec3_len(&basis_right) <= 0.0001f)
		vec3_set(&basis_right, 1.0f, 0.0f, 0.0f);
	else
		vec3_norm(&basis_right, &basis_right);

	/* Keep right-axis sign continuity between drag segments. */
	if (context->camera_orbit_pitch_axis_valid && vec3_len(&context->camera_orbit_pitch_axis) > 0.0001f) {
		pitch_axis = context->camera_orbit_pitch_axis;
		vec3_norm(&pitch_axis, &pitch_axis);
		if (vec3_dot(&basis_right, &pitch_axis) < 0.0f)
			vec3_mulf(&basis_right, &basis_right, -1.0f);
	}

	vec3_cross(&basis_up, &basis_right, &basis_forward);
	if (vec3_len(&basis_up) <= 0.0001f)
		vec3_set(&basis_up, 0.0f, 0.0f, 1.0f);
	else
		vec3_norm(&basis_up, &basis_up);

	*forward = basis_forward;
	*right = basis_right;
	*up = basis_up;
}

static void vspace_source_orthonormalize_camera_locked(struct vspace_source *context)
{
	struct vec3 forward;
	struct vec3 right;
	struct vec3 up;

	if (!context)
		return;

	vspace_source_get_camera_basis_locked(context, &forward, &right, &up);
	context->camera_up = up;
}

static void vspace_source_recompute_camera_position_locked(struct vspace_source *context)
{
	struct vec3 orbit_offset;
	float orbit_len;

	if (!context)
		return;

	if (context->camera_orbit_distance < 0.05f)
		context->camera_orbit_distance = 0.05f;

	if (context->default_camera_valid) {
		vec3_sub(&orbit_offset, &context->default_camera_position, &context->camera_target);
		orbit_len = vec3_len(&orbit_offset);
		if (orbit_len > 0.0001f) {
			vec3_mulf(&orbit_offset, &orbit_offset, context->camera_orbit_distance / orbit_len);
		} else {
			vec3_set(&orbit_offset, 0.0f, -context->camera_orbit_distance, 0.0f);
		}
	} else {
		vec3_set(&orbit_offset, 0.0f, -context->camera_orbit_distance, 0.0f);
	}

	vec3_add(&context->default_camera_position, &context->camera_target, &orbit_offset);
	if (vec3_len(&context->camera_up) <= 0.0001f)
		vec3_set(&context->camera_up, 0.0f, 0.0f, 1.0f);
	vspace_source_orthonormalize_camera_locked(context);
	context->default_camera_valid = true;
}

static void vspace_source_apply_camera_view_matrix(const struct vec3 *camera_position, const struct vec3 *camera_target,
						     const struct vec3 *camera_up_hint)
{
	struct vec3 forward;
	struct vec3 right;
	struct vec3 up;
	struct matrix4 view;
	float forward_len;
	float right_len;
	float up_len;

	if (!camera_position || !camera_target || !camera_up_hint)
		return;

	vec3_sub(&forward, camera_target, camera_position);
	forward_len = vec3_len(&forward);
	if (forward_len <= 0.0001f)
		vec3_set(&forward, 0.0f, 1.0f, 0.0f);
	else
		vec3_mulf(&forward, &forward, 1.0f / forward_len);

	up = *camera_up_hint;
	if (vec3_len(&up) <= 0.0001f)
		vec3_set(&up, 0.0f, 0.0f, 1.0f);
	else
		vec3_norm(&up, &up);

	vec3_cross(&right, &forward, &up);
	right_len = vec3_len(&right);
	if (right_len <= 0.0001f) {
		vec3_set(&up, 0.0f, 0.0f, 1.0f);
		vec3_cross(&right, &forward, &up);
		if (vec3_len(&right) <= 0.0001f) {
			vec3_set(&up, 0.0f, 1.0f, 0.0f);
			vec3_cross(&right, &forward, &up);
		}
	}
	if (vec3_len(&right) <= 0.0001f)
		vec3_set(&right, 1.0f, 0.0f, 0.0f);
	else
		vec3_norm(&right, &right);

	vec3_cross(&up, &right, &forward);
	up_len = vec3_len(&up);
	if (up_len <= 0.0001f)
		vec3_set(&up, 0.0f, 0.0f, 1.0f);
	else
		vec3_mulf(&up, &up, 1.0f / up_len);

	matrix4_identity(&view);
	vec4_set(&view.x, right.x, up.x, -forward.x, 0.0f);
	vec4_set(&view.y, right.y, up.y, -forward.y, 0.0f);
	vec4_set(&view.z, right.z, up.z, -forward.z, 0.0f);
	vec4_set(&view.t, -vec3_dot(&right, camera_position), -vec3_dot(&up, camera_position),
		 vec3_dot(&forward, camera_position), 1.0f);
	gs_matrix_set(&view);
}

static void vspace_source_apply_camera_projection_matrix(float fov_deg, float aspect, float znear, float zfar)
{
	float ymax;
	float ymin;
	float xmin;
	float xmax;

	if (fov_deg <= 0.0f)
		fov_deg = 50.0f;
	if (aspect < 0.1f)
		aspect = 0.1f;
	if (znear < 0.01f)
		znear = 0.01f;
	if (zfar < znear + 1.0f)
		zfar = znear + 1.0f;

	ymax = znear * tanf(RAD(fov_deg) * 0.5f);
	ymin = -ymax;
	xmin = ymin * aspect;
	xmax = ymax * aspect;

	/*
	 * OBS frustum parameter order is (left, right, top, bottom).
	 * Keep top/bottom swapped here to avoid vertical inversion in this path.
	 */
	gs_frustum(xmin, xmax, ymax, ymin, znear, zfar);
}

static bool vspace_source_should_auto_fit_camera(struct vspace_source *context)
{
	bool model_bounds_valid = false;
	bool manual_override = false;

	if (!context)
		return false;

	vspace_source_lock_camera(context);
	model_bounds_valid = context->model_bounds_valid;
	manual_override = context->camera_manual_override;
	vspace_source_unlock_camera(context);

	return model_bounds_valid && !manual_override;
}

static void vspace_source_begin_camera_drag(struct vspace_source *context, bool orbit, bool pan, bool zoom, bool dolly,
					      int32_t x, int32_t y)
{
	if (!context)
		return;

	vspace_source_lock_camera(context);
	context->camera_drag_orbit = orbit;
	context->camera_drag_pan = pan;
	context->camera_drag_zoom = zoom;
	context->camera_drag_dolly = dolly;
	context->camera_last_input_x = x;
	context->camera_last_input_y = y;
	context->camera_last_input_valid = true;
	context->camera_manual_override = true;
	vspace_source_unlock_camera(context);
}

static void vspace_source_end_camera_drag(struct vspace_source *context, bool orbit, bool pan, bool zoom, bool dolly)
{
	if (!context)
		return;

	vspace_source_lock_camera(context);
	if (orbit)
		context->camera_drag_orbit = false;
	if (pan)
		context->camera_drag_pan = false;
	if (zoom)
		context->camera_drag_zoom = false;
	if (dolly)
		context->camera_drag_dolly = false;
	if (!context->camera_drag_orbit && !context->camera_drag_pan && !context->camera_drag_zoom &&
	    !context->camera_drag_dolly)
		context->camera_last_input_valid = false;
	vspace_source_unlock_camera(context);
}

static void vspace_source_rotate_vec3_axis_angle(struct vec3 *dst, const struct vec3 *src, const struct vec3 *axis,
						 float angle)
{
	struct vec3 axis_normalized;
	struct vec3 term_axis;
	struct vec3 term_cross;
	struct vec3 term_parallel;
	struct vec3 cross_axis_src;
	float cosine;
	float sine;
	float dot_axis_src;

	if (!dst || !src || !axis)
		return;

	if (!isfinite(angle) || fabsf(angle) <= 0.000001f) {
		vec3_copy(dst, src);
		return;
	}

	axis_normalized = *axis;
	if (vec3_len(&axis_normalized) <= 0.0001f) {
		vec3_copy(dst, src);
		return;
	}
	vec3_norm(&axis_normalized, &axis_normalized);

	cosine = cosf(angle);
	sine = sinf(angle);
	dot_axis_src = vec3_dot(&axis_normalized, src);

	/* Rodrigues' rotation formula: v' = v*cos(a) + (k x v)*sin(a) + k*(k.v)*(1-cos(a)) */
	vec3_mulf(&term_axis, src, cosine);
	vec3_cross(&cross_axis_src, &axis_normalized, src);
	vec3_mulf(&term_cross, &cross_axis_src, sine);
	vec3_mulf(&term_parallel, &axis_normalized, dot_axis_src * (1.0f - cosine));

	vec3_add(dst, &term_axis, &term_cross);
	vec3_add(dst, dst, &term_parallel);
}

static void vspace_source_orbit_camera(struct vspace_source *context, int32_t delta_x, int32_t delta_y)
{
	struct vec3 orbit_offset;
	float sensitivity;
	struct vec3 world_up = {0.0f, 0.0f, 1.0f};
	struct vec3 candidate_right;
	struct vec3 stable_right;
	float axis_dot;

	if (!context)
		return;
	if (delta_x == 0 && delta_y == 0)
		return;

	vspace_source_lock_camera(context);
	if (!context->model_bounds_valid) {
		vspace_source_unlock_camera(context);
		return;
	}

	if (!context->default_camera_valid)
		vspace_source_recompute_camera_position_locked(context);

	vec3_sub(&orbit_offset, &context->default_camera_position, &context->camera_target);
	sensitivity = RAD(VSPACE_CAMERA_ORBIT_DEG_PER_PIXEL);

	/* 1. Yaw around global Z (Turntable style) */
	vspace_source_rotate_vec3_axis_angle(&orbit_offset, &orbit_offset, &world_up, -(float)delta_x * sensitivity);

	/* 2. Pitch axis from azimuth with sign continuity.
	 * Without continuity, atan2 branch changes can flip the axis by 180 degrees near poles and stall pitch. */
	float azimuth = atan2f(orbit_offset.x, -orbit_offset.y);
	vec3_set(&candidate_right, cosf(azimuth), sinf(azimuth), 0.0f);
	if (vec3_len(&candidate_right) <= 0.0001f)
		vec3_set(&candidate_right, 1.0f, 0.0f, 0.0f);
	else
		vec3_norm(&candidate_right, &candidate_right);

	if (context->camera_orbit_pitch_axis_valid) {
		axis_dot = vec3_dot(&candidate_right, &context->camera_orbit_pitch_axis);
		if (axis_dot < 0.0f)
			vec3_mulf(&candidate_right, &candidate_right, -1.0f);
	}

	stable_right = candidate_right;
	context->camera_orbit_pitch_axis = stable_right;
	context->camera_orbit_pitch_axis_valid = true;

	/* Invert vertical drag direction for pitch only. */
	vspace_source_rotate_vec3_axis_angle(&orbit_offset, &orbit_offset, &stable_right, -(float)delta_y * sensitivity);

	vec3_add(&context->default_camera_position, &context->camera_target, &orbit_offset);
	context->camera_orbit_distance = vec3_len(&orbit_offset);
	context->camera_manual_override = true;

	vspace_source_orthonormalize_camera_locked(context);
	vspace_source_update_camera_clip_locked(context);
	vspace_source_unlock_camera(context);
}

static void vspace_source_pan_camera(struct vspace_source *context, int32_t delta_x, int32_t delta_y)
{
	struct vec3 forward;
	struct vec3 right;
	struct vec3 up;
	struct vec3 delta_pan;
	float aspect;
	float view_height;
	float view_width;
	float pan_right;
	float pan_up;

	if (!context)
		return;

	vspace_source_lock_camera(context);
	if (!context->model_bounds_valid) {
		vspace_source_unlock_camera(context);
		return;
	}
	if (!context->default_camera_valid)
		vspace_source_recompute_camera_position_locked(context);

	aspect = (float)context->width / (float)(context->height ? context->height : 1);
	view_height = 2.0f * tanf(RAD(context->default_camera_fov_deg * 0.5f)) * context->camera_orbit_distance;
	view_width = view_height * fmaxf(aspect, 0.1f);
	pan_right = -((float)delta_x / (float)(context->width ? context->width : 1)) * view_width;
	pan_up = ((float)delta_y / (float)(context->height ? context->height : 1)) * view_height;

	vspace_source_get_camera_basis_locked(context, &forward, &right, &up);

	vec3_mulf(&right, &right, pan_right);
	vec3_mulf(&up, &up, pan_up);
	vec3_add(&delta_pan, &right, &up);
	vec3_add(&context->camera_target, &context->camera_target, &delta_pan);
	vec3_add(&context->default_camera_position, &context->default_camera_position, &delta_pan);

	context->camera_manual_override = true;
	vspace_source_orthonormalize_camera_locked(context);
	vspace_source_update_camera_clip_locked(context);
	vspace_source_unlock_camera(context);
}

static void vspace_source_apply_zoom_steps_locked(struct vspace_source *context, float zoom_steps)
{
	struct vec3 orbit_offset;
	float orbit_len;
	float zoom_factor;
	float extent_max;
	float min_distance;
	float max_distance;

	if (!context || zoom_steps == 0.0f)
		return;

	if (!context->model_bounds_valid) {
		return;
	}

	zoom_factor = powf(VSPACE_CAMERA_ZOOM_STEP, zoom_steps);
	context->camera_orbit_distance *= zoom_factor;

	extent_max = vspace_source_model_extent_locked(context);
	min_distance = fmaxf(0.05f, extent_max * 0.05f);
	max_distance = fmaxf(min_distance * 4.0f, extent_max * 50.0f);

	if (context->camera_orbit_distance < min_distance)
		context->camera_orbit_distance = min_distance;
	if (context->camera_orbit_distance > max_distance)
		context->camera_orbit_distance = max_distance;

	if (!context->default_camera_valid)
		vspace_source_recompute_camera_position_locked(context);

	vec3_sub(&orbit_offset, &context->default_camera_position, &context->camera_target);
	orbit_len = vec3_len(&orbit_offset);
	if (orbit_len <= 0.0001f) {
		vec3_set(&orbit_offset, 0.0f, -context->camera_orbit_distance, 0.0f);
	} else {
		vec3_mulf(&orbit_offset, &orbit_offset, context->camera_orbit_distance / orbit_len);
	}
	vec3_add(&context->default_camera_position, &context->camera_target, &orbit_offset);

	context->camera_manual_override = true;
	vspace_source_orthonormalize_camera_locked(context);
	vspace_source_update_camera_clip_locked(context);
}

static void vspace_source_zoom_camera(struct vspace_source *context, int32_t wheel_delta_y)
{
	float zoom_steps;

	if (!context || wheel_delta_y == 0)
		return;

	zoom_steps = (float)wheel_delta_y / 120.0f;

	vspace_source_lock_camera(context);
	vspace_source_apply_zoom_steps_locked(context, zoom_steps);
	vspace_source_unlock_camera(context);
}

static void vspace_source_zoom_drag_camera(struct vspace_source *context, int32_t delta_y)
{
	float zoom_steps;

	if (!context || delta_y == 0)
		return;

	zoom_steps = -(float)delta_y * VSPACE_CAMERA_ZOOM_DRAG_STEPS_PER_PIXEL;

	vspace_source_lock_camera(context);
	vspace_source_apply_zoom_steps_locked(context, zoom_steps);
	vspace_source_unlock_camera(context);
}

static void vspace_source_dolly_camera(struct vspace_source *context, int32_t delta_x, int32_t delta_y)
{
	float zoom_steps;
	float abs_delta_x;
	float abs_delta_y;
	float dolly_delta;

	if (!context || (delta_x == 0 && delta_y == 0))
		return;

	/*
	 * Blender-style dolly drag (Shift+Ctrl+MMB):
	 * - vertical drag uses Y only
	 * - horizontal drag uses X only
	 * This keeps dolly motion axis-locked and avoids diagonal mixing.
	 */
	abs_delta_x = fabsf((float)delta_x);
	abs_delta_y = fabsf((float)delta_y);

	if (abs_delta_y >= abs_delta_x)
		dolly_delta = -(float)delta_y;
	else
		dolly_delta = (float)delta_x;

	zoom_steps = dolly_delta * VSPACE_CAMERA_DOLLY_STEPS_PER_PIXEL;

	vspace_source_lock_camera(context);
	vspace_source_apply_zoom_steps_locked(context, zoom_steps);
	vspace_source_unlock_camera(context);
}

static void vspace_source_device_loss_release(void *data)
{
	struct vspace_source *context = data;

	if (!context)
		return;

	os_atomic_store_bool(&context->device_loss_active, true);
	os_atomic_store_bool(&context->device_rebuild_pending, false);
	blog(LOG_WARNING, "[vspace-source: '%s'] Graphics device loss detected.", vspace_source_log_name(context));
}

static void vspace_source_device_loss_rebuild(void *device, void *data)
{
	struct vspace_source *context = data;

	UNUSED_PARAMETER(device);

	if (!context)
		return;

	os_atomic_store_bool(&context->device_loss_active, false);
	os_atomic_store_bool(&context->device_rebuild_pending, true);
	blog(LOG_INFO, "[vspace-source: '%s'] Graphics device rebuilt. Scheduling resource refresh.",
	     vspace_source_log_name(context));
}

static void vspace_source_register_device_loss_callbacks(struct vspace_source *context)
{
	if (!context || context->device_loss_callbacks_registered)
		return;

	context->device_loss_callbacks.device_loss_release = vspace_source_device_loss_release;
	context->device_loss_callbacks.device_loss_rebuild = vspace_source_device_loss_rebuild;
	context->device_loss_callbacks.data = context;

	obs_enter_graphics();
	gs_register_loss_callbacks(&context->device_loss_callbacks);
	obs_leave_graphics();

	context->device_loss_callbacks_registered = true;
}

static void vspace_source_unregister_device_loss_callbacks(struct vspace_source *context)
{
	if (!context || !context->device_loss_callbacks_registered)
		return;

	obs_enter_graphics();
	gs_unregister_loss_callbacks(context);
	obs_leave_graphics();

	context->device_loss_callbacks_registered = false;
	os_atomic_store_bool(&context->device_loss_active, false);
	os_atomic_store_bool(&context->device_rebuild_pending, false);
}

static void vspace_source_refresh_size(struct vspace_source *context)
{
	struct obs_video_info ovi = {0};

	if (!context)
		return;

	/* Width/height policy: use OBS base canvas size, fallback to 1920x1080. */
	if (obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0) {
		context->width = ovi.base_width;
		context->height = ovi.base_height;
		return;
	}

	context->width = context->width ? context->width : 1920;
	context->height = context->height ? context->height : 1080;
}

static void vspace_source_reset_default_camera(struct vspace_source *context)
{
	if (!context)
		return;

	vspace_source_lock_camera(context);
	vec3_zero(&context->model_bounds_min);
	vec3_zero(&context->model_bounds_max);
	vec3_zero(&context->camera_target);
	vec3_zero(&context->default_camera_position);
	vec3_set(&context->camera_up, 0.0f, 0.0f, 1.0f);
	context->camera_orbit_distance = 5.0f;
	context->default_camera_fov_deg = 50.0f;
	context->default_camera_znear = 0.1f;
	context->default_camera_zfar = 100.0f;
	context->model_bounds_valid = false;
	context->default_camera_valid = false;
	context->camera_manual_override = false;
	context->camera_drag_orbit = false;
	context->camera_drag_pan = false;
	context->camera_drag_zoom = false;
	context->camera_drag_dolly = false;
	context->camera_last_input_valid = false;
	vec3_zero(&context->camera_orbit_pitch_axis);
	context->camera_orbit_pitch_axis_valid = false;
	vspace_source_unlock_camera(context);
}

static void vspace_source_reset_default_light(struct vspace_source *context)
{
	if (!context)
		return;

	vec3_set(&context->default_light_direction, -0.35f, -0.65f, -0.70f);
	vec3_norm(&context->default_light_direction, &context->default_light_direction);

	context->default_light_ambient_strength = 0.32f;
	context->default_light_diffuse_strength = 0.82f;
	context->default_light_specular_strength = 0.28f;
	context->default_light_shininess = 24.0f;
}

static bool vspace_source_compute_primitive_bounds(const struct vspace_cpu_primitive_payload *primitive,
						     struct vec3 *bounds_min, struct vec3 *bounds_max)
{
	size_t i;
	struct vec3 min_v;
	struct vec3 max_v;

	if (!primitive || !primitive->positions || primitive->vertex_count == 0 || !bounds_min || !bounds_max)
		return false;

	vec3_set(&min_v, primitive->positions[0], primitive->positions[1], primitive->positions[2]);
	vec3_copy(&max_v, &min_v);

	for (i = 1; i < primitive->vertex_count; i++) {
		const float *src_pos = primitive->positions + (i * 3);
		struct vec3 cur;

		vec3_set(&cur, src_pos[0], src_pos[1], src_pos[2]);
		vec3_min(&min_v, &min_v, &cur);
		vec3_max(&max_v, &max_v, &cur);
	}

	*bounds_min = min_v;
	*bounds_max = max_v;
	return true;
}

static void vspace_source_update_default_camera(struct vspace_source *context)
{
	struct vec3 center;
	struct vec3 half_extent;
	float aspect;
	float tan_half_fov;
	float fit_dist_x;
	float fit_dist_y;
	float fit_distance;
	float extent_max;
	float camera_distance;

	if (!context || !context->model_bounds_valid)
		return;

	if (!context->height)
		vspace_source_refresh_size(context);

	center.x = (context->model_bounds_min.x + context->model_bounds_max.x) * 0.5f;
	center.y = (context->model_bounds_min.y + context->model_bounds_max.y) * 0.5f;
	center.z = (context->model_bounds_min.z + context->model_bounds_max.z) * 0.5f;
	center.w = 0.0f;

	half_extent.x = (context->model_bounds_max.x - context->model_bounds_min.x) * 0.5f;
	half_extent.y = (context->model_bounds_max.y - context->model_bounds_min.y) * 0.5f;
	half_extent.z = (context->model_bounds_max.z - context->model_bounds_min.z) * 0.5f;
	half_extent.w = 0.0f;

	extent_max = fmaxf(fmaxf(half_extent.x, half_extent.y), half_extent.z);
	if (extent_max < 0.01f)
		extent_max = 0.01f;

	aspect = (float)context->width / (float)(context->height ? context->height : 1);
	context->default_camera_fov_deg = 50.0f;
	tan_half_fov = tanf(RAD(context->default_camera_fov_deg * 0.5f));
	if (tan_half_fov < 0.001f)
		tan_half_fov = 0.001f;

	fit_dist_y = half_extent.z / tan_half_fov;
	fit_dist_x = half_extent.x / (tan_half_fov * fmaxf(aspect, 0.1f));
	fit_distance = fmaxf(fit_dist_x, fit_dist_y);

	half_extent.y = fmaxf(half_extent.y, 0.01f);
	camera_distance = fit_distance + half_extent.y + (extent_max * 0.35f) + 0.5f;
	if (camera_distance < 0.5f)
		camera_distance = 0.5f;

	vspace_source_lock_camera(context);
	context->camera_target = center;
	context->camera_orbit_distance = camera_distance;
	context->default_camera_fov_deg = 50.0f;
	context->default_camera_valid = false;
	vec3_set(&context->camera_up, 0.0f, 0.0f, 1.0f);
	context->camera_manual_override = false;
	context->camera_drag_orbit = false;
	context->camera_drag_pan = false;
	context->camera_drag_zoom = false;
	context->camera_drag_dolly = false;
	context->camera_last_input_valid = false;
	vec3_zero(&context->camera_orbit_pitch_axis);
	context->camera_orbit_pitch_axis_valid = false;
	vspace_source_recompute_camera_position_locked(context);
	vspace_source_update_camera_clip_locked(context);
	vspace_source_unlock_camera(context);
}

static void vspace_source_log_camera_fit(const struct vspace_source *context)
{
	struct vspace_source *mutable_context = (struct vspace_source *)context;
	struct vec3 camera_position;
	float camera_znear;
	float camera_zfar;
	bool camera_valid;
	float view_min_z;
	float view_max_z;

	if (!context || !context->model_bounds_valid)
		return;

	vspace_source_lock_camera(mutable_context);
	camera_valid = context->default_camera_valid;
	camera_position = context->default_camera_position;
	camera_znear = context->default_camera_znear;
	camera_zfar = context->default_camera_zfar;
	vspace_source_unlock_camera(mutable_context);

	if (!camera_valid)
		return;

	view_min_z = context->model_bounds_min.z - camera_position.z;
	view_max_z = context->model_bounds_max.z - camera_position.z;

	blog(LOG_INFO,
	     "[vspace-source: '%s'] Camera fit: bounds_min=(%.3f, %.3f, %.3f), bounds_max=(%.3f, %.3f, %.3f), "
	     "camera=(%.3f, %.3f, %.3f), clip=[%.3f, %.3f], view_z=[%.3f, %.3f]",
	     vspace_source_log_name(context), context->model_bounds_min.x, context->model_bounds_min.y,
	     context->model_bounds_min.z, context->model_bounds_max.x, context->model_bounds_max.y,
	     context->model_bounds_max.z, camera_position.x, camera_position.y, camera_position.z, camera_znear,
	     camera_zfar, view_min_z, view_max_z);
}

static void vspace_source_unload_effect(struct vspace_source *context)
{
	if (!context || !context->effect)
		return;

	obs_enter_graphics();
	gs_effect_destroy(context->effect);
	obs_leave_graphics();

	context->effect = NULL;
	context->effect_base_color_param = NULL;
	context->effect_camera_position_param = NULL;
	context->effect_light_direction_param = NULL;
	context->effect_ambient_strength_param = NULL;
	context->effect_diffuse_strength_param = NULL;
	context->effect_specular_strength_param = NULL;
	context->effect_shininess_param = NULL;
	context->effect_grid_forward_param = NULL;
	context->effect_grid_right_param = NULL;
	context->effect_grid_up_param = NULL;
	context->effect_grid_tan_half_fov_param = NULL;
	context->effect_grid_aspect_param = NULL;
	context->effect_grid_step_param = NULL;
	context->effect_grid_origin_param = NULL;
	context->effect_grid_extent_param = NULL;
	context->effect_composite_image_param = NULL;
	context->effect_composite_background_alpha_param = NULL;
}

static void vspace_source_load_effect(struct vspace_source *context)
{
	char *effect_path;

	if (!context)
		return;

	vspace_source_unload_effect(context);

	effect_path = obs_module_file("effects/vspace.effect");
	context->effect_load_attempted = true;
	if (!effect_path) {
		blog(LOG_WARNING, "[vspace-source: '%s'] Could not resolve effects/vspace.effect.",
		     vspace_source_log_name(context));
		return;
	}

	obs_enter_graphics();
	context->effect = gs_effect_create_from_file(effect_path, NULL);
	if (context->effect) {
		gs_technique_t *fill_tech = gs_effect_get_technique(context->effect, "DrawBlinnPhongWireframe");
		gs_technique_t *wire_tech = gs_effect_get_technique(context->effect, "DrawWireframe");
		gs_technique_t *grid_tech = gs_effect_get_technique(context->effect, "DrawGrid");
		gs_technique_t *composite_tech = gs_effect_get_technique(context->effect, "DrawComposite");

		context->effect_base_color_param = gs_effect_get_param_by_name(context->effect, "effect_base_color_param");
		context->effect_camera_position_param =
			gs_effect_get_param_by_name(context->effect, "effect_camera_position");
		context->effect_light_direction_param =
			gs_effect_get_param_by_name(context->effect, "effect_light_direction");
		context->effect_ambient_strength_param =
			gs_effect_get_param_by_name(context->effect, "effect_ambient_strength");
		context->effect_diffuse_strength_param =
			gs_effect_get_param_by_name(context->effect, "effect_diffuse_strength");
		context->effect_specular_strength_param =
			gs_effect_get_param_by_name(context->effect, "effect_specular_strength");
		context->effect_shininess_param = gs_effect_get_param_by_name(context->effect, "effect_shininess");
		context->effect_grid_forward_param = gs_effect_get_param_by_name(context->effect, "effect_grid_forward");
		context->effect_grid_right_param = gs_effect_get_param_by_name(context->effect, "effect_grid_right");
		context->effect_grid_up_param = gs_effect_get_param_by_name(context->effect, "effect_grid_up");
		context->effect_grid_tan_half_fov_param =
			gs_effect_get_param_by_name(context->effect, "effect_grid_tan_half_fov");
		context->effect_grid_aspect_param = gs_effect_get_param_by_name(context->effect, "effect_grid_aspect");
		context->effect_grid_step_param = gs_effect_get_param_by_name(context->effect, "effect_grid_step");
		context->effect_grid_origin_param = gs_effect_get_param_by_name(context->effect, "effect_grid_origin");
		context->effect_grid_extent_param = gs_effect_get_param_by_name(context->effect, "effect_grid_extent");
		context->effect_composite_image_param = gs_effect_get_param_by_name(context->effect, "image");
		context->effect_composite_background_alpha_param =
			gs_effect_get_param_by_name(context->effect, "effect_background_alpha");

		blog(LOG_INFO,
		     "[vspace-source: '%s'] Effect loaded: base_color_param=%s, camera_param=%s, "
		     "light_dir_param=%s, tech_fill=%s, tech_wire=%s, tech_grid=%s, tech_composite=%s",
		     vspace_source_log_name(context), context->effect_base_color_param ? "yes" : "no",
		     context->effect_camera_position_param ? "yes" : "no",
		     context->effect_light_direction_param ? "yes" : "no", fill_tech ? "yes" : "no",
		     wire_tech ? "yes" : "no", grid_tech ? "yes" : "no", composite_tech ? "yes" : "no");
	}
	obs_leave_graphics();

	if (!context->effect) {
		blog(LOG_WARNING, "[vspace-source: '%s'] Failed to load effect file: %s",
		     vspace_source_log_name(context), effect_path);
	}

	bfree(effect_path);
}

static bool vspace_source_is_supported_model_path(const char *path)
{
	const char *extension = strrchr(path, '.');

	if (!extension)
		return false;

	return astrcmpi(extension, ".glb") == 0 || astrcmpi(extension, ".gltf") == 0;
}

static void vspace_source_validate_model_path(struct vspace_source *context)
{
	if (!context || !context->model_path || !*context->model_path)
		return;

	if (!os_file_exists(context->model_path)) {
		blog(LOG_WARNING, "[vspace-source: '%s'] Model path does not exist: %s",
		     vspace_source_log_name(context), context->model_path);
		return;
	}

	if (!vspace_source_is_supported_model_path(context->model_path)) {
		blog(LOG_WARNING,
		     "[vspace-source: '%s'] Unsupported model format. Only .glb or .gltf is supported: %s",
		     vspace_source_log_name(context), context->model_path);
	}
}

static bool vspace_source_model_path_is_loadable(const struct vspace_source *context)
{
	if (!context || !context->model_path || !*context->model_path)
		return false;

	if (!os_file_exists(context->model_path))
		return false;

	return vspace_source_is_supported_model_path(context->model_path);
}

static bool vspace_source_model_path_uses_draco(const char *model_path)
{
	if (!model_path || !*model_path)
		return false;

	if (!os_file_exists(model_path))
		return false;

	if (!vspace_source_is_supported_model_path(model_path))
		return false;

	return vspace_gltf_model_uses_draco(model_path);
}

static void vspace_source_set_draco_property_state(obs_properties_t *props, bool enabled)
{
	obs_property_t *draco_enabled_prop;
	obs_property_t *draco_decoder_prop;

	if (!props)
		return;

	draco_enabled_prop = obs_properties_get(props, S_DRACO_ENABLED);
	draco_decoder_prop = obs_properties_get(props, S_DRACO_DECODER);

	if (draco_enabled_prop)
		obs_property_set_enabled(draco_enabled_prop, enabled);

	if (draco_decoder_prop)
		obs_property_set_enabled(draco_decoder_prop, enabled);
}

static bool vspace_source_model_path_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	const char *model_path = settings ? obs_data_get_string(settings, S_MODEL_PATH) : NULL;
	const bool model_uses_draco = vspace_source_model_path_uses_draco(model_path);

	vspace_source_set_draco_property_state(props, model_uses_draco);

	if (!model_uses_draco && settings) {
		obs_data_set_bool(settings, S_DRACO_ENABLED, false);
		obs_data_set_string(settings, S_DRACO_DECODER, S_DRACO_DECODER_AUTO);
	}

	UNUSED_PARAMETER(property);
	return true;
}

static void vspace_source_free_worker_job(struct vspace_source *context)
{
	if (!context)
		return;

	bfree(context->worker_job.model_path);
	context->worker_job.model_path = NULL;
	bfree(context->worker_job.draco_decoder);
	context->worker_job.draco_decoder = NULL;
	context->worker_job.has_job = false;
}

static void vspace_source_release_pending_upload(struct vspace_source *context)
{
	if (!context)
		return;

	vspace_gltf_free_cpu_payload(&context->pending_upload.payload);

	if (context->pending_upload.base_color_image_valid)
		gs_image_file4_free(&context->pending_upload.base_color_image);

	memset(&context->pending_upload, 0, sizeof(context->pending_upload));
}

static void vspace_source_release_gpu_mesh(struct vspace_gpu_mesh *gpu_mesh)
{
	if (!gpu_mesh)
		return;

	if (gpu_mesh->vertex_buffer) {
		gs_vertexbuffer_destroy(gpu_mesh->vertex_buffer);
		gpu_mesh->vertex_buffer = NULL;
	}

	if (gpu_mesh->wireframe_vertex_buffer) {
		gs_vertexbuffer_destroy(gpu_mesh->wireframe_vertex_buffer);
		gpu_mesh->wireframe_vertex_buffer = NULL;
	}

	if (gpu_mesh->index_buffer) {
		gs_indexbuffer_destroy(gpu_mesh->index_buffer);
		gpu_mesh->index_buffer = NULL;
	}

	if (gpu_mesh->wireframe_index_buffer) {
		gs_indexbuffer_destroy(gpu_mesh->wireframe_index_buffer);
		gpu_mesh->wireframe_index_buffer = NULL;
	}

	gpu_mesh->material_index = -1;
	gpu_mesh->draw_vertex_count = 0;
	gpu_mesh->draw_index_count = 0;
	gpu_mesh->wireframe_vertex_count = 0;
	gpu_mesh->wireframe_index_count = 0;
}

static void vspace_source_release_gpu_meshes(struct vspace_gpu_mesh *gpu_meshes, size_t mesh_count)
{
	size_t i;

	if (!gpu_meshes)
		return;

	for (i = 0; i < mesh_count; i++)
		vspace_source_release_gpu_mesh(gpu_meshes + i);

	bfree(gpu_meshes);
}

/* Must be called inside an active graphics context. */
static void vspace_source_release_gpu_resources(struct vspace_source *context)
{
	if (!context)
		return;

	vspace_source_release_gpu_meshes(context->gpu_meshes, context->gpu_mesh_count);
	context->gpu_meshes = NULL;
	context->gpu_mesh_count = 0;

	if (context->bounds_line_buffer) {
		gs_vertexbuffer_destroy(context->bounds_line_buffer);
		context->bounds_line_buffer = NULL;
	}

	if (context->grid_triangle_buffer) {
		gs_vertexbuffer_destroy(context->grid_triangle_buffer);
		context->grid_triangle_buffer = NULL;
	}

	if (context->model_texrender) {
		gs_texrender_destroy(context->model_texrender);
		context->model_texrender = NULL;
	}

	if (context->base_color_image_valid) {
		gs_image_file4_free(&context->base_color_image);
		context->base_color_image_valid = false;
	}

	context->draw_vertex_count = 0;
	context->draw_index_count = 0;
	context->wireframe_vertex_count = 0;
	context->wireframe_index_count = 0;
	context->diagnostics_logged_draw = false;
}

/* Must be called in an active graphics context. */
static bool vspace_source_ensure_bounds_line_buffer(struct vspace_source *context)
{
	static const float line_points[][3] = {
		{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f},
		{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
	};
	static const uint8_t line_indices[][2] = {
		{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
		{7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
	};
	struct gs_vb_data *vb_data = NULL;
	struct vec3 *points;
	size_t i;

	if (!context)
		return false;

	if (context->bounds_line_buffer)
		return true;

	vb_data = gs_vbdata_create();
	vb_data->num = OBS_COUNTOF(line_indices) * 2;
	vb_data->points = bmalloc(sizeof(struct vec3) * vb_data->num);

	points = vb_data->points;
	for (i = 0; i < OBS_COUNTOF(line_indices); i++) {
		const float *src_a = line_points[line_indices[i][0]];
		const float *src_b = line_points[line_indices[i][1]];

		vec3_set(&points[i * 2], src_a[0], src_a[1], src_a[2]);
		vec3_set(&points[i * 2 + 1], src_b[0], src_b[1], src_b[2]);
	}

	context->bounds_line_buffer = gs_vertexbuffer_create(vb_data, 0);
	if (!context->bounds_line_buffer) {
		gs_vbdata_destroy(vb_data);
		blog(LOG_WARNING, "[vspace-source: '%s'] Failed to create bounds line vertex buffer.",
		     vspace_source_log_name(context));
		return false;
	}

	return true;
}

/* Must be called in an active graphics context. */
static void vspace_source_draw_bounds(struct vspace_source *context)
{
	gs_effect_t *solid_effect;
	gs_eparam_t *solid_color_param;
	struct vec3 bounds_scale;
	struct vec4 bounds_color;

	if (!context || !context->model_bounds_valid)
		return;

	if (!vspace_source_ensure_bounds_line_buffer(context))
		return;

	solid_effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	if (!solid_effect)
		return;

	solid_color_param = gs_effect_get_param_by_name(solid_effect, "color");
	if (!solid_color_param)
		return;

	bounds_scale.x = context->model_bounds_max.x - context->model_bounds_min.x;
	bounds_scale.y = context->model_bounds_max.y - context->model_bounds_min.y;
	bounds_scale.z = context->model_bounds_max.z - context->model_bounds_min.z;
	bounds_scale.w = 0.0f;

	if (bounds_scale.x < 0.001f)
		bounds_scale.x = 0.001f;
	if (bounds_scale.y < 0.001f)
		bounds_scale.y = 0.001f;
	if (bounds_scale.z < 0.001f)
		bounds_scale.z = 0.001f;

	vec4_from_rgba_srgb(&bounds_color, 0xFF1FD4A5);
	gs_effect_set_vec4(solid_color_param, &bounds_color);

	gs_matrix_push();
	gs_matrix_translate3f(context->model_bounds_min.x, context->model_bounds_min.y, context->model_bounds_min.z);
	gs_matrix_scale3f(bounds_scale.x, bounds_scale.y, bounds_scale.z);
	gs_load_vertexbuffer(context->bounds_line_buffer);
	while (gs_effect_loop(solid_effect, "Solid"))
		gs_draw(GS_LINES, 0, 0);
	gs_load_vertexbuffer(NULL);
	gs_matrix_pop();
}

static float vspace_source_snap_grid_step_125(float raw_step)
{
	float exponent;
	float magnitude;
	float normalized;

	if (!isfinite(raw_step) || raw_step < VSPACE_GRID_MIN_STEP)
		raw_step = VSPACE_GRID_MIN_STEP;

	exponent = floorf(log10f(raw_step));
	magnitude = powf(10.0f, exponent);
	if (!isfinite(magnitude) || magnitude <= 0.0f)
		magnitude = 1.0f;

	normalized = raw_step / magnitude;
	if (normalized <= 1.0f)
		normalized = 1.0f;
	else if (normalized <= 2.0f)
		normalized = 2.0f;
	else if (normalized <= 5.0f)
		normalized = 5.0f;
	else
		normalized = 10.0f;

	return normalized * magnitude;
}

/* Must be called in an active graphics context. */
static bool vspace_source_ensure_grid_triangle_buffer(struct vspace_source *context)
{
	struct gs_vb_data *vb_data;

	if (!context)
		return false;

	if (context->grid_triangle_buffer)
		return true;

	vb_data = gs_vbdata_create();
	if (!vb_data)
		return false;

	vb_data->num = 3;
	vb_data->points = bmalloc(sizeof(struct vec3) * 3);
	vb_data->num_tex = 0;
	vb_data->tvarray = NULL;
	vb_data->normals = NULL;
	vb_data->tangents = NULL;
	vb_data->colors = NULL;

	if (!vb_data->points) {
		gs_vbdata_destroy(vb_data);
		return false;
	}

	/* Full-screen triangle in clip-space. */
	vec3_set(vb_data->points + 0, -1.0f, -1.0f, 0.0f);
	vec3_set(vb_data->points + 1, -1.0f, 3.0f, 0.0f);
	vec3_set(vb_data->points + 2, 3.0f, -1.0f, 0.0f);

	context->grid_triangle_buffer = gs_vertexbuffer_create(vb_data, 0);
	if (!context->grid_triangle_buffer) {
		gs_vbdata_destroy(vb_data);
		blog(LOG_WARNING, "[vspace-source: '%s'] Failed to create fullscreen grid triangle buffer.",
		     vspace_source_log_name(context));
		return false;
	}

	return true;
}

static void vspace_source_draw_grid_line_parallel_x(float y, float extent, float half_width)
{
	gs_render_start(false);
	gs_vertex3f(-extent, y - half_width, 0.0f);
	gs_vertex3f(-extent, y + half_width, 0.0f);
	gs_vertex3f(extent, y - half_width, 0.0f);
	gs_vertex3f(extent, y + half_width, 0.0f);
	gs_render_stop(GS_TRISTRIP);
}

static void vspace_source_draw_grid_line_parallel_z(float x, float extent, float half_width)
{
	gs_render_start(false);
	gs_vertex3f(x - half_width, -extent, 0.0f);
	gs_vertex3f(x + half_width, -extent, 0.0f);
	gs_vertex3f(x - half_width, extent, 0.0f);
	gs_vertex3f(x + half_width, extent, 0.0f);
	gs_render_stop(GS_TRISTRIP);
}

static void vspace_source_draw_grid_iteration(gs_effect_t *solid_effect, gs_eparam_t *color_param, float step, float extent,
					      float origin_x, float origin_y, float line_half_width,
					      const struct vec4 *grid_color)
{
	int half_lines;
	int idx;
	float snapped_extent;

	if (!solid_effect || !color_param || !grid_color)
		return;
	if (!isfinite(step) || step <= 0.0f)
		return;
	if (!isfinite(extent) || extent <= 0.0f)
		return;

	half_lines = (int)ceilf(extent / step);
	if (half_lines < 1)
		half_lines = 1;
	if (half_lines > VSPACE_GRID_MAX_HALF_LINES)
		half_lines = VSPACE_GRID_MAX_HALF_LINES;
	snapped_extent = step * (float)half_lines;

	gs_effect_set_vec4(color_param, grid_color);
	while (gs_effect_loop(solid_effect, "Solid")) {
		for (idx = -half_lines; idx <= half_lines; idx++) {
			const float x = origin_x + (float)idx * step;

			if (fabsf(x) <= (step * 0.5f))
				continue;
			vspace_source_draw_grid_line_parallel_z(x, snapped_extent, line_half_width);
		}

		for (idx = -half_lines; idx <= half_lines; idx++) {
			const float y = origin_y + (float)idx * step;

			if (fabsf(y) <= (step * 0.5f))
				continue;
			vspace_source_draw_grid_line_parallel_x(y, snapped_extent, line_half_width);
		}
	}
}

static uint32_t vspace_source_get_aa_dim(uint32_t base_dim)
{
	uint64_t scaled;

	if (base_dim == 0)
		base_dim = 1;

	scaled = (uint64_t)base_dim * (uint64_t)VSPACE_RENDER_AA_SCALE;
	if (scaled > VSPACE_RENDER_AA_MAX_DIM)
		scaled = VSPACE_RENDER_AA_MAX_DIM;
	if (scaled < 1)
		scaled = 1;

	return (uint32_t)scaled;
}

static void vspace_source_draw_world_grid(struct vspace_source *context, const struct vec3 *camera_position,
					  const struct vec3 *camera_target, const struct vec3 *camera_up,
					  float camera_fov_deg,
					  uint32_t viewport_width_px, uint32_t viewport_height_px)
{
	struct vec3 forward;
	struct vec3 right;
	struct vec3 up;
	struct vec3 camera_to_target;
	float camera_distance;
	float base_step;
	float grid_step;
	float viewport_height;
	float viewport_width;
	float aspect;
	float tan_half_fov;
	float view_height_world;
	float units_per_pixel;
	float required_extent;
	struct vec2 snapped_origin;
	gs_technique_t *grid_tech;

	if (!context || !context->effect || !camera_position || !camera_target || !camera_up)
		return;

	if (!vspace_source_ensure_grid_triangle_buffer(context))
		return;

	grid_tech = gs_effect_get_technique(context->effect, "DrawGrid");
	if (!grid_tech)
		return;

	vec3_sub(&camera_to_target, camera_position, camera_target);
	camera_distance = vec3_len(&camera_to_target);
	if (!isfinite(camera_distance) || camera_distance < 0.05f)
		camera_distance = 0.05f;

	viewport_width = (float)(viewport_width_px ? viewport_width_px : 1920);
	viewport_height = (float)(viewport_height_px ? viewport_height_px : 1080);
	aspect = viewport_width / viewport_height;
	tan_half_fov = tanf(RAD(camera_fov_deg * 0.5f));
	if (!isfinite(tan_half_fov) || tan_half_fov < 0.001f)
		tan_half_fov = tanf(RAD(25.0f));
	if (!isfinite(tan_half_fov) || tan_half_fov < 0.001f)
		tan_half_fov = 0.001f;

	view_height_world = 2.0f * tan_half_fov * camera_distance;
	if (!isfinite(view_height_world) || view_height_world < VSPACE_GRID_MIN_STEP)
		view_height_world = 2.0f * camera_distance;
	units_per_pixel = view_height_world / viewport_height;
	if (!isfinite(units_per_pixel) || units_per_pixel < (VSPACE_GRID_MIN_STEP * 0.25f))
		units_per_pixel = VSPACE_GRID_MIN_STEP * 0.25f;

	base_step = vspace_source_snap_grid_step_125(units_per_pixel * VSPACE_GRID_TARGET_PIXEL_STEP);
	grid_step = fmaxf(base_step, VSPACE_GRID_MIN_STEP);
	vec2_set(&snapped_origin, roundf(camera_target->x / grid_step) * grid_step,
		 roundf(camera_target->y / grid_step) * grid_step);

	required_extent = fmaxf(camera_distance * 6.0f, fmaxf(view_height_world, view_height_world * aspect) * 1.2f);
	if (!isfinite(required_extent) || required_extent < 16.0f)
		required_extent = 16.0f;

	vec3_sub(&forward, camera_target, camera_position);
	if (vec3_len(&forward) <= 0.0001f)
		vec3_set(&forward, 0.0f, 1.0f, 0.0f);
	else
		vec3_norm(&forward, &forward);

	up = *camera_up;
	if (vec3_len(&up) <= 0.0001f)
		vec3_set(&up, 0.0f, 0.0f, 1.0f);
	else
		vec3_norm(&up, &up);

	vec3_cross(&right, &forward, &up);
	if (vec3_len(&right) <= 0.0001f) {
		vec3_set(&up, 0.0f, 0.0f, 1.0f);
		vec3_cross(&right, &forward, &up);
	}
	if (vec3_len(&right) <= 0.0001f) {
		vec3_set(&up, 0.0f, 1.0f, 0.0f);
		vec3_cross(&right, &forward, &up);
	}
	if (vec3_len(&right) <= 0.0001f)
		vec3_set(&right, 1.0f, 0.0f, 0.0f);
	else
		vec3_norm(&right, &right);

	vec3_cross(&up, &right, &forward);
	if (vec3_len(&up) <= 0.0001f)
		vec3_set(&up, 0.0f, 0.0f, 1.0f);
	else
		vec3_norm(&up, &up);

	if (context->effect_grid_forward_param)
		gs_effect_set_vec3(context->effect_grid_forward_param, &forward);
	if (context->effect_grid_right_param)
		gs_effect_set_vec3(context->effect_grid_right_param, &right);
	if (context->effect_grid_up_param)
		gs_effect_set_vec3(context->effect_grid_up_param, &up);
	if (context->effect_grid_tan_half_fov_param)
		gs_effect_set_float(context->effect_grid_tan_half_fov_param, tan_half_fov);
	if (context->effect_grid_aspect_param)
		gs_effect_set_float(context->effect_grid_aspect_param, aspect);
	if (context->effect_grid_step_param)
		gs_effect_set_float(context->effect_grid_step_param, grid_step);
	if (context->effect_grid_origin_param)
		gs_effect_set_vec2(context->effect_grid_origin_param, &snapped_origin);
	if (context->effect_grid_extent_param)
		gs_effect_set_float(context->effect_grid_extent_param, required_extent);

	gs_load_vertexbuffer(context->grid_triangle_buffer);
	gs_load_indexbuffer(NULL);
	while (gs_effect_loop(context->effect, "DrawGrid"))
		gs_draw(GS_TRIS, 0, 3);
	gs_load_vertexbuffer(NULL);
}

/* Must be called in an active graphics context. */
static bool vspace_source_ensure_model_texrender(struct vspace_source *context)
{
	if (!context)
		return false;

	if (context->model_texrender)
		return true;

	context->model_texrender = gs_texrender_create(GS_RGBA, GS_Z24_S8);
	if (!context->model_texrender) {
		blog(LOG_WARNING, "[vspace-source: '%s'] Failed to create model texrender target.",
		     vspace_source_log_name(context));
		return false;
	}

	return true;
}

/* Must be called from source render (active graphics context). */
static gs_texture_t *vspace_source_render_model_to_texture(struct vspace_source *context)
{
	struct vec4 clear_color;
	struct vec3 camera_position;
	struct vec3 camera_target;
	struct vec3 camera_up;
	float aspect;
	float camera_fov_deg;
	float camera_znear;
	float camera_zfar;
	uint32_t render_width;
	uint32_t render_height;
	bool camera_valid = false;
	const bool inspect_render_mode = os_atomic_load_bool(&context->inspect_render_mode);
	const char *fill_technique = "DrawBlinnPhongWireframe";
	const char *wireframe_technique = "DrawWireframe";
	enum gs_cull_mode previous_cull_mode;
	size_t batch_idx;

	if (!context || !context->effect || context->gpu_mesh_count == 0)
		return NULL;

	vspace_source_lock_camera(context);
	camera_valid = context->default_camera_valid;
	camera_position = context->default_camera_position;
	camera_target = context->camera_target;
	camera_up = context->camera_up;
	camera_fov_deg = context->default_camera_fov_deg;
	camera_znear = context->default_camera_znear;
	camera_zfar = context->default_camera_zfar;
	vspace_source_unlock_camera(context);

	if (!camera_valid)
		return NULL;

	if (!vspace_source_ensure_model_texrender(context))
		return NULL;

	render_width = vspace_source_get_aa_dim(context->width);
	render_height = vspace_source_get_aa_dim(context->height);

	gs_texrender_reset(context->model_texrender);
	if (!gs_texrender_begin_with_color_space(context->model_texrender, render_width, render_height, GS_CS_SRGB))
		return NULL;

	if (!context->diagnostics_logged_draw) {
		blog(LOG_INFO,
		     "[vspace-source: '%s'] Render path active: batches=%zu, fill=%s, wire=%s, indices=%zu, "
		     "wire_vertices=%zu, "
		     "vertices=%zu",
		     vspace_source_log_name(context), context->gpu_mesh_count, fill_technique, wireframe_technique,
		     context->draw_index_count, context->wireframe_vertex_count, context->draw_vertex_count);
		context->diagnostics_logged_draw = true;
	}

	aspect = (float)render_width / (float)(render_height ? render_height : 1);
	vec4_from_rgba_srgb(&clear_color, inspect_render_mode ? 0x00000000 : context->background_color);
	clear_color.x *= clear_color.w;
	clear_color.y *= clear_color.w;
	clear_color.z *= clear_color.w;
	gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &clear_color, 1.0f, 0);

	gs_enable_framebuffer_srgb(true);
	gs_enable_depth_test(true);
	gs_depth_function(GS_LESS);

	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_set_viewport(0, 0, (int)render_width, (int)render_height);
	vspace_source_apply_camera_projection_matrix(camera_fov_deg, aspect, camera_znear, camera_zfar);
	gs_matrix_identity();
	vspace_source_apply_camera_view_matrix(&camera_position, &camera_target, &camera_up);

	if (context->effect_base_color_param)
		gs_effect_set_texture(context->effect_base_color_param, NULL);
	if (context->effect_camera_position_param)
		gs_effect_set_vec3(context->effect_camera_position_param, &camera_position);
	if (context->effect_light_direction_param)
		gs_effect_set_vec3(context->effect_light_direction_param, &context->default_light_direction);
	if (context->effect_ambient_strength_param)
		gs_effect_set_float(context->effect_ambient_strength_param, context->default_light_ambient_strength);
	if (context->effect_diffuse_strength_param)
		gs_effect_set_float(context->effect_diffuse_strength_param, context->default_light_diffuse_strength);
	if (context->effect_specular_strength_param)
		gs_effect_set_float(context->effect_specular_strength_param, context->default_light_specular_strength);
	if (context->effect_shininess_param)
		gs_effect_set_float(context->effect_shininess_param, context->default_light_shininess);

	previous_cull_mode = gs_get_cull_mode();
	gs_blend_state_push();
	gs_enable_blending(false);

	/* Keep mesh winding-agnostic; depth test handles visibility. */
	gs_set_cull_mode(GS_NEITHER);

	for (batch_idx = 0; batch_idx < context->gpu_mesh_count; batch_idx++) {
		const struct vspace_gpu_mesh *gpu_mesh = context->gpu_meshes + batch_idx;

		if (!gpu_mesh->vertex_buffer)
			continue;

		gs_load_vertexbuffer(gpu_mesh->vertex_buffer);
		gs_load_indexbuffer(gpu_mesh->index_buffer);
		while (gs_effect_loop(context->effect, fill_technique))
			gs_draw(GS_TRIS, 0, gpu_mesh->index_buffer ? 0 : (uint32_t)gpu_mesh->draw_vertex_count);

		if (gpu_mesh->wireframe_vertex_buffer && gpu_mesh->wireframe_vertex_count >= 3) {
			/*
			 * Draw wireframe as barycentric triangles (not GS_LINES) so coplanar edge
			 * rasterization remains stable and back-side wires fail depth tests.
			 */
			gs_enable_blending(true);
			gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
			gs_depth_function(GS_LEQUAL);
			gs_load_vertexbuffer(gpu_mesh->wireframe_vertex_buffer);
			gs_load_indexbuffer(NULL);
			while (gs_effect_loop(context->effect, wireframe_technique))
				gs_draw(GS_TRIS, 0, (uint32_t)gpu_mesh->wireframe_vertex_count);
			gs_depth_function(GS_LESS);
			gs_enable_blending(false);
		}
	}

	gs_load_indexbuffer(NULL);
	gs_load_vertexbuffer(NULL);
	gs_set_cull_mode(previous_cull_mode);

	gs_blend_state_pop();
	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();

	gs_enable_depth_test(false);
	gs_texrender_end(context->model_texrender);
	return gs_texrender_get_texture(context->model_texrender);
}

static const char *vspace_source_find_first_texture_path(const struct vspace_cpu_payload *payload)
{
	size_t mesh_idx;

	if (!payload)
		return NULL;

	for (mesh_idx = 0; mesh_idx < payload->mesh_count; mesh_idx++) {
		const struct vspace_cpu_mesh_payload *mesh = payload->meshes + mesh_idx;
		size_t primitive_idx;

		for (primitive_idx = 0; primitive_idx < mesh->primitive_count; primitive_idx++) {
			const struct vspace_cpu_primitive_payload *primitive = mesh->primitives + primitive_idx;

			if (primitive->base_color_texture && *primitive->base_color_texture)
				return primitive->base_color_texture;
		}
	}

	return NULL;
}

static bool vspace_source_decode_base_color_image(struct vspace_source *context, const struct vspace_cpu_payload *payload,
						    gs_image_file4_t *decoded_image)
{
	const char *image_path;

	if (!context || !payload || !decoded_image)
		return false;

	memset(decoded_image, 0, sizeof(*decoded_image));

	image_path = vspace_source_find_first_texture_path(payload);
	if (!image_path || !*image_path)
		return false;

	if (!os_file_exists(image_path)) {
		blog(LOG_WARNING, "[vspace-source: '%s'] BaseColor texture path does not exist: %s",
		     vspace_source_log_name(context), image_path);
		return false;
	}

	gs_image_file4_init(decoded_image, image_path, GS_IMAGE_ALPHA_PREMULTIPLY);
	if (!decoded_image->image3.image2.image.loaded) {
		blog(LOG_WARNING, "[vspace-source: '%s'] Failed to decode BaseColor texture: %s",
		     vspace_source_log_name(context), image_path);
		gs_image_file4_free(decoded_image);
		memset(decoded_image, 0, sizeof(*decoded_image));
		return false;
	}

	return true;
}

static void vspace_source_log_payload_summary(const struct vspace_source *context, const struct vspace_cpu_payload *payload)
{
	size_t mesh_idx;
	size_t primitive_count = 0;
	size_t draco_extension_count = 0;

	if (!context || !payload)
		return;

	for (mesh_idx = 0; mesh_idx < payload->mesh_count; mesh_idx++) {
		const struct vspace_cpu_mesh_payload *mesh = payload->meshes + mesh_idx;
		size_t primitive_idx;

		primitive_count += mesh->primitive_count;

		for (primitive_idx = 0; primitive_idx < mesh->primitive_count; primitive_idx++) {
			const struct vspace_cpu_primitive_payload *primitive = mesh->primitives + primitive_idx;

			if (primitive->used_draco_extension)
				draco_extension_count++;
		}
	}

	blog(LOG_INFO,
	     "[vspace-source: '%s'] glTF payload ready: meshes=%zu, primitives=%zu, draco_extension_primitives=%zu",
	     vspace_source_log_name(context), payload->mesh_count, primitive_count, draco_extension_count);
}

static bool vspace_source_publish_pending_upload(struct vspace_source *context, struct vspace_cpu_payload *payload,
						   gs_image_file4_t *decoded_image, bool decoded_image_valid, uint64_t token)
{
	bool canceled;

	if (!context || !payload)
		return false;

	if (context->worker_mutex_valid)
		pthread_mutex_lock(&context->worker_mutex);
	canceled = os_atomic_load_bool(&context->worker_stop) || token != context->worker_cancel_token;

	if (!canceled) {
		vspace_source_release_pending_upload(context);
		context->pending_upload.payload = *payload;
		memset(payload, 0, sizeof(*payload));

		if (decoded_image && decoded_image_valid) {
			context->pending_upload.base_color_image = *decoded_image;
			memset(decoded_image, 0, sizeof(*decoded_image));
		}

		context->pending_upload.base_color_image_valid = decoded_image_valid;
		context->pending_upload.token = token;
		context->pending_upload.ready = true;
	} else {
		blog(LOG_INFO,
		     "[vspace-source: '%s'] Dropping decoded payload token=%llu (active_token=%llu, stop=%d).",
		     vspace_source_log_name(context), (unsigned long long)token,
		     (unsigned long long)context->worker_cancel_token, os_atomic_load_bool(&context->worker_stop));
	}

	if (context->worker_mutex_valid)
		pthread_mutex_unlock(&context->worker_mutex);
	return !canceled;
}

static bool vspace_source_take_pending_upload(struct vspace_source *context, struct vspace_cpu_payload *payload,
						gs_image_file4_t *decoded_image, bool *decoded_image_valid)
{
	if (!context || !payload || !decoded_image || !decoded_image_valid)
		return false;

	memset(payload, 0, sizeof(*payload));
	memset(decoded_image, 0, sizeof(*decoded_image));
	*decoded_image_valid = false;

	if (context->worker_mutex_valid)
		pthread_mutex_lock(&context->worker_mutex);
	if (!context->pending_upload.ready) {
		if (context->worker_mutex_valid)
			pthread_mutex_unlock(&context->worker_mutex);
		return false;
	}

	*payload = context->pending_upload.payload;
	context->pending_upload.payload.meshes = NULL;
	context->pending_upload.payload.mesh_count = 0;

	if (context->pending_upload.base_color_image_valid) {
		*decoded_image = context->pending_upload.base_color_image;
		memset(&context->pending_upload.base_color_image, 0, sizeof(context->pending_upload.base_color_image));
	}

	*decoded_image_valid = context->pending_upload.base_color_image_valid;
	context->pending_upload.base_color_image_valid = false;
	context->pending_upload.ready = false;
	context->pending_upload.token = 0;
	if (context->worker_mutex_valid)
		pthread_mutex_unlock(&context->worker_mutex);
	return true;
}

struct vspace_upload_material_batch {
	int32_t material_index;
	size_t total_vertices;
	size_t total_indices;
	size_t total_wireframe_vertices;
	size_t primitive_count;
	size_t vertex_offset;
	size_t index_offset;
	size_t wire_vertex_offset;
	size_t uploaded_primitives;
	struct gs_vb_data *vb_data;
	struct gs_vb_data *wire_vb_data;
	uint32_t *flat_indices;
};

static bool vspace_source_find_upload_batch(const struct vspace_upload_material_batch *batches, size_t batch_count,
					      int32_t material_index, size_t *batch_index)
{
	size_t i;

	if (!batches || !batch_index)
		return false;

	for (i = 0; i < batch_count; i++) {
		if (batches[i].material_index == material_index) {
			*batch_index = i;
			return true;
		}
	}

	return false;
}

static bool vspace_source_find_or_add_upload_batch(struct vspace_upload_material_batch *batches, size_t *batch_count,
						   size_t batch_capacity, int32_t material_index, size_t *batch_index)
{
	size_t i;

	if (!batches || !batch_count || !batch_index)
		return false;

	for (i = 0; i < *batch_count; i++) {
		if (batches[i].material_index == material_index) {
			*batch_index = i;
			return true;
		}
	}

	if (*batch_count >= batch_capacity)
		return false;

	memset(batches + *batch_count, 0, sizeof(*batches));
	batches[*batch_count].material_index = material_index;
	*batch_index = *batch_count;
	(*batch_count)++;
	return true;
}

static void vspace_source_release_upload_material_batches(struct vspace_upload_material_batch *batches, size_t batch_count)
{
	size_t i;

	if (!batches)
		return;

	for (i = 0; i < batch_count; i++) {
		if (batches[i].vb_data) {
			gs_vbdata_destroy(batches[i].vb_data);
			batches[i].vb_data = NULL;
		}
		if (batches[i].wire_vb_data) {
			gs_vbdata_destroy(batches[i].wire_vb_data);
			batches[i].wire_vb_data = NULL;
		}

		bfree(batches[i].flat_indices);
		batches[i].flat_indices = NULL;
	}

	bfree(batches);
}

static void vspace_source_upload_pending_payload(struct vspace_source *context, struct vspace_cpu_payload *payload,
						   gs_image_file4_t *decoded_image, bool decoded_image_valid)
{
	struct vec3 bounds_min = {0};
	struct vec3 bounds_max = {0};
	struct vspace_upload_material_batch *upload_batches = NULL;
	struct vspace_gpu_mesh *new_gpu_meshes = NULL;
	size_t upload_batch_count = 0;
	size_t primitive_capacity = 0;
	size_t new_gpu_mesh_count = 0;
	size_t mesh_idx;
	size_t total_uploaded_primitives = 0;
	size_t total_uploaded_vertices = 0;
	size_t total_uploaded_indices = 0;
	size_t total_uploaded_wireframe_vertices = 0;
	bool bounds_valid = false;
	bool upload_failed = false;
	bool graphics_active = false;

	if (!context || !payload)
		return;

	if (!payload->mesh_count) {
		vspace_source_reset_default_camera(context);
		obs_enter_graphics();
		vspace_source_release_gpu_resources(context);
		obs_leave_graphics();
		return;
	}

	for (mesh_idx = 0; mesh_idx < payload->mesh_count; mesh_idx++) {
		const struct vspace_cpu_mesh_payload *mesh = payload->meshes + mesh_idx;

		if (primitive_capacity > SIZE_MAX - mesh->primitive_count) {
			blog(LOG_WARNING,
			     "[vspace-source: '%s'] Primitive capacity overflow while scanning mesh[%zu].",
			     vspace_source_log_name(context), mesh_idx);
			return;
		}
		primitive_capacity += mesh->primitive_count;
	}

	if (!primitive_capacity) {
		vspace_source_reset_default_camera(context);
		obs_enter_graphics();
		vspace_source_release_gpu_resources(context);
		obs_leave_graphics();
		blog(LOG_WARNING, "[vspace-source: '%s'] No valid material batches were uploaded.",
		     vspace_source_log_name(context));
		return;
	}

	upload_batches = bzalloc(sizeof(*upload_batches) * primitive_capacity);
	if (!upload_batches) {
		blog(LOG_WARNING, "[vspace-source: '%s'] Failed to allocate material batch descriptors.",
		     vspace_source_log_name(context));
		return;
	}

	for (mesh_idx = 0; mesh_idx < payload->mesh_count && !upload_failed; mesh_idx++) {
		const struct vspace_cpu_mesh_payload *mesh = payload->meshes + mesh_idx;
		size_t primitive_idx;

		for (primitive_idx = 0; primitive_idx < mesh->primitive_count; primitive_idx++) {
			const struct vspace_cpu_primitive_payload *primitive = mesh->primitives + primitive_idx;
			size_t primitive_index_count;
			size_t primitive_triangle_count;
			size_t batch_idx;
			struct vspace_upload_material_batch *batch;
			struct vec3 primitive_bounds_min;
			struct vec3 primitive_bounds_max;

			if (!primitive->positions || primitive->vertex_count == 0)
				continue;

			primitive_index_count = primitive->index_count ? primitive->index_count : primitive->vertex_count;
			if (primitive_index_count == 0)
				continue;
			primitive_triangle_count = primitive_index_count / 3;
			if (primitive_triangle_count == 0)
				continue;

			if (!vspace_source_find_or_add_upload_batch(upload_batches, &upload_batch_count, primitive_capacity,
									    primitive->material_index, &batch_idx)) {
				blog(LOG_WARNING,
				     "[vspace-source: '%s'] Failed to create material batch for material=%d.",
				     vspace_source_log_name(context), primitive->material_index);
				upload_failed = true;
				break;
			}

			batch = upload_batches + batch_idx;
			if (batch->total_vertices > SIZE_MAX - primitive->vertex_count ||
			    batch->total_indices > SIZE_MAX - primitive_index_count ||
			    batch->total_wireframe_vertices > SIZE_MAX - (primitive_triangle_count * 3)) {
				blog(LOG_WARNING,
				     "[vspace-source: '%s'] Primitive accumulation overflow at mesh[%zu] primitive[%zu].",
				     vspace_source_log_name(context), mesh_idx, primitive_idx);
				upload_failed = true;
				break;
			}

			batch->total_vertices += primitive->vertex_count;
			batch->total_indices += primitive_index_count;
			batch->total_wireframe_vertices += primitive_triangle_count * 3;
			batch->primitive_count++;

			if (vspace_source_compute_primitive_bounds(primitive, &primitive_bounds_min, &primitive_bounds_max)) {
				if (!bounds_valid) {
					bounds_min = primitive_bounds_min;
					bounds_max = primitive_bounds_max;
					bounds_valid = true;
				} else {
					vec3_min(&bounds_min, &bounds_min, &primitive_bounds_min);
					vec3_max(&bounds_max, &bounds_max, &primitive_bounds_max);
				}
			}
		}
	}

	if (upload_failed)
		goto cleanup;

	if (!upload_batch_count) {
		vspace_source_reset_default_camera(context);
		obs_enter_graphics();
		vspace_source_release_gpu_resources(context);
		obs_leave_graphics();
		blog(LOG_WARNING, "[vspace-source: '%s'] No valid material batches were uploaded.",
		     vspace_source_log_name(context));
		goto cleanup;
	}

	new_gpu_meshes = bzalloc(sizeof(*new_gpu_meshes) * upload_batch_count);
	if (!new_gpu_meshes) {
		blog(LOG_WARNING, "[vspace-source: '%s'] Failed to allocate GPU material batch array.",
		     vspace_source_log_name(context));
		goto cleanup;
	}

	obs_enter_graphics();
	graphics_active = true;

	{
		size_t batch_idx;

		for (batch_idx = 0; batch_idx < upload_batch_count; batch_idx++) {
			struct vspace_upload_material_batch *batch = upload_batches + batch_idx;
			struct gs_vb_data *vb_data;
			struct gs_vb_data *wire_vb_data;

			if (batch->total_vertices > UINT32_MAX) {
				blog(LOG_WARNING,
				     "[vspace-source: '%s'] Too many vertices for 32-bit index buffer at batch[%zu]: "
				     "%zu.",
				     vspace_source_log_name(context), batch_idx, batch->total_vertices);
				upload_failed = true;
				break;
			}

			vb_data = gs_vbdata_create();
			wire_vb_data = gs_vbdata_create();
			if (!vb_data) {
				blog(LOG_WARNING, "[vspace-source: '%s'] Failed to allocate vertex data for batch[%zu].",
				     vspace_source_log_name(context), batch_idx);
				upload_failed = true;
				break;
			}
			if (!wire_vb_data) {
				gs_vbdata_destroy(vb_data);
				blog(LOG_WARNING,
				     "[vspace-source: '%s'] Failed to allocate wire vertex data for batch[%zu].",
				     vspace_source_log_name(context), batch_idx);
				upload_failed = true;
				break;
			}

			batch->vb_data = vb_data;
			batch->wire_vb_data = wire_vb_data;
			vb_data->num = batch->total_vertices;
			vb_data->points = bmalloc(sizeof(struct vec3) * batch->total_vertices);
			vb_data->normals = bmalloc(sizeof(struct vec3) * batch->total_vertices);
			vb_data->num_tex = 1;
			vb_data->tvarray = bzalloc(sizeof(struct gs_tvertarray));
			if (vb_data->tvarray) {
				vb_data->tvarray[0].width = 2;
				vb_data->tvarray[0].array = bmalloc(sizeof(struct vec2) * batch->total_vertices);
			}

			wire_vb_data->num = batch->total_wireframe_vertices;
			wire_vb_data->points = bmalloc(sizeof(struct vec3) * batch->total_wireframe_vertices);
			wire_vb_data->normals = bmalloc(sizeof(struct vec3) * batch->total_wireframe_vertices);
			wire_vb_data->num_tex = 2;
			wire_vb_data->tvarray = bzalloc(sizeof(struct gs_tvertarray) * 2);
			if (wire_vb_data->tvarray) {
				wire_vb_data->tvarray[0].width = 2;
				wire_vb_data->tvarray[0].array =
					bmalloc(sizeof(struct vec2) * batch->total_wireframe_vertices);
				wire_vb_data->tvarray[1].width = 4;
				wire_vb_data->tvarray[1].array =
					bmalloc(sizeof(struct vec4) * batch->total_wireframe_vertices);
			}

			batch->flat_indices = bmalloc(sizeof(uint32_t) * batch->total_indices);

			if (!vb_data->points || !vb_data->normals || !vb_data->tvarray || !vb_data->tvarray[0].array ||
			    !batch->flat_indices || !wire_vb_data->points || !wire_vb_data->normals ||
			    !wire_vb_data->tvarray || !wire_vb_data->tvarray[0].array ||
			    !wire_vb_data->tvarray[1].array) {
				blog(LOG_WARNING,
				     "[vspace-source: '%s'] Failed to allocate batch buffers for batch[%zu].",
				     vspace_source_log_name(context), batch_idx);
				upload_failed = true;
				break;
			}
		}
	}

	if (upload_failed)
		goto cleanup;

	for (mesh_idx = 0; mesh_idx < payload->mesh_count && !upload_failed; mesh_idx++) {
		const struct vspace_cpu_mesh_payload *mesh = payload->meshes + mesh_idx;
		size_t primitive_idx;

		for (primitive_idx = 0; primitive_idx < mesh->primitive_count; primitive_idx++) {
			const struct vspace_cpu_primitive_payload *primitive = mesh->primitives + primitive_idx;
			size_t primitive_vertex_count = primitive->vertex_count;
			size_t primitive_index_count = primitive->index_count ? primitive->index_count : primitive->vertex_count;
			size_t primitive_triangle_count = primitive_index_count / 3;
			size_t batch_idx;
			struct vspace_upload_material_batch *batch;
			struct gs_vb_data *vb_data;
			struct gs_vb_data *wire_vb_data;
			struct vec3 *points;
			struct vec3 *normals;
			struct vec2 *uvs;
			struct vec3 *wire_points;
			struct vec3 *wire_normals;
			struct vec2 *wire_uvs;
			struct vec4 *wire_barycentrics;
			size_t i;

			if (!primitive->positions || primitive_vertex_count == 0 || primitive_index_count == 0 ||
			    primitive_triangle_count == 0)
				continue;

			if (!vspace_source_find_upload_batch(upload_batches, upload_batch_count, primitive->material_index,
							      &batch_idx)) {
				blog(LOG_WARNING,
				     "[vspace-source: '%s'] Missing material batch while filling mesh[%zu] "
				     "primitive[%zu] material=%d.",
				     vspace_source_log_name(context), mesh_idx, primitive_idx, primitive->material_index);
				upload_failed = true;
				break;
			}

			batch = upload_batches + batch_idx;
			vb_data = batch->vb_data;
			wire_vb_data = batch->wire_vb_data;
			if (!vb_data || !wire_vb_data) {
				upload_failed = true;
				break;
			}

			if (batch->vertex_offset > batch->total_vertices - primitive_vertex_count ||
			    batch->index_offset > batch->total_indices - primitive_index_count ||
			    batch->wire_vertex_offset > batch->total_wireframe_vertices - (primitive_triangle_count * 3)) {
				blog(LOG_WARNING,
				     "[vspace-source: '%s'] Batch write overflow at mesh[%zu] primitive[%zu] "
				     "(batch=%zu).",
				     vspace_source_log_name(context), mesh_idx, primitive_idx, batch_idx);
				upload_failed = true;
				break;
			}

			points = vb_data->points;
			normals = vb_data->normals;
			uvs = vb_data->tvarray[0].array;
			wire_points = wire_vb_data->points;
			wire_normals = wire_vb_data->normals;
			wire_uvs = wire_vb_data->tvarray[0].array;
			wire_barycentrics = wire_vb_data->tvarray[1].array;

			for (i = 0; i < primitive_vertex_count; i++) {
				const float *src_pos = primitive->positions + (i * 3);
				struct vec3 *dst_pos = &points[batch->vertex_offset + i];
				struct vec3 *dst_normal = &normals[batch->vertex_offset + i];
				struct vec2 *dst_uv = &uvs[batch->vertex_offset + i];

				dst_pos->x = src_pos[0];
				dst_pos->y = src_pos[1];
				dst_pos->z = src_pos[2];

				if (primitive->normals) {
					const float *src_normal = primitive->normals + (i * 3);

					dst_normal->x = src_normal[0];
					dst_normal->y = src_normal[1];
					dst_normal->z = src_normal[2];
				} else {
					dst_normal->x = src_pos[0];
					dst_normal->y = src_pos[1];
					dst_normal->z = src_pos[2];
					vec3_norm(dst_normal, dst_normal);
					if (vec3_len(dst_normal) < 0.0001f)
						vec3_set(dst_normal, 0.0f, 0.0f, 1.0f);
				}

				if (primitive->texcoords) {
					const float *src_uv = primitive->texcoords + (i * 2);

					dst_uv->x = src_uv[0];
					dst_uv->y = src_uv[1];
				} else {
					dst_uv->x = 0.0f;
					dst_uv->y = 0.0f;
				}
			}

			for (i = 0; i < primitive_index_count; i++) {
				uint32_t local_index = primitive->indices ? primitive->indices[i] : (uint32_t)i;

				if ((size_t)local_index >= primitive_vertex_count) {
					blog(LOG_WARNING,
					     "[vspace-source: '%s'] Invalid local index at mesh[%zu] primitive[%zu]: "
					     "%u >= %zu. Clamping to 0.",
					     vspace_source_log_name(context), mesh_idx, primitive_idx, local_index,
					     primitive_vertex_count);
					local_index = 0;
				}

				batch->flat_indices[batch->index_offset + i] =
					(uint32_t)(batch->vertex_offset + (size_t)local_index);
			}

			if ((primitive_index_count % 3) != 0) {
				blog(LOG_WARNING,
				     "[vspace-source: '%s'] Primitive index count is not divisible by 3 at "
				     "mesh[%zu] primitive[%zu] (indices=%zu). Trailing indices are ignored for "
				     "wireframe.",
				     vspace_source_log_name(context), mesh_idx, primitive_idx, primitive_index_count);
			}

			for (i = 0; i < primitive_triangle_count; i++) {
				size_t corner;
				uint32_t local_indices[3];
				static const struct vec4 bary_values[3] = {
					{1.0f, 0.0f, 0.0f, 0.0f},
					{0.0f, 1.0f, 0.0f, 0.0f},
					{0.0f, 0.0f, 1.0f, 0.0f},
				};

				local_indices[0] = primitive->indices ? primitive->indices[i * 3 + 0] : (uint32_t)(i * 3 + 0);
				local_indices[1] = primitive->indices ? primitive->indices[i * 3 + 1] : (uint32_t)(i * 3 + 1);
				local_indices[2] = primitive->indices ? primitive->indices[i * 3 + 2] : (uint32_t)(i * 3 + 2);

				for (corner = 0; corner < 3; corner++) {
					uint32_t local_index = local_indices[corner];
					const float *src_pos;
					const float *src_normal = NULL;
					const float *src_uv = NULL;
					const size_t wire_dst = batch->wire_vertex_offset + corner;
					struct vec3 *dst_wire_pos = wire_points + wire_dst;
					struct vec3 *dst_wire_normal = wire_normals + wire_dst;
					struct vec2 *dst_wire_uv = wire_uvs + wire_dst;
					struct vec4 *dst_wire_bary = wire_barycentrics + wire_dst;

					if ((size_t)local_index >= primitive_vertex_count) {
						blog(LOG_WARNING,
						     "[vspace-source: '%s'] Invalid wire local index at mesh[%zu] "
						     "primitive[%zu]: %u >= %zu. Clamping to 0.",
						     vspace_source_log_name(context), mesh_idx, primitive_idx, local_index,
						     primitive_vertex_count);
						local_index = 0;
					}

					src_pos = primitive->positions + ((size_t)local_index * 3);
					if (primitive->normals)
						src_normal = primitive->normals + ((size_t)local_index * 3);
					if (primitive->texcoords)
						src_uv = primitive->texcoords + ((size_t)local_index * 2);

					dst_wire_pos->x = src_pos[0];
					dst_wire_pos->y = src_pos[1];
					dst_wire_pos->z = src_pos[2];

					if (src_normal) {
						dst_wire_normal->x = src_normal[0];
						dst_wire_normal->y = src_normal[1];
						dst_wire_normal->z = src_normal[2];
					} else {
						dst_wire_normal->x = src_pos[0];
						dst_wire_normal->y = src_pos[1];
						dst_wire_normal->z = src_pos[2];
						vec3_norm(dst_wire_normal, dst_wire_normal);
						if (vec3_len(dst_wire_normal) < 0.0001f)
							vec3_set(dst_wire_normal, 0.0f, 0.0f, 1.0f);
					}

					if (src_uv) {
						dst_wire_uv->x = src_uv[0];
						dst_wire_uv->y = src_uv[1];
					} else {
						dst_wire_uv->x = 0.0f;
						dst_wire_uv->y = 0.0f;
					}

					*dst_wire_bary = bary_values[corner];
				}

				batch->wire_vertex_offset += 3;
			}

			batch->vertex_offset += primitive_vertex_count;
			batch->index_offset += primitive_index_count;
			batch->uploaded_primitives++;

			blog(LOG_INFO,
			     "[vspace-source: '%s'] GPU primitive upload complete: mesh[%zu] primitive[%zu] "
			     "material=%d batch[%zu] vertices=%zu indices=%zu",
			     vspace_source_log_name(context), mesh_idx, primitive_idx, primitive->material_index,
			     batch_idx, primitive_vertex_count, primitive_index_count);
		}
	}

	if (upload_failed)
		goto cleanup;

	{
		size_t batch_idx;

		for (batch_idx = 0; batch_idx < upload_batch_count; batch_idx++) {
			struct vspace_upload_material_batch *batch = upload_batches + batch_idx;
			gs_vertbuffer_t *new_vertex_buffer = NULL;
			gs_vertbuffer_t *new_wireframe_vertex_buffer = NULL;
			gs_indexbuffer_t *new_index_buffer = NULL;

			if (batch->uploaded_primitives == 0 || batch->vertex_offset == 0 || batch->index_offset == 0) {
				blog(LOG_WARNING,
				     "[vspace-source: '%s'] Empty material batch generated for batch[%zu] (material=%d).",
				     vspace_source_log_name(context), batch_idx, batch->material_index);
				upload_failed = true;
				break;
			}

			new_vertex_buffer = gs_vertexbuffer_create(batch->vb_data, 0);
			if (!new_vertex_buffer) {
				upload_failed = true;
				break;
			}
			batch->vb_data = NULL;

			new_index_buffer = gs_indexbuffer_create(GS_UNSIGNED_LONG, batch->flat_indices, batch->index_offset,
								  GS_DUP_BUFFER);
			if (!new_index_buffer) {
				gs_vertexbuffer_destroy(new_vertex_buffer);
				upload_failed = true;
				break;
			}

			if (batch->wire_vertex_offset > 0) {
				new_wireframe_vertex_buffer = gs_vertexbuffer_create(batch->wire_vb_data, 0);
				if (!new_wireframe_vertex_buffer) {
					gs_indexbuffer_destroy(new_index_buffer);
					gs_vertexbuffer_destroy(new_vertex_buffer);
					upload_failed = true;
					break;
				}
				batch->wire_vb_data = NULL;
			}

			bfree(batch->flat_indices);
			batch->flat_indices = NULL;

			new_gpu_meshes[new_gpu_mesh_count].vertex_buffer = new_vertex_buffer;
			new_gpu_meshes[new_gpu_mesh_count].index_buffer = new_index_buffer;
			new_gpu_meshes[new_gpu_mesh_count].wireframe_vertex_buffer = new_wireframe_vertex_buffer;
			new_gpu_meshes[new_gpu_mesh_count].wireframe_index_buffer = NULL;
			new_gpu_meshes[new_gpu_mesh_count].material_index = batch->material_index;
			new_gpu_meshes[new_gpu_mesh_count].draw_vertex_count = batch->vertex_offset;
			new_gpu_meshes[new_gpu_mesh_count].draw_index_count = batch->index_offset;
			new_gpu_meshes[new_gpu_mesh_count].wireframe_vertex_count = batch->wire_vertex_offset;
			new_gpu_meshes[new_gpu_mesh_count].wireframe_index_count = 0;
			new_gpu_mesh_count++;

			total_uploaded_primitives += batch->uploaded_primitives;
			total_uploaded_vertices += batch->vertex_offset;
			total_uploaded_indices += batch->index_offset;
			total_uploaded_wireframe_vertices += batch->wire_vertex_offset;
		}
	}

	if (!upload_failed && new_gpu_mesh_count > 0) {
		if (decoded_image_valid)
			gs_image_file4_init_texture(decoded_image);

		vspace_source_release_gpu_resources(context);

		context->gpu_meshes = new_gpu_meshes;
		context->gpu_mesh_count = new_gpu_mesh_count;
		context->draw_vertex_count = total_uploaded_vertices;
		context->draw_index_count = total_uploaded_indices;
		context->wireframe_vertex_count = total_uploaded_wireframe_vertices;
		context->wireframe_index_count = 0;
		new_gpu_meshes = NULL;

		if (decoded_image_valid && decoded_image->image3.image2.image.texture) {
			context->base_color_image = *decoded_image;
			memset(decoded_image, 0, sizeof(*decoded_image));
			context->base_color_image_valid = true;
		}

		if (bounds_valid) {
			vspace_source_lock_camera(context);
			context->model_bounds_min = bounds_min;
			context->model_bounds_max = bounds_max;
			context->model_bounds_valid = true;
			vspace_source_unlock_camera(context);
			vspace_source_update_default_camera(context);
			vspace_source_log_camera_fit(context);
		} else {
			vspace_source_reset_default_camera(context);
		}

		if (!context->diagnostics_logged_upload) {
			blog(LOG_INFO,
			     "[vspace-source: '%s'] GPU upload complete: source_meshes=%zu, batches=%zu, "
			     "primitives=%zu, vertices=%zu, indices=%zu, wire_vertices=%zu, texture=%s",
			     vspace_source_log_name(context), payload->mesh_count, context->gpu_mesh_count,
			     total_uploaded_primitives, context->draw_vertex_count, context->draw_index_count,
			     context->wireframe_vertex_count, context->base_color_image_valid ? "yes" : "no");
			context->diagnostics_logged_upload = true;
		}
	} else if (!upload_failed) {
		vspace_source_reset_default_camera(context);
		vspace_source_release_gpu_resources(context);
		blog(LOG_WARNING, "[vspace-source: '%s'] No valid material batches were uploaded.",
		     vspace_source_log_name(context));
	} else {
		blog(LOG_WARNING,
		     "[vspace-source: '%s'] GPU upload failed: material batch vertex/index buffer creation failed.",
		     vspace_source_log_name(context));
	}

cleanup:
	if (new_gpu_meshes)
		vspace_source_release_gpu_meshes(new_gpu_meshes, new_gpu_mesh_count);
	vspace_source_release_upload_material_batches(upload_batches, upload_batch_count);
	if (graphics_active)
		obs_leave_graphics();
}

static bool vspace_source_load_cpu_payload(struct vspace_source *context, const char *model_path, bool draco_enabled,
					     const char *draco_decoder, struct vspace_cpu_payload *payload,
					     gs_image_file4_t *decoded_image, bool *decoded_image_valid)
{
	struct vspace_gltf_load_options options;
	struct vspace_gltf_error error = {0};

	if (!context || !model_path || !*model_path || !payload || !decoded_image || !decoded_image_valid)
		return false;

	options.draco_enabled = draco_enabled;
	options.draco_decoder = (draco_decoder && *draco_decoder) ? draco_decoder : S_DRACO_DECODER_AUTO;

	if (!vspace_gltf_load_cpu_payload(model_path, payload, &options, &error)) {
		blog(LOG_WARNING, "[vspace-source: '%s'] glTF load failed (%s): %s",
		     vspace_source_log_name(context), vspace_gltf_error_to_string(error.code),
		     error.message ? error.message : "no details");
		vspace_gltf_clear_error(&error);
		return false;
	}

	vspace_source_log_payload_summary(context, payload);
	*decoded_image_valid = vspace_source_decode_base_color_image(context, payload, decoded_image);
	return true;
}

static void *vspace_source_worker_main(void *data)
{
	struct vspace_source *context = data;

	os_set_thread_name("vspace-loader");

	while (os_event_wait(context->worker_event) == 0) {
		struct vspace_cpu_payload payload = {0};
		gs_image_file4_t decoded_image = {0};
		bool decoded_image_valid = false;
		char *job_model_path = NULL;
		char *job_draco_decoder = NULL;
		bool job_draco_enabled = false;
		uint64_t token = 0;
		bool has_job = false;

		if (os_atomic_load_bool(&context->worker_stop))
			break;

		pthread_mutex_lock(&context->worker_mutex);
		if (context->worker_job.has_job) {
			job_model_path = context->worker_job.model_path;
			context->worker_job.model_path = NULL;
			job_draco_decoder = context->worker_job.draco_decoder;
			context->worker_job.draco_decoder = NULL;
			job_draco_enabled = context->worker_job.draco_enabled;
			token = context->worker_job.token;
			context->worker_job.has_job = false;
			has_job = true;
		}
		pthread_mutex_unlock(&context->worker_mutex);

		if (!has_job)
			continue;

		if (job_model_path && *job_model_path)
			vspace_source_load_cpu_payload(context, job_model_path, job_draco_enabled, job_draco_decoder,
							 &payload, &decoded_image, &decoded_image_valid);

		vspace_source_publish_pending_upload(context, &payload, &decoded_image, decoded_image_valid, token);

		vspace_gltf_free_cpu_payload(&payload);
		if (decoded_image_valid)
			gs_image_file4_free(&decoded_image);
		bfree(job_model_path);
		bfree(job_draco_decoder);
	}

	return NULL;
}

static bool vspace_source_start_worker(struct vspace_source *context)
{
	if (!context)
		return false;

	pthread_mutex_init_value(&context->worker_mutex);
	if (pthread_mutex_init(&context->worker_mutex, NULL) != 0)
		return false;
	context->worker_mutex_valid = true;

	if (os_event_init(&context->worker_event, OS_EVENT_TYPE_AUTO) != 0)
		return false;

	os_atomic_store_bool(&context->worker_stop, false);
	context->worker_thread_active = pthread_create(&context->worker_thread, NULL, vspace_source_worker_main, context) == 0;
	if (!context->worker_thread_active)
		return false;

	return true;
}

static void vspace_source_stop_worker(struct vspace_source *context)
{
	if (!context)
		return;

	os_atomic_store_bool(&context->worker_stop, true);

	if (context->worker_event)
		os_event_signal(context->worker_event);

	if (context->worker_thread_active) {
		pthread_join(context->worker_thread, NULL);
		context->worker_thread_active = false;
	}

	if (context->worker_event) {
		os_event_destroy(context->worker_event);
		context->worker_event = NULL;
	}

	if (context->worker_mutex_valid) {
		pthread_mutex_destroy(&context->worker_mutex);
		context->worker_mutex_valid = false;
	}
}

static void vspace_source_queue_load_job(struct vspace_source *context)
{
	uint64_t token;

	if (!context)
		return;

	if (!context->worker_thread_active) {
		struct vspace_cpu_payload payload = {0};
		gs_image_file4_t decoded_image = {0};
		bool decoded_image_valid = false;
		uint64_t token = ++context->worker_next_token;

		context->worker_cancel_token = token;

		if (vspace_source_model_path_is_loadable(context))
			vspace_source_load_cpu_payload(context, context->model_path, context->draco_enabled,
							 context->draco_decoder, &payload, &decoded_image,
							 &decoded_image_valid);

		vspace_source_publish_pending_upload(context, &payload, &decoded_image, decoded_image_valid, token);
		vspace_gltf_free_cpu_payload(&payload);
		if (decoded_image_valid)
			gs_image_file4_free(&decoded_image);
		return;
	}

	pthread_mutex_lock(&context->worker_mutex);

	token = ++context->worker_next_token;
	context->worker_cancel_token = token;

	vspace_source_free_worker_job(context);
	context->worker_job.model_path =
		vspace_source_model_path_is_loadable(context) ? bstrdup(context->model_path) : NULL;
	context->worker_job.draco_decoder =
		context->draco_decoder ? bstrdup(context->draco_decoder) : bstrdup(S_DRACO_DECODER_AUTO);
	context->worker_job.draco_enabled = context->draco_enabled;
	context->worker_job.token = token;
	context->worker_job.has_job = true;

	pthread_mutex_unlock(&context->worker_mutex);

	os_event_signal(context->worker_event);
}

static void vspace_source_process_pending_upload(struct vspace_source *context)
{
	struct vspace_cpu_payload payload = {0};
	gs_image_file4_t decoded_image = {0};
	bool decoded_image_valid = false;

	if (!context)
		return;

	if (os_atomic_load_bool(&context->device_loss_active))
		return;

	if (!vspace_source_take_pending_upload(context, &payload, &decoded_image, &decoded_image_valid))
		return;

	blog(LOG_INFO, "[vspace-source: '%s'] Consuming pending payload on render thread.",
	     vspace_source_log_name(context));
	vspace_source_upload_pending_payload(context, &payload, &decoded_image, decoded_image_valid);
	vspace_gltf_free_cpu_payload(&payload);

	if (decoded_image_valid)
		gs_image_file4_free(&decoded_image);
}

static void vspace_source_interaction_reset_camera(struct vspace_source *context)
{
	bool model_bounds_valid = false;

	if (!context)
		return;

	vspace_source_lock_camera(context);
	model_bounds_valid = context->model_bounds_valid;
	vspace_source_unlock_camera(context);

	if (!model_bounds_valid)
		return;

	vspace_source_update_default_camera(context);
}

static void vspace_source_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
					uint32_t click_count)
{
	struct vspace_source *context = data;
	const bool shift_pressed = event && ((event->modifiers & INTERACT_SHIFT_KEY) != 0);
	const bool ctrl_pressed =
		event && ((event->modifiers & (INTERACT_CONTROL_KEY | INTERACT_COMMAND_KEY)) != 0);

	UNUSED_PARAMETER(click_count);

	if (!context || !event)
		return;

	if (type != MOUSE_MIDDLE)
		return;

	if (mouse_up) {
		vspace_source_end_camera_drag(context, true, true, true, true);
		return;
	}

	/*
	 * Blender-style viewport navigation:
	 *   MMB               -> orbit (rotate)
	 *   Shift+MMB         -> pan (move)
	 *   Ctrl+MMB          -> zoom
	 *   Shift+Ctrl+MMB    -> dolly
	 */
	if (shift_pressed && ctrl_pressed)
		vspace_source_begin_camera_drag(context, false, false, false, true, event->x, event->y);
	else if (shift_pressed)
		vspace_source_begin_camera_drag(context, false, true, false, false, event->x, event->y);
	else if (ctrl_pressed)
		vspace_source_begin_camera_drag(context, false, false, true, false, event->x, event->y);
	else
		vspace_source_begin_camera_drag(context, true, false, false, false, event->x, event->y);
}

static void vspace_source_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	struct vspace_source *context = data;
	bool drag_orbit = false;
	bool drag_pan = false;
	bool drag_zoom = false;
	bool drag_dolly = false;
	bool have_last = false;
	int32_t delta_x = 0;
	int32_t delta_y = 0;

	if (!context)
		return;

	if (mouse_leave || !event) {
		vspace_source_end_camera_drag(context, true, true, true, true);
		return;
	}

	vspace_source_lock_camera(context);
	drag_orbit = context->camera_drag_orbit;
	drag_pan = context->camera_drag_pan;
	drag_zoom = context->camera_drag_zoom;
	drag_dolly = context->camera_drag_dolly;

	have_last = context->camera_last_input_valid;
	if (have_last) {
		delta_x = event->x - context->camera_last_input_x;
		delta_y = event->y - context->camera_last_input_y;
	}
	context->camera_last_input_x = event->x;
	context->camera_last_input_y = event->y;
	context->camera_last_input_valid = true;
	vspace_source_unlock_camera(context);

	if (!have_last || (!drag_orbit && !drag_pan && !drag_zoom && !drag_dolly))
		return;

	if (drag_orbit)
		vspace_source_orbit_camera(context, delta_x, delta_y);
	if (drag_pan)
		vspace_source_pan_camera(context, delta_x, delta_y);
	if (drag_zoom)
		vspace_source_zoom_drag_camera(context, delta_y);
	if (drag_dolly)
		vspace_source_dolly_camera(context, delta_x, delta_y);
}

static void vspace_source_mouse_wheel(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	struct vspace_source *context = data;

	UNUSED_PARAMETER(event);
	UNUSED_PARAMETER(x_delta);

	if (!context)
		return;

	vspace_source_zoom_camera(context, y_delta);
}

static void vspace_source_focus(void *data, bool focus)
{
	struct vspace_source *context = data;

	if (!focus)
		vspace_source_end_camera_drag(context, true, true, true, true);
}

static bool vspace_source_is_reset_key(const struct obs_key_event *event)
{
	if (!event)
		return false;

	if (event->text && *event->text) {
		const char key = (char)tolower((unsigned char)event->text[0]);

		if (key == 'r')
			return true;
	}

	return event->native_vkey == 'R' || event->native_vkey == 'r';
}

static void vspace_source_key_click(void *data, const struct obs_key_event *event, bool key_up)
{
	struct vspace_source *context = data;

	if (!context || key_up || !event)
		return;

	if (vspace_source_is_reset_key(event))
		vspace_source_interaction_reset_camera(context);
}

static void vspace_source_get_camera_basis_proc(void *data, calldata_t *params)
{
	struct vspace_source *context = data;
	struct vec3 right;
	struct vec3 up;
	struct vec3 forward;
	bool available = false;

	if (!params)
		return;

	vec3_set(&right, 1.0f, 0.0f, 0.0f);
	vec3_set(&up, 0.0f, 0.0f, 1.0f);
	vec3_set(&forward, 0.0f, 1.0f, 0.0f);

	if (context) {
		vspace_source_lock_camera(context);
		if (context->default_camera_valid) {
			vspace_source_get_camera_basis_locked(context, &forward, &right, &up);
			available = true;
		}
		vspace_source_unlock_camera(context);
	}

	calldata_set_bool(params, "available", available);
	calldata_set_float(params, "forward_x", forward.x);
	calldata_set_float(params, "forward_y", forward.y);
	calldata_set_float(params, "forward_z", forward.z);
	calldata_set_float(params, "right_x", right.x);
	calldata_set_float(params, "right_y", right.y);
	calldata_set_float(params, "right_z", right.z);
	calldata_set_float(params, "up_x", up.x);
	calldata_set_float(params, "up_y", up.y);
	calldata_set_float(params, "up_z", up.z);
}

static void vspace_source_get_camera_state_proc(void *data, calldata_t *params)
{
	struct vspace_source *context = data;
	struct vec3 camera_position;
	struct vec3 camera_target;
	struct vec3 camera_up;
	float fov_deg = 50.0f;
	float znear = 0.1f;
	float zfar = 100.0f;
	bool available = false;

	if (!params)
		return;

	vec3_zero(&camera_position);
	vec3_zero(&camera_target);
	vec3_set(&camera_up, 0.0f, 0.0f, 1.0f);

	if (context) {
		vspace_source_lock_camera(context);
		if (context->default_camera_valid) {
			camera_position = context->default_camera_position;
			camera_target = context->camera_target;
			camera_up = context->camera_up;
			fov_deg = context->default_camera_fov_deg;
			znear = context->default_camera_znear;
			zfar = context->default_camera_zfar;
			available = true;
		}
		vspace_source_unlock_camera(context);
	}

	calldata_set_bool(params, "available", available);
	calldata_set_float(params, "camera_x", camera_position.x);
	calldata_set_float(params, "camera_y", camera_position.y);
	calldata_set_float(params, "camera_z", camera_position.z);
	calldata_set_float(params, "target_x", camera_target.x);
	calldata_set_float(params, "target_y", camera_target.y);
	calldata_set_float(params, "target_z", camera_target.z);
	calldata_set_float(params, "up_x", camera_up.x);
	calldata_set_float(params, "up_y", camera_up.y);
	calldata_set_float(params, "up_z", camera_up.z);
	calldata_set_float(params, "fov_deg", fov_deg);
	calldata_set_float(params, "znear", znear);
	calldata_set_float(params, "zfar", zfar);
}

static void vspace_source_get_model_bounds_proc(void *data, calldata_t *params)
{
	struct vspace_source *context = data;
	struct vec3 bounds_min;
	struct vec3 bounds_max;
	bool available = false;

	if (!params)
		return;

	vec3_zero(&bounds_min);
	vec3_zero(&bounds_max);

	if (context) {
		vspace_source_lock_camera(context);
		if (context->model_bounds_valid) {
			bounds_min = context->model_bounds_min;
			bounds_max = context->model_bounds_max;
			available = true;
		}
		vspace_source_unlock_camera(context);
	}

	calldata_set_bool(params, "available", available);
	calldata_set_float(params, "min_x", bounds_min.x);
	calldata_set_float(params, "min_y", bounds_min.y);
	calldata_set_float(params, "min_z", bounds_min.z);
	calldata_set_float(params, "max_x", bounds_max.x);
	calldata_set_float(params, "max_y", bounds_max.y);
	calldata_set_float(params, "max_z", bounds_max.z);
}

static void vspace_source_set_inspect_render_mode_proc(void *data, calldata_t *params)
{
	struct vspace_source *context = data;
	bool enabled;

	if (!context || !params)
		return;

	enabled = calldata_bool(params, "enabled");
	os_atomic_store_bool(&context->inspect_render_mode, enabled);
}

static void vspace_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_MODEL_PATH, "");
	obs_data_set_default_bool(settings, S_DRACO_ENABLED, true);
	obs_data_set_default_string(settings, S_DRACO_DECODER, S_DRACO_DECODER_AUTO);
	obs_data_set_default_int(settings, S_BACKGROUND_COLOR, 0xFF101010);
}

static void vspace_source_update(void *data, obs_data_t *settings)
{
	struct vspace_source *context = data;
	const char *model_path_raw = obs_data_get_string(settings, S_MODEL_PATH);
	const char *draco_decoder_raw = obs_data_get_string(settings, S_DRACO_DECODER);
	const char *new_model_path = (model_path_raw && *model_path_raw) ? model_path_raw : NULL;
	const bool new_draco_enabled = obs_data_get_bool(settings, S_DRACO_ENABLED);
	const char *new_draco_decoder =
		(draco_decoder_raw && *draco_decoder_raw) ? draco_decoder_raw : S_DRACO_DECODER_AUTO;
	const bool model_path_changed = !vspace_source_nullable_streq(context->model_path, new_model_path);
	const bool draco_enabled_changed = context->draco_enabled != new_draco_enabled;
	const bool draco_decoder_changed = !vspace_source_nullable_streq(context->draco_decoder, new_draco_decoder);
	const bool requires_reload = model_path_changed || draco_enabled_changed || draco_decoder_changed;

	if (model_path_changed) {
		bfree(context->model_path);
		context->model_path = new_model_path ? bstrdup(new_model_path) : NULL;
	}

	context->draco_enabled = new_draco_enabled;
	context->background_color = (uint32_t)obs_data_get_int(settings, S_BACKGROUND_COLOR);

	if (draco_decoder_changed) {
		bfree(context->draco_decoder);
		context->draco_decoder = bstrdup(new_draco_decoder);
	}

	vspace_source_validate_model_path(context);

	if (context->draco_enabled && astrcmpi(context->draco_decoder, S_DRACO_DECODER_EXTERNAL) == 0) {
		blog(LOG_WARNING,
		     "[vspace-source: '%s'] External Draco decoder mode is not implemented in this scaffold.",
		     vspace_source_log_name(context));
	}

	if (requires_reload) {
		context->diagnostics_logged_upload = false;
		context->diagnostics_logged_draw = false;
		vspace_source_queue_load_job(context);
	}
	vspace_source_refresh_size(context);
}

static obs_properties_t *vspace_source_properties(void *data)
{
	struct vspace_source *context = data;
	obs_property_t *model_path;
	obs_property_t *draco_decoder;
	obs_properties_t *props = obs_properties_create();
	const bool model_uses_draco = vspace_source_model_path_uses_draco(context ? context->model_path : NULL);

	model_path = obs_properties_add_path(props, S_MODEL_PATH, obs_module_text("Vspace.ModelFile"), OBS_PATH_FILE,
					     obs_module_text("Vspace.ModelFile.Filter"), NULL);
	obs_property_set_modified_callback(model_path, vspace_source_model_path_modified);
	obs_properties_add_color_alpha(props, S_BACKGROUND_COLOR, obs_module_text("Vspace.BackgroundColor"));
	obs_properties_add_bool(props, S_DRACO_ENABLED, obs_module_text("Vspace.Draco.Enable"));

	draco_decoder = obs_properties_add_list(props, S_DRACO_DECODER, obs_module_text("Vspace.Draco.Decoder"),
						OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(draco_decoder, obs_module_text("Vspace.Draco.Decoder.Auto"), S_DRACO_DECODER_AUTO);
	obs_property_list_add_string(draco_decoder, obs_module_text("Vspace.Draco.Decoder.Builtin"),
				     S_DRACO_DECODER_BUILTIN);
	obs_property_list_add_string(draco_decoder, obs_module_text("Vspace.Draco.Decoder.External"),
				     S_DRACO_DECODER_EXTERNAL);

	vspace_source_set_draco_property_state(props, model_uses_draco);
	return props;
}

static const char *vspace_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Model3DSource");
}

static void *vspace_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct vspace_source *context = bzalloc(sizeof(*context));
	proc_handler_t *proc_handler = NULL;

	context->source = source;
	proc_handler = obs_source_get_proc_handler(source);
	if (proc_handler) {
		proc_handler_add(proc_handler,
				 "void get_vspace_camera_basis("
				 "out bool available, "
				 "out float forward_x, out float forward_y, out float forward_z, "
				 "out float right_x, out float right_y, out float right_z, "
				 "out float up_x, out float up_y, out float up_z)",
				 vspace_source_get_camera_basis_proc, context);
		proc_handler_add(proc_handler,
				 "void get_vspace_camera_state("
				 "out bool available, "
				 "out float camera_x, out float camera_y, out float camera_z, "
				 "out float target_x, out float target_y, out float target_z, "
				 "out float up_x, out float up_y, out float up_z, "
				 "out float fov_deg, out float znear, out float zfar)",
				 vspace_source_get_camera_state_proc, context);
		proc_handler_add(proc_handler,
				 "void get_vspace_model_bounds("
				 "out bool available, "
				 "out float min_x, out float min_y, out float min_z, "
				 "out float max_x, out float max_y, out float max_z)",
				 vspace_source_get_model_bounds_proc, context);
		proc_handler_add(proc_handler, "void set_vspace_inspect_render_mode(bool enabled)",
				 vspace_source_set_inspect_render_mode_proc, context);
	}

	pthread_mutex_init_value(&context->camera_mutex);
	if (pthread_mutex_init(&context->camera_mutex, NULL) == 0)
		context->camera_mutex_valid = true;

	vspace_source_reset_default_camera(context);
	vspace_source_reset_default_light(context);
	if (!vspace_source_start_worker(context)) {
		blog(LOG_WARNING,
		     "[vspace-source: '%s'] Failed to start async loader worker. Falling back to inline loading.",
		     vspace_source_log_name(context));

		if (!context->worker_mutex_valid) {
			pthread_mutex_init_value(&context->worker_mutex);
			if (pthread_mutex_init(&context->worker_mutex, NULL) == 0)
				context->worker_mutex_valid = true;
		}
	}

	vspace_source_register_device_loss_callbacks(context);
	vspace_source_refresh_size(context);
	vspace_source_update(context, settings);
	vspace_source_load_effect(context);
	blog(LOG_INFO, "[vspace-source: '%s'] Camera axis mode active: Z-up / XY-grid / orbit-global-up=+Z",
	     vspace_source_log_name(context));
	return context;
}

static void vspace_source_destroy(void *data)
{
	struct vspace_source *context = data;

	vspace_source_unregister_device_loss_callbacks(context);
	vspace_source_stop_worker(context);
	vspace_source_free_worker_job(context);
	vspace_source_release_pending_upload(context);

	obs_enter_graphics();
	vspace_source_release_gpu_resources(context);
	obs_leave_graphics();

	vspace_source_unload_effect(context);
	bfree(context->model_path);
	bfree(context->draco_decoder);
	if (context->camera_mutex_valid) {
		pthread_mutex_destroy(&context->camera_mutex);
		context->camera_mutex_valid = false;
	}
	bfree(context);
}

static void vspace_source_show(void *data)
{
	struct vspace_source *context = data;
	context->showing = true;
}

static void vspace_source_hide(void *data)
{
	struct vspace_source *context = data;
	context->showing = false;
}

static void vspace_source_activate(void *data)
{
	struct vspace_source *context = data;
	context->active = true;
}

static void vspace_source_deactivate(void *data)
{
	struct vspace_source *context = data;
	context->active = false;
}

static void vspace_source_video_tick(void *data, float seconds)
{
	struct vspace_source *context = data;

	UNUSED_PARAMETER(seconds);

	if (!context)
		return;

	vspace_source_refresh_size(context);
	if (vspace_source_should_auto_fit_camera(context))
		vspace_source_update_default_camera(context);

	if (os_atomic_load_bool(&context->device_rebuild_pending)) {
		os_atomic_store_bool(&context->device_rebuild_pending, false);
		os_atomic_store_bool(&context->device_loss_active, false);
		context->effect_load_attempted = false;
		vspace_source_load_effect(context);
		vspace_source_queue_load_job(context);
	}

	vspace_source_process_pending_upload(context);

	if (!context->effect && !context->effect_load_attempted)
		vspace_source_load_effect(context);
}

static void vspace_source_render(void *data, gs_effect_t *effect)
{
	struct vspace_source *context = data;
	gs_texture_t *model_texture = NULL;
	bool rendered = false;
	const bool inspect_render_mode = os_atomic_load_bool(&context->inspect_render_mode);
	const bool opaque_background = !inspect_render_mode && ((context->background_color >> 24) == 0xFF);
	const bool previous_srgb = gs_framebuffer_srgb_enabled();

	UNUSED_PARAMETER(effect);

	if (!context)
		return;

	if (os_atomic_load_bool(&context->device_loss_active))
		return;

	/* BaseColor is sampled as sRGB and shaded in linear space before output. */
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	if (opaque_background)
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	else
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	model_texture = vspace_source_render_model_to_texture(context);
	if (model_texture) {
		const float background_alpha = inspect_render_mode ? 0.0f : (float)((context->background_color >> 24) & 0xFF) / 255.0f;

		if (context->effect && context->effect_composite_image_param && context->effect_composite_background_alpha_param) {
			gs_effect_set_texture_srgb(context->effect_composite_image_param, model_texture);
			gs_effect_set_float(context->effect_composite_background_alpha_param, background_alpha);
			while (gs_effect_loop(context->effect, "DrawComposite"))
				gs_draw_sprite(model_texture, 0, context->width, context->height);
			rendered = true;
		}

		if (!rendered) {
			gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *image_param = default_effect ? gs_effect_get_param_by_name(default_effect, "image") : NULL;

			if (default_effect && image_param) {
				gs_effect_set_texture_srgb(image_param, model_texture);
				while (gs_effect_loop(default_effect, "Draw"))
					gs_draw_sprite(model_texture, 0, context->width, context->height);
				rendered = true;
			}
		}
	}

	if (!rendered) {
		/* Fallback draw path keeps source visible even when custom effect is unavailable. */
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *color = solid ? gs_effect_get_param_by_name(solid, "color") : NULL;
		struct vec4 placeholder_color;

		if (solid && color) {
			vec4_from_rgba_srgb(&placeholder_color, inspect_render_mode ? 0x00000000 : context->background_color);
			placeholder_color.x *= placeholder_color.w;
			placeholder_color.y *= placeholder_color.w;
			placeholder_color.z *= placeholder_color.w;
			gs_effect_set_vec4(color, &placeholder_color);

			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(NULL, 0, context->width, context->height);
		}
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
}

static enum gs_color_space vspace_source_get_color_space(void *data, size_t count,
							    const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	return GS_CS_SRGB;
}

static uint32_t vspace_source_get_width(void *data)
{
	const struct vspace_source *context = data;

	return context ? context->width : 0;
}

static uint32_t vspace_source_get_height(void *data)
{
	const struct vspace_source *context = data;

	return context ? context->height : 0;
}

struct obs_source_info vspace_source_info = {
	.id = "vspace_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_INTERACTION,
	.get_name = vspace_source_get_name,
	.create = vspace_source_create,
	.destroy = vspace_source_destroy,
	.update = vspace_source_update,
	.get_defaults = vspace_source_defaults,
	.show = vspace_source_show,
	.hide = vspace_source_hide,
	.activate = vspace_source_activate,
	.deactivate = vspace_source_deactivate,
	.get_properties = vspace_source_properties,
	.mouse_click = vspace_source_mouse_click,
	.mouse_move = vspace_source_mouse_move,
	.mouse_wheel = vspace_source_mouse_wheel,
	.focus = vspace_source_focus,
	.key_click = vspace_source_key_click,
	.video_tick = vspace_source_video_tick,
	.video_render = vspace_source_render,
	.get_width = vspace_source_get_width,
	.get_height = vspace_source_get_height,
	.icon_type = OBS_ICON_TYPE_CUSTOM,
	.video_get_color_space = vspace_source_get_color_space,
};
