/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

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

#include "OBSBasicInteraction.hpp"

#include <dialogs/OBSBasicProperties.hpp>
#include <utility/OBSEventFilter.hpp>
#include <utility/display-helpers.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <QInputEvent>
#include <QLabel>
#include <QGuiApplication>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <Windows.h>
#endif

#include "moc_OBSBasicInteraction.cpp"

using namespace std;

namespace {
struct VspaceCameraBasis {
	float right_x;
	float right_y;
	float right_z;
	float up_x;
	float up_y;
	float up_z;
	float forward_x;
	float forward_y;
	float forward_z;
};

struct VspaceCameraState {
	bool available;
	float camera_x;
	float camera_y;
	float camera_z;
	float target_x;
	float target_y;
	float target_z;
	float up_x;
	float up_y;
	float up_z;
	float fov_deg;
	float znear;
	float zfar;
};

struct VspaceModelBounds {
	bool available;
	float min_x;
	float min_y;
	float min_z;
	float max_x;
	float max_y;
	float max_z;
};

struct GizmoAxis {
	char label;
	float screen_x;
	float screen_y;
	float depth;
	struct vec4 color;
};

struct VspaceInspectGridRenderer {
	gs_effect_t *effect = nullptr;
	gs_vertbuffer_t *triangle = nullptr;
	gs_eparam_t *camera_position = nullptr;
	gs_eparam_t *grid_forward = nullptr;
	gs_eparam_t *grid_right = nullptr;
	gs_eparam_t *grid_up = nullptr;
	gs_eparam_t *grid_tan_half_fov = nullptr;
	gs_eparam_t *grid_aspect = nullptr;
	gs_eparam_t *grid_step = nullptr;
	gs_eparam_t *grid_origin = nullptr;
	gs_eparam_t *grid_extent = nullptr;
};

static void DrawGizmoLine(float x1, float y1, float x2, float y2, float thickness);

static float SnapGridStep125(float raw_step)
{
	float step = fmaxf(raw_step, 0.01f);
	const float magnitude = powf(10.0f, floorf(log10f(step)));
	const float normalized = step / magnitude;

	if (normalized < 2.0f)
		return magnitude;
	if (normalized < 5.0f)
		return 2.0f * magnitude;
	return 5.0f * magnitude;
}

static bool EnsureInspectGridRenderer(VspaceInspectGridRenderer &renderer)
{
	static const char *effect_source = R"OBS_EFFECT(
uniform float3 effect_camera_position = {0.0, 0.0, -3.0};
uniform float3 effect_grid_forward = {0.0, 1.0, 0.0};
uniform float3 effect_grid_right = {1.0, 0.0, 0.0};
uniform float3 effect_grid_up = {0.0, 0.0, 1.0};
uniform float effect_grid_tan_half_fov = 0.4663077;
uniform float effect_grid_aspect = 1.7777778;
uniform float effect_grid_step = 1.0;
uniform float2 effect_grid_origin = {0.0, 0.0};
uniform float effect_grid_extent = 64.0;
uniform float4 effect_grid_color = {0.52, 0.52, 0.52, 0.68};
uniform float4 effect_grid_x_axis_color = {0.95, 0.32, 0.32, 0.92};
uniform float4 effect_grid_y_axis_color = {0.36, 0.88, 0.38, 0.92};
uniform float effect_grid_line_width = 0.25;
uniform float effect_grid_axis_width = 1.8;

struct VertDataGrid {
	float4 pos : POSITION;
};

struct GridDataOut {
	float4 pos : POSITION;
	float2 ndc : TEXCOORD0;
};

GridDataOut VSGrid(VertDataGrid v)
{
	GridDataOut v_out;
	v_out.pos = float4(v.pos.xy, 0.0, 1.0);
	v_out.ndc = v.pos.xy;
	return v_out;
}

float grid_axis_alpha(float dist, float width_px)
{
	float fw = max(fwidth(dist), 1e-6f);
	float dist_px = abs(dist) / fw;
	return 1.0f - smoothstep(width_px, width_px + 1.0f, dist_px);
}

float4 PSGrid(GridDataOut v) : TARGET
{
	float step_value = max(effect_grid_step, 1e-5f);
	float extent_value = max(effect_grid_extent, step_value);
	float3 forward = normalize(effect_grid_forward);
	float3 right = normalize(effect_grid_right);
	float3 up = normalize(effect_grid_up);
	float3 ray_dir;
	float denom;
	float t;
	float3 hit;
	float2 local;
	float2 grid_uv;
	float2 grid_uv_fw;
	float2 grid_dist;
	float grid_alpha;
	float axis_x_alpha;
	float axis_y_alpha;
	float4 color;

	ray_dir = normalize(forward + right * (v.ndc.x * effect_grid_aspect * effect_grid_tan_half_fov) +
			    up * (v.ndc.y * effect_grid_tan_half_fov));
	denom = ray_dir.z;

	if (abs(denom) < 1e-6f)
		discard;

	t = -effect_camera_position.z / denom;
	if (t <= 0.0f)
		discard;

	hit = effect_camera_position + ray_dir * t;
	local = hit.xy - effect_grid_origin;
	if (abs(local.x) > extent_value || abs(local.y) > extent_value)
		discard;

	grid_uv = local / step_value;
	grid_uv_fw = max(fwidth(grid_uv), float2(1e-6f, 1e-6f));
	grid_dist = abs(frac(grid_uv - 0.5f) - 0.5f) / grid_uv_fw;
	grid_alpha = 1.0f - smoothstep(effect_grid_line_width, effect_grid_line_width + 1.0f,
				       min(grid_dist.x, grid_dist.y));

	axis_x_alpha = grid_axis_alpha(hit.y, effect_grid_axis_width);
	axis_y_alpha = grid_axis_alpha(hit.x, effect_grid_axis_width);

	color = float4(effect_grid_color.rgb, effect_grid_color.a * grid_alpha);
	color = lerp(color, effect_grid_y_axis_color, axis_y_alpha);
	color = lerp(color, effect_grid_x_axis_color, axis_x_alpha);

	if (color.a < 0.001f)
		discard;

	return color;
}

technique DrawGrid
{
	pass
	{
		vertex_shader = VSGrid(v);
		pixel_shader = PSGrid(v);
	}
}
)OBS_EFFECT";

