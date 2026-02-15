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
    Modified: 2026-02-15
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
#include <string.h>

#include "scene-3d-gltf-loader.h"
#include "scene-3d-source.h"

#define S_MODEL_PATH "model_path"
#define S_DRACO_ENABLED "draco_enabled"
#define S_DRACO_DECODER "draco_decoder"
#define S_DRACO_DECODER_AUTO "auto"
#define S_DRACO_DECODER_BUILTIN "builtin"
#define S_DRACO_DECODER_EXTERNAL "external"

#define SCENE_3D_CAMERA_ORBIT_DEG_PER_PIXEL 0.20f
#define SCENE_3D_CAMERA_ZOOM_STEP 0.90f
#define SCENE_3D_CAMERA_DOLLY_STEPS_PER_PIXEL 0.020f

struct scene_3d_source {
	obs_source_t *source;
	gs_effect_t *effect;
	gs_eparam_t *effect_base_color_param;
	gs_eparam_t *effect_camera_position_param;
	gs_eparam_t *effect_light_direction_param;
	gs_eparam_t *effect_ambient_strength_param;
	gs_eparam_t *effect_diffuse_strength_param;
	gs_eparam_t *effect_specular_strength_param;
	gs_eparam_t *effect_shininess_param;
	char *model_path;
	char *draco_decoder;
	gs_vertbuffer_t *vertex_buffer;
	gs_indexbuffer_t *index_buffer;
	gs_vertbuffer_t *bounds_line_buffer;
	gs_texrender_t *model_texrender;
	gs_image_file4_t base_color_image;
	size_t draw_vertex_count;
	size_t draw_index_count;
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
		struct scene_3d_cpu_payload payload;
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
	bool camera_manual_override;
	bool camera_drag_orbit;
	bool camera_drag_pan;
	bool camera_drag_zoom;
	int32_t camera_last_input_x;
	int32_t camera_last_input_y;
	bool camera_last_input_valid;
};

static const char *scene_3d_source_log_name(const struct scene_3d_source *context)
{
	if (context && context->source)
		return obs_source_get_name(context->source);

	return "scene_3d_source";
}

static inline void scene_3d_source_lock_camera(struct scene_3d_source *context)
{
	if (context && context->camera_mutex_valid)
		pthread_mutex_lock(&context->camera_mutex);
}

static inline void scene_3d_source_unlock_camera(struct scene_3d_source *context)
{
	if (context && context->camera_mutex_valid)
		pthread_mutex_unlock(&context->camera_mutex);
}

static float scene_3d_source_model_extent_locked(const struct scene_3d_source *context)
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

static void scene_3d_source_update_camera_clip_locked(struct scene_3d_source *context)
{
	float extent_max;
	float half_depth = 1.0f;
	float near_plane;
	float far_plane;

	if (!context)
		return;

	extent_max = scene_3d_source_model_extent_locked(context);
	if (context->model_bounds_valid)
		half_depth = fabsf(context->model_bounds_max.z - context->model_bounds_min.z) * 0.5f;

	near_plane = context->camera_orbit_distance - (half_depth + extent_max * 0.75f);
	if (near_plane < 0.01f)
		near_plane = 0.01f;

	far_plane = context->camera_orbit_distance + (half_depth + extent_max * 2.0f);
	if (far_plane < near_plane + 1.0f)
		far_plane = near_plane + 1.0f;

	context->default_camera_znear = near_plane;
	context->default_camera_zfar = far_plane;
}

static void scene_3d_source_rotate_vec3_axis(struct vec3 *vector, const struct vec3 *axis, float angle_rad)
{
	struct vec3 axis_normalized;
	struct matrix4 rotation;

	if (!vector || !axis)
		return;

	axis_normalized = *axis;
	if (vec3_len(&axis_normalized) <= 0.0001f)
		return;
	vec3_norm(&axis_normalized, &axis_normalized);

	matrix4_identity(&rotation);
	matrix4_rotate_aa4f(&rotation, &rotation, axis_normalized.x, axis_normalized.y, axis_normalized.z, angle_rad);
	vec3_transform(vector, vector, &rotation);
}

static void scene_3d_source_get_camera_basis_locked(const struct scene_3d_source *context, struct vec3 *forward,
						    struct vec3 *right, struct vec3 *up)
{
	struct vec3 world_up;
	struct vec3 basis_forward;
	struct vec3 basis_up;
	struct vec3 basis_right;

	if (!context || !forward || !right || !up)
		return;

	vec3_sub(&basis_forward, &context->camera_target, &context->default_camera_position);
	if (vec3_len(&basis_forward) <= 0.0001f)
		vec3_set(&basis_forward, 0.0f, 0.0f, -1.0f);
	else
		vec3_norm(&basis_forward, &basis_forward);

	/*
	 * Keep the default camera aligned with scene-3d world axes:
	 *   world right = +X, world up = +Y.
	 * Manual interaction can still override camera up to support free orbit.
	 */
	if (context->camera_manual_override) {
		basis_up = context->camera_up;
	} else {
		vec3_set(&basis_up, 0.0f, 1.0f, 0.0f);
	}

	if (vec3_len(&basis_up) <= 0.0001f)
		vec3_set(&basis_up, 0.0f, 1.0f, 0.0f);
	else
		vec3_norm(&basis_up, &basis_up);

	vec3_cross(&basis_right, &basis_forward, &basis_up);
	if (vec3_len(&basis_right) <= 0.0001f) {
		if (context->camera_manual_override) {
			vec3_set(&world_up, 0.0f, 1.0f, 0.0f);
			vec3_cross(&basis_right, &basis_forward, &world_up);
			if (vec3_len(&basis_right) <= 0.0001f) {
				vec3_set(&world_up, 0.0f, 0.0f, 1.0f);
				vec3_cross(&basis_right, &basis_forward, &world_up);
			}
		} else {
			vec3_set(&basis_right, 1.0f, 0.0f, 0.0f);
		}
	}
	if (vec3_len(&basis_right) <= 0.0001f)
		vec3_set(&basis_right, 1.0f, 0.0f, 0.0f);
	else
		vec3_norm(&basis_right, &basis_right);

	vec3_cross(&basis_up, &basis_right, &basis_forward);
	if (vec3_len(&basis_up) <= 0.0001f)
		vec3_set(&basis_up, 0.0f, 1.0f, 0.0f);
	else
		vec3_norm(&basis_up, &basis_up);

	*forward = basis_forward;
	*right = basis_right;
	*up = basis_up;
}

static void scene_3d_source_orthonormalize_camera_locked(struct scene_3d_source *context)
{
	struct vec3 forward;
	struct vec3 right;
	struct vec3 up;

	if (!context)
		return;

	scene_3d_source_get_camera_basis_locked(context, &forward, &right, &up);
	context->camera_up = up;
}

static void scene_3d_source_recompute_camera_position_locked(struct scene_3d_source *context)
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
			vec3_set(&orbit_offset, 0.0f, 0.0f, context->camera_orbit_distance);
		}
	} else {
		vec3_set(&orbit_offset, 0.0f, 0.0f, context->camera_orbit_distance);
	}

	vec3_add(&context->default_camera_position, &context->camera_target, &orbit_offset);
	if (vec3_len(&context->camera_up) <= 0.0001f)
		vec3_set(&context->camera_up, 0.0f, 1.0f, 0.0f);
	scene_3d_source_orthonormalize_camera_locked(context);
	context->default_camera_valid = true;
}

