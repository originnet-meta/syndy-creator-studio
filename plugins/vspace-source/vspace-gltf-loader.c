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

#include "vspace-gltf-loader.h"

#include <jansson.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <util/dstr.h>
#include <util/platform.h>

#define GLTF_MAGIC 0x46546C67u
#define GLTF_VERSION_2 2u
#define GLTF_CHUNK_JSON 0x4E4F534Au
#define GLTF_CHUNK_BIN 0x004E4942u

#define GLTF_COMPONENT_UNSIGNED_BYTE 5121
#define GLTF_COMPONENT_UNSIGNED_SHORT 5123
#define GLTF_COMPONENT_UNSIGNED_INT 5125
#define GLTF_COMPONENT_FLOAT 5126

#define GLTF_MODE_TRIANGLES 4

struct raw_buffer {
	uint8_t *data;
	size_t size;
};

struct accessor_view {
	const uint8_t *data;
	size_t count;
	size_t stride;
	size_t comp_count;
	uint32_t comp_type;
};

struct loader_ctx {
	obs_data_t *root;
	struct raw_buffer *buffers;
	size_t buffer_count;
	struct dstr base_dir;
	bool draco_enabled;
	const char *draco_decoder;
};

struct node_mat4 {
	float m[16];
};

static uint32_t read_u32_le(const uint8_t *data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool safe_add(size_t a, size_t b, size_t *out)
{
	if (a > SIZE_MAX - b)
		return false;
	*out = a + b;
	return true;
}

static bool safe_mul(size_t a, size_t b, size_t *out)
{
	if (a && b > SIZE_MAX / a)
		return false;
	*out = a * b;
	return true;
}

static void mat4_identity(struct node_mat4 *out)
{
	size_t i;

	if (!out)
		return;

	for (i = 0; i < 16; i++)
		out->m[i] = 0.0f;

	out->m[0] = 1.0f;
	out->m[5] = 1.0f;
	out->m[10] = 1.0f;
	out->m[15] = 1.0f;
}

static void mat4_mul(struct node_mat4 *out, const struct node_mat4 *a, const struct node_mat4 *b)
{
	struct node_mat4 result;
	size_t row;
	size_t col;

	if (!out || !a || !b)
		return;

	for (col = 0; col < 4; col++) {
		for (row = 0; row < 4; row++) {
			const float a0 = a->m[(0 * 4) + row];
			const float a1 = a->m[(1 * 4) + row];
			const float a2 = a->m[(2 * 4) + row];
			const float a3 = a->m[(3 * 4) + row];
			const float b0 = b->m[(col * 4) + 0];
			const float b1 = b->m[(col * 4) + 1];
			const float b2 = b->m[(col * 4) + 2];
			const float b3 = b->m[(col * 4) + 3];

			result.m[(col * 4) + row] = (a0 * b0) + (a1 * b1) + (a2 * b2) + (a3 * b3);
		}
	}

	*out = result;
}

static void mat4_from_trs(struct node_mat4 *out, const float translation[3], const float rotation[4],
			  const float scale[3])
{
	const float x = rotation[0];
	const float y = rotation[1];
	const float z = rotation[2];
	const float w = rotation[3];
	const float xx = x * x;
	const float yy = y * y;
	const float zz = z * z;
	const float xy = x * y;
	const float xz = x * z;
	const float yz = y * z;
	const float xw = x * w;
	const float yw = y * w;
	const float zw = z * w;

	if (!out)
		return;

	mat4_identity(out);

	/* glTF uses column-major matrices with column vectors: M = T * R * S */
	out->m[0] = (1.0f - 2.0f * (yy + zz)) * scale[0];
	out->m[1] = (2.0f * (xy + zw)) * scale[0];
	out->m[2] = (2.0f * (xz - yw)) * scale[0];
	out->m[3] = 0.0f;

	out->m[4] = (2.0f * (xy - zw)) * scale[1];
	out->m[5] = (1.0f - 2.0f * (xx + zz)) * scale[1];
	out->m[6] = (2.0f * (yz + xw)) * scale[1];
	out->m[7] = 0.0f;

	out->m[8] = (2.0f * (xz + yw)) * scale[2];
	out->m[9] = (2.0f * (yz - xw)) * scale[2];
	out->m[10] = (1.0f - 2.0f * (xx + yy)) * scale[2];
	out->m[11] = 0.0f;

	out->m[12] = translation[0];
	out->m[13] = translation[1];
	out->m[14] = translation[2];
	out->m[15] = 1.0f;
}

static void mat4_transform_position(const struct node_mat4 *matrix, const float in_pos[3], float out_pos[3])
{
	const float x = in_pos[0];
	const float y = in_pos[1];
	const float z = in_pos[2];

	if (!matrix || !out_pos)
		return;

	out_pos[0] = matrix->m[0] * x + matrix->m[4] * y + matrix->m[8] * z + matrix->m[12];
	out_pos[1] = matrix->m[1] * x + matrix->m[5] * y + matrix->m[9] * z + matrix->m[13];
	out_pos[2] = matrix->m[2] * x + matrix->m[6] * y + matrix->m[10] * z + matrix->m[14];
}

static bool mat4_compute_normal_matrix(const struct node_mat4 *matrix, float out3x3[9])
{
	float a00;
	float a01;
	float a02;
	float a10;
	float a11;
	float a12;
	float a20;
	float a21;
	float a22;
	float c00;
	float c01;
	float c02;
	float c10;
	float c11;
	float c12;
	float c20;
	float c21;
	float c22;
	float det;
	float inv_det;

	if (!matrix || !out3x3)
		return false;

	a00 = matrix->m[0];
	a01 = matrix->m[4];
	a02 = matrix->m[8];
	a10 = matrix->m[1];
	a11 = matrix->m[5];
	a12 = matrix->m[9];
	a20 = matrix->m[2];
	a21 = matrix->m[6];
	a22 = matrix->m[10];

	c00 = (a11 * a22) - (a12 * a21);
	c01 = (a02 * a21) - (a01 * a22);
	c02 = (a01 * a12) - (a02 * a11);
	c10 = (a12 * a20) - (a10 * a22);
	c11 = (a00 * a22) - (a02 * a20);
	c12 = (a02 * a10) - (a00 * a12);
	c20 = (a10 * a21) - (a11 * a20);
	c21 = (a01 * a20) - (a00 * a21);
	c22 = (a00 * a11) - (a01 * a10);
	det = (a00 * c00) + (a01 * c10) + (a02 * c20);
	inv_det = fabsf(det) > 1e-8f ? (1.0f / det) : 0.0f;

	if (inv_det == 0.0f)
		return false;

	/* transpose(inverse(A)) where A is the upper-left 3x3 of the world matrix */
	out3x3[0] = c00 * inv_det;
	out3x3[1] = c10 * inv_det;
	out3x3[2] = c20 * inv_det;
	out3x3[3] = c01 * inv_det;
	out3x3[4] = c11 * inv_det;
	out3x3[5] = c21 * inv_det;
	out3x3[6] = c02 * inv_det;
	out3x3[7] = c12 * inv_det;
	out3x3[8] = c22 * inv_det;
	return true;
}

static void mat3_transform_vector(const float matrix3x3[9], const float in_vec[3], float out_vec[3])
{
	const float x = in_vec[0];
	const float y = in_vec[1];
	const float z = in_vec[2];

	if (!matrix3x3 || !out_vec)
		return;

	out_vec[0] = matrix3x3[0] * x + matrix3x3[1] * y + matrix3x3[2] * z;
	out_vec[1] = matrix3x3[3] * x + matrix3x3[4] * y + matrix3x3[5] * z;
	out_vec[2] = matrix3x3[6] * x + matrix3x3[7] * y + matrix3x3[8] * z;
}

static void normalize_vec3(float vec[3])
{
	const float len = sqrtf(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]);

	if (len > 1e-8f) {
		const float inv_len = 1.0f / len;

		vec[0] *= inv_len;
		vec[1] *= inv_len;
		vec[2] *= inv_len;
		return;
	}

	vec[0] = 0.0f;
	vec[1] = 0.0f;
	vec[2] = 1.0f;
}

/*
 * glTF uses a Y-up convention. The vspace viewport is aligned to Blender-like
 * Z-up world-space. Rotate all decoded geometry +90 deg around +X:
 *   (x, y, z) -> (x, -z, y)
 */
static void convert_gltf_y_up_to_vspace_z_up(float vec[3])
{
	const float x = vec[0];
	const float y = vec[1];
	const float z = vec[2];

	vec[0] = x;
	vec[1] = -z;
	vec[2] = y;
}