	if (!renderer.effect) {
		renderer.effect = gs_effect_create(effect_source, "vspace-inspect-grid.effect", nullptr);
		if (renderer.effect) {
			renderer.camera_position = gs_effect_get_param_by_name(renderer.effect, "effect_camera_position");
			renderer.grid_forward = gs_effect_get_param_by_name(renderer.effect, "effect_grid_forward");
			renderer.grid_right = gs_effect_get_param_by_name(renderer.effect, "effect_grid_right");
			renderer.grid_up = gs_effect_get_param_by_name(renderer.effect, "effect_grid_up");
			renderer.grid_tan_half_fov = gs_effect_get_param_by_name(renderer.effect, "effect_grid_tan_half_fov");
			renderer.grid_aspect = gs_effect_get_param_by_name(renderer.effect, "effect_grid_aspect");
			renderer.grid_step = gs_effect_get_param_by_name(renderer.effect, "effect_grid_step");
			renderer.grid_origin = gs_effect_get_param_by_name(renderer.effect, "effect_grid_origin");
			renderer.grid_extent = gs_effect_get_param_by_name(renderer.effect, "effect_grid_extent");
		}
	}

	if (!renderer.triangle) {
		struct gs_vb_data *vb_data = gs_vbdata_create();
		if (vb_data) {
			vb_data->num = 3;
			vb_data->points = (struct vec3 *)bmalloc(sizeof(struct vec3) * 3);
			vb_data->num_tex = 0;
			vb_data->tvarray = nullptr;
			vb_data->normals = nullptr;
			vb_data->tangents = nullptr;
			vb_data->colors = nullptr;

			if (!vb_data->points) {
				gs_vbdata_destroy(vb_data);
				vb_data = nullptr;
			} else {
				vec3_set(vb_data->points + 0, -1.0f, -1.0f, 0.0f);
				vec3_set(vb_data->points + 1, -1.0f, 3.0f, 0.0f);
				vec3_set(vb_data->points + 2, 3.0f, -1.0f, 0.0f);
				renderer.triangle = gs_vertexbuffer_create(vb_data, 0);
				if (!renderer.triangle)
					gs_vbdata_destroy(vb_data);
			}
		}
	}

	return renderer.effect && renderer.triangle && renderer.camera_position && renderer.grid_forward &&
	       renderer.grid_right && renderer.grid_up && renderer.grid_tan_half_fov && renderer.grid_aspect &&
	       renderer.grid_step && renderer.grid_origin && renderer.grid_extent;
}

static float VecLength(float x, float y, float z)
{
	return sqrtf(x * x + y * y + z * z);
}

static void NormalizeVec3(float &x, float &y, float &z, float fallback_x, float fallback_y, float fallback_z)
{
	const float length = VecLength(x, y, z);

	if (length > 0.0001f) {
		const float inv_length = 1.0f / length;
		x *= inv_length;
		y *= inv_length;
		z *= inv_length;
		return;
	}

	x = fallback_x;
	y = fallback_y;
	z = fallback_z;
}

static void NormalizeCameraBasis(VspaceCameraBasis &basis)
{
	NormalizeVec3(basis.right_x, basis.right_y, basis.right_z, 1.0f, 0.0f, 0.0f);
	NormalizeVec3(basis.up_x, basis.up_y, basis.up_z, 0.0f, 0.0f, 1.0f);
	NormalizeVec3(basis.forward_x, basis.forward_y, basis.forward_z, 0.0f, 1.0f, 0.0f);
}

static bool IsVspaceSource(obs_source_t *source)
{
	const char *id = source ? obs_source_get_id(source) : nullptr;

	return id && strcmp(id, "vspace_source") == 0;
}

static bool GetVspaceCameraBasis(obs_source_t *source, VspaceCameraBasis &basis)
{
	proc_handler_t *proc_handler = nullptr;
	calldata_t cd = {};
	bool success = false;

	basis = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f};

	if (!source || !IsVspaceSource(source))
		return false;

	proc_handler = obs_source_get_proc_handler(source);
	if (!proc_handler)
		return false;

	success = proc_handler_call(proc_handler, "get_vspace_camera_basis", &cd);
	if (!success) {
		calldata_free(&cd);
		return false;
	}

	if (!calldata_bool(&cd, "available")) {
		calldata_free(&cd);
		return false;
	}

	basis.forward_x = (float)calldata_float(&cd, "forward_x");
	basis.forward_y = (float)calldata_float(&cd, "forward_y");
	basis.forward_z = (float)calldata_float(&cd, "forward_z");
	basis.right_x = (float)calldata_float(&cd, "right_x");
	basis.right_y = (float)calldata_float(&cd, "right_y");
	basis.right_z = (float)calldata_float(&cd, "right_z");
	basis.up_x = (float)calldata_float(&cd, "up_x");
	basis.up_y = (float)calldata_float(&cd, "up_y");
	basis.up_z = (float)calldata_float(&cd, "up_z");
	calldata_free(&cd);

	NormalizeCameraBasis(basis);
	return true;
}

static bool GetVspaceCameraState(obs_source_t *source, VspaceCameraState &state)
{
	proc_handler_t *proc_handler = nullptr;
	calldata_t cd = {};
	bool success = false;

	state = {false, 0.0f, -3.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 50.0f, 0.1f, 100.0f};

	if (!source || !IsVspaceSource(source))
		return false;

	proc_handler = obs_source_get_proc_handler(source);
	if (!proc_handler)
		return false;

	success = proc_handler_call(proc_handler, "get_vspace_camera_state", &cd);
	if (!success) {
		calldata_free(&cd);
		return false;
	}

	if (!calldata_bool(&cd, "available")) {
		calldata_free(&cd);
		return false;
	}

	state.available = true;
	state.camera_x = (float)calldata_float(&cd, "camera_x");
	state.camera_y = (float)calldata_float(&cd, "camera_y");
	state.camera_z = (float)calldata_float(&cd, "camera_z");
	state.target_x = (float)calldata_float(&cd, "target_x");
	state.target_y = (float)calldata_float(&cd, "target_y");
	state.target_z = (float)calldata_float(&cd, "target_z");
	state.up_x = (float)calldata_float(&cd, "up_x");
	state.up_y = (float)calldata_float(&cd, "up_y");
	state.up_z = (float)calldata_float(&cd, "up_z");
	state.fov_deg = (float)calldata_float(&cd, "fov_deg");
	state.znear = (float)calldata_float(&cd, "znear");
	state.zfar = (float)calldata_float(&cd, "zfar");
	calldata_free(&cd);

	return true;
}