static void scene_3d_source_apply_camera_view_matrix(const struct vec3 *camera_position, const struct vec3 *camera_target,
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
		vec3_set(&forward, 0.0f, 0.0f, -1.0f);
	else
		vec3_mulf(&forward, &forward, 1.0f / forward_len);

	up = *camera_up_hint;
	if (vec3_len(&up) <= 0.0001f)
		vec3_set(&up, 0.0f, 1.0f, 0.0f);
	else
		vec3_norm(&up, &up);

	vec3_cross(&right, &forward, &up);
	right_len = vec3_len(&right);
	if (right_len <= 0.0001f) {
		vec3_set(&up, 0.0f, 1.0f, 0.0f);
		vec3_cross(&right, &forward, &up);
		if (vec3_len(&right) <= 0.0001f) {
			vec3_set(&up, 0.0f, 0.0f, 1.0f);
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
		vec3_set(&up, 0.0f, 1.0f, 0.0f);
	else
		vec3_mulf(&up, &up, 1.0f / up_len);

	matrix4_identity(&view);
	vec4_set(&view.x, right.x, right.y, right.z, 0.0f);
	vec4_set(&view.y, up.x, up.y, up.z, 0.0f);
	vec4_set(&view.z, -forward.x, -forward.y, -forward.z, 0.0f);
	vec4_set(&view.t, -vec3_dot(&right, camera_position), -vec3_dot(&up, camera_position),
		 vec3_dot(&forward, camera_position), 1.0f);
	gs_matrix_set(&view);
}

static void scene_3d_source_apply_camera_projection_matrix(float fov_deg, float aspect, float znear, float zfar)
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
	 * OBS' default gs_perspective() uses (top=ymin, bottom=ymax), which
	 * results in a vertically flipped image for this world-space camera path.
	 * Swap top/bottom here so +Y in world appears upward on screen.
	 */
	gs_frustum(xmin, xmax, ymax, ymin, znear, zfar);
}

static bool scene_3d_source_should_auto_fit_camera(struct scene_3d_source *context)
{
	bool model_bounds_valid = false;
	bool manual_override = false;

	if (!context)
		return false;

	scene_3d_source_lock_camera(context);
	model_bounds_valid = context->model_bounds_valid;
	manual_override = context->camera_manual_override;
	scene_3d_source_unlock_camera(context);

	return model_bounds_valid && !manual_override;
}

static void scene_3d_source_begin_camera_drag(struct scene_3d_source *context, bool orbit, bool pan, bool zoom, int32_t x,
					      int32_t y)
{
	if (!context)
		return;

	scene_3d_source_lock_camera(context);
	context->camera_drag_orbit = orbit;
	context->camera_drag_pan = pan;
	context->camera_drag_zoom = zoom;
	context->camera_last_input_x = x;
	context->camera_last_input_y = y;
	context->camera_last_input_valid = true;
	context->camera_manual_override = true;
	scene_3d_source_unlock_camera(context);
}

static void scene_3d_source_end_camera_drag(struct scene_3d_source *context, bool orbit, bool pan, bool zoom)
{
	if (!context)
		return;

	scene_3d_source_lock_camera(context);
	if (orbit)
		context->camera_drag_orbit = false;
	if (pan)
		context->camera_drag_pan = false;
	if (zoom)
		context->camera_drag_zoom = false;
	if (!context->camera_drag_orbit && !context->camera_drag_pan && !context->camera_drag_zoom)
		context->camera_last_input_valid = false;
	scene_3d_source_unlock_camera(context);
}

static void scene_3d_source_orbit_camera(struct scene_3d_source *context, int32_t delta_x, int32_t delta_y)
{
	struct vec3 orbit_offset;
	struct vec3 forward;
	struct vec3 right;
	struct vec3 up;
	float orbit_len;
	float yaw_rad;
	float pitch_rad;

	if (!context)
		return;

	scene_3d_source_lock_camera(context);
	if (!context->model_bounds_valid) {
		scene_3d_source_unlock_camera(context);
		return;
	}

	if (!context->default_camera_valid)
		scene_3d_source_recompute_camera_position_locked(context);

	scene_3d_source_get_camera_basis_locked(context, &forward, &right, &up);
	vec3_sub(&orbit_offset, &context->default_camera_position, &context->camera_target);
	orbit_len = vec3_len(&orbit_offset);
	if (orbit_len <= 0.0001f) {
		vec3_set(&orbit_offset, 0.0f, 0.0f, context->camera_orbit_distance);
		orbit_len = context->camera_orbit_distance;
	}

	yaw_rad = -(float)delta_x * RAD(SCENE_3D_CAMERA_ORBIT_DEG_PER_PIXEL);
	pitch_rad = -(float)delta_y * RAD(SCENE_3D_CAMERA_ORBIT_DEG_PER_PIXEL);

	if (yaw_rad != 0.0f)
		scene_3d_source_rotate_vec3_axis(&orbit_offset, &up, yaw_rad);

	if (pitch_rad != 0.0f) {
		vec3_mulf(&forward, &orbit_offset, -1.0f);
		if (vec3_len(&forward) <= 0.0001f)
			vec3_set(&forward, 0.0f, 0.0f, -1.0f);
		else
			vec3_norm(&forward, &forward);

		vec3_cross(&right, &forward, &up);
		if (vec3_len(&right) <= 0.0001f)
			vec3_set(&right, 1.0f, 0.0f, 0.0f);
		else
			vec3_norm(&right, &right);

		scene_3d_source_rotate_vec3_axis(&orbit_offset, &right, pitch_rad);
		scene_3d_source_rotate_vec3_axis(&up, &right, pitch_rad);
	}

	if (vec3_len(&up) <= 0.0001f)
		vec3_set(&up, 0.0f, 1.0f, 0.0f);
	else
		vec3_norm(&up, &up);

	orbit_len = vec3_len(&orbit_offset);
	if (orbit_len <= 0.0001f) {
		vec3_set(&orbit_offset, 0.0f, 0.0f, context->camera_orbit_distance);
	} else {
		if (context->camera_orbit_distance < 0.05f)
			context->camera_orbit_distance = 0.05f;
		vec3_mulf(&orbit_offset, &orbit_offset, context->camera_orbit_distance / orbit_len);
	}

	vec3_add(&context->default_camera_position, &context->camera_target, &orbit_offset);
	context->camera_up = up;
	context->camera_manual_override = true;
	scene_3d_source_orthonormalize_camera_locked(context);
	scene_3d_source_update_camera_clip_locked(context);
	scene_3d_source_unlock_camera(context);
}

static void scene_3d_source_pan_camera(struct scene_3d_source *context, int32_t delta_x, int32_t delta_y)
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

	scene_3d_source_lock_camera(context);
	if (!context->model_bounds_valid) {
		scene_3d_source_unlock_camera(context);
		return;
	}
	if (!context->default_camera_valid)
		scene_3d_source_recompute_camera_position_locked(context);

	aspect = (float)context->width / (float)(context->height ? context->height : 1);
	view_height = 2.0f * tanf(RAD(context->default_camera_fov_deg * 0.5f)) * context->camera_orbit_distance;
	view_width = view_height * fmaxf(aspect, 0.1f);
	pan_right = -((float)delta_x / (float)(context->width ? context->width : 1)) * view_width;
	pan_up = ((float)delta_y / (float)(context->height ? context->height : 1)) * view_height;

	scene_3d_source_get_camera_basis_locked(context, &forward, &right, &up);

	vec3_mulf(&right, &right, pan_right);
	vec3_mulf(&up, &up, pan_up);
	vec3_add(&delta_pan, &right, &up);
	vec3_add(&context->camera_target, &context->camera_target, &delta_pan);
	vec3_add(&context->default_camera_position, &context->default_camera_position, &delta_pan);

	context->camera_manual_override = true;
	scene_3d_source_orthonormalize_camera_locked(context);
	scene_3d_source_update_camera_clip_locked(context);
	scene_3d_source_unlock_camera(context);
}