static void convert_payload_axes_y_up_to_z_up(struct vspace_cpu_payload *payload)
{
	size_t mesh_idx;

	if (!payload || !payload->meshes)
		return;

	for (mesh_idx = 0; mesh_idx < payload->mesh_count; mesh_idx++) {
		struct vspace_cpu_mesh_payload *mesh = payload->meshes + mesh_idx;
		size_t prim_idx;

		if (!mesh->primitives)
			continue;

		for (prim_idx = 0; prim_idx < mesh->primitive_count; prim_idx++) {
			struct vspace_cpu_primitive_payload *prim = mesh->primitives + prim_idx;
			size_t vtx_idx;

			if (prim->positions) {
				for (vtx_idx = 0; vtx_idx < prim->vertex_count; vtx_idx++)
					convert_gltf_y_up_to_vspace_z_up(prim->positions + (vtx_idx * 3));
			}

			if (prim->normals) {
				for (vtx_idx = 0; vtx_idx < prim->vertex_count; vtx_idx++) {
					float *normal = prim->normals + (vtx_idx * 3);

					convert_gltf_y_up_to_vspace_z_up(normal);
					normalize_vec3(normal);
				}
			}
		}
	}
}

static void set_error(struct vspace_gltf_error *error, enum vspace_gltf_error_code code, const char *fmt, ...)
{
	va_list args;
	struct dstr text;

	if (!error)
		return;

	vspace_gltf_clear_error(error);
	error->code = code;
	if (!fmt)
		return;

	dstr_init(&text);
	va_start(args, fmt);
	dstr_vprintf(&text, fmt, args);
	va_end(args);

	error->message = text.array ? bstrdup(text.array) : NULL;
	dstr_free(&text);
}

static void set_error_errno(struct vspace_gltf_error *error, enum vspace_gltf_error_code code, const char *path,
			    const char *action)
{
	set_error(error, code, "%s (%s): %s", action, path, strerror(errno));
}

static bool get_required_index(obs_data_t *object, const char *field, size_t *value, struct vspace_gltf_error *error,
			       const char *context)
{
	int64_t raw;

	if (!obs_data_has_user_value(object, field)) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "%s missing field '%s'", context, field);
		return false;
	}

	raw = obs_data_get_int(object, field);
	if (raw < 0 || (uint64_t)raw > SIZE_MAX) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "%s field '%s' out of range", context, field);
		return false;
	}

	*value = (size_t)raw;
	return true;
}

static bool get_optional_index(obs_data_t *object, const char *field, size_t *value, size_t default_value,
			       struct vspace_gltf_error *error, const char *context)
{
	if (!obs_data_has_user_value(object, field)) {
		*value = default_value;
		return true;
	}
	return get_required_index(object, field, value, error, context);
}

static int32_t get_optional_material_index(obs_data_t *primitive)
{
	int64_t raw;

	if (!primitive || !obs_data_has_user_value(primitive, "material"))
		return -1;

	raw = obs_data_get_int(primitive, "material");
	if (raw < 0 || raw > INT32_MAX)
		return -1;

	return (int32_t)raw;
}

static bool get_array_item(obs_data_t *root, const char *array_name, size_t index, obs_data_t **item,
			   struct vspace_gltf_error *error)
{
	obs_data_array_t *array = obs_data_get_array(root, array_name);
	size_t count;

	*item = NULL;
	if (!array) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Missing top-level array '%s'", array_name);
		return false;
	}

	count = obs_data_array_count(array);
	if (index >= count) {
		obs_data_array_release(array);
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Index %zu out of range for '%s' (count=%zu)", index,
			  array_name, count);
		return false;
	}

	*item = obs_data_array_item(array, index);
	obs_data_array_release(array);
	return true;
}

static size_t component_count(const char *type)
{
	if (!type)
		return 0;
	if (strcmp(type, "SCALAR") == 0)
		return 1;
	if (strcmp(type, "VEC2") == 0)
		return 2;
	if (strcmp(type, "VEC3") == 0)
		return 3;
	if (strcmp(type, "VEC4") == 0)
		return 4;
	if (strcmp(type, "MAT2") == 0)
		return 4;
	if (strcmp(type, "MAT3") == 0)
		return 9;
	if (strcmp(type, "MAT4") == 0)
		return 16;
	return 0;
}

static size_t component_size(uint32_t comp_type)
{
	switch (comp_type) {
	case GLTF_COMPONENT_UNSIGNED_BYTE:
		return 1;
	case GLTF_COMPONENT_UNSIGNED_SHORT:
		return 2;
	case GLTF_COMPONENT_UNSIGNED_INT:
	case GLTF_COMPONENT_FLOAT:
		return 4;
	default:
		return 0;
	}
}

static bool is_absolute_path(const char *path)
{
	if (!path || !*path)
		return false;
	if (path[0] == '/' || path[0] == '\\')
		return true;
	return isalpha((unsigned char)path[0]) && path[1] == ':';
}

static bool uri_has_scheme(const char *uri)
{
	const char *colon;
	const char *cursor;

	if (!uri || !*uri)
		return false;
	if (isalpha((unsigned char)uri[0]) && uri[1] == ':')
		return false;

	colon = strchr(uri, ':');
	if (!colon)
		return false;

	for (cursor = uri; cursor < colon; cursor++) {
		unsigned char c = (unsigned char)*cursor;
		if (!isalnum(c) && c != '+' && c != '-' && c != '.')
			return false;
	}
	return true;
}

static bool is_data_uri(const char *uri)
{
	return uri && astrcmpi_n(uri, "data:", 5) == 0;
}

static void build_base_dir(const char *path, struct dstr *base_dir)
{
	const char *slash = strrchr(path, '/');
	const char *backslash = strrchr(path, '\\');
	const char *sep = NULL;

	dstr_free(base_dir);
	dstr_init(base_dir);

	if (slash && backslash)
		sep = slash > backslash ? slash : backslash;
	else if (slash)
		sep = slash;
	else
		sep = backslash;

	if (sep)
		dstr_ncopy(base_dir, path, (size_t)(sep - path + 1));
}

static char *resolve_uri_path(const struct dstr *base_dir, const char *uri, struct vspace_gltf_error *error)
{
	struct dstr full;

	if (!uri || !*uri) {
		set_error(error, VSPACE_GLTF_ERROR_IO, "Empty URI");
		return NULL;
	}

	if (uri_has_scheme(uri) && !is_data_uri(uri)) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "Unsupported URI scheme: %s", uri);
		return NULL;
	}

	if (is_absolute_path(uri))
		return bstrdup(uri);

	dstr_init(&full);
	if (base_dir && base_dir->array)
		dstr_copy(&full, base_dir->array);

	if (full.len && full.array[full.len - 1] != '/' && full.array[full.len - 1] != '\\')
		dstr_cat_ch(&full, '/');
	dstr_cat(&full, uri);
	return full.array;
}

static bool read_file_bytes(const char *path, uint8_t **bytes, size_t *size, struct vspace_gltf_error *error)
{
	FILE *file = NULL;
	int64_t file_size_i64;
	size_t file_size;
	size_t read_bytes;
	uint8_t *buffer = NULL;

	*bytes = NULL;
	*size = 0;

	file_size_i64 = os_get_file_size(path);
	if (file_size_i64 < 0) {
		set_error_errno(error, VSPACE_GLTF_ERROR_IO, path, "Could not stat file");
		return false;
	}
	if ((uint64_t)file_size_i64 > SIZE_MAX) {
		set_error(error, VSPACE_GLTF_ERROR_IO, "File too large: %s", path);
		return false;
	}

	file_size = (size_t)file_size_i64;
	file = os_fopen(path, "rb");
	if (!file) {
		set_error_errno(error, VSPACE_GLTF_ERROR_IO, path, "Could not open file");
		return false;
	}

	if (file_size) {
		buffer = bmalloc(file_size);
		read_bytes = fread(buffer, 1, file_size, file);
		if (read_bytes != file_size) {
			fclose(file);
			bfree(buffer);
			set_error_errno(error, VSPACE_GLTF_ERROR_IO, path, "Could not read file");
			return false;
		}
	}

	fclose(file);
	*bytes = buffer;
	*size = file_size;
	return true;
}

static int b64_value(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

static bool decode_base64(const char *input, uint8_t **output, size_t *output_size, struct vspace_gltf_error *error)
{
	size_t in_len = strlen(input);
	size_t cap = ((in_len + 3) / 4) * 3;
	uint8_t *dst = cap ? bmalloc(cap) : NULL;
	size_t out_len = 0;
	int acc = 0;
	int bits = -8;
	const char *p;

	for (p = input; *p; p++) {
		int val;
		unsigned char c = (unsigned char)*p;

		if (c == '=')
			break;
		if (c == '\r' || c == '\n' || c == '\t' || c == ' ')
			continue;

		val = b64_value((char)c);
		if (val < 0) {
			bfree(dst);
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "Invalid base64 payload");
			return false;
		}

		acc = (acc << 6) | val;
		bits += 6;
		if (bits >= 0) {
			dst[out_len++] = (uint8_t)((acc >> bits) & 0xFF);
			bits -= 8;
		}
	}

	*output = dst;
	*output_size = out_len;
	return true;
}

static bool decode_data_uri(const char *uri, uint8_t **bytes, size_t *size, struct vspace_gltf_error *error)
{
	const char *comma;
	const char *base64_tag;

	if (!is_data_uri(uri)) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Not a data URI");
		return false;
	}

	comma = strchr(uri, ',');
	if (!comma) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Malformed data URI");
		return false;
	}

	base64_tag = astrstri(uri, ";base64");
	if (!base64_tag || base64_tag > comma) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "Only base64 data URI is supported");
		return false;
	}

	return decode_base64(comma + 1, bytes, size, error);
}