static bool GetVspaceModelBounds(obs_source_t *source, VspaceModelBounds &bounds)
{
	proc_handler_t *proc_handler = nullptr;
	calldata_t cd = {};
	bool success = false;

	bounds = {false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

	if (!source || !IsVspaceSource(source))
		return false;

	proc_handler = obs_source_get_proc_handler(source);
	if (!proc_handler)
		return false;

	success = proc_handler_call(proc_handler, "get_vspace_model_bounds", &cd);
	if (!success) {
		calldata_free(&cd);
		return false;
	}

	if (!calldata_bool(&cd, "available")) {
		calldata_free(&cd);
		return false;
	}

	bounds.available = true;
	bounds.min_x = (float)calldata_float(&cd, "min_x");
	bounds.min_y = (float)calldata_float(&cd, "min_y");
	bounds.min_z = (float)calldata_float(&cd, "min_z");
	bounds.max_x = (float)calldata_float(&cd, "max_x");
	bounds.max_y = (float)calldata_float(&cd, "max_y");
	bounds.max_z = (float)calldata_float(&cd, "max_z");
	calldata_free(&cd);

	return true;
}

static void SetVspaceInspectRenderMode(obs_source_t *source, bool enabled)
{
	proc_handler_t *proc_handler = nullptr;
	calldata_t cd = {};

	if (!source || !IsVspaceSource(source))
		return;

	proc_handler = obs_source_get_proc_handler(source);
	if (!proc_handler)
		return;

	calldata_set_bool(&cd, "enabled", enabled);
	proc_handler_call(proc_handler, "set_vspace_inspect_render_mode", &cd);
	calldata_free(&cd);
}

static void ResolveProjectionBasis(const VspaceCameraState &state, const VspaceCameraBasis *basis_hint, float &right_x,
				   float &right_y, float &right_z, float &up_x, float &up_y, float &up_z,
				   float &forward_x, float &forward_y, float &forward_z)
{
	forward_x = state.target_x - state.camera_x;
	forward_y = state.target_y - state.camera_y;
	forward_z = state.target_z - state.camera_z;
	up_x = state.up_x;
	up_y = state.up_y;
	up_z = state.up_z;

	NormalizeVec3(forward_x, forward_y, forward_z, 0.0f, 1.0f, 0.0f);
	NormalizeVec3(up_x, up_y, up_z, 0.0f, 0.0f, 1.0f);

	right_x = forward_y * up_z - forward_z * up_y;
	right_y = forward_z * up_x - forward_x * up_z;
	right_z = forward_x * up_y - forward_y * up_x;
	NormalizeVec3(right_x, right_y, right_z, 1.0f, 0.0f, 0.0f);

	up_x = right_y * forward_z - right_z * forward_y;
	up_y = right_z * forward_x - right_x * forward_z;
	up_z = right_x * forward_y - right_y * forward_x;
	NormalizeVec3(up_x, up_y, up_z, 0.0f, 0.0f, 1.0f);

	if (!basis_hint)
		return;

	right_x = basis_hint->right_x;
	right_y = basis_hint->right_y;
	right_z = basis_hint->right_z;
	up_x = basis_hint->up_x;
	up_y = basis_hint->up_y;
	up_z = basis_hint->up_z;
	forward_x = basis_hint->forward_x;
	forward_y = basis_hint->forward_y;
	forward_z = basis_hint->forward_z;

	NormalizeVec3(right_x, right_y, right_z, 1.0f, 0.0f, 0.0f);
	NormalizeVec3(up_x, up_y, up_z, 0.0f, 0.0f, 1.0f);
	NormalizeVec3(forward_x, forward_y, forward_z, 0.0f, 1.0f, 0.0f);
}

static bool ProjectVspacePointToScreen(const VspaceCameraState &state, uint32_t source_cx, uint32_t source_cy,
					float world_x, float world_y, float world_z, float &screen_x, float &screen_y)
{
	float forward_x = state.target_x - state.camera_x;
	float forward_y = state.target_y - state.camera_y;
	float forward_z = state.target_z - state.camera_z;
	float up_x = state.up_x;
	float up_y = state.up_y;
	float up_z = state.up_z;
	float right_x;
	float right_y;
	float right_z;
	float view_x;
	float view_y;
	float view_z;
	float dx;
	float dy;
	float dz;
	float tan_half_fov;
	float aspect;
	float ndc_x;
	float ndc_y;
	const float pi = 3.14159265358979323846f;

	if (!state.available || source_cx == 0 || source_cy == 0)
		return false;

	ResolveProjectionBasis(state, nullptr, right_x, right_y, right_z, up_x, up_y, up_z, forward_x, forward_y,
			       forward_z);

	dx = world_x - state.camera_x;
	dy = world_y - state.camera_y;
	dz = world_z - state.camera_z;

	view_x = dx * right_x + dy * right_y + dz * right_z;
	view_y = dx * up_x + dy * up_y + dz * up_z;
	view_z = dx * forward_x + dy * forward_y + dz * forward_z;

	if (view_z <= fmaxf(state.znear * 0.25f, 0.001f))
		return false;

	aspect = (float)source_cx / (float)source_cy;
	tan_half_fov = tanf((state.fov_deg * pi / 180.0f) * 0.5f);
	if (tan_half_fov < 0.001f)
		tan_half_fov = 0.001f;

	ndc_x = view_x / (view_z * tan_half_fov * fmaxf(aspect, 0.1f));
	ndc_y = view_y / (view_z * tan_half_fov);

	if (!isfinite(ndc_x) || !isfinite(ndc_y))
		return false;

	screen_x = (ndc_x * 0.5f + 0.5f) * (float)source_cx;
	screen_y = (0.5f - ndc_y * 0.5f) * (float)source_cy;

	return true;
}

static bool ProjectVspaceLineToScreen(const VspaceCameraState &state, const VspaceCameraBasis *basis_hint,
				      uint32_t source_cx, uint32_t source_cy, float world0_x, float world0_y,
				      float world0_z, float world1_x, float world1_y, float world1_z,
				      float &screen0_x, float &screen0_y, float &screen1_x, float &screen1_y)
{
	float forward_x = state.target_x - state.camera_x;
	float forward_y = state.target_y - state.camera_y;
	float forward_z = state.target_z - state.camera_z;
	float up_x = state.up_x;
	float up_y = state.up_y;
	float up_z = state.up_z;
	float right_x;
	float right_y;
	float right_z;
	float dx0;
	float dy0;
	float dz0;
	float dx1;
	float dy1;
	float dz1;
	float view0_x;
	float view0_y;
	float view0_z;
	float view1_x;
	float view1_y;
	float view1_z;
	float aspect;
	float tan_half_fov;
	float near_z;
	float t;
	const float pi = 3.14159265358979323846f;

	if (!state.available || source_cx == 0 || source_cy == 0)
		return false;

	ResolveProjectionBasis(state, basis_hint, right_x, right_y, right_z, up_x, up_y, up_z, forward_x, forward_y,
			       forward_z);

	dx0 = world0_x - state.camera_x;
	dy0 = world0_y - state.camera_y;
	dz0 = world0_z - state.camera_z;
	dx1 = world1_x - state.camera_x;
	dy1 = world1_y - state.camera_y;
	dz1 = world1_z - state.camera_z;

	view0_x = dx0 * right_x + dy0 * right_y + dz0 * right_z;
	view0_y = dx0 * up_x + dy0 * up_y + dz0 * up_z;
	view0_z = dx0 * forward_x + dy0 * forward_y + dz0 * forward_z;
	view1_x = dx1 * right_x + dy1 * right_y + dz1 * right_z;
	view1_y = dx1 * up_x + dy1 * up_y + dz1 * up_z;
	view1_z = dx1 * forward_x + dy1 * forward_y + dz1 * forward_z;

	near_z = fmaxf(state.znear * 0.25f, 0.001f);
	if (view0_z <= near_z && view1_z <= near_z)
		return false;

	if (view0_z <= near_z) {
		t = (near_z - view0_z) / (view1_z - view0_z);
		view0_x = view0_x + (view1_x - view0_x) * t;
		view0_y = view0_y + (view1_y - view0_y) * t;
		view0_z = near_z;
	} else if (view1_z <= near_z) {
		t = (near_z - view1_z) / (view0_z - view1_z);
		view1_x = view1_x + (view0_x - view1_x) * t;
		view1_y = view1_y + (view0_y - view1_y) * t;
		view1_z = near_z;
	}

	aspect = (float)source_cx / (float)source_cy;
	tan_half_fov = tanf((state.fov_deg * pi / 180.0f) * 0.5f);
	if (tan_half_fov < 0.001f)
		tan_half_fov = 0.001f;

	screen0_x = ((view0_x / (view0_z * tan_half_fov * fmaxf(aspect, 0.1f))) * 0.5f + 0.5f) * (float)source_cx;
	screen0_y = (0.5f - (view0_y / (view0_z * tan_half_fov)) * 0.5f) * (float)source_cy;
	screen1_x = ((view1_x / (view1_z * tan_half_fov * fmaxf(aspect, 0.1f))) * 0.5f + 0.5f) * (float)source_cx;
	screen1_y = (0.5f - (view1_y / (view1_z * tan_half_fov)) * 0.5f) * (float)source_cy;

	return isfinite(screen0_x) && isfinite(screen0_y) && isfinite(screen1_x) && isfinite(screen1_y);
}

static bool LineIntersectsSourceRect(float x0, float y0, float x1, float y1, float width, float height)
{
	if ((x0 < 0.0f && x1 < 0.0f) || (x0 > width && x1 > width))
		return false;
	if ((y0 < 0.0f && y1 < 0.0f) || (y0 > height && y1 > height))
		return false;

	return true;
}

static void DrawVspaceGrid(obs_source_t *source, uint32_t source_cx, uint32_t source_cy)
{
	VspaceCameraBasis basis = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
	static VspaceInspectGridRenderer renderer = {};
	VspaceCameraState state = {};
	gs_effect_t *solid = nullptr;
	gs_eparam_t *color_param = nullptr;
	struct vec4 x_axis_color;
	struct vec4 y_axis_color;
	struct vec4 grid_color;
	float camera_distance;
	float raw_step;
	float step_power2;
	float grid_step;
	int half_lines;
	float grid_extent;
	float tan_half_fov;
	float aspect;
	float view_height_world;
	float units_per_pixel;
	float required_extent;
	float grazing_extent;
	float line_extent_along_x;
	float line_extent_along_y;
	float forward_abs_x;
	float forward_abs_y;
	float forward_abs_z;
	float origin_x;
	float origin_y;
	bool draw_y_axis;
	bool draw_x_axis;
	const int min_half_lines = 48;
	const int max_half_lines = 320;
	const float axis_thickness = 2.0f;
	const float line_thickness = 1.0f;
	int index;
	const float pi = 3.14159265358979323846f;

	if (!source || !IsVspaceSource(source))
		return;
	if (!GetVspaceCameraState(source, state))
		return;
	GetVspaceCameraBasis(source, basis);

	solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	color_param = solid ? gs_effect_get_param_by_name(solid, "color") : nullptr;
	if (!solid || !color_param)
		return;

	camera_distance = VecLength(state.camera_x - state.target_x, state.camera_y - state.target_y,
				    state.camera_z - state.target_z);
	if (camera_distance < 1.0f)
		camera_distance = 1.0f;
	aspect = (float)source_cx / (float)source_cy;
	tan_half_fov = tanf((state.fov_deg * pi / 180.0f) * 0.5f);
	if (tan_half_fov < 0.001f)
		tan_half_fov = 0.001f;
	view_height_world = camera_distance * tan_half_fov;
	units_per_pixel = (view_height_world * 2.0f) / fmaxf((float)source_cy, 1.0f);
	raw_step = units_per_pixel * 96.0f;
	step_power2 = SnapGridStep125(raw_step);
	grid_step = fmaxf(step_power2, 0.01f);
	required_extent = fmaxf(camera_distance * 6.0f, fmaxf(view_height_world, view_height_world * aspect) * 1.2f);
	forward_abs_z = fabsf(basis.forward_z);
	grazing_extent = fabsf(state.camera_z) / fmaxf(forward_abs_z, 0.02f);
	required_extent = fmaxf(required_extent, grazing_extent * 1.5f);
	if (!isfinite(required_extent) || required_extent < 16.0f)
		required_extent = 16.0f;

	forward_abs_x = fabsf(basis.forward_x);
	forward_abs_y = fabsf(basis.forward_y);
	line_extent_along_y = required_extent / fmaxf(forward_abs_y, 0.08f);
	line_extent_along_x = required_extent / fmaxf(forward_abs_x, 0.08f);
	line_extent_along_y = clamp(line_extent_along_y, required_extent, required_extent * 24.0f);
	line_extent_along_x = clamp(line_extent_along_x, required_extent, required_extent * 24.0f);

	half_lines = (int)ceilf(required_extent / grid_step);
	if (half_lines > max_half_lines) {
		half_lines = max_half_lines;
		grid_step = required_extent / (float)half_lines;
	}
	half_lines = clamp(half_lines, min_half_lines, max_half_lines);
	grid_extent = grid_step * (float)half_lines;
	origin_x = roundf(state.target_x / grid_step) * grid_step;
	origin_y = roundf(state.target_y / grid_step) * grid_step;

	if (EnsureInspectGridRenderer(renderer)) {
		struct vec3 camera_position;
		struct vec3 forward;
		struct vec3 right;
		struct vec3 up;
		struct vec2 origin;

		vec3_set(&camera_position, state.camera_x, state.camera_y, state.camera_z);
		vec3_set(&forward, basis.forward_x, basis.forward_y, basis.forward_z);
		vec3_set(&right, basis.right_x, basis.right_y, basis.right_z);
		vec3_set(&up, basis.up_x, basis.up_y, basis.up_z);
		vec2_set(&origin, origin_x, origin_y);

		gs_effect_set_vec3(renderer.camera_position, &camera_position);
		gs_effect_set_vec3(renderer.grid_forward, &forward);
		gs_effect_set_vec3(renderer.grid_right, &right);
		gs_effect_set_vec3(renderer.grid_up, &up);
		gs_effect_set_float(renderer.grid_tan_half_fov, tan_half_fov);
		gs_effect_set_float(renderer.grid_aspect, aspect);
		gs_effect_set_float(renderer.grid_step, grid_step);
		gs_effect_set_vec2(renderer.grid_origin, &origin);
		gs_effect_set_float(renderer.grid_extent, required_extent);

		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
		gs_load_vertexbuffer(renderer.triangle);
		gs_load_indexbuffer(nullptr);
		while (gs_effect_loop(renderer.effect, "DrawGrid"))
			gs_draw(GS_TRIS, 0, 3);
		gs_load_vertexbuffer(nullptr);
		gs_blend_state_pop();
		return;
	}

	vec4_set(&x_axis_color, 0.95f, 0.32f, 0.32f, 0.92f);
	vec4_set(&y_axis_color, 0.36f, 0.88f, 0.38f, 0.92f);
	vec4_set(&grid_color, 0.52f, 0.52f, 0.52f, 0.68f);

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	for (index = -half_lines; index <= half_lines; index++) {
		const float x = origin_x + (float)index * grid_step;
		float x0;
		float y0;
		float x1;
		float y1;

		if (!ProjectVspaceLineToScreen(state, &basis, source_cx, source_cy, x, -line_extent_along_y, 0.0f, x,
					       line_extent_along_y, 0.0f, x0, y0, x1, y1))
			continue;
		if (!LineIntersectsSourceRect(x0, y0, x1, y1, (float)source_cx, (float)source_cy))
			continue;
		draw_y_axis = fabsf(x) <= fmaxf(grid_step * 0.02f, 1e-4f);

		gs_effect_set_vec4(color_param, draw_y_axis ? &y_axis_color : &grid_color);
		while (gs_effect_loop(solid, "Solid"))
			DrawGizmoLine(x0, y0, x1, y1, draw_y_axis ? axis_thickness : line_thickness);
	}

	for (index = -half_lines; index <= half_lines; index++) {
		const float y = origin_y + (float)index * grid_step;
		float x0;
		float y0;
		float x1;
		float y1;

		if (!ProjectVspaceLineToScreen(state, &basis, source_cx, source_cy, -line_extent_along_x, y, 0.0f,
					       line_extent_along_x, y, 0.0f, x0, y0, x1, y1))
			continue;
		if (!LineIntersectsSourceRect(x0, y0, x1, y1, (float)source_cx, (float)source_cy))
			continue;
		draw_x_axis = fabsf(y) <= fmaxf(grid_step * 0.02f, 1e-4f);

		gs_effect_set_vec4(color_param, draw_x_axis ? &x_axis_color : &grid_color);
		while (gs_effect_loop(solid, "Solid"))
			DrawGizmoLine(x0, y0, x1, y1, draw_x_axis ? axis_thickness : line_thickness);
	}

	gs_blend_state_pop();
}

static void DrawVspaceBoundingBox(obs_source_t *source, uint32_t source_cx, uint32_t source_cy)
{
	VspaceCameraState state = {};
	VspaceModelBounds bounds = {};
	gs_effect_t *solid = nullptr;
	gs_eparam_t *color_param = nullptr;
	struct vec4 bounds_color;
	float corner_x[8];
	float corner_y[8];
	float corner_z[8];
	int edge_index;
	static const int edges[12][2] = {
		{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
		{6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
	};
	const float line_thickness = 1.5f;

	if (!source || !IsVspaceSource(source))
		return;
	if (!GetVspaceCameraState(source, state))
		return;
	if (!GetVspaceModelBounds(source, bounds) || !bounds.available)
		return;

	solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	color_param = solid ? gs_effect_get_param_by_name(solid, "color") : nullptr;
	if (!solid || !color_param)
		return;

	corner_x[0] = bounds.min_x;
	corner_y[0] = bounds.min_y;
	corner_z[0] = bounds.min_z;
	corner_x[1] = bounds.max_x;
	corner_y[1] = bounds.min_y;
	corner_z[1] = bounds.min_z;
	corner_x[2] = bounds.max_x;
	corner_y[2] = bounds.max_y;
	corner_z[2] = bounds.min_z;
	corner_x[3] = bounds.min_x;
	corner_y[3] = bounds.max_y;
	corner_z[3] = bounds.min_z;
	corner_x[4] = bounds.min_x;
	corner_y[4] = bounds.min_y;
	corner_z[4] = bounds.max_z;
	corner_x[5] = bounds.max_x;
	corner_y[5] = bounds.min_y;
	corner_z[5] = bounds.max_z;
	corner_x[6] = bounds.max_x;
	corner_y[6] = bounds.max_y;
	corner_z[6] = bounds.max_z;
	corner_x[7] = bounds.min_x;
	corner_y[7] = bounds.max_y;
	corner_z[7] = bounds.max_z;

	vec4_set(&bounds_color, 0.98f, 0.79f, 0.24f, 0.95f);

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	for (edge_index = 0; edge_index < 12; edge_index++) {
		const int a = edges[edge_index][0];
		const int b = edges[edge_index][1];
		float x0;
		float y0;
		float x1;
		float y1;

		if (!ProjectVspacePointToScreen(state, source_cx, source_cy, corner_x[a], corner_y[a], corner_z[a], x0, y0))
			continue;
		if (!ProjectVspacePointToScreen(state, source_cx, source_cy, corner_x[b], corner_y[b], corner_z[b], x1, y1))
			continue;
		if (!LineIntersectsSourceRect(x0, y0, x1, y1, (float)source_cx, (float)source_cy))
			continue;

		gs_effect_set_vec4(color_param, &bounds_color);
		while (gs_effect_loop(solid, "Solid"))
			DrawGizmoLine(x0, y0, x1, y1, line_thickness);
	}

	gs_blend_state_pop();
}

static void DrawGizmoLine(float x1, float y1, float x2, float y2, float thickness)
{
	const float dx = x2 - x1;
	const float dy = y2 - y1;
	const float length = sqrtf(dx * dx + dy * dy);

	if (length <= 0.0001f)
		return;

	gs_matrix_push();
	gs_matrix_translate3f(x1, y1, 0.0f);
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, atan2f(dy, dx));
	gs_matrix_translate3f(0.0f, -thickness * 0.5f, 0.0f);
	gs_draw_quadf(NULL, 0.0f, length, thickness);
	gs_matrix_pop();
}

static void DrawGizmoQuad(float x, float y, float width, float height)
{
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_draw_quadf(NULL, 0, width, height);
	gs_matrix_pop();
}

static void DrawGizmoLabelGlyph(gs_effect_t *solid, gs_eparam_t *color_param, char glyph, float x, float y, float size,
				float thickness, const struct vec4 *color)
{
	const float half = size * 0.5f;
	const float join = size * 0.15f;

	if (!solid || !color_param || !color)
		return;

	gs_effect_set_vec4(color_param, color);

	while (gs_effect_loop(solid, "Solid")) {
		switch (glyph) {
		case 'x':
			DrawGizmoLine(x - half, y - half, x + half, y + half, thickness);
			DrawGizmoLine(x - half, y + half, x + half, y - half, thickness);
			break;
		case 'y':
			DrawGizmoLine(x, y + half, x, y - join, thickness);
			DrawGizmoLine(x - half, y - half, x, y - join, thickness);
			DrawGizmoLine(x + half, y - half, x, y - join, thickness);
			break;
		case 'z':
			DrawGizmoLine(x - half, y - half, x + half, y - half, thickness);
			DrawGizmoLine(x + half, y - half, x - half, y + half, thickness);
			DrawGizmoLine(x - half, y + half, x + half, y + half, thickness);
			break;
		default:
			break;
		}
	}
}

static void DrawVspaceGizmo(obs_source_t *source, int viewport_x, int viewport_y, int viewport_cx, int viewport_cy)
{
	VspaceCameraBasis basis = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
	gs_effect_t *solid = nullptr;
	gs_eparam_t *color_param = nullptr;
	std::array<GizmoAxis, 3> axes;
	const int gizmo_size = clamp(min(viewport_cx, viewport_cy) / 4, 96, 168);
	const int gizmo_margin = max(8, gizmo_size / 8);
	const int gizmo_x = viewport_x + viewport_cx - gizmo_size - gizmo_margin;
	const int gizmo_y = viewport_y + gizmo_margin;
	const float center_x = 0.5f;
	const float center_y = 0.5f;
	const float axis_radius = 0.31f;
	const float axis_thickness = 0.028f;
	const float label_size = 0.10f;
	const float label_offset = 0.08f;
	struct vec4 background_color;
	struct vec4 center_color;

	if (!source || !IsVspaceSource(source))
		return;
	if (viewport_cx <= 0 || viewport_cy <= 0)
		return;

	if (!GetVspaceCameraBasis(source, basis))
		return;

	solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	color_param = solid ? gs_effect_get_param_by_name(solid, "color") : nullptr;
	if (!solid || !color_param)
		return;

	axes[0].label = 'x';
	axes[0].screen_x = basis.right_x;
	axes[0].screen_y = basis.up_x;
	axes[0].depth = basis.forward_x;
	vec4_set(&axes[0].color, 0.95f, 0.32f, 0.32f, 1.0f);

	axes[1].label = 'y';
	axes[1].screen_x = basis.right_y;
	axes[1].screen_y = basis.up_y;
	axes[1].depth = basis.forward_y;
	vec4_set(&axes[1].color, 0.36f, 0.88f, 0.38f, 1.0f);

	axes[2].label = 'z';
	axes[2].screen_x = basis.right_z;
	axes[2].screen_y = basis.up_z;
	axes[2].depth = basis.forward_z;
	vec4_set(&axes[2].color, 0.38f, 0.55f, 0.98f, 1.0f);

	sort(axes.begin(), axes.end(), [](const GizmoAxis &a, const GizmoAxis &b) { return a.depth > b.depth; });

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();

	gs_set_viewport(gizmo_x, gizmo_y, gizmo_size, gizmo_size);
	/*
	 * Keep overlay coordinates consistent with OBS preview space:
	 * x grows right, y grows down.
	 */
	gs_ortho(0.0f, 1.0f, 0.0f, 1.0f, -100.0f, 100.0f);

	vec4_set(&background_color, 0.05f, 0.06f, 0.08f, 0.58f);
	gs_effect_set_vec4(color_param, &background_color);
	while (gs_effect_loop(solid, "Solid"))
		DrawGizmoQuad(0.02f, 0.02f, 0.96f, 0.96f);

	for (const GizmoAxis &axis : axes) {
		const float end_x = center_x + axis.screen_x * axis_radius;
		const float end_y = center_y - axis.screen_y * axis_radius;
		float dir_x = end_x - center_x;
		float dir_y = end_y - center_y;
		const float dir_len = sqrtf(dir_x * dir_x + dir_y * dir_y);
		float label_x;
		float label_y;

		gs_effect_set_vec4(color_param, &axis.color);
		while (gs_effect_loop(solid, "Solid"))
			DrawGizmoLine(center_x, center_y, end_x, end_y, axis_thickness);

		while (gs_effect_loop(solid, "Solid"))
			DrawGizmoQuad(end_x - 0.015f, end_y - 0.015f, 0.03f, 0.03f);

		if (dir_len > 0.0001f) {
			dir_x /= dir_len;
			dir_y /= dir_len;
		} else {
			dir_x = 0.0f;
			dir_y = -1.0f;
		}

		label_x = end_x + dir_x * label_offset;
		label_y = end_y + dir_y * label_offset;
		DrawGizmoLabelGlyph(solid, color_param, axis.label, label_x, label_y, label_size, axis_thickness * 0.65f,
				    &axis.color);
	}

	vec4_set(&center_color, 0.93f, 0.93f, 0.93f, 1.0f);
	gs_effect_set_vec4(color_param, &center_color);
	while (gs_effect_loop(solid, "Solid"))
		DrawGizmoQuad(center_x - 0.017f, center_y - 0.017f, 0.034f, 0.034f);

	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();

	gs_blend_state_pop();
}
} // namespace

OBSBasicInteraction::OBSBasicInteraction(QWidget *parent, OBSSource source_)
	: QDialog(parent),
	  main(qobject_cast<OBSBasic *>(parent)),
	  ui(new Ui::OBSBasicInteraction),
	  source(source_),
	  removedSignal(obs_source_get_signal_handler(source), "remove", OBSBasicInteraction::SourceRemoved, this),
	  renamedSignal(obs_source_get_signal_handler(source), "rename", OBSBasicInteraction::SourceRenamed, this),
	  eventFilter(BuildEventFilter())
{
	int cx = (int)config_get_int(App()->GetAppConfig(), "InteractionWindow", "cx");
	int cy = (int)config_get_int(App()->GetAppConfig(), "InteractionWindow", "cy");

	Qt::WindowFlags flags = windowFlags();
	flags &= ~Qt::WindowContextHelpButtonHint;
	flags |= Qt::WindowSystemMenuHint;
	flags |= Qt::WindowMinMaxButtonsHint;
	setWindowFlags(flags);

	ui->setupUi(this);
	{
		QLabel *interactionHint = new QLabel(QTStr("Basic.InteractionWindow.Hint"), this);
		interactionHint->setObjectName("interactionHint");
		interactionHint->setWordWrap(true);
		ui->verticalLayout->insertWidget(0, interactionHint);
	}

	ui->preview->setMouseTracking(true);
	ui->preview->setFocusPolicy(Qt::StrongFocus);
	ui->preview->installEventFilter(eventFilter.get());

	if (cx > 400 && cy > 400)
		resize(cx, cy);

	const char *name = obs_source_get_name(source);
	setWindowTitle(QTStr("Basic.InteractionWindow").arg(QT_UTF8(name)));

	auto addDrawCallback = [this]() {
		obs_display_add_draw_callback(ui->preview->GetDisplay(), OBSBasicInteraction::DrawPreview, this);
	};

	connect(ui->preview, &OBSQTDisplay::DisplayCreated, this, addDrawCallback);
}

OBSBasicInteraction::~OBSBasicInteraction()
{
	// since QT fakes a mouse movement while destructing a widget
	// remove our event filter
	ui->preview->removeEventFilter(eventFilter.get());
}

OBSEventFilter *OBSBasicInteraction::BuildEventFilter()
{
	return new OBSEventFilter([this](QObject *, QEvent *event) {
		switch (event->type()) {
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonRelease:
		case QEvent::MouseButtonDblClick:
			return this->HandleMouseClickEvent(static_cast<QMouseEvent *>(event));
		case QEvent::MouseMove:
		case QEvent::Enter:
		case QEvent::Leave:
			return this->HandleMouseMoveEvent(static_cast<QMouseEvent *>(event));

		case QEvent::Wheel:
			return this->HandleMouseWheelEvent(static_cast<QWheelEvent *>(event));
		case QEvent::FocusIn:
		case QEvent::FocusOut:
			return this->HandleFocusEvent(static_cast<QFocusEvent *>(event));
		case QEvent::KeyPress:
		case QEvent::KeyRelease:
			return this->HandleKeyEvent(static_cast<QKeyEvent *>(event));
		default:
			return false;
		}
	});
}

void OBSBasicInteraction::SourceRemoved(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<OBSBasicInteraction *>(data), "close");
}