static void scene_3d_source_apply_zoom_steps_locked(struct scene_3d_source *context, float zoom_steps)
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

	zoom_factor = powf(SCENE_3D_CAMERA_ZOOM_STEP, zoom_steps);
	context->camera_orbit_distance *= zoom_factor;

	extent_max = scene_3d_source_model_extent_locked(context);
	min_distance = fmaxf(0.05f, extent_max * 0.05f);
	max_distance = fmaxf(min_distance * 4.0f, extent_max * 50.0f);

	if (context->camera_orbit_distance < min_distance)
		context->camera_orbit_distance = min_distance;
	if (context->camera_orbit_distance > max_distance)
		context->camera_orbit_distance = max_distance;

	if (!context->default_camera_valid)
		scene_3d_source_recompute_camera_position_locked(context);

	vec3_sub(&orbit_offset, &context->default_camera_position, &context->camera_target);
	orbit_len = vec3_len(&orbit_offset);
	if (orbit_len <= 0.0001f) {
		vec3_set(&orbit_offset, 0.0f, 0.0f, context->camera_orbit_distance);
	} else {
		vec3_mulf(&orbit_offset, &orbit_offset, context->camera_orbit_distance / orbit_len);
	}
	vec3_add(&context->default_camera_position, &context->camera_target, &orbit_offset);

	context->camera_manual_override = true;
	scene_3d_source_orthonormalize_camera_locked(context);
	scene_3d_source_update_camera_clip_locked(context);
}

static void scene_3d_source_zoom_camera(struct scene_3d_source *context, int32_t wheel_delta_y)
{
	float zoom_steps;

	if (!context || wheel_delta_y == 0)
		return;

	zoom_steps = (float)wheel_delta_y / 120.0f;

	scene_3d_source_lock_camera(context);
	scene_3d_source_apply_zoom_steps_locked(context, zoom_steps);
	scene_3d_source_unlock_camera(context);
}

static void scene_3d_source_dolly_camera(struct scene_3d_source *context, int32_t delta_y)
{
	float zoom_steps;

	if (!context || delta_y == 0)
		return;

	/*
	 * Blender-style Ctrl+MMB dolly:
	 * dragging up zooms in, dragging down zooms out.
	 */
	zoom_steps = -(float)delta_y * SCENE_3D_CAMERA_DOLLY_STEPS_PER_PIXEL;

	scene_3d_source_lock_camera(context);
	scene_3d_source_apply_zoom_steps_locked(context, zoom_steps);
	scene_3d_source_unlock_camera(context);
}

static void scene_3d_source_device_loss_release(void *data)
{
	struct scene_3d_source *context = data;

	if (!context)
		return;

	os_atomic_store_bool(&context->device_loss_active, true);
	os_atomic_store_bool(&context->device_rebuild_pending, false);
	blog(LOG_WARNING, "[scene-3d-source: '%s'] Graphics device loss detected.", scene_3d_source_log_name(context));
}

static void scene_3d_source_device_loss_rebuild(void *device, void *data)
{
	struct scene_3d_source *context = data;

	UNUSED_PARAMETER(device);

	if (!context)
		return;

	os_atomic_store_bool(&context->device_loss_active, false);
	os_atomic_store_bool(&context->device_rebuild_pending, true);
	blog(LOG_INFO, "[scene-3d-source: '%s'] Graphics device rebuilt. Scheduling resource refresh.",
	     scene_3d_source_log_name(context));
}

static void scene_3d_source_register_device_loss_callbacks(struct scene_3d_source *context)
{
	if (!context || context->device_loss_callbacks_registered)
		return;

	context->device_loss_callbacks.device_loss_release = scene_3d_source_device_loss_release;
	context->device_loss_callbacks.device_loss_rebuild = scene_3d_source_device_loss_rebuild;
	context->device_loss_callbacks.data = context;

	obs_enter_graphics();
	gs_register_loss_callbacks(&context->device_loss_callbacks);
	obs_leave_graphics();

	context->device_loss_callbacks_registered = true;
}

static void scene_3d_source_unregister_device_loss_callbacks(struct scene_3d_source *context)
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

static void scene_3d_source_refresh_size(struct scene_3d_source *context)
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

static void scene_3d_source_reset_default_camera(struct scene_3d_source *context)
{
	if (!context)
		return;

	scene_3d_source_lock_camera(context);
	vec3_zero(&context->model_bounds_min);
	vec3_zero(&context->model_bounds_max);
	vec3_zero(&context->camera_target);
	vec3_zero(&context->default_camera_position);
	vec3_set(&context->camera_up, 0.0f, 1.0f, 0.0f);
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
	context->camera_last_input_valid = false;
	scene_3d_source_unlock_camera(context);
}

static void scene_3d_source_reset_default_light(struct scene_3d_source *context)
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

static bool scene_3d_source_compute_primitive_bounds(const struct scene_3d_cpu_primitive_payload *primitive,
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

static void scene_3d_source_update_default_camera(struct scene_3d_source *context)
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
		scene_3d_source_refresh_size(context);

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

	fit_dist_y = half_extent.y / tan_half_fov;
	fit_dist_x = half_extent.x / (tan_half_fov * fmaxf(aspect, 0.1f));
	fit_distance = fmaxf(fit_dist_x, fit_dist_y);

	half_extent.z = fmaxf(half_extent.z, 0.01f);
	camera_distance = fit_distance + half_extent.z + (extent_max * 0.35f) + 0.5f;
	if (camera_distance < 0.5f)
		camera_distance = 0.5f;

	scene_3d_source_lock_camera(context);
	context->camera_target = center;
	context->camera_orbit_distance = camera_distance;
	context->default_camera_fov_deg = 50.0f;
	context->default_camera_valid = false;
	vec3_set(&context->camera_up, 0.0f, 1.0f, 0.0f);
	context->camera_manual_override = false;
	context->camera_drag_orbit = false;
	context->camera_drag_pan = false;
	context->camera_drag_zoom = false;
	context->camera_last_input_valid = false;
	scene_3d_source_recompute_camera_position_locked(context);
	scene_3d_source_update_camera_clip_locked(context);
	scene_3d_source_unlock_camera(context);
}

static void scene_3d_source_log_camera_fit(const struct scene_3d_source *context)
{
	struct scene_3d_source *mutable_context = (struct scene_3d_source *)context;
	struct vec3 camera_position;
	float camera_znear;
	float camera_zfar;
	bool camera_valid;
	float view_min_z;
	float view_max_z;

	if (!context || !context->model_bounds_valid)
		return;

	scene_3d_source_lock_camera(mutable_context);
	camera_valid = context->default_camera_valid;
	camera_position = context->default_camera_position;
	camera_znear = context->default_camera_znear;
	camera_zfar = context->default_camera_zfar;
	scene_3d_source_unlock_camera(mutable_context);

	if (!camera_valid)
		return;

	view_min_z = context->model_bounds_min.z - camera_position.z;
	view_max_z = context->model_bounds_max.z - camera_position.z;

	blog(LOG_INFO,
	     "[scene-3d-source: '%s'] Camera fit: bounds_min=(%.3f, %.3f, %.3f), bounds_max=(%.3f, %.3f, %.3f), "
	     "camera=(%.3f, %.3f, %.3f), clip=[%.3f, %.3f], view_z=[%.3f, %.3f]",
	     scene_3d_source_log_name(context), context->model_bounds_min.x, context->model_bounds_min.y,
	     context->model_bounds_min.z, context->model_bounds_max.x, context->model_bounds_max.y,
	     context->model_bounds_max.z, camera_position.x, camera_position.y, camera_position.z, camera_znear,
	     camera_zfar, view_min_z, view_max_z);
}

static void scene_3d_source_unload_effect(struct scene_3d_source *context)
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
}

static void scene_3d_source_load_effect(struct scene_3d_source *context)
{
	char *effect_path;

	if (!context)
		return;

	scene_3d_source_unload_effect(context);

	effect_path = obs_module_file("effects/scene-3d.effect");
	context->effect_load_attempted = true;
	if (!effect_path) {
		blog(LOG_WARNING, "[scene-3d-source: '%s'] Could not resolve effects/scene-3d.effect.",
		     scene_3d_source_log_name(context));
		return;
	}

	obs_enter_graphics();
	context->effect = gs_effect_create_from_file(effect_path, NULL);
	if (context->effect) {
		gs_technique_t *textured_tech = gs_effect_get_technique(context->effect, "DrawBlinnPhongTextured");
		gs_technique_t *solid_tech = gs_effect_get_technique(context->effect, "DrawBlinnPhongSolid");

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

		blog(LOG_INFO,
		     "[scene-3d-source: '%s'] Effect loaded: base_color_param=%s, camera_param=%s, "
		     "light_dir_param=%s, tech_textured=%s, tech_solid=%s",
		     scene_3d_source_log_name(context), context->effect_base_color_param ? "yes" : "no",
		     context->effect_camera_position_param ? "yes" : "no",
		     context->effect_light_direction_param ? "yes" : "no", textured_tech ? "yes" : "no",
		     solid_tech ? "yes" : "no");
	}
	obs_leave_graphics();

	if (!context->effect) {
		blog(LOG_WARNING, "[scene-3d-source: '%s'] Failed to load effect file: %s",
		     scene_3d_source_log_name(context), effect_path);
	}

	bfree(effect_path);
}