static bool parse_json_model(const char *model_path, obs_data_t **root, uint8_t **glb_bin, size_t *glb_bin_size,
			     struct vspace_gltf_error *error)
{
	const char *ext;

	*root = NULL;
	*glb_bin = NULL;
	*glb_bin_size = 0;

	ext = strrchr(model_path, '.');
	if (!ext) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "Model path has no extension: %s", model_path);
		return false;
	}

	if (astrcmpi(ext, ".gltf") == 0) {
		*root = obs_data_create_from_json_file(model_path);
		if (!*root) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "Failed to parse glTF JSON: %s", model_path);
			return false;
		}
		return true;
	}

	if (astrcmpi(ext, ".glb") == 0) {
		uint8_t *file_data = NULL;
		size_t file_size = 0;
		size_t off = 12;
		const uint8_t *json_chunk = NULL;
		size_t json_chunk_size = 0;
		const uint8_t *bin_chunk = NULL;
		size_t bin_chunk_size = 0;
		char *json_text;
		uint32_t version;
		uint32_t length;

		if (!read_file_bytes(model_path, &file_data, &file_size, error))
			return false;

		if (file_size < 12 || read_u32_le(file_data) != GLTF_MAGIC) {
			bfree(file_data);
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "Invalid GLB header: %s", model_path);
			return false;
		}

		version = read_u32_le(file_data + 4);
		if (version != GLTF_VERSION_2) {
			bfree(file_data);
			set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "Unsupported GLB version %u", version);
			return false;
		}

		length = read_u32_le(file_data + 8);
		if ((size_t)length > file_size || length < 12) {
			bfree(file_data);
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "Invalid GLB length in header");
			return false;
		}

		while (off + 8 <= (size_t)length) {
			uint32_t chunk_len = read_u32_le(file_data + off);
			uint32_t chunk_type = read_u32_le(file_data + off + 4);
			size_t chunk_data_off;
			size_t chunk_end;

			if (!safe_add(off, 8, &chunk_data_off) || !safe_add(chunk_data_off, chunk_len, &chunk_end) ||
			    chunk_end > (size_t)length) {
				bfree(file_data);
				set_error(error, VSPACE_GLTF_ERROR_PARSE, "Malformed GLB chunk");
				return false;
			}

			if (chunk_type == GLTF_CHUNK_JSON && !json_chunk) {
				json_chunk = file_data + chunk_data_off;
				json_chunk_size = chunk_len;
			} else if (chunk_type == GLTF_CHUNK_BIN && !bin_chunk) {
				bin_chunk = file_data + chunk_data_off;
				bin_chunk_size = chunk_len;
			}

			off = chunk_end;
		}

		if (!json_chunk || !json_chunk_size) {
			bfree(file_data);
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "GLB JSON chunk is missing");
			return false;
		}

		json_text = bmalloc(json_chunk_size + 1);
		memcpy(json_text, json_chunk, json_chunk_size);
		json_text[json_chunk_size] = '\0';
		*root = obs_data_create_from_json(json_text);
		bfree(json_text);

		if (!*root) {
			bfree(file_data);
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "Failed to parse GLB JSON chunk");
			return false;
		}

		if (bin_chunk && bin_chunk_size) {
			*glb_bin = bmemdup(bin_chunk, bin_chunk_size);
			*glb_bin_size = bin_chunk_size;
		}

		bfree(file_data);
		return true;
	}

	set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "Unsupported extension: %s", ext);
	return false;
}

static bool load_model_json_root(const char *model_path, json_t **root_json, struct vspace_gltf_error *error)
{
	const char *ext;
	uint8_t *file_data = NULL;
	size_t file_size = 0;
	json_t *parsed = NULL;
	json_error_t jerror;

	if (!root_json)
		return false;

	*root_json = NULL;
	ext = strrchr(model_path, '.');
	if (!ext) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "Model path has no extension: %s", model_path);
		return false;
	}

	if (!read_file_bytes(model_path, &file_data, &file_size, error))
		return false;

	if (astrcmpi(ext, ".gltf") == 0) {
		parsed = json_loadb((const char *)file_data, file_size, 0, &jerror);
		if (!parsed) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "Failed to parse glTF JSON at line %d: %s",
				  jerror.line, jerror.text ? jerror.text : "unknown error");
			bfree(file_data);
			return false;
		}

		*root_json = parsed;
		bfree(file_data);
		return true;
	}

	if (astrcmpi(ext, ".glb") == 0) {
		size_t off = 12;
		const uint8_t *json_chunk = NULL;
		size_t json_chunk_size = 0;
		uint32_t version;
		uint32_t length;

		if (file_size < 12 || read_u32_le(file_data) != GLTF_MAGIC) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "Invalid GLB header: %s", model_path);
			bfree(file_data);
			return false;
		}

		version = read_u32_le(file_data + 4);
		if (version != GLTF_VERSION_2) {
			set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "Unsupported GLB version %u", version);
			bfree(file_data);
			return false;
		}

		length = read_u32_le(file_data + 8);
		if ((size_t)length > file_size || length < 12) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "Invalid GLB length in header");
			bfree(file_data);
			return false;
		}

		while (off + 8 <= (size_t)length) {
			uint32_t chunk_len = read_u32_le(file_data + off);
			uint32_t chunk_type = read_u32_le(file_data + off + 4);
			size_t chunk_data_off;
			size_t chunk_end;

			if (!safe_add(off, 8, &chunk_data_off) || !safe_add(chunk_data_off, chunk_len, &chunk_end) ||
			    chunk_end > (size_t)length) {
				set_error(error, VSPACE_GLTF_ERROR_PARSE, "Malformed GLB chunk");
				bfree(file_data);
				return false;
			}

			if (chunk_type == GLTF_CHUNK_JSON && !json_chunk) {
				json_chunk = file_data + chunk_data_off;
				json_chunk_size = chunk_len;
			}

			off = chunk_end;
		}

		if (!json_chunk || !json_chunk_size) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "GLB JSON chunk is missing");
			bfree(file_data);
			return false;
		}

		parsed = json_loadb((const char *)json_chunk, json_chunk_size, 0, &jerror);
		if (!parsed) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "Failed to parse GLB JSON at line %d: %s",
				  jerror.line, jerror.text ? jerror.text : "unknown error");
			bfree(file_data);
			return false;
		}

		*root_json = parsed;
		bfree(file_data);
		return true;
	}

	set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "Unsupported extension: %s", ext);
	bfree(file_data);
	return false;
}

static bool json_value_to_index(json_t *value, size_t *out_index)
{
	json_int_t raw;

	if (!value || !out_index || !json_is_integer(value))
		return false;

	raw = json_integer_value(value);
	if (raw < 0 || (uint64_t)raw > SIZE_MAX)
		return false;

	*out_index = (size_t)raw;
	return true;
}

static bool json_value_to_float(json_t *value, float *out)
{
	double number;

	if (!value || !out)
		return false;

	if (json_is_integer(value)) {
		number = (double)json_integer_value(value);
	} else if (json_is_real(value)) {
		number = json_real_value(value);
	} else {
		return false;
	}

	*out = (float)number;
	return true;
}

static bool json_parse_float_array(json_t *array, size_t expected_count, float *out_values)
{
	size_t i;

	if (!array || !out_values || !json_is_array(array) || json_array_size(array) != expected_count)
		return false;

	for (i = 0; i < expected_count; i++) {
		if (!json_value_to_float(json_array_get(array, i), out_values + i))
			return false;
	}

	return true;
}

static bool parse_node_local_matrix(json_t *node, struct node_mat4 *out, struct vspace_gltf_error *error)
{
	json_t *matrix = NULL;
	json_t *translation = NULL;
	json_t *rotation = NULL;
	json_t *scale = NULL;
	float t[3] = {0.0f, 0.0f, 0.0f};
	float r[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	float s[3] = {1.0f, 1.0f, 1.0f};
	float qlen;
	size_t i;

	if (!node || !out || !json_is_object(node)) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Invalid glTF node object");
		return false;
	}

	matrix = json_object_get(node, "matrix");
	if (matrix) {
		if (!json_is_array(matrix) || json_array_size(matrix) != 16) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "node.matrix must contain 16 numeric values");
			return false;
		}

		for (i = 0; i < 16; i++) {
			if (!json_value_to_float(json_array_get(matrix, i), out->m + i)) {
				set_error(error, VSPACE_GLTF_ERROR_PARSE, "node.matrix contains non-numeric values");
				return false;
			}
		}
		return true;
	}

	translation = json_object_get(node, "translation");
	if (translation && !json_parse_float_array(translation, 3, t)) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "node.translation must contain 3 numeric values");
		return false;
	}

	rotation = json_object_get(node, "rotation");
	if (rotation && !json_parse_float_array(rotation, 4, r)) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "node.rotation must contain 4 numeric values");
		return false;
	}

	scale = json_object_get(node, "scale");
	if (scale && !json_parse_float_array(scale, 3, s)) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "node.scale must contain 3 numeric values");
		return false;
	}

	qlen = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
	if (qlen > 1e-8f) {
		const float inv_qlen = 1.0f / qlen;

		r[0] *= inv_qlen;
		r[1] *= inv_qlen;
		r[2] *= inv_qlen;
		r[3] *= inv_qlen;
	} else {
		r[0] = 0.0f;
		r[1] = 0.0f;
		r[2] = 0.0f;
		r[3] = 1.0f;
	}

	mat4_from_trs(out, t, r, s);
	return true;
}