void OBSBasicInteraction::SourceRenamed(void *data, calldata_t *params)
{
	const char *name = calldata_string(params, "new_name");
	QString title = QTStr("Basic.InteractionWindow").arg(QT_UTF8(name));

	QMetaObject::invokeMethod(static_cast<OBSBasicProperties *>(data), "setWindowTitle", Q_ARG(QString, title));
}

void OBSBasicInteraction::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	OBSBasicInteraction *window = static_cast<OBSBasicInteraction *>(data);
	gs_effect_t *solid = nullptr;
	gs_eparam_t *color = nullptr;
	struct vec4 background_color;

	if (!window->source)
		return;

	uint32_t sourceCX = max(obs_source_get_width(window->source), 1u);
	uint32_t sourceCY = max(obs_source_get_height(window->source), 1u);

	int x, y;
	int newCX, newCY;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, cx, cy, x, y, scale);

	newCX = int(scale * float(sourceCX));
	newCY = int(scale * float(sourceCY));

	gs_viewport_push();
	gs_projection_push();
	const bool previous = gs_set_linear_srgb(true);

	solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	color = solid ? gs_effect_get_param_by_name(solid, "color") : nullptr;
	if (solid && color) {
		vec4_from_rgba_srgb(&background_color, 0xFF101010);
		gs_ortho(0.0f, float(cx), 0.0f, float(cy), -100.0f, 100.0f);
		gs_set_viewport(0, 0, (int)cx, (int)cy);
		gs_effect_set_vec4(color, &background_color);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, cx, cy);
	}

	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, newCX, newCY);
	DrawVspaceGrid(window->source, sourceCX, sourceCY);
	DrawVspaceBoundingBox(window->source, sourceCX, sourceCY);
	SetVspaceInspectRenderMode(window->source, true);
	obs_source_video_render(window->source);
	SetVspaceInspectRenderMode(window->source, false);
	DrawVspaceGizmo(window->source, x, y, newCX, newCY);

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();
}

