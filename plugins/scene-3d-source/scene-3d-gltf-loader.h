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

#pragma once

#include <obs.h>

#include <stddef.h>
#include <stdint.h>

enum scene_3d_gltf_error_code {
	SCENE_3D_GLTF_SUCCESS = 0,
	SCENE_3D_GLTF_ERROR_INVALID_ARGUMENT,
	SCENE_3D_GLTF_ERROR_IO,
	SCENE_3D_GLTF_ERROR_PARSE,
	SCENE_3D_GLTF_ERROR_UNSUPPORTED,
	SCENE_3D_GLTF_ERROR_DRACO_DECODER_UNAVAILABLE,
	SCENE_3D_GLTF_ERROR_DRACO_DECODE_FAILED,
	SCENE_3D_GLTF_ERROR_ACCESSOR_DECODE_FAILED,
};

enum scene_3d_decode_path {
	SCENE_3D_DECODE_PATH_ACCESSOR = 0,
	SCENE_3D_DECODE_PATH_DRACO,
};

struct scene_3d_cpu_primitive_payload {
	enum scene_3d_decode_path decode_path;
	bool used_draco_extension;

	float *positions;
	float *normals;
	float *texcoords;
	uint32_t *indices;

	size_t vertex_count;
	size_t index_count;

	char *base_color_texture;
};

struct scene_3d_cpu_mesh_payload {
	char *name;
	struct scene_3d_cpu_primitive_payload *primitives;
	size_t primitive_count;
};

struct scene_3d_cpu_payload {
	struct scene_3d_cpu_mesh_payload *meshes;
	size_t mesh_count;
};

struct scene_3d_gltf_load_options {
	bool draco_enabled;
	const char *draco_decoder;
};

struct scene_3d_gltf_error {
	enum scene_3d_gltf_error_code code;
	char *message;
};

const char *scene_3d_gltf_error_to_string(enum scene_3d_gltf_error_code code);

void scene_3d_gltf_clear_error(struct scene_3d_gltf_error *error);
void scene_3d_gltf_free_cpu_payload(struct scene_3d_cpu_payload *payload);

bool scene_3d_gltf_load_cpu_payload(const char *model_path, struct scene_3d_cpu_payload *payload,
				    const struct scene_3d_gltf_load_options *options,
				    struct scene_3d_gltf_error *error);

bool scene_3d_gltf_model_uses_draco(const char *model_path);