static void free_single_mesh_payload(struct vspace_cpu_mesh_payload *mesh)
{
	size_t p;

	if (!mesh)
		return;

	bfree(mesh->name);
	mesh->name = NULL;

	for (p = 0; p < mesh->primitive_count; p++) {
		struct vspace_cpu_primitive_payload *prim = mesh->primitives + p;

		bfree(prim->positions);
		prim->positions = NULL;
		bfree(prim->normals);
		prim->normals = NULL;
		bfree(prim->texcoords);
		prim->texcoords = NULL;
		bfree(prim->indices);
		prim->indices = NULL;
		bfree(prim->base_color_texture);
		prim->base_color_texture = NULL;
	}

	bfree(mesh->primitives);
	mesh->primitives = NULL;
	mesh->primitive_count = 0;
}

static bool duplicate_primitive_with_transform(const struct vspace_cpu_primitive_payload *src,
					       struct vspace_cpu_primitive_payload *dst,
					       const struct node_mat4 *world)
{
	size_t positions_size = 0;
	size_t texcoords_size = 0;
	size_t indices_size = 0;
	float normal_matrix[9];
	float linear_matrix[9];
	bool has_normal_matrix = false;
	size_t i;

	if (!src || !dst || !world)
		return false;

	memset(dst, 0, sizeof(*dst));
	dst->decode_path = src->decode_path;
	dst->used_draco_extension = src->used_draco_extension;
	dst->material_index = src->material_index;
	dst->vertex_count = src->vertex_count;
	dst->index_count = src->index_count;

	if (src->base_color_texture && *src->base_color_texture) {
		dst->base_color_texture = bstrdup(src->base_color_texture);
		if (!dst->base_color_texture)
			return false;
	}

	if (!safe_mul(src->vertex_count, sizeof(float) * 3, &positions_size))
		return false;
	if (!safe_mul(src->vertex_count, sizeof(float) * 2, &texcoords_size))
		return false;
	if (!safe_mul(src->index_count, sizeof(uint32_t), &indices_size))
		return false;

	if (src->positions && positions_size > 0) {
		dst->positions = bmalloc(positions_size);
		if (!dst->positions)
			return false;

		for (i = 0; i < src->vertex_count; i++) {
			const float *in_pos = src->positions + (i * 3);
			float *out_pos = dst->positions + (i * 3);

			mat4_transform_position(world, in_pos, out_pos);
		}
	}

	linear_matrix[0] = world->m[0];
	linear_matrix[1] = world->m[4];
	linear_matrix[2] = world->m[8];
	linear_matrix[3] = world->m[1];
	linear_matrix[4] = world->m[5];
	linear_matrix[5] = world->m[9];
	linear_matrix[6] = world->m[2];
	linear_matrix[7] = world->m[6];
	linear_matrix[8] = world->m[10];
	has_normal_matrix = mat4_compute_normal_matrix(world, normal_matrix);

	if (src->normals && positions_size > 0) {
		dst->normals = bmalloc(positions_size);
		if (!dst->normals)
			return false;

		for (i = 0; i < src->vertex_count; i++) {
			const float *in_normal = src->normals + (i * 3);
			float *out_normal = dst->normals + (i * 3);

			if (has_normal_matrix)
				mat3_transform_vector(normal_matrix, in_normal, out_normal);
			else
				mat3_transform_vector(linear_matrix, in_normal, out_normal);
			normalize_vec3(out_normal);
		}
	}

	if (src->texcoords && texcoords_size > 0) {
		dst->texcoords = bmemdup(src->texcoords, texcoords_size);
		if (!dst->texcoords)
			return false;
	}

	if (src->indices && indices_size > 0) {
		dst->indices = bmemdup(src->indices, indices_size);
		if (!dst->indices)
			return false;
	}

	return true;
}

static bool append_transformed_mesh_instance(const struct vspace_cpu_mesh_payload *src_mesh, const struct node_mat4 *world,
					     const char *node_name, struct vspace_cpu_payload *out_payload,
					     size_t *mesh_capacity)
{
	struct vspace_cpu_mesh_payload mesh_copy;
	struct vspace_cpu_mesh_payload *new_meshes;
	size_t p;
	size_t new_capacity;

	if (!src_mesh || !world || !out_payload || !mesh_capacity)
		return false;

	memset(&mesh_copy, 0, sizeof(mesh_copy));

	if (node_name && *node_name)
		mesh_copy.name = bstrdup(node_name);
	else if (src_mesh->name && *src_mesh->name)
		mesh_copy.name = bstrdup(src_mesh->name);

	mesh_copy.primitive_count = src_mesh->primitive_count;
	if (mesh_copy.primitive_count > 0) {
		mesh_copy.primitives = bzalloc(sizeof(*mesh_copy.primitives) * mesh_copy.primitive_count);
		if (!mesh_copy.primitives) {
			free_single_mesh_payload(&mesh_copy);
			return false;
		}

		for (p = 0; p < mesh_copy.primitive_count; p++) {
			if (!duplicate_primitive_with_transform(src_mesh->primitives + p, mesh_copy.primitives + p, world)) {
				free_single_mesh_payload(&mesh_copy);
				return false;
			}
		}
	}

	if (out_payload->mesh_count >= *mesh_capacity) {
		new_capacity = *mesh_capacity ? (*mesh_capacity * 2) : 16;
		if (new_capacity <= out_payload->mesh_count)
			new_capacity = out_payload->mesh_count + 1;

		new_meshes = brealloc(out_payload->meshes, sizeof(*new_meshes) * new_capacity);
		if (!new_meshes) {
			free_single_mesh_payload(&mesh_copy);
			return false;
		}

		out_payload->meshes = new_meshes;
		*mesh_capacity = new_capacity;
	}

	out_payload->meshes[out_payload->mesh_count] = mesh_copy;
	out_payload->mesh_count++;
	return true;
}

struct node_transform_build_ctx {
	json_t *nodes;
	size_t node_count;
	const struct vspace_cpu_payload *decoded_payload;
	struct vspace_cpu_payload *out_payload;
	size_t out_mesh_capacity;
	bool *visit_stack;
};

static bool traverse_node_transform(struct node_transform_build_ctx *ctx, size_t node_index,
				    const struct node_mat4 *parent_world, struct vspace_gltf_error *error)
{
	json_t *node;
	json_t *mesh_value;
	json_t *children;
	json_t *name_value;
	struct node_mat4 local;
	struct node_mat4 world;
	size_t mesh_index;
	size_t child_count;
	size_t i;
	const char *node_name = NULL;

	if (!ctx || !parent_world || node_index >= ctx->node_count) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Node index out of range: %zu", node_index);
		return false;
	}

	if (ctx->visit_stack[node_index]) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Cycle detected in glTF node graph at node[%zu]", node_index);
		return false;
	}

	node = json_array_get(ctx->nodes, node_index);
	if (!json_is_object(node)) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "nodes[%zu] is not a valid object", node_index);
		return false;
	}

	ctx->visit_stack[node_index] = true;

	if (!parse_node_local_matrix(node, &local, error)) {
		ctx->visit_stack[node_index] = false;
		return false;
	}

	mat4_mul(&world, parent_world, &local);

	mesh_value = json_object_get(node, "mesh");
	if (mesh_value) {
		if (!json_value_to_index(mesh_value, &mesh_index)) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "nodes[%zu].mesh is not a valid non-negative index",
				  node_index);
			ctx->visit_stack[node_index] = false;
			return false;
		}

		if (mesh_index >= ctx->decoded_payload->mesh_count) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE,
				  "nodes[%zu].mesh index %zu out of range (decoded meshes=%zu)", node_index,
				  mesh_index, ctx->decoded_payload->mesh_count);
			ctx->visit_stack[node_index] = false;
			return false;
		}

		name_value = json_object_get(node, "name");
		if (json_is_string(name_value))
			node_name = json_string_value(name_value);

		if (!append_transformed_mesh_instance(ctx->decoded_payload->meshes + mesh_index, &world, node_name,
						      ctx->out_payload, &ctx->out_mesh_capacity)) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "Failed to append transformed mesh instance at node[%zu]",
				  node_index);
			ctx->visit_stack[node_index] = false;
			return false;
		}
	}

	children = json_object_get(node, "children");
	if (children) {
		if (!json_is_array(children)) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "nodes[%zu].children must be an array", node_index);
			ctx->visit_stack[node_index] = false;
			return false;
		}

		child_count = json_array_size(children);
		for (i = 0; i < child_count; i++) {
			size_t child_index;

			if (!json_value_to_index(json_array_get(children, i), &child_index)) {
				set_error(error, VSPACE_GLTF_ERROR_PARSE,
					  "nodes[%zu].children[%zu] is not a valid node index", node_index, i);
				ctx->visit_stack[node_index] = false;
				return false;
			}

			if (!traverse_node_transform(ctx, child_index, &world, error)) {
				ctx->visit_stack[node_index] = false;
				return false;
			}
		}
	}

	ctx->visit_stack[node_index] = false;
	return true;
}