void OBSBasicInteraction::closeEvent(QCloseEvent *event)
{
	QDialog::closeEvent(event);
	if (!event->isAccepted())
		return;

	config_set_int(App()->GetAppConfig(), "InteractionWindow", "cx", width());
	config_set_int(App()->GetAppConfig(), "InteractionWindow", "cy", height());

	obs_display_remove_draw_callback(ui->preview->GetDisplay(), OBSBasicInteraction::DrawPreview, this);
}

bool OBSBasicInteraction::nativeEvent(const QByteArray &, void *message, qintptr *)
{
#ifdef _WIN32
	const MSG &msg = *static_cast<MSG *>(message);
	switch (msg.message) {
	case WM_MOVE:
		for (OBSQTDisplay *const display : findChildren<OBSQTDisplay *>()) {
			display->OnMove();
		}
		break;
	case WM_DISPLAYCHANGE:
		for (OBSQTDisplay *const display : findChildren<OBSQTDisplay *>()) {
			display->OnDisplayChange();
		}
	}
#else
	UNUSED_PARAMETER(message);
#endif

	return false;
}

static int TranslateQtKeyboardEventModifiers(QInputEvent *event, bool mouseEvent)
{
	int obsModifiers = INTERACT_NONE;

	if (event->modifiers().testFlag(Qt::ShiftModifier))
		obsModifiers |= INTERACT_SHIFT_KEY;
	if (event->modifiers().testFlag(Qt::AltModifier))
		obsModifiers |= INTERACT_ALT_KEY;
#ifdef __APPLE__
	// Mac: Meta = Control, Control = Command
	if (event->modifiers().testFlag(Qt::ControlModifier))
		obsModifiers |= INTERACT_COMMAND_KEY;
	if (event->modifiers().testFlag(Qt::MetaModifier))
		obsModifiers |= INTERACT_CONTROL_KEY;
#else
	// Handle windows key? Can a browser even trap that key?
	if (event->modifiers().testFlag(Qt::ControlModifier))
		obsModifiers |= INTERACT_CONTROL_KEY;
#endif

	if (!mouseEvent) {
		if (event->modifiers().testFlag(Qt::KeypadModifier))
			obsModifiers |= INTERACT_IS_KEY_PAD;
	}

	return obsModifiers;
}