static bool scene_3d_source_is_supported_model_path(const char *path)
{
	const char *extension = strrchr(path, '.');

	if (!extension)
		return false;

	return astrcmpi(extension, ".glb") == 0 || astrcmpi(extension, ".gltf") == 0;
}

static void scene_3d_source_validate_model_path(struct scene_3d_source *context)
{
	if (!context || !context->model_path || !*context->model_path)
		return;

	if (!os_file_exists(context->model_path)) {
		blog(LOG_WARNING, "[scene-3d-source: '%s'] Model path does not exist: %s",
		     scene_3d_source_log_name(context), context->model_path);
		return;
	}

	if (!scene_3d_source_is_supported_model_path(context->model_path)) {
		blog(LOG_WARNING,
		     "[scene-3d-source: '%s'] Unsupported model format. Only .glb or .gltf is supported: %s",
		     scene_3d_source_log_name(context), context->model_path);
	}
}

static bool scene_3d_source_model_path_is_loadable(const struct scene_3d_source *context)
{
	if (!context || !context->model_path || !*context->model_path)
		return false;

	if (!os_file_exists(context->model_path))
		return false;

	return scene_3d_source_is_supported_model_path(context->model_path);
}

static bool scene_3d_source_model_path_uses_draco(const char *model_path)
{
	if (!model_path || !*model_path)
		return false;

	if (!os_file_exists(model_path))
		return false;

	if (!scene_3d_source_is_supported_model_path(model_path))
		return false;

	return scene_3d_gltf_model_uses_draco(model_path);
}

static void scene_3d_source_set_draco_property_state(obs_properties_t *props, bool enabled)
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

static bool scene_3d_source_model_path_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	const char *model_path = settings ? obs_data_get_string(settings, S_MODEL_PATH) : NULL;
	const bool model_uses_draco = scene_3d_source_model_path_uses_draco(model_path);

	scene_3d_source_set_draco_property_state(props, model_uses_draco);

	if (!model_uses_draco && settings) {
		obs_data_set_bool(settings, S_DRACO_ENABLED, false);
		obs_data_set_string(settings, S_DRACO_DECODER, S_DRACO_DECODER_AUTO);
	}

	UNUSED_PARAMETER(property);
	return true;
}

static void scene_3d_source_free_worker_job(struct scene_3d_source *context)
{
	if (!context)
		return;

	bfree(context->worker_job.model_path);
	context->worker_job.model_path = NULL;
	bfree(context->worker_job.draco_decoder);
	context->worker_job.draco_decoder = NULL;
	context->worker_job.has_job = false;
}

static void scene_3d_source_release_pending_upload(struct scene_3d_source *context)
{
	if (!context)
		return;

	scene_3d_gltf_free_cpu_payload(&context->pending_upload.payload);

	if (context->pending_upload.base_color_image_valid)
		gs_image_file4_free(&context->pending_upload.base_color_image);

	memset(&context->pending_upload, 0, sizeof(context->pending_upload));
}

/* Must be called inside an active graphics context. */
static void scene_3d_source_release_gpu_resources(struct scene_3d_source *context)
{
	if (!context)
		return;

	if (context->vertex_buffer) {
		gs_vertexbuffer_destroy(context->vertex_buffer);
		context->vertex_buffer = NULL;
	}

	if (context->index_buffer) {
		gs_indexbuffer_destroy(context->index_buffer);
		context->index_buffer = NULL;
	}

	if (context->bounds_line_buffer) {
		gs_vertexbuffer_destroy(context->bounds_line_buffer);
		context->bounds_line_buffer = NULL;
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
	context->diagnostics_logged_draw = false;
}

/* Must be called in an active graphics context. */
static bool scene_3d_source_ensure_bounds_line_buffer(struct scene_3d_source *context)
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
		blog(LOG_WARNING, "[scene-3d-source: '%s'] Failed to create bounds line vertex buffer.",
		     scene_3d_source_log_name(context));
		return false;
	}

	return true;
}

/* Must be called in an active graphics context. */
static void scene_3d_source_draw_bounds(struct scene_3d_source *context)
{
	gs_effect_t *solid_effect;
	gs_eparam_t *solid_color_param;
	struct vec3 bounds_scale;
	struct vec4 bounds_color;

	if (!context || !context->model_bounds_valid)
		return;

	if (!scene_3d_source_ensure_bounds_line_buffer(context))
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

/* Must be called in an active graphics context. */
static bool scene_3d_source_ensure_model_texrender(struct scene_3d_source *context)
{
	if (!context)
		return false;

	if (context->model_texrender)
		return true;

	context->model_texrender = gs_texrender_create(GS_RGBA, GS_Z24_S8);
	if (!context->model_texrender) {
		blog(LOG_WARNING, "[scene-3d-source: '%s'] Failed to create model texrender target.",
		     scene_3d_source_log_name(context));
		return false;
	}

	return true;
}

/* Must be called from source render (active graphics context). */
static gs_texture_t *scene_3d_source_render_model_to_texture(struct scene_3d_source *context)
{
	struct vec4 clear_color;
	gs_texture_t *base_color_texture = NULL;
	struct vec3 camera_position;
	struct vec3 camera_target;
	struct vec3 camera_up;
	float aspect;
	float camera_fov_deg;
	float camera_znear;
	float camera_zfar;
	bool camera_valid = false;
	const bool has_base_color_texture =
		context->base_color_image_valid && context->base_color_image.image3.image2.image.texture != NULL;
	const char *technique = has_base_color_texture ? "DrawBlinnPhongTextured" : "DrawBlinnPhongSolid";
	enum gs_cull_mode previous_cull_mode;

	if (!context || !context->effect || !context->vertex_buffer)
		return NULL;

	scene_3d_source_lock_camera(context);
	camera_valid = context->default_camera_valid;
	camera_position = context->default_camera_position;
	camera_target = context->camera_target;
	camera_up = context->camera_up;
	camera_fov_deg = context->default_camera_fov_deg;
	camera_znear = context->default_camera_znear;
	camera_zfar = context->default_camera_zfar;
	scene_3d_source_unlock_camera(context);

	if (!camera_valid)
		return NULL;

	if (!scene_3d_source_ensure_model_texrender(context))
		return NULL;

	gs_texrender_reset(context->model_texrender);
	if (!gs_texrender_begin_with_color_space(context->model_texrender, context->width, context->height, GS_CS_SRGB))
		return NULL;

	if (!context->diagnostics_logged_draw) {
		blog(LOG_INFO, "[scene-3d-source: '%s'] Render path active: technique=%s, indices=%zu, vertices=%zu",
		     scene_3d_source_log_name(context), technique, context->draw_index_count, context->draw_vertex_count);
		context->diagnostics_logged_draw = true;
	}

	if (context->base_color_image_valid)
		base_color_texture = context->base_color_image.image3.image2.image.texture;

	aspect = (float)context->width / (float)(context->height ? context->height : 1);
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &clear_color, 1.0f, 0);

	gs_enable_framebuffer_srgb(true);
	gs_enable_depth_test(true);
	gs_depth_function(GS_LESS);

	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_set_viewport(0, 0, (int)context->width, (int)context->height);
	scene_3d_source_apply_camera_projection_matrix(camera_fov_deg, aspect, camera_znear, camera_zfar);
	gs_matrix_identity();
	scene_3d_source_apply_camera_view_matrix(&camera_position, &camera_target, &camera_up);

	if (context->effect_base_color_param) {
		if (has_base_color_texture)
			gs_effect_set_texture_srgb(context->effect_base_color_param, base_color_texture);
		else
			gs_effect_set_texture(context->effect_base_color_param, NULL);
	}
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
	gs_set_cull_mode(GS_NEITHER);

	gs_load_vertexbuffer(context->vertex_buffer);
	gs_load_indexbuffer(context->index_buffer);
	while (gs_effect_loop(context->effect, technique))
		gs_draw(GS_TRIS, 0, context->index_buffer ? 0 : (uint32_t)context->draw_vertex_count);
	gs_load_indexbuffer(NULL);
	gs_load_vertexbuffer(NULL);
	gs_set_cull_mode(previous_cull_mode);

	if (context->model_bounds_valid)
		scene_3d_source_draw_bounds(context);

	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();

	gs_enable_depth_test(false);
	gs_texrender_end(context->model_texrender);
	return gs_texrender_get_texture(context->model_texrender);
}