static bool apply_node_transforms_to_payload(const char *model_path, const struct vspace_cpu_payload *decoded_payload,
					     struct vspace_cpu_payload *transformed_payload, bool *applied,
					     struct vspace_gltf_error *error)
{
	json_t *json_root = NULL;
	json_t *nodes = NULL;
	json_t *scenes = NULL;
	json_t *scene_nodes = NULL;
	struct node_transform_build_ctx ctx;
	struct node_mat4 identity;
	bool *has_parent = NULL;
	bool transformed_any = false;
	size_t i;
	size_t node_count;

	if (applied)
		*applied = false;

	if (!model_path || !decoded_payload || !transformed_payload)
		return true;

	memset(transformed_payload, 0, sizeof(*transformed_payload));

	if (!load_model_json_root(model_path, &json_root, error))
		return false;

	nodes = json_object_get(json_root, "nodes");
	if (!json_is_array(nodes)) {
		json_decref(json_root);
		return true;
	}

	node_count = json_array_size(nodes);
	if (node_count == 0) {
		json_decref(json_root);
		return true;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.nodes = nodes;
	ctx.node_count = node_count;
	ctx.decoded_payload = decoded_payload;
	ctx.out_payload = transformed_payload;
	ctx.visit_stack = bzalloc(sizeof(bool) * node_count);
	has_parent = bzalloc(sizeof(bool) * node_count);
	if (!ctx.visit_stack || !has_parent) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Failed to allocate node transform traversal buffers");
		bfree(ctx.visit_stack);
		bfree(has_parent);
		json_decref(json_root);
		return false;
	}

	for (i = 0; i < node_count; i++) {
		json_t *node = json_array_get(nodes, i);
		json_t *children = json_is_object(node) ? json_object_get(node, "children") : NULL;
		size_t child_count;
		size_t c;

		if (!children)
			continue;

		if (!json_is_array(children)) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "nodes[%zu].children must be an array", i);
			goto fail;
		}

		child_count = json_array_size(children);
		for (c = 0; c < child_count; c++) {
			size_t child_index;

			if (!json_value_to_index(json_array_get(children, c), &child_index)) {
				set_error(error, VSPACE_GLTF_ERROR_PARSE, "nodes[%zu].children[%zu] is invalid", i, c);
				goto fail;
			}
			if (child_index >= node_count) {
				set_error(error, VSPACE_GLTF_ERROR_PARSE,
					  "nodes[%zu].children[%zu] index %zu out of range", i, c, child_index);
				goto fail;
			}

			has_parent[child_index] = true;
		}
	}

	mat4_identity(&identity);
	scenes = json_object_get(json_root, "scenes");
	if (json_is_array(scenes) && json_array_size(scenes) > 0) {
		size_t scene_index = 0;
		json_t *scene_index_value = json_object_get(json_root, "scene");

		if (scene_index_value) {
			if (!json_value_to_index(scene_index_value, &scene_index)) {
				set_error(error, VSPACE_GLTF_ERROR_PARSE, "Top-level 'scene' is not a valid index");
				goto fail;
			}
			if (scene_index >= json_array_size(scenes)) {
				set_error(error, VSPACE_GLTF_ERROR_PARSE,
					  "Top-level 'scene' index %zu out of range (count=%zu)", scene_index,
					  json_array_size(scenes));
				goto fail;
			}
		}

		{
			json_t *scene = json_array_get(scenes, scene_index);

			scene_nodes = json_is_object(scene) ? json_object_get(scene, "nodes") : NULL;
		}

		if (scene_nodes) {
			size_t root_count;

			if (!json_is_array(scene_nodes)) {
				set_error(error, VSPACE_GLTF_ERROR_PARSE, "scene[%zu].nodes must be an array", scene_index);
				goto fail;
			}

			root_count = json_array_size(scene_nodes);
			for (i = 0; i < root_count; i++) {
				size_t root_index;

				if (!json_value_to_index(json_array_get(scene_nodes, i), &root_index)) {
					set_error(error, VSPACE_GLTF_ERROR_PARSE,
						  "scene[%zu].nodes[%zu] is not a valid node index", scene_index, i);
					goto fail;
				}

				if (!traverse_node_transform(&ctx, root_index, &identity, error))
					goto fail;
				transformed_any = true;
			}
		}
	}

	if (!transformed_any) {
		for (i = 0; i < node_count; i++) {
			if (has_parent[i])
				continue;

			if (!traverse_node_transform(&ctx, i, &identity, error))
				goto fail;
			transformed_any = true;
		}
	}

	if (!transformed_any) {
		for (i = 0; i < node_count; i++) {
			if (!traverse_node_transform(&ctx, i, &identity, error))
				goto fail;
			transformed_any = true;
		}
	}

	if (applied)
		*applied = transformed_any && (transformed_payload->mesh_count > 0);

	if (transformed_any && transformed_payload->mesh_count > 0) {
		blog(LOG_INFO,
		     "[vspace-source:gltf-loader] Applied node transforms: nodes=%zu, mesh_instances=%zu, "
		     "decoded_meshes=%zu",
		     node_count, transformed_payload->mesh_count, decoded_payload->mesh_count);
	}

	bfree(ctx.visit_stack);
	bfree(has_parent);
	json_decref(json_root);
	return true;

fail:
	vspace_gltf_free_cpu_payload(transformed_payload);
	bfree(ctx.visit_stack);
	bfree(has_parent);
	json_decref(json_root);
	return false;
}

static bool model_root_uses_draco_extension(obs_data_t *root)
{
	obs_data_array_t *meshes;
	size_t mesh_count;
	size_t m;

	if (!root)
		return false;

	meshes = obs_data_get_array(root, "meshes");
	if (!meshes)
		return false;

	mesh_count = obs_data_array_count(meshes);
	for (m = 0; m < mesh_count; m++) {
		obs_data_t *mesh = obs_data_array_item(meshes, m);
		obs_data_array_t *primitives = mesh ? obs_data_get_array(mesh, "primitives") : NULL;
		size_t primitive_count = primitives ? obs_data_array_count(primitives) : 0;
		size_t p;

		for (p = 0; p < primitive_count; p++) {
			obs_data_t *primitive = obs_data_array_item(primitives, p);
			obs_data_t *extensions = primitive ? obs_data_get_obj(primitive, "extensions") : NULL;
			bool has_draco = extensions && obs_data_has_user_value(extensions, "KHR_draco_mesh_compression");

			obs_data_release(extensions);
			obs_data_release(primitive);

			if (has_draco) {
				obs_data_array_release(primitives);
				obs_data_release(mesh);
				obs_data_array_release(meshes);
				return true;
			}
		}

		obs_data_array_release(primitives);
		obs_data_release(mesh);
	}

	obs_data_array_release(meshes);
	return false;
}

static void free_raw_buffers(struct loader_ctx *ctx)
{
	size_t i;

	if (!ctx || !ctx->buffers)
		return;

	for (i = 0; i < ctx->buffer_count; i++)
		bfree(ctx->buffers[i].data);

	bfree(ctx->buffers);
	ctx->buffers = NULL;
	ctx->buffer_count = 0;
}

static bool resolve_buffers(struct loader_ctx *ctx, const uint8_t *glb_bin, size_t glb_bin_size,
			    struct vspace_gltf_error *error)
{
	obs_data_array_t *buffers_arr = obs_data_get_array(ctx->root, "buffers");
	size_t count;
	size_t i;