static int TranslateQtMouseEventModifiers(QMouseEvent *event)
{
	int modifiers = TranslateQtKeyboardEventModifiers(event, true);

	if (event->buttons().testFlag(Qt::LeftButton))
		modifiers |= INTERACT_MOUSE_LEFT;
	if (event->buttons().testFlag(Qt::MiddleButton))
		modifiers |= INTERACT_MOUSE_MIDDLE;
	if (event->buttons().testFlag(Qt::RightButton))
		modifiers |= INTERACT_MOUSE_RIGHT;

	return modifiers;
}

bool OBSBasicInteraction::GetSourceRelativeXY(int mouseX, int mouseY, int &relX, int &relY)
{
	float pixelRatio = devicePixelRatioF();
	int mouseXscaled = (int)roundf(mouseX * pixelRatio);
	int mouseYscaled = (int)roundf(mouseY * pixelRatio);

	QSize size = GetPixelSize(ui->preview);

	uint32_t sourceCX = max(obs_source_get_width(source), 1u);
	uint32_t sourceCY = max(obs_source_get_height(source), 1u);

	int x, y;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, size.width(), size.height(), x, y, scale);

	if (x > 0) {
		relX = int(float(mouseXscaled - x) / scale);
		relY = int(float(mouseYscaled / scale));
	} else {
		relX = int(float(mouseXscaled / scale));
		relY = int(float(mouseYscaled - y) / scale);
	}

	// Confirm mouse is inside the source
	if (relX < 0 || relX > int(sourceCX))
		return false;
	if (relY < 0 || relY > int(sourceCY))
		return false;

	return true;
}