static const char *scene_3d_source_find_first_texture_path(const struct scene_3d_cpu_payload *payload)
{
	size_t mesh_idx;

	if (!payload)
		return NULL;

	for (mesh_idx = 0; mesh_idx < payload->mesh_count; mesh_idx++) {
		const struct scene_3d_cpu_mesh_payload *mesh = payload->meshes + mesh_idx;
		size_t primitive_idx;

		for (primitive_idx = 0; primitive_idx < mesh->primitive_count; primitive_idx++) {
			const struct scene_3d_cpu_primitive_payload *primitive = mesh->primitives + primitive_idx;

			if (primitive->base_color_texture && *primitive->base_color_texture)
				return primitive->base_color_texture;
		}
	}

	return NULL;
}

static bool scene_3d_source_decode_base_color_image(struct scene_3d_source *context, const struct scene_3d_cpu_payload *payload,
						    gs_image_file4_t *decoded_image)
{
	const char *image_path;

	if (!context || !payload || !decoded_image)
		return false;

	memset(decoded_image, 0, sizeof(*decoded_image));

	image_path = scene_3d_source_find_first_texture_path(payload);
	if (!image_path || !*image_path)
		return false;

	if (!os_file_exists(image_path)) {
		blog(LOG_WARNING, "[scene-3d-source: '%s'] BaseColor texture path does not exist: %s",
		     scene_3d_source_log_name(context), image_path);
		return false;
	}

	gs_image_file4_init(decoded_image, image_path, GS_IMAGE_ALPHA_PREMULTIPLY);
	if (!decoded_image->image3.image2.image.loaded) {
		blog(LOG_WARNING, "[scene-3d-source: '%s'] Failed to decode BaseColor texture: %s",
		     scene_3d_source_log_name(context), image_path);
		gs_image_file4_free(decoded_image);
		memset(decoded_image, 0, sizeof(*decoded_image));
		return false;
	}

	return true;
}

static void scene_3d_source_log_payload_summary(const struct scene_3d_source *context, const struct scene_3d_cpu_payload *payload)
{
	size_t mesh_idx;
	size_t primitive_count = 0;
	size_t draco_extension_count = 0;

	if (!context || !payload)
		return;

	for (mesh_idx = 0; mesh_idx < payload->mesh_count; mesh_idx++) {
		const struct scene_3d_cpu_mesh_payload *mesh = payload->meshes + mesh_idx;
		size_t primitive_idx;

		primitive_count += mesh->primitive_count;

		for (primitive_idx = 0; primitive_idx < mesh->primitive_count; primitive_idx++) {
			const struct scene_3d_cpu_primitive_payload *primitive = mesh->primitives + primitive_idx;

			if (primitive->used_draco_extension)
				draco_extension_count++;
		}
	}

	blog(LOG_INFO,
	     "[scene-3d-source: '%s'] glTF payload ready: meshes=%zu, primitives=%zu, draco_extension_primitives=%zu",
	     scene_3d_source_log_name(context), payload->mesh_count, primitive_count, draco_extension_count);
}

static bool scene_3d_source_publish_pending_upload(struct scene_3d_source *context, struct scene_3d_cpu_payload *payload,
						   gs_image_file4_t *decoded_image, bool decoded_image_valid, uint64_t token)
{
	bool canceled;

	if (!context || !payload)
		return false;

	if (context->worker_mutex_valid)
		pthread_mutex_lock(&context->worker_mutex);
	canceled = os_atomic_load_bool(&context->worker_stop) || token != context->worker_cancel_token;

	if (!canceled) {
		scene_3d_source_release_pending_upload(context);
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
		     "[scene-3d-source: '%s'] Dropping decoded payload token=%llu (active_token=%llu, stop=%d).",
		     scene_3d_source_log_name(context), (unsigned long long)token,
		     (unsigned long long)context->worker_cancel_token, os_atomic_load_bool(&context->worker_stop));
	}

	if (context->worker_mutex_valid)
		pthread_mutex_unlock(&context->worker_mutex);
	return !canceled;
}