	if (!buffers_arr) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Missing top-level array 'buffers'");
		return false;
	}

	count = obs_data_array_count(buffers_arr);
	if (!count) {
		obs_data_array_release(buffers_arr);
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "No buffers in glTF file");
		return false;
	}

	ctx->buffers = bzalloc(sizeof(*ctx->buffers) * count);
	ctx->buffer_count = count;

	for (i = 0; i < count; i++) {
		obs_data_t *buffer_obj = obs_data_array_item(buffers_arr, i);
		size_t expected_size = 0;
		const char *uri = obs_data_get_string(buffer_obj, "uri");

		if (!get_required_index(buffer_obj, "byteLength", &expected_size, error, "buffer")) {
			obs_data_release(buffer_obj);
			obs_data_array_release(buffers_arr);
			return false;
		}

		if (uri && *uri) {
			if (is_data_uri(uri)) {
				if (!decode_data_uri(uri, &ctx->buffers[i].data, &ctx->buffers[i].size, error)) {
					obs_data_release(buffer_obj);
					obs_data_array_release(buffers_arr);
					return false;
				}
			} else {
				char *path = resolve_uri_path(&ctx->base_dir, uri, error);
				if (!path || !read_file_bytes(path, &ctx->buffers[i].data, &ctx->buffers[i].size,
						      error)) {
					bfree(path);
					obs_data_release(buffer_obj);
					obs_data_array_release(buffers_arr);
					return false;
				}
				bfree(path);
			}
		} else if (i == 0 && glb_bin && glb_bin_size) {
			ctx->buffers[i].data = bmemdup(glb_bin, glb_bin_size);
			ctx->buffers[i].size = glb_bin_size;
		} else {
			obs_data_release(buffer_obj);
			obs_data_array_release(buffers_arr);
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "buffer[%zu] has no URI and no GLB BIN fallback", i);
			return false;
		}

		if (expected_size > ctx->buffers[i].size) {
			obs_data_release(buffer_obj);
			obs_data_array_release(buffers_arr);
			set_error(error, VSPACE_GLTF_ERROR_PARSE,
				  "buffer[%zu] byteLength (%zu) exceeds data size (%zu)", i, expected_size,
				  ctx->buffers[i].size);
			return false;
		}

		if (expected_size < ctx->buffers[i].size)
			ctx->buffers[i].size = expected_size;

		obs_data_release(buffer_obj);
	}

	obs_data_array_release(buffers_arr);
	return true;
}

static bool get_accessor_view(struct loader_ctx *ctx, size_t accessor_index, struct accessor_view *view,
			      struct vspace_gltf_error *error)
{
	obs_data_t *accessor = NULL;
	obs_data_t *buffer_view = NULL;
	size_t buffer_view_index;
	size_t accessor_offset = 0;
	size_t accessor_count;
	size_t buffer_index;
	size_t buffer_view_offset = 0;
	size_t buffer_view_length;
	size_t buffer_view_stride = 0;
	size_t elem_comp_count;
	size_t elem_comp_size;
	size_t elem_size;
	size_t data_bytes = 0;
	size_t last_off = 0;
	const char *type;
	uint32_t comp_type;
	bool ok = false;
	int64_t comp_raw;

	memset(view, 0, sizeof(*view));

	if (!get_array_item(ctx->root, "accessors", accessor_index, &accessor, error))
		return false;

	if (!get_required_index(accessor, "bufferView", &buffer_view_index, error, "accessor"))
		goto cleanup;
	if (!get_optional_index(accessor, "byteOffset", &accessor_offset, 0, error, "accessor"))
		goto cleanup;
	if (!get_required_index(accessor, "count", &accessor_count, error, "accessor"))
		goto cleanup;

	comp_raw = obs_data_get_int(accessor, "componentType");
	if (comp_raw < 0 || (uint64_t)comp_raw > UINT32_MAX) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "accessor[%zu] invalid componentType", accessor_index);
		goto cleanup;
	}
	comp_type = (uint32_t)comp_raw;
	elem_comp_size = component_size(comp_type);
	if (!elem_comp_size) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] unsupported componentType=%u",
			  accessor_index, comp_type);
		goto cleanup;
	}

	type = obs_data_get_string(accessor, "type");
	elem_comp_count = component_count(type);
	if (!elem_comp_count) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] unsupported type '%s'",
			  accessor_index, type ? type : "(null)");
		goto cleanup;
	}

	if (!get_array_item(ctx->root, "bufferViews", buffer_view_index, &buffer_view, error))
		goto cleanup;
	if (!get_required_index(buffer_view, "buffer", &buffer_index, error, "bufferView"))
		goto cleanup;
	if (!get_optional_index(buffer_view, "byteOffset", &buffer_view_offset, 0, error, "bufferView"))
		goto cleanup;
	if (!get_required_index(buffer_view, "byteLength", &buffer_view_length, error, "bufferView"))
		goto cleanup;
	if (!get_optional_index(buffer_view, "byteStride", &buffer_view_stride, 0, error, "bufferView"))
		goto cleanup;

	if (buffer_index >= ctx->buffer_count) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "bufferView[%zu] references invalid buffer index %zu",
			  buffer_view_index, buffer_index);
		goto cleanup;
	}

	if (!safe_mul(elem_comp_count, elem_comp_size, &elem_size)) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "accessor[%zu] element size overflow", accessor_index);
		goto cleanup;
	}

	if (!buffer_view_stride)
		buffer_view_stride = elem_size;
	else if (buffer_view_stride < elem_size) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "accessor[%zu] byteStride (%zu) < element size (%zu)",
			  accessor_index, buffer_view_stride, elem_size);
		goto cleanup;
	}

	if (buffer_view_offset > ctx->buffers[buffer_index].size ||
	    buffer_view_length > ctx->buffers[buffer_index].size - buffer_view_offset) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE,
			  "bufferView[%zu] exceeds buffer[%zu] bounds (offset=%zu len=%zu size=%zu)",
			  buffer_view_index, buffer_index, buffer_view_offset, buffer_view_length,
			  ctx->buffers[buffer_index].size);
		goto cleanup;
	}

	if (accessor_count) {
		if (!safe_mul(buffer_view_stride, accessor_count - 1, &last_off) || !safe_add(last_off, elem_size, &data_bytes) ||
		    accessor_offset > buffer_view_length || data_bytes > buffer_view_length - accessor_offset) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE,
				  "accessor[%zu] range exceeds bufferView[%zu]", accessor_index, buffer_view_index);
			goto cleanup;
		}
	}

	view->data = ctx->buffers[buffer_index].data + buffer_view_offset + accessor_offset;
	view->count = accessor_count;
	view->stride = buffer_view_stride;
	view->comp_count = elem_comp_count;
	view->comp_type = comp_type;
	ok = true;

cleanup:
	obs_data_release(accessor);
	obs_data_release(buffer_view);
	return ok;
}

static bool decode_float_accessor(struct loader_ctx *ctx, size_t accessor_index, size_t expected_comp, float **out,
				  size_t *elem_count, struct vspace_gltf_error *error)
{
	struct accessor_view view;
	size_t total_values;
	size_t total_bytes;
	size_t i;
	float *dst;

	*out = NULL;
	*elem_count = 0;

	if (!get_accessor_view(ctx, accessor_index, &view, error))
		return false;
	if (view.comp_type != GLTF_COMPONENT_FLOAT) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] must use FLOAT component type",
			  accessor_index);
		return false;
	}
	if (view.comp_count != expected_comp) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] expected %zu components, got %zu",
			  accessor_index, expected_comp, view.comp_count);
		return false;
	}

	if (!view.count)
		return true;

	if (!safe_mul(view.count, expected_comp, &total_values) || !safe_mul(total_values, sizeof(float), &total_bytes)) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "accessor[%zu] float decode size overflow", accessor_index);
		return false;
	}

	dst = bmalloc(total_bytes);
	for (i = 0; i < view.count; i++) {
		const uint8_t *src = view.data + (view.stride * i);
		memcpy(dst + (i * expected_comp), src, expected_comp * sizeof(float));
	}

	*out = dst;
	*elem_count = view.count;
	return true;
}

static bool decode_index_accessor(struct loader_ctx *ctx, size_t accessor_index, uint32_t **out, size_t *count,
				  struct vspace_gltf_error *error)
{
	struct accessor_view view;
	size_t bytes;
	size_t i;
	uint32_t *dst;

	*out = NULL;
	*count = 0;

	if (!get_accessor_view(ctx, accessor_index, &view, error))
		return false;
	if (view.comp_count != 1) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] index accessor must be SCALAR",
			  accessor_index);
		return false;
	}
	if (view.comp_type != GLTF_COMPONENT_UNSIGNED_BYTE && view.comp_type != GLTF_COMPONENT_UNSIGNED_SHORT &&
	    view.comp_type != GLTF_COMPONENT_UNSIGNED_INT) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] unsupported index componentType=%u",
			  accessor_index, view.comp_type);
		return false;
	}

	if (!view.count)
		return true;

	if (!safe_mul(view.count, sizeof(uint32_t), &bytes)) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "accessor[%zu] index decode size overflow", accessor_index);
		return false;
	}

	dst = bmalloc(bytes);
	for (i = 0; i < view.count; i++) {
		const uint8_t *src = view.data + (view.stride * i);
		if (view.comp_type == GLTF_COMPONENT_UNSIGNED_BYTE)
			dst[i] = src[0];
		else if (view.comp_type == GLTF_COMPONENT_UNSIGNED_SHORT)
			dst[i] = (uint32_t)(src[0] | (src[1] << 8));
		else
			memcpy(&dst[i], src, sizeof(uint32_t));
	}

	*out = dst;
	*count = view.count;
	return true;
}

static bool generate_indices(size_t vertex_count, uint32_t **out, size_t *count, struct vspace_gltf_error *error)
{
	uint32_t *dst;
	size_t bytes;
	size_t i;

	*out = NULL;
	*count = 0;

	if (!vertex_count)
		return true;
	if (vertex_count > UINT32_MAX) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "vertex count exceeds uint32 range: %zu", vertex_count);
		return false;
	}
	if (!safe_mul(vertex_count, sizeof(uint32_t), &bytes)) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "index buffer size overflow");
		return false;
	}

	dst = bmalloc(bytes);
	for (i = 0; i < vertex_count; i++)
		dst[i] = (uint32_t)i;

	*out = dst;
	*count = vertex_count;
	return true;
}