bool OBSBasicInteraction::HandleMouseClickEvent(QMouseEvent *event)
{
	bool mouseUp = event->type() == QEvent::MouseButtonRelease;
	int clickCount = 1;
	if (event->type() == QEvent::MouseButtonDblClick)
		clickCount = 2;

	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);

	int32_t button = 0;

	switch (event->button()) {
	case Qt::LeftButton:
		button = MOUSE_LEFT;
		break;
	case Qt::MiddleButton:
		button = MOUSE_MIDDLE;
		break;
	case Qt::RightButton:
		button = MOUSE_RIGHT;
		break;
	default:
		blog(LOG_WARNING, "unknown button type %d", event->button());
		return false;
	}

	// Why doesn't this work?
	//if (event->flags().testFlag(Qt::MouseEventCreatedDoubleClick))
	//	clickCount = 2;

	QPoint pos = event->pos();
	bool insideSource = GetSourceRelativeXY(pos.x(), pos.y(), mouseEvent.x, mouseEvent.y);

	if (IsVspaceSource(source) && button == MOUSE_MIDDLE) {
		if (!mouseUp) {
			ui->preview->grabMouse();
		} else if (QWidget::mouseGrabber() == ui->preview) {
			ui->preview->releaseMouse();
		}
	}

	if (mouseUp || insideSource)
		obs_source_send_mouse_click(source, &mouseEvent, button, mouseUp, clickCount);

	return true;
}