static bool scene_3d_source_take_pending_upload(struct scene_3d_source *context, struct scene_3d_cpu_payload *payload,
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

static void scene_3d_source_upload_pending_payload(struct scene_3d_source *context, struct scene_3d_cpu_payload *payload,
						   gs_image_file4_t *decoded_image, bool decoded_image_valid)
{
	struct gs_vb_data *vb_data = NULL;
	gs_vertbuffer_t *new_vertex_buffer = NULL;
	gs_indexbuffer_t *new_index_buffer = NULL;
	struct vec3 bounds_min = {0};
	struct vec3 bounds_max = {0};
	size_t mesh_idx;
	size_t primitive_idx;
	size_t total_vertices = 0;
	size_t total_indices = 0;
	size_t vertex_offset = 0;
	size_t index_offset = 0;
	size_t uploaded_primitives = 0;
	bool bounds_valid = false;
	struct vec3 *points;
	struct vec3 *normals;
	struct vec2 *uvs;
	uint32_t *flat_indices = NULL;

	if (!context || !payload)
		return;

	for (mesh_idx = 0; mesh_idx < payload->mesh_count; mesh_idx++) {
		const struct scene_3d_cpu_mesh_payload *mesh = payload->meshes + mesh_idx;

		for (primitive_idx = 0; primitive_idx < mesh->primitive_count; primitive_idx++) {
			const struct scene_3d_cpu_primitive_payload *primitive = mesh->primitives + primitive_idx;
			size_t primitive_index_count;
			struct vec3 primitive_bounds_min;
			struct vec3 primitive_bounds_max;

			if (!primitive->positions || primitive->vertex_count == 0)
				continue;

			primitive_index_count = primitive->index_count ? primitive->index_count : primitive->vertex_count;
			if (primitive_index_count == 0)
				continue;

			if (total_vertices > SIZE_MAX - primitive->vertex_count ||
			    total_indices > SIZE_MAX - primitive_index_count) {
				blog(LOG_WARNING,
				     "[scene-3d-source: '%s'] Primitive accumulation overflow at mesh[%zu] primitive[%zu].",
				     scene_3d_source_log_name(context), mesh_idx, primitive_idx);
				continue;
			}

			total_vertices += primitive->vertex_count;
			total_indices += primitive_index_count;

			if (scene_3d_source_compute_primitive_bounds(primitive, &primitive_bounds_min, &primitive_bounds_max)) {
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

	if (!total_vertices || !total_indices) {
		scene_3d_source_reset_default_camera(context);
		obs_enter_graphics();
		scene_3d_source_release_gpu_resources(context);
		obs_leave_graphics();
		return;
	}

	if (total_vertices > UINT32_MAX) {
		blog(LOG_WARNING,
		     "[scene-3d-source: '%s'] Too many vertices for 32-bit index buffer: %zu. Upload aborted.",
		     scene_3d_source_log_name(context), total_vertices);
		return;
	}

	vb_data = gs_vbdata_create();
	vb_data->num = total_vertices;
	vb_data->points = bmalloc(sizeof(struct vec3) * total_vertices);
	vb_data->normals = bmalloc(sizeof(struct vec3) * total_vertices);
	vb_data->num_tex = 1;
	vb_data->tvarray = bzalloc(sizeof(struct gs_tvertarray));
	vb_data->tvarray[0].width = 2;
	vb_data->tvarray[0].array = bmalloc(sizeof(struct vec2) * total_vertices);
	flat_indices = bmalloc(sizeof(uint32_t) * total_indices);

	points = vb_data->points;
	normals = vb_data->normals;
	uvs = vb_data->tvarray[0].array;

	for (mesh_idx = 0; mesh_idx < payload->mesh_count; mesh_idx++) {
		const struct scene_3d_cpu_mesh_payload *mesh = payload->meshes + mesh_idx;

		for (primitive_idx = 0; primitive_idx < mesh->primitive_count; primitive_idx++) {
			const struct scene_3d_cpu_primitive_payload *primitive = mesh->primitives + primitive_idx;
			const size_t primitive_vertex_count = primitive->vertex_count;
			const size_t primitive_index_count =
				primitive->index_count ? primitive->index_count : primitive->vertex_count;
			size_t i;

			if (!primitive->positions || primitive_vertex_count == 0 || primitive_index_count == 0)
				continue;

			for (i = 0; i < primitive_vertex_count; i++) {
				const float *src_pos = primitive->positions + (i * 3);
				struct vec3 *dst_pos = &points[vertex_offset + i];
				struct vec3 *dst_normal = &normals[vertex_offset + i];
				struct vec2 *dst_uv = &uvs[vertex_offset + i];

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
					     "[scene-3d-source: '%s'] Invalid local index at mesh[%zu] primitive[%zu]: %u >= %zu. "
					     "Clamping to 0.",
					     scene_3d_source_log_name(context), mesh_idx, primitive_idx, local_index,
					     primitive_vertex_count);
					local_index = 0;
				}

				flat_indices[index_offset + i] = (uint32_t)(vertex_offset + (size_t)local_index);
			}

			vertex_offset += primitive_vertex_count;
			index_offset += primitive_index_count;
			uploaded_primitives++;

			blog(LOG_INFO,
			     "[scene-3d-source: '%s'] GPU primitive upload complete: mesh[%zu] primitive[%zu] "
			     "vertices=%zu indices=%zu",
			     scene_3d_source_log_name(context), mesh_idx, primitive_idx, primitive_vertex_count,
			     primitive_index_count);
		}
	}

	if (uploaded_primitives == 0 || vertex_offset == 0 || index_offset == 0) {
		blog(LOG_WARNING, "[scene-3d-source: '%s'] No valid primitives were uploaded.",
		     scene_3d_source_log_name(context));
		gs_vbdata_destroy(vb_data);
		bfree(flat_indices);
		return;
	}

	obs_enter_graphics();

	new_vertex_buffer = gs_vertexbuffer_create(vb_data, 0);
	if (!new_vertex_buffer)
		gs_vbdata_destroy(vb_data);

	if (new_vertex_buffer) {
		new_index_buffer = gs_indexbuffer_create(GS_UNSIGNED_LONG, flat_indices, index_offset, GS_DUP_BUFFER);
		if (!new_index_buffer) {
			gs_vertexbuffer_destroy(new_vertex_buffer);
			new_vertex_buffer = NULL;
		}
	}

	bfree(flat_indices);
	flat_indices = NULL;

	if (new_vertex_buffer) {
		if (decoded_image_valid)
			gs_image_file4_init_texture(decoded_image);

		scene_3d_source_release_gpu_resources(context);

		context->vertex_buffer = new_vertex_buffer;
		context->index_buffer = new_index_buffer;
		context->draw_vertex_count = vertex_offset;
		context->draw_index_count = index_offset;

		if (decoded_image_valid && decoded_image->image3.image2.image.texture) {
			context->base_color_image = *decoded_image;
			memset(decoded_image, 0, sizeof(*decoded_image));
			context->base_color_image_valid = true;
		}

		if (bounds_valid) {
			scene_3d_source_lock_camera(context);
			context->model_bounds_min = bounds_min;
			context->model_bounds_max = bounds_max;
			context->model_bounds_valid = true;
			scene_3d_source_unlock_camera(context);
			scene_3d_source_update_default_camera(context);
			scene_3d_source_log_camera_fit(context);
		} else {
			scene_3d_source_reset_default_camera(context);
		}

		if (!context->diagnostics_logged_upload) {
			blog(LOG_INFO,
			     "[scene-3d-source: '%s'] GPU upload complete: primitives=%zu, vertices=%zu, indices=%zu, "
			     "texture=%s",
			     scene_3d_source_log_name(context), uploaded_primitives, context->draw_vertex_count,
			     context->draw_index_count, context->base_color_image_valid ? "yes" : "no");
			context->diagnostics_logged_upload = true;
		}
	} else {
		blog(LOG_WARNING, "[scene-3d-source: '%s'] GPU upload failed: vertex/index buffer creation failed.",
		     scene_3d_source_log_name(context));
	}

	obs_leave_graphics();
}

static bool scene_3d_source_load_cpu_payload(struct scene_3d_source *context, const char *model_path, bool draco_enabled,
					     const char *draco_decoder, struct scene_3d_cpu_payload *payload,
					     gs_image_file4_t *decoded_image, bool *decoded_image_valid)
{
	struct scene_3d_gltf_load_options options;
	struct scene_3d_gltf_error error = {0};

	if (!context || !model_path || !*model_path || !payload || !decoded_image || !decoded_image_valid)
		return false;

	options.draco_enabled = draco_enabled;
	options.draco_decoder = (draco_decoder && *draco_decoder) ? draco_decoder : S_DRACO_DECODER_AUTO;

	if (!scene_3d_gltf_load_cpu_payload(model_path, payload, &options, &error)) {
		blog(LOG_WARNING, "[scene-3d-source: '%s'] glTF load failed (%s): %s",
		     scene_3d_source_log_name(context), scene_3d_gltf_error_to_string(error.code),
		     error.message ? error.message : "no details");
		scene_3d_gltf_clear_error(&error);
		return false;
	}

	scene_3d_source_log_payload_summary(context, payload);
	*decoded_image_valid = scene_3d_source_decode_base_color_image(context, payload, decoded_image);
	return true;
}

static void *scene_3d_source_worker_main(void *data)
{
	struct scene_3d_source *context = data;

	os_set_thread_name("scene-3d-loader");

	while (os_event_wait(context->worker_event) == 0) {
		struct scene_3d_cpu_payload payload = {0};
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
			scene_3d_source_load_cpu_payload(context, job_model_path, job_draco_enabled, job_draco_decoder,
							 &payload, &decoded_image, &decoded_image_valid);

		scene_3d_source_publish_pending_upload(context, &payload, &decoded_image, decoded_image_valid, token);

		scene_3d_gltf_free_cpu_payload(&payload);
		if (decoded_image_valid)
			gs_image_file4_free(&decoded_image);
		bfree(job_model_path);
		bfree(job_draco_decoder);
	}

	return NULL;
}

static bool scene_3d_source_start_worker(struct scene_3d_source *context)
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
	context->worker_thread_active = pthread_create(&context->worker_thread, NULL, scene_3d_source_worker_main, context) == 0;
	if (!context->worker_thread_active)
		return false;

	return true;
}

static void scene_3d_source_stop_worker(struct scene_3d_source *context)
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

static void scene_3d_source_queue_load_job(struct scene_3d_source *context)
{
	uint64_t token;

	if (!context)
		return;

	if (!context->worker_thread_active) {
		struct scene_3d_cpu_payload payload = {0};
		gs_image_file4_t decoded_image = {0};
		bool decoded_image_valid = false;
		uint64_t token = ++context->worker_next_token;

		context->worker_cancel_token = token;

		if (scene_3d_source_model_path_is_loadable(context))
			scene_3d_source_load_cpu_payload(context, context->model_path, context->draco_enabled,
							 context->draco_decoder, &payload, &decoded_image,
							 &decoded_image_valid);

		scene_3d_source_publish_pending_upload(context, &payload, &decoded_image, decoded_image_valid, token);
		scene_3d_gltf_free_cpu_payload(&payload);
		if (decoded_image_valid)
			gs_image_file4_free(&decoded_image);
		return;
	}

	pthread_mutex_lock(&context->worker_mutex);

	token = ++context->worker_next_token;
	context->worker_cancel_token = token;

	scene_3d_source_free_worker_job(context);
	context->worker_job.model_path =
		scene_3d_source_model_path_is_loadable(context) ? bstrdup(context->model_path) : NULL;
	context->worker_job.draco_decoder =
		context->draco_decoder ? bstrdup(context->draco_decoder) : bstrdup(S_DRACO_DECODER_AUTO);
	context->worker_job.draco_enabled = context->draco_enabled;
	context->worker_job.token = token;
	context->worker_job.has_job = true;

	pthread_mutex_unlock(&context->worker_mutex);

	os_event_signal(context->worker_event);
}

static void scene_3d_source_process_pending_upload(struct scene_3d_source *context)
{
	struct scene_3d_cpu_payload payload = {0};
	gs_image_file4_t decoded_image = {0};
	bool decoded_image_valid = false;

	if (!context)
		return;

	if (os_atomic_load_bool(&context->device_loss_active))
		return;

	if (!scene_3d_source_take_pending_upload(context, &payload, &decoded_image, &decoded_image_valid))
		return;

	blog(LOG_INFO, "[scene-3d-source: '%s'] Consuming pending payload on render thread.",
	     scene_3d_source_log_name(context));
	scene_3d_source_upload_pending_payload(context, &payload, &decoded_image, decoded_image_valid);
	scene_3d_gltf_free_cpu_payload(&payload);

	if (decoded_image_valid)
		gs_image_file4_free(&decoded_image);
}

static void scene_3d_source_interaction_reset_camera(struct scene_3d_source *context)
{
	bool model_bounds_valid = false;

	if (!context)
		return;

	scene_3d_source_lock_camera(context);
	model_bounds_valid = context->model_bounds_valid;
	scene_3d_source_unlock_camera(context);

	if (!model_bounds_valid)
		return;

	scene_3d_source_update_default_camera(context);
}

static void scene_3d_source_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
					uint32_t click_count)
{
	struct scene_3d_source *context = data;
	const bool shift_pressed = event && ((event->modifiers & INTERACT_SHIFT_KEY) != 0);
	const bool ctrl_pressed =
		event && ((event->modifiers & (INTERACT_CONTROL_KEY | INTERACT_COMMAND_KEY)) != 0);

	UNUSED_PARAMETER(click_count);

	if (!context || !event)
		return;

	if (type != MOUSE_MIDDLE)
		return;

	if (mouse_up) {
		scene_3d_source_end_camera_drag(context, true, true, true);
		return;
	}

	/*
	 * Blender-style viewport navigation:
	 *   MMB          -> orbit
	 *   Shift+MMB    -> pan
	 *   Ctrl+MMB     -> dolly
	 */
	if (shift_pressed)
		scene_3d_source_begin_camera_drag(context, false, true, false, event->x, event->y);
	else if (ctrl_pressed)
		scene_3d_source_begin_camera_drag(context, false, false, true, event->x, event->y);
	else
		scene_3d_source_begin_camera_drag(context, true, false, false, event->x, event->y);
}