static char *resolve_base_color_texture(struct loader_ctx *ctx, obs_data_t *primitive, struct vspace_gltf_error *error)
{
	obs_data_array_t *materials = NULL;
	obs_data_array_t *textures = NULL;
	obs_data_array_t *images = NULL;
	obs_data_t *material = NULL;
	obs_data_t *pbr = NULL;
	obs_data_t *base_color_tex = NULL;
	obs_data_t *texture = NULL;
	obs_data_t *image = NULL;
	size_t material_idx;
	size_t texture_idx;
	size_t image_idx;
	size_t count;
	const char *uri;
	char *path = NULL;

	if (!obs_data_has_user_value(primitive, "material"))
		return NULL;
	if (!get_required_index(primitive, "material", &material_idx, error, "primitive"))
		return NULL;

	materials = obs_data_get_array(ctx->root, "materials");
	if (!materials)
		goto cleanup;

	count = obs_data_array_count(materials);
	if (material_idx >= count)
		goto cleanup;

	material = obs_data_array_item(materials, material_idx);
	pbr = obs_data_get_obj(material, "pbrMetallicRoughness");
	if (!pbr)
		goto cleanup;

	base_color_tex = obs_data_get_obj(pbr, "baseColorTexture");
	if (!base_color_tex)
		goto cleanup;

	if (!get_required_index(base_color_tex, "index", &texture_idx, error, "baseColorTexture"))
		goto cleanup;

	textures = obs_data_get_array(ctx->root, "textures");
	if (!textures)
		goto cleanup;

	count = obs_data_array_count(textures);
	if (texture_idx >= count)
		goto cleanup;

	texture = obs_data_array_item(textures, texture_idx);
	if (!get_required_index(texture, "source", &image_idx, error, "texture"))
		goto cleanup;

	images = obs_data_get_array(ctx->root, "images");
	if (!images)
		goto cleanup;

	count = obs_data_array_count(images);
	if (image_idx >= count)
		goto cleanup;

	image = obs_data_array_item(images, image_idx);
	if (obs_data_has_user_value(image, "bufferView")) {
		blog(LOG_WARNING,
		     "[vspace-source:gltf-loader] Embedded image bufferView is not supported yet. Texture skipped.");
		goto cleanup;
	}

	uri = obs_data_get_string(image, "uri");
	if (!uri || !*uri)
		goto cleanup;
	if (is_data_uri(uri)) {
		blog(LOG_WARNING,
		     "[vspace-source:gltf-loader] Data-URI image is not supported yet. Texture skipped.");
		goto cleanup;
	}

	path = resolve_uri_path(&ctx->base_dir, uri, error);

cleanup:
	obs_data_release(material);
	obs_data_release(pbr);
	obs_data_release(base_color_tex);
	obs_data_release(texture);
	obs_data_release(image);
	obs_data_array_release(materials);
	obs_data_array_release(textures);
	obs_data_array_release(images);
	return path;
}

static bool decode_accessor_primitive(struct loader_ctx *ctx, obs_data_t *primitive,
				      struct vspace_cpu_primitive_payload *out, bool draco_ext_present,
				      struct vspace_gltf_error *error)
{
	obs_data_t *attributes = NULL;
	size_t mode = GLTF_MODE_TRIANGLES;
	size_t pos_acc;
	size_t normal_acc = 0;
	size_t uv_acc = 0;
	size_t idx_acc = 0;
	bool has_normal;
	bool has_uv;
	bool has_idx;
	float *positions = NULL;
	float *normals = NULL;
	float *texcoords = NULL;
	uint32_t *indices = NULL;
	size_t vertex_count = 0;
	size_t normal_count = 0;
	size_t uv_count = 0;
	size_t index_count = 0;
	char *texture_path = NULL;
	bool ok = false;

	memset(out, 0, sizeof(*out));
	out->used_draco_extension = draco_ext_present;
	out->decode_path = VSPACE_DECODE_PATH_ACCESSOR;
	out->material_index = get_optional_material_index(primitive);

	if (!get_optional_index(primitive, "mode", &mode, GLTF_MODE_TRIANGLES, error, "primitive"))
		goto cleanup;
	if (mode != GLTF_MODE_TRIANGLES) {
		set_error(error, VSPACE_GLTF_ERROR_UNSUPPORTED, "Unsupported primitive mode %zu (only TRIANGLES=4)",
			  mode);
		goto cleanup;
	}

	attributes = obs_data_get_obj(primitive, "attributes");
	if (!attributes) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Primitive missing attributes object");
		goto cleanup;
	}
	if (!get_required_index(attributes, "POSITION", &pos_acc, error, "attributes"))
		goto cleanup;
	if (!decode_float_accessor(ctx, pos_acc, 3, &positions, &vertex_count, error))
		goto cleanup;

	has_normal = obs_data_has_user_value(attributes, "NORMAL");
	if (has_normal) {
		if (!get_required_index(attributes, "NORMAL", &normal_acc, error, "attributes"))
			goto cleanup;
		if (!decode_float_accessor(ctx, normal_acc, 3, &normals, &normal_count, error))
			goto cleanup;
		if (normal_count != vertex_count) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "NORMAL count (%zu) != POSITION count (%zu)",
				  normal_count, vertex_count);
			goto cleanup;
		}
	}

	has_uv = obs_data_has_user_value(attributes, "TEXCOORD_0");
	if (has_uv) {
		if (!get_required_index(attributes, "TEXCOORD_0", &uv_acc, error, "attributes"))
			goto cleanup;
		if (!decode_float_accessor(ctx, uv_acc, 2, &texcoords, &uv_count, error))
			goto cleanup;
		if (uv_count != vertex_count) {
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "TEXCOORD_0 count (%zu) != POSITION count (%zu)",
				  uv_count, vertex_count);
			goto cleanup;
		}
	}

	has_idx = obs_data_has_user_value(primitive, "indices");
	if (has_idx) {
		if (!get_required_index(primitive, "indices", &idx_acc, error, "primitive"))
			goto cleanup;
		if (!decode_index_accessor(ctx, idx_acc, &indices, &index_count, error))
			goto cleanup;
	} else if (!generate_indices(vertex_count, &indices, &index_count, error)) {
		goto cleanup;
	}

	texture_path = resolve_base_color_texture(ctx, primitive, error);
	if (error && error->code != VSPACE_GLTF_SUCCESS && !texture_path)
		goto cleanup;

	out->positions = positions;
	out->normals = normals;
	out->texcoords = texcoords;
	out->indices = indices;
	out->vertex_count = vertex_count;
	out->index_count = index_count;
	out->base_color_texture = texture_path;

	positions = NULL;
	normals = NULL;
	texcoords = NULL;
	indices = NULL;
	texture_path = NULL;
	ok = true;

cleanup:
	obs_data_release(attributes);
	bfree(positions);
	bfree(normals);
	bfree(texcoords);
	bfree(indices);
	bfree(texture_path);
	return ok;
}

static bool decode_draco_primitive(struct loader_ctx *ctx, obs_data_t *primitive, struct vspace_cpu_primitive_payload *out,
				   struct vspace_gltf_error *error)
{
	obs_data_t *extensions = NULL;
	obs_data_t *draco = NULL;
	size_t draco_buffer_view = 0;

	memset(out, 0, sizeof(*out));
	out->used_draco_extension = true;
	out->decode_path = VSPACE_DECODE_PATH_DRACO;
	out->material_index = get_optional_material_index(primitive);

	extensions = obs_data_get_obj(primitive, "extensions");
	draco = extensions ? obs_data_get_obj(extensions, "KHR_draco_mesh_compression") : NULL;
	if (!draco) {
		obs_data_release(draco);
		obs_data_release(extensions);
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Primitive does not contain KHR_draco_mesh_compression data");
		return false;
	}

