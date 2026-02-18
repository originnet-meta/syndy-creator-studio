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

enum vspace_gltf_error_code {
	VSPACE_GLTF_SUCCESS = 0,
	VSPACE_GLTF_ERROR_INVALID_ARGUMENT,
	VSPACE_GLTF_ERROR_IO,
	VSPACE_GLTF_ERROR_PARSE,
	VSPACE_GLTF_ERROR_UNSUPPORTED,
	VSPACE_GLTF_ERROR_DRACO_DECODER_UNAVAILABLE,
	VSPACE_GLTF_ERROR_DRACO_DECODE_FAILED,
	VSPACE_GLTF_ERROR_ACCESSOR_DECODE_FAILED,
};

enum vspace_decode_path {
	VSPACE_DECODE_PATH_ACCESSOR = 0,
	VSPACE_DECODE_PATH_DRACO,
};

struct vspace_cpu_primitive_payload {
	enum vspace_decode_path decode_path;
	bool used_draco_extension;
	int32_t material_index;

	float *positions;
	float *normals;
	float *texcoords;
	uint32_t *indices;

	size_t vertex_count;
	size_t index_count;

	char *base_color_texture;
};

struct vspace_cpu_mesh_payload {
	char *name;
	struct vspace_cpu_primitive_payload *primitives;
	size_t primitive_count;
};

struct vspace_cpu_payload {
	struct vspace_cpu_mesh_payload *meshes;
	size_t mesh_count;
};

struct vspace_gltf_load_options {
	bool draco_enabled;
	const char *draco_decoder;
};

struct vspace_gltf_error {
	enum vspace_gltf_error_code code;
	char *message;
};

const char *vspace_gltf_error_to_string(enum vspace_gltf_error_code code);

void vspace_gltf_clear_error(struct vspace_gltf_error *error);
void vspace_gltf_free_cpu_payload(struct vspace_cpu_payload *payload);

bool vspace_gltf_load_cpu_payload(const char *model_path, struct vspace_cpu_payload *payload,
				    const struct vspace_gltf_load_options *options,
				    struct vspace_gltf_error *error);

bool vspace_gltf_model_uses_draco(const char *model_path);