static void scene_3d_source_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	struct scene_3d_source *context = data;
	bool drag_orbit = false;
	bool drag_pan = false;
	bool drag_zoom = false;
	bool have_last = false;
	int32_t delta_x = 0;
	int32_t delta_y = 0;

	if (!context)
		return;

	if (mouse_leave || !event) {
		scene_3d_source_end_camera_drag(context, true, true, true);
		return;
	}

	scene_3d_source_lock_camera(context);
	drag_orbit = context->camera_drag_orbit;
	drag_pan = context->camera_drag_pan;
	drag_zoom = context->camera_drag_zoom;
	have_last = context->camera_last_input_valid;
	if (have_last) {
		delta_x = event->x - context->camera_last_input_x;
		delta_y = event->y - context->camera_last_input_y;
	}
	context->camera_last_input_x = event->x;
	context->camera_last_input_y = event->y;
	context->camera_last_input_valid = true;
	scene_3d_source_unlock_camera(context);

	if (!have_last || (!drag_orbit && !drag_pan && !drag_zoom))
		return;

	if (drag_orbit)
		scene_3d_source_orbit_camera(context, delta_x, delta_y);
	if (drag_pan)
		scene_3d_source_pan_camera(context, delta_x, delta_y);
	if (drag_zoom)
		scene_3d_source_dolly_camera(context, delta_y);
}

static void scene_3d_source_mouse_wheel(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	struct scene_3d_source *context = data;

	UNUSED_PARAMETER(event);
	UNUSED_PARAMETER(x_delta);

	if (!context)
		return;

	scene_3d_source_zoom_camera(context, y_delta);
}

static void scene_3d_source_focus(void *data, bool focus)
{
	struct scene_3d_source *context = data;

	if (!focus)
		scene_3d_source_end_camera_drag(context, true, true, true);
}

static bool scene_3d_source_is_reset_key(const struct obs_key_event *event)
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

static void scene_3d_source_key_click(void *data, const struct obs_key_event *event, bool key_up)
{
	struct scene_3d_source *context = data;

	if (!context || key_up || !event)
		return;

	if (scene_3d_source_is_reset_key(event))
		scene_3d_source_interaction_reset_camera(context);
}

static void scene_3d_source_get_camera_basis_proc(void *data, calldata_t *params)
{
	struct scene_3d_source *context = data;
	struct vec3 right;
	struct vec3 up;
	struct vec3 forward;
	bool available = false;

	if (!params)
		return;

	vec3_set(&right, 1.0f, 0.0f, 0.0f);
	vec3_set(&up, 0.0f, 1.0f, 0.0f);
	vec3_set(&forward, 0.0f, 0.0f, -1.0f);

	if (context) {
		scene_3d_source_lock_camera(context);
		if (context->default_camera_valid) {
			scene_3d_source_get_camera_basis_locked(context, &forward, &right, &up);
			available = true;
		}
		scene_3d_source_unlock_camera(context);
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

static void scene_3d_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_MODEL_PATH, "");
	obs_data_set_default_bool(settings, S_DRACO_ENABLED, true);
	obs_data_set_default_string(settings, S_DRACO_DECODER, S_DRACO_DECODER_AUTO);
}

static void scene_3d_source_update(void *data, obs_data_t *settings)
{
	struct scene_3d_source *context = data;
	const char *model_path = obs_data_get_string(settings, S_MODEL_PATH);
	const char *draco_decoder = obs_data_get_string(settings, S_DRACO_DECODER);

	bfree(context->model_path);
	context->model_path = (model_path && *model_path) ? bstrdup(model_path) : NULL;

	context->draco_enabled = obs_data_get_bool(settings, S_DRACO_ENABLED);

	bfree(context->draco_decoder);
	context->draco_decoder =
		(draco_decoder && *draco_decoder) ? bstrdup(draco_decoder) : bstrdup(S_DRACO_DECODER_AUTO);

	scene_3d_source_validate_model_path(context);

	if (context->draco_enabled && astrcmpi(context->draco_decoder, S_DRACO_DECODER_EXTERNAL) == 0) {
		blog(LOG_WARNING,
		     "[scene-3d-source: '%s'] External Draco decoder mode is not implemented in this scaffold.",
		     scene_3d_source_log_name(context));
	}

	context->diagnostics_logged_upload = false;
	context->diagnostics_logged_draw = false;
	scene_3d_source_queue_load_job(context);
	scene_3d_source_refresh_size(context);
}