	if (!get_required_index(draco, "bufferView", &draco_buffer_view, error, "KHR_draco_mesh_compression")) {
		obs_data_release(draco);
		obs_data_release(extensions);
		return false;
	}

#if defined(VSPACE_ENABLE_DRACO_DECODER)
	/* TODO: wire real Draco decoder implementation. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(out);
	obs_data_release(draco);
	obs_data_release(extensions);
	set_error(error, VSPACE_GLTF_ERROR_DRACO_DECODE_FAILED,
		  "Draco decode failed for bitstream bufferView index %zu", draco_buffer_view);
	return false;
#else
	UNUSED_PARAMETER(draco_buffer_view);
	if (obs_data_has_user_value(primitive, "attributes")) {
		blog(LOG_WARNING,
		     "[vspace-source:gltf-loader] Draco extension detected but decoder is unavailable. Using accessor fallback.");
		obs_data_release(draco);
		obs_data_release(extensions);
		return decode_accessor_primitive(ctx, primitive, out, true, error);
	}

	obs_data_release(draco);
	obs_data_release(extensions);
	UNUSED_PARAMETER(out);
	set_error(error, VSPACE_GLTF_ERROR_DRACO_DECODER_UNAVAILABLE,
		  "KHR_draco_mesh_compression requires a Draco decoder, but none is available");
	return false;
#endif
}

static bool decode_meshes(struct loader_ctx *ctx, struct vspace_cpu_payload *out, struct vspace_gltf_error *error)
{
	obs_data_array_t *meshes = obs_data_get_array(ctx->root, "meshes");
	size_t mesh_count;
	size_t m;

	if (!meshes) {
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "Missing top-level array 'meshes'");
		return false;
	}

	mesh_count = obs_data_array_count(meshes);
	if (!mesh_count) {
		obs_data_array_release(meshes);
		set_error(error, VSPACE_GLTF_ERROR_PARSE, "No meshes found in glTF");
		return false;
	}

	out->meshes = bzalloc(sizeof(*out->meshes) * mesh_count);
	out->mesh_count = mesh_count;

	for (m = 0; m < mesh_count; m++) {
		obs_data_t *mesh = obs_data_array_item(meshes, m);
		obs_data_array_t *primitives = obs_data_get_array(mesh, "primitives");
		const char *mesh_name = obs_data_get_string(mesh, "name");
		size_t prim_count;
		size_t p;

		if (mesh_name && *mesh_name)
			out->meshes[m].name = bstrdup(mesh_name);

		if (!primitives) {
			obs_data_release(mesh);
			obs_data_array_release(meshes);
			set_error(error, VSPACE_GLTF_ERROR_PARSE, "mesh[%zu] has no 'primitives' array", m);
			return false;
		}

		prim_count = obs_data_array_count(primitives);
		out->meshes[m].primitives = bzalloc(sizeof(*out->meshes[m].primitives) * prim_count);
		out->meshes[m].primitive_count = prim_count;

		for (p = 0; p < prim_count; p++) {
			obs_data_t *primitive = obs_data_array_item(primitives, p);
			obs_data_t *extensions = obs_data_get_obj(primitive, "extensions");
			obs_data_t *draco = extensions ? obs_data_get_obj(extensions, "KHR_draco_mesh_compression") : NULL;
			bool decoded;

			if (draco && ctx->draco_enabled) {
				decoded = decode_draco_primitive(ctx, primitive, &out->meshes[m].primitives[p], error);
			} else {
				if (draco && !ctx->draco_enabled) {
					blog(LOG_WARNING,
					     "[vspace-source:gltf-loader] Draco extension found but disabled by option '%s'. Using accessor fallback.",
					     ctx->draco_decoder ? ctx->draco_decoder : "auto");
				}
				decoded = decode_accessor_primitive(ctx, primitive, &out->meshes[m].primitives[p],
							    draco != NULL, error);
			}

			obs_data_release(draco);
			obs_data_release(extensions);
			obs_data_release(primitive);

			if (!decoded) {
				obs_data_array_release(primitives);
				obs_data_release(mesh);
				obs_data_array_release(meshes);
				return false;
			}

			{
				const struct vspace_cpu_primitive_payload *decoded_primitive =
					&out->meshes[m].primitives[p];
				const char *decode_path =
					decoded_primitive->decode_path == VSPACE_DECODE_PATH_DRACO ? "draco" : "accessor";
				const char *mesh_label = (mesh_name && *mesh_name) ? mesh_name : "(unnamed)";
				const char *texture_label = (decoded_primitive->base_color_texture &&
							    *decoded_primitive->base_color_texture)
								   ? decoded_primitive->base_color_texture
								   : "none";

				blog(LOG_INFO,
				     "[vspace-source:gltf-loader] Parsed mesh[%zu] '%s' primitive[%zu]: vertices=%zu, "
				     "indices=%zu, material=%d, decode=%s, texture=%s",
				     m, mesh_label, p, decoded_primitive->vertex_count, decoded_primitive->index_count,
				     decoded_primitive->material_index, decode_path, texture_label);
			}
		}

		obs_data_array_release(primitives);
		obs_data_release(mesh);
	}

	obs_data_array_release(meshes);
	return true;
}

const char *vspace_gltf_error_to_string(enum vspace_gltf_error_code code)
{
	switch (code) {
	case VSPACE_GLTF_SUCCESS:
		return "success";
	case VSPACE_GLTF_ERROR_INVALID_ARGUMENT:
		return "invalid_argument";
	case VSPACE_GLTF_ERROR_IO:
		return "io_error";
	case VSPACE_GLTF_ERROR_PARSE:
		return "parse_error";
	case VSPACE_GLTF_ERROR_UNSUPPORTED:
		return "unsupported";
	case VSPACE_GLTF_ERROR_DRACO_DECODER_UNAVAILABLE:
		return "draco_decoder_unavailable";
	case VSPACE_GLTF_ERROR_DRACO_DECODE_FAILED:
		return "draco_decode_failed";
	case VSPACE_GLTF_ERROR_ACCESSOR_DECODE_FAILED:
		return "accessor_decode_failed";
	default:
		return "unknown";
	}
}

void vspace_gltf_clear_error(struct vspace_gltf_error *error)
{
	if (!error)
		return;

	bfree(error->message);
	error->message = NULL;
	error->code = VSPACE_GLTF_SUCCESS;
}

void vspace_gltf_free_cpu_payload(struct vspace_cpu_payload *payload)
{
	size_t m;

	if (!payload)
		return;

	for (m = 0; m < payload->mesh_count; m++) {
		size_t p;
		struct vspace_cpu_mesh_payload *mesh = payload->meshes + m;

		bfree(mesh->name);
		for (p = 0; p < mesh->primitive_count; p++) {
			struct vspace_cpu_primitive_payload *prim = mesh->primitives + p;
			bfree(prim->positions);
			bfree(prim->normals);
			bfree(prim->texcoords);
			bfree(prim->indices);
			bfree(prim->base_color_texture);
		}
		bfree(mesh->primitives);
	}

	bfree(payload->meshes);
	memset(payload, 0, sizeof(*payload));
}

bool vspace_gltf_model_uses_draco(const char *model_path)
{
	obs_data_t *root = NULL;
	uint8_t *glb_bin = NULL;
	size_t glb_bin_size = 0;
	struct vspace_gltf_error error = {0};
	bool uses_draco = false;

	if (!model_path || !*model_path)
		return false;

	if (!parse_json_model(model_path, &root, &glb_bin, &glb_bin_size, &error))
		goto cleanup;

	uses_draco = model_root_uses_draco_extension(root);

cleanup:
	obs_data_release(root);
	bfree(glb_bin);
	vspace_gltf_clear_error(&error);
	return uses_draco;
}

bool vspace_gltf_load_cpu_payload(const char *model_path, struct vspace_cpu_payload *payload,
				    const struct vspace_gltf_load_options *options,
				    struct vspace_gltf_error *error)
{
	struct loader_ctx ctx;
	struct vspace_cpu_payload decoded;
	struct vspace_cpu_payload transformed;
	uint8_t *glb_bin = NULL;
	size_t glb_bin_size = 0;
	bool node_transform_applied = false;
	bool ok = false;

	if (error)
		vspace_gltf_clear_error(error);

	if (!model_path || !payload) {
		set_error(error, VSPACE_GLTF_ERROR_INVALID_ARGUMENT, "Invalid arguments");
		return false;
	}

	memset(&ctx, 0, sizeof(ctx));
	memset(&decoded, 0, sizeof(decoded));
	memset(&transformed, 0, sizeof(transformed));
	dstr_init(&ctx.base_dir);

	ctx.draco_enabled = true;
	ctx.draco_decoder = "auto";
	if (options) {
		ctx.draco_enabled = options->draco_enabled;
		if (options->draco_decoder && *options->draco_decoder)
			ctx.draco_decoder = options->draco_decoder;
	}

	vspace_gltf_free_cpu_payload(payload);
	build_base_dir(model_path, &ctx.base_dir);

	if (!parse_json_model(model_path, &ctx.root, &glb_bin, &glb_bin_size, error))
		goto cleanup;
	if (!resolve_buffers(&ctx, glb_bin, glb_bin_size, error))
		goto cleanup;
	if (!decode_meshes(&ctx, &decoded, error))
		goto cleanup;

	if (!apply_node_transforms_to_payload(model_path, &decoded, &transformed, &node_transform_applied, error))
		goto cleanup;
	if (node_transform_applied) {
		vspace_gltf_free_cpu_payload(&decoded);
		decoded = transformed;
		memset(&transformed, 0, sizeof(transformed));
	}

	convert_payload_axes_y_up_to_z_up(&decoded);

	*payload = decoded;
	memset(&decoded, 0, sizeof(decoded));
	ok = true;

cleanup:
	vspace_gltf_free_cpu_payload(&decoded);
	vspace_gltf_free_cpu_payload(&transformed);
	free_raw_buffers(&ctx);
	obs_data_release(ctx.root);
	dstr_free(&ctx.base_dir);
	bfree(glb_bin);
	return ok;
}