bool OBSBasicInteraction::HandleMouseMoveEvent(QMouseEvent *event)
{
	struct obs_mouse_event mouseEvent = {};

	bool mouseLeave = event->type() == QEvent::Leave;
	const bool vspaceSource = IsVspaceSource(source);
	const bool middleHeld = (QGuiApplication::mouseButtons() & Qt::MiddleButton) != 0;

	if (mouseLeave && vspaceSource && middleHeld)
		return true;

	if (!mouseLeave) {
		mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);
		QPoint pos = event->pos();
		const bool insideSource = GetSourceRelativeXY(pos.x(), pos.y(), mouseEvent.x, mouseEvent.y);
		mouseLeave = !insideSource;
		if (mouseLeave && vspaceSource && middleHeld)
			mouseLeave = false;
	}

	obs_source_send_mouse_move(source, &mouseEvent, mouseLeave);

	return true;
}

bool OBSBasicInteraction::HandleMouseWheelEvent(QWheelEvent *event)
{
	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtKeyboardEventModifiers(event, true);

	int xDelta = 0;
	int yDelta = 0;

	const QPoint angleDelta = event->angleDelta();
	if (!event->pixelDelta().isNull()) {
		if (angleDelta.x())
			xDelta = event->pixelDelta().x();
		else
			yDelta = event->pixelDelta().y();
	} else {
		if (angleDelta.x())
			xDelta = angleDelta.x();
		else
			yDelta = angleDelta.y();
	}

	const QPointF position = event->position();
	const int x = position.x();
	const int y = position.y();

	if (GetSourceRelativeXY(x, y, mouseEvent.x, mouseEvent.y)) {
		obs_source_send_mouse_wheel(source, &mouseEvent, xDelta, yDelta);
	}

	return true;
}

bool OBSBasicInteraction::HandleFocusEvent(QFocusEvent *event)
{
	bool focus = event->type() == QEvent::FocusIn;

	if (!focus && QWidget::mouseGrabber() == ui->preview)
		ui->preview->releaseMouse();

	obs_source_send_focus(source, focus);

	return true;
}

bool OBSBasicInteraction::HandleKeyEvent(QKeyEvent *event)
{
	struct obs_key_event keyEvent;

	QByteArray text = event->text().toUtf8();
	keyEvent.modifiers = TranslateQtKeyboardEventModifiers(event, false);
	keyEvent.text = text.data();
	keyEvent.native_modifiers = event->nativeModifiers();
	keyEvent.native_scancode = event->nativeScanCode();
	keyEvent.native_vkey = event->nativeVirtualKey();

	bool keyUp = event->type() == QEvent::KeyRelease;

	obs_source_send_key_click(source, &keyEvent, keyUp);

	return true;
}

void OBSBasicInteraction::Init()
{
	show();
}