static obs_properties_t *scene_3d_source_properties(void *data)
{
	struct scene_3d_source *context = data;
	obs_property_t *model_path;
	obs_property_t *draco_decoder;
	obs_properties_t *props = obs_properties_create();
	const bool model_uses_draco = scene_3d_source_model_path_uses_draco(context ? context->model_path : NULL);

	model_path = obs_properties_add_path(props, S_MODEL_PATH, obs_module_text("Scene3D.ModelFile"), OBS_PATH_FILE,
					     obs_module_text("Scene3D.ModelFile.Filter"), NULL);
	obs_property_set_modified_callback(model_path, scene_3d_source_model_path_modified);
	obs_properties_add_bool(props, S_DRACO_ENABLED, obs_module_text("Scene3D.Draco.Enable"));

	draco_decoder = obs_properties_add_list(props, S_DRACO_DECODER, obs_module_text("Scene3D.Draco.Decoder"),
						OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(draco_decoder, obs_module_text("Scene3D.Draco.Decoder.Auto"), S_DRACO_DECODER_AUTO);
	obs_property_list_add_string(draco_decoder, obs_module_text("Scene3D.Draco.Decoder.Builtin"),
				     S_DRACO_DECODER_BUILTIN);
	obs_property_list_add_string(draco_decoder, obs_module_text("Scene3D.Draco.Decoder.External"),
				     S_DRACO_DECODER_EXTERNAL);

	scene_3d_source_set_draco_property_state(props, model_uses_draco);
	return props;
}

static const char *scene_3d_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Model3DSource");
}

static void *scene_3d_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct scene_3d_source *context = bzalloc(sizeof(*context));
	proc_handler_t *proc_handler = NULL;

	context->source = source;
	proc_handler = obs_source_get_proc_handler(source);
	if (proc_handler) {
		proc_handler_add(proc_handler,
				 "void get_scene_3d_camera_basis("
				 "out bool available, "
				 "out float forward_x, out float forward_y, out float forward_z, "
				 "out float right_x, out float right_y, out float right_z, "
				 "out float up_x, out float up_y, out float up_z)",
				 scene_3d_source_get_camera_basis_proc, context);
	}

	pthread_mutex_init_value(&context->camera_mutex);
	if (pthread_mutex_init(&context->camera_mutex, NULL) == 0)
		context->camera_mutex_valid = true;

	scene_3d_source_reset_default_camera(context);
	scene_3d_source_reset_default_light(context);
	if (!scene_3d_source_start_worker(context)) {
		blog(LOG_WARNING,
		     "[scene-3d-source: '%s'] Failed to start async loader worker. Falling back to inline loading.",
		     scene_3d_source_log_name(context));

		if (!context->worker_mutex_valid) {
			pthread_mutex_init_value(&context->worker_mutex);
			if (pthread_mutex_init(&context->worker_mutex, NULL) == 0)
				context->worker_mutex_valid = true;
		}
	}

	scene_3d_source_register_device_loss_callbacks(context);
	scene_3d_source_refresh_size(context);
	scene_3d_source_update(context, settings);
	scene_3d_source_load_effect(context);
	return context;
}

static void scene_3d_source_destroy(void *data)
{
	struct scene_3d_source *context = data;

	scene_3d_source_unregister_device_loss_callbacks(context);
	scene_3d_source_stop_worker(context);
	scene_3d_source_free_worker_job(context);
	scene_3d_source_release_pending_upload(context);

	obs_enter_graphics();
	scene_3d_source_release_gpu_resources(context);
	obs_leave_graphics();

	scene_3d_source_unload_effect(context);
	bfree(context->model_path);
	bfree(context->draco_decoder);
	if (context->camera_mutex_valid) {
		pthread_mutex_destroy(&context->camera_mutex);
		context->camera_mutex_valid = false;
	}
	bfree(context);
}

static void scene_3d_source_show(void *data)
{
	struct scene_3d_source *context = data;
	context->showing = true;
}

static void scene_3d_source_hide(void *data)
{
	struct scene_3d_source *context = data;
	context->showing = false;
}

static void scene_3d_source_activate(void *data)
{
	struct scene_3d_source *context = data;
	context->active = true;
}

static void scene_3d_source_deactivate(void *data)
{
	struct scene_3d_source *context = data;
	context->active = false;
}

static void scene_3d_source_video_tick(void *data, float seconds)
{
	struct scene_3d_source *context = data;

	UNUSED_PARAMETER(seconds);

	if (!context)
		return;

	scene_3d_source_refresh_size(context);
	if (scene_3d_source_should_auto_fit_camera(context))
		scene_3d_source_update_default_camera(context);

	if (os_atomic_load_bool(&context->device_rebuild_pending)) {
		os_atomic_store_bool(&context->device_rebuild_pending, false);
		os_atomic_store_bool(&context->device_loss_active, false);
		context->effect_load_attempted = false;
		scene_3d_source_load_effect(context);
		scene_3d_source_queue_load_job(context);
	}

	scene_3d_source_process_pending_upload(context);

	if (!context->effect && !context->effect_load_attempted)
		scene_3d_source_load_effect(context);
}

static void scene_3d_source_render(void *data, gs_effect_t *effect)
{
	struct scene_3d_source *context = data;
	gs_texture_t *model_texture = NULL;
	bool rendered = false;
	const bool previous_srgb = gs_framebuffer_srgb_enabled();

	UNUSED_PARAMETER(effect);

	if (!context)
		return;

	if (os_atomic_load_bool(&context->device_loss_active))
		return;

	/* BaseColor is sampled as sRGB and shaded in linear space before output. */
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	model_texture = scene_3d_source_render_model_to_texture(context);
	if (model_texture) {
		gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image_param = default_effect ? gs_effect_get_param_by_name(default_effect, "image") : NULL;

		if (default_effect && image_param) {
			gs_effect_set_texture_srgb(image_param, model_texture);
			while (gs_effect_loop(default_effect, "Draw"))
				gs_draw_sprite(model_texture, 0, context->width, context->height);
			rendered = true;
		}
	}

	if (!rendered) {
		/* Fallback draw path keeps source visible even when custom effect is unavailable. */
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *color = solid ? gs_effect_get_param_by_name(solid, "color") : NULL;
		struct vec4 placeholder_color;

		if (solid && color) {
			vec4_from_rgba_srgb(&placeholder_color, 0xFF2D313A);
			gs_effect_set_vec4(color, &placeholder_color);

			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(NULL, 0, context->width, context->height);
		}
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
}

static enum gs_color_space scene_3d_source_get_color_space(void *data, size_t count,
							    const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	return GS_CS_SRGB;
}

static uint32_t scene_3d_source_get_width(void *data)
{
	const struct scene_3d_source *context = data;

	return context ? context->width : 0;
}

static uint32_t scene_3d_source_get_height(void *data)
{
	const struct scene_3d_source *context = data;

	return context ? context->height : 0;
}

struct obs_source_info scene_3d_source_info = {
	.id = "scene_3d_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_INTERACTION,
	.get_name = scene_3d_source_get_name,
	.create = scene_3d_source_create,
	.destroy = scene_3d_source_destroy,
	.update = scene_3d_source_update,
	.get_defaults = scene_3d_source_defaults,
	.show = scene_3d_source_show,
	.hide = scene_3d_source_hide,
	.activate = scene_3d_source_activate,
	.deactivate = scene_3d_source_deactivate,
	.get_properties = scene_3d_source_properties,
	.mouse_click = scene_3d_source_mouse_click,
	.mouse_move = scene_3d_source_mouse_move,
	.mouse_wheel = scene_3d_source_mouse_wheel,
	.focus = scene_3d_source_focus,
	.key_click = scene_3d_source_key_click,
	.video_tick = scene_3d_source_video_tick,
	.video_render = scene_3d_source_render,
	.get_width = scene_3d_source_get_width,
	.get_height = scene_3d_source_get_height,
	.icon_type = OBS_ICON_TYPE_CUSTOM,
	.video_get_color_space = scene_3d_source_get_color_space,
};
