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

#include "scene-3d-gltf-loader.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
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

static void set_error(struct scene_3d_gltf_error *error, enum scene_3d_gltf_error_code code, const char *fmt, ...)
{
	va_list args;
	struct dstr text;

	if (!error)
		return;

	scene_3d_gltf_clear_error(error);
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

static void set_error_errno(struct scene_3d_gltf_error *error, enum scene_3d_gltf_error_code code, const char *path,
			    const char *action)
{
	set_error(error, code, "%s (%s): %s", action, path, strerror(errno));
}

static bool get_required_index(obs_data_t *object, const char *field, size_t *value, struct scene_3d_gltf_error *error,
			       const char *context)
{
	int64_t raw;

	if (!obs_data_has_user_value(object, field)) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "%s missing field '%s'", context, field);
		return false;
	}

	raw = obs_data_get_int(object, field);
	if (raw < 0 || (uint64_t)raw > SIZE_MAX) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "%s field '%s' out of range", context, field);
		return false;
	}

	*value = (size_t)raw;
	return true;
}

static bool get_optional_index(obs_data_t *object, const char *field, size_t *value, size_t default_value,
			       struct scene_3d_gltf_error *error, const char *context)
{
	if (!obs_data_has_user_value(object, field)) {
		*value = default_value;
		return true;
	}
	return get_required_index(object, field, value, error, context);
}

static bool get_array_item(obs_data_t *root, const char *array_name, size_t index, obs_data_t **item,
			   struct scene_3d_gltf_error *error)
{
	obs_data_array_t *array = obs_data_get_array(root, array_name);
	size_t count;

	*item = NULL;
	if (!array) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Missing top-level array '%s'", array_name);
		return false;
	}

	count = obs_data_array_count(array);
	if (index >= count) {
		obs_data_array_release(array);
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Index %zu out of range for '%s' (count=%zu)", index,
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

static char *resolve_uri_path(const struct dstr *base_dir, const char *uri, struct scene_3d_gltf_error *error)
{
	struct dstr full;

	if (!uri || !*uri) {
		set_error(error, SCENE_3D_GLTF_ERROR_IO, "Empty URI");
		return NULL;
	}

	if (uri_has_scheme(uri) && !is_data_uri(uri)) {
		set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "Unsupported URI scheme: %s", uri);
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

static bool read_file_bytes(const char *path, uint8_t **bytes, size_t *size, struct scene_3d_gltf_error *error)
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
		set_error_errno(error, SCENE_3D_GLTF_ERROR_IO, path, "Could not stat file");
		return false;
	}
	if ((uint64_t)file_size_i64 > SIZE_MAX) {
		set_error(error, SCENE_3D_GLTF_ERROR_IO, "File too large: %s", path);
		return false;
	}

	file_size = (size_t)file_size_i64;
	file = os_fopen(path, "rb");
	if (!file) {
		set_error_errno(error, SCENE_3D_GLTF_ERROR_IO, path, "Could not open file");
		return false;
	}

	if (file_size) {
		buffer = bmalloc(file_size);
		read_bytes = fread(buffer, 1, file_size, file);
		if (read_bytes != file_size) {
			fclose(file);
			bfree(buffer);
			set_error_errno(error, SCENE_3D_GLTF_ERROR_IO, path, "Could not read file");
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

static bool decode_base64(const char *input, uint8_t **output, size_t *output_size, struct scene_3d_gltf_error *error)
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
			set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Invalid base64 payload");
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

static bool decode_data_uri(const char *uri, uint8_t **bytes, size_t *size, struct scene_3d_gltf_error *error)
{
	const char *comma;
	const char *base64_tag;

	if (!is_data_uri(uri)) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Not a data URI");
		return false;
	}

	comma = strchr(uri, ',');
	if (!comma) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Malformed data URI");
		return false;
	}

	base64_tag = astrstri(uri, ";base64");
	if (!base64_tag || base64_tag > comma) {
		set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "Only base64 data URI is supported");
		return false;
	}

	return decode_base64(comma + 1, bytes, size, error);
}

static bool parse_json_model(const char *model_path, obs_data_t **root, uint8_t **glb_bin, size_t *glb_bin_size,
			     struct scene_3d_gltf_error *error)
{
	const char *ext;

	*root = NULL;
	*glb_bin = NULL;
	*glb_bin_size = 0;

	ext = strrchr(model_path, '.');
	if (!ext) {
		set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "Model path has no extension: %s", model_path);
		return false;
	}

	if (astrcmpi(ext, ".gltf") == 0) {
		*root = obs_data_create_from_json_file(model_path);
		if (!*root) {
			set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Failed to parse glTF JSON: %s", model_path);
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
			set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Invalid GLB header: %s", model_path);
			return false;
		}

		version = read_u32_le(file_data + 4);
		if (version != GLTF_VERSION_2) {
			bfree(file_data);
			set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "Unsupported GLB version %u", version);
			return false;
		}

		length = read_u32_le(file_data + 8);
		if ((size_t)length > file_size || length < 12) {
			bfree(file_data);
			set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Invalid GLB length in header");
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
				set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Malformed GLB chunk");
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
			set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "GLB JSON chunk is missing");
			return false;
		}

		json_text = bmalloc(json_chunk_size + 1);
		memcpy(json_text, json_chunk, json_chunk_size);
		json_text[json_chunk_size] = '\0';
		*root = obs_data_create_from_json(json_text);
		bfree(json_text);

		if (!*root) {
			bfree(file_data);
			set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Failed to parse GLB JSON chunk");
			return false;
		}

		if (bin_chunk && bin_chunk_size) {
			*glb_bin = bmemdup(bin_chunk, bin_chunk_size);
			*glb_bin_size = bin_chunk_size;
		}

		bfree(file_data);
		return true;
	}

	set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "Unsupported extension: %s", ext);
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
			    struct scene_3d_gltf_error *error)
{
	obs_data_array_t *buffers_arr = obs_data_get_array(ctx->root, "buffers");
	size_t count;
	size_t i;

	if (!buffers_arr) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Missing top-level array 'buffers'");
		return false;
	}

	count = obs_data_array_count(buffers_arr);
	if (!count) {
		obs_data_array_release(buffers_arr);
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "No buffers in glTF file");
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
			set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "buffer[%zu] has no URI and no GLB BIN fallback", i);
			return false;
		}

		if (expected_size > ctx->buffers[i].size) {
			obs_data_release(buffer_obj);
			obs_data_array_release(buffers_arr);
			set_error(error, SCENE_3D_GLTF_ERROR_PARSE,
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
			      struct scene_3d_gltf_error *error)
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
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "accessor[%zu] invalid componentType", accessor_index);
		goto cleanup;
	}
	comp_type = (uint32_t)comp_raw;
	elem_comp_size = component_size(comp_type);
	if (!elem_comp_size) {
		set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] unsupported componentType=%u",
			  accessor_index, comp_type);
		goto cleanup;
	}

	type = obs_data_get_string(accessor, "type");
	elem_comp_count = component_count(type);
	if (!elem_comp_count) {
		set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] unsupported type '%s'",
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
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "bufferView[%zu] references invalid buffer index %zu",
			  buffer_view_index, buffer_index);
		goto cleanup;
	}

	if (!safe_mul(elem_comp_count, elem_comp_size, &elem_size)) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "accessor[%zu] element size overflow", accessor_index);
		goto cleanup;
	}

	if (!buffer_view_stride)
		buffer_view_stride = elem_size;
	else if (buffer_view_stride < elem_size) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "accessor[%zu] byteStride (%zu) < element size (%zu)",
			  accessor_index, buffer_view_stride, elem_size);
		goto cleanup;
	}

	if (buffer_view_offset > ctx->buffers[buffer_index].size ||
	    buffer_view_length > ctx->buffers[buffer_index].size - buffer_view_offset) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE,
			  "bufferView[%zu] exceeds buffer[%zu] bounds (offset=%zu len=%zu size=%zu)",
			  buffer_view_index, buffer_index, buffer_view_offset, buffer_view_length,
			  ctx->buffers[buffer_index].size);
		goto cleanup;
	}

	if (accessor_count) {
		if (!safe_mul(buffer_view_stride, accessor_count - 1, &last_off) || !safe_add(last_off, elem_size, &data_bytes) ||
		    accessor_offset > buffer_view_length || data_bytes > buffer_view_length - accessor_offset) {
			set_error(error, SCENE_3D_GLTF_ERROR_PARSE,
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
				  size_t *elem_count, struct scene_3d_gltf_error *error)
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
		set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] must use FLOAT component type",
			  accessor_index);
		return false;
	}
	if (view.comp_count != expected_comp) {
		set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] expected %zu components, got %zu",
			  accessor_index, expected_comp, view.comp_count);
		return false;
	}

	if (!view.count)
		return true;

	if (!safe_mul(view.count, expected_comp, &total_values) || !safe_mul(total_values, sizeof(float), &total_bytes)) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "accessor[%zu] float decode size overflow", accessor_index);
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
				  struct scene_3d_gltf_error *error)
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
		set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] index accessor must be SCALAR",
			  accessor_index);
		return false;
	}
	if (view.comp_type != GLTF_COMPONENT_UNSIGNED_BYTE && view.comp_type != GLTF_COMPONENT_UNSIGNED_SHORT &&
	    view.comp_type != GLTF_COMPONENT_UNSIGNED_INT) {
		set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "accessor[%zu] unsupported index componentType=%u",
			  accessor_index, view.comp_type);
		return false;
	}

	if (!view.count)
		return true;

	if (!safe_mul(view.count, sizeof(uint32_t), &bytes)) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "accessor[%zu] index decode size overflow", accessor_index);
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

static bool generate_indices(size_t vertex_count, uint32_t **out, size_t *count, struct scene_3d_gltf_error *error)
{
	uint32_t *dst;
	size_t bytes;
	size_t i;

	*out = NULL;
	*count = 0;

	if (!vertex_count)
		return true;
	if (vertex_count > UINT32_MAX) {
		set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "vertex count exceeds uint32 range: %zu", vertex_count);
		return false;
	}
	if (!safe_mul(vertex_count, sizeof(uint32_t), &bytes)) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "index buffer size overflow");
		return false;
	}

	dst = bmalloc(bytes);
	for (i = 0; i < vertex_count; i++)
		dst[i] = (uint32_t)i;

	*out = dst;
	*count = vertex_count;
	return true;
}

static char *resolve_base_color_texture(struct loader_ctx *ctx, obs_data_t *primitive, struct scene_3d_gltf_error *error)
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
		     "[scene-3d-source:gltf-loader] Embedded image bufferView is not supported yet. Texture skipped.");
		goto cleanup;
	}

	uri = obs_data_get_string(image, "uri");
	if (!uri || !*uri)
		goto cleanup;
	if (is_data_uri(uri)) {
		blog(LOG_WARNING,
		     "[scene-3d-source:gltf-loader] Data-URI image is not supported yet. Texture skipped.");
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
				      struct scene_3d_cpu_primitive_payload *out, bool draco_ext_present,
				      struct scene_3d_gltf_error *error)
{
	obs_data_t *attributes = NULL;
	size_t mode = GLTF_MODE_TRIANGLES;
	size_t pos_acc;
	size_t uv_acc = 0;
	size_t idx_acc = 0;
	bool has_uv;
	bool has_idx;
	float *positions = NULL;
	float *texcoords = NULL;
	uint32_t *indices = NULL;
	size_t vertex_count = 0;
	size_t uv_count = 0;
	size_t index_count = 0;
	char *texture_path = NULL;
	bool ok = false;

	memset(out, 0, sizeof(*out));
	out->used_draco_extension = draco_ext_present;
	out->decode_path = SCENE_3D_DECODE_PATH_ACCESSOR;

	if (!get_optional_index(primitive, "mode", &mode, GLTF_MODE_TRIANGLES, error, "primitive"))
		goto cleanup;
	if (mode != GLTF_MODE_TRIANGLES) {
		set_error(error, SCENE_3D_GLTF_ERROR_UNSUPPORTED, "Unsupported primitive mode %zu (only TRIANGLES=4)",
			  mode);
		goto cleanup;
	}

	attributes = obs_data_get_obj(primitive, "attributes");
	if (!attributes) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Primitive missing attributes object");
		goto cleanup;
	}
	if (!get_required_index(attributes, "POSITION", &pos_acc, error, "attributes"))
		goto cleanup;
	if (!decode_float_accessor(ctx, pos_acc, 3, &positions, &vertex_count, error))
		goto cleanup;

	has_uv = obs_data_has_user_value(attributes, "TEXCOORD_0");
	if (has_uv) {
		if (!get_required_index(attributes, "TEXCOORD_0", &uv_acc, error, "attributes"))
			goto cleanup;
		if (!decode_float_accessor(ctx, uv_acc, 2, &texcoords, &uv_count, error))
			goto cleanup;
		if (uv_count != vertex_count) {
			set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "TEXCOORD_0 count (%zu) != POSITION count (%zu)",
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
	if (error && error->code != SCENE_3D_GLTF_SUCCESS && !texture_path)
		goto cleanup;

	out->positions = positions;
	out->texcoords = texcoords;
	out->indices = indices;
	out->vertex_count = vertex_count;
	out->index_count = index_count;
	out->base_color_texture = texture_path;

	positions = NULL;
	texcoords = NULL;
	indices = NULL;
	texture_path = NULL;
	ok = true;

cleanup:
	obs_data_release(attributes);
	bfree(positions);
	bfree(texcoords);
	bfree(indices);
	bfree(texture_path);
	return ok;
}

static bool decode_draco_primitive(struct loader_ctx *ctx, obs_data_t *primitive, struct scene_3d_cpu_primitive_payload *out,
				   struct scene_3d_gltf_error *error)
{
	obs_data_t *extensions = NULL;
	obs_data_t *draco = NULL;
	size_t draco_buffer_view = 0;

	extensions = obs_data_get_obj(primitive, "extensions");
	draco = extensions ? obs_data_get_obj(extensions, "KHR_draco_mesh_compression") : NULL;
	if (!draco) {
		obs_data_release(draco);
		obs_data_release(extensions);
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Primitive does not contain KHR_draco_mesh_compression data");
		return false;
	}

	if (!get_required_index(draco, "bufferView", &draco_buffer_view, error, "KHR_draco_mesh_compression")) {
		obs_data_release(draco);
		obs_data_release(extensions);
		return false;
	}

#if defined(SCENE_3D_ENABLE_DRACO_DECODER)
	/* TODO: wire real Draco decoder implementation. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(out);
	obs_data_release(draco);
	obs_data_release(extensions);
	set_error(error, SCENE_3D_GLTF_ERROR_DRACO_DECODE_FAILED,
		  "Draco decode failed for bitstream bufferView index %zu", draco_buffer_view);
	return false;
#else
	UNUSED_PARAMETER(draco_buffer_view);
	if (obs_data_has_user_value(primitive, "attributes")) {
		blog(LOG_WARNING,
		     "[scene-3d-source:gltf-loader] Draco extension detected but decoder is unavailable. Using accessor fallback.");
		obs_data_release(draco);
		obs_data_release(extensions);
		return decode_accessor_primitive(ctx, primitive, out, true, error);
	}

	obs_data_release(draco);
	obs_data_release(extensions);
	UNUSED_PARAMETER(out);
	set_error(error, SCENE_3D_GLTF_ERROR_DRACO_DECODER_UNAVAILABLE,
		  "KHR_draco_mesh_compression requires a Draco decoder, but none is available");
	return false;
#endif
}

static bool decode_meshes(struct loader_ctx *ctx, struct scene_3d_cpu_payload *out, struct scene_3d_gltf_error *error)
{
	obs_data_array_t *meshes = obs_data_get_array(ctx->root, "meshes");
	size_t mesh_count;
	size_t m;

	if (!meshes) {
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "Missing top-level array 'meshes'");
		return false;
	}

	mesh_count = obs_data_array_count(meshes);
	if (!mesh_count) {
		obs_data_array_release(meshes);
		set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "No meshes found in glTF");
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
			set_error(error, SCENE_3D_GLTF_ERROR_PARSE, "mesh[%zu] has no 'primitives' array", m);
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
					     "[scene-3d-source:gltf-loader] Draco extension found but disabled by option '%s'. Using accessor fallback.",
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
		}

		obs_data_array_release(primitives);
		obs_data_release(mesh);
	}

	obs_data_array_release(meshes);
	return true;
}

const char *scene_3d_gltf_error_to_string(enum scene_3d_gltf_error_code code)
{
	switch (code) {
	case SCENE_3D_GLTF_SUCCESS:
		return "success";
	case SCENE_3D_GLTF_ERROR_INVALID_ARGUMENT:
		return "invalid_argument";
	case SCENE_3D_GLTF_ERROR_IO:
		return "io_error";
	case SCENE_3D_GLTF_ERROR_PARSE:
		return "parse_error";
	case SCENE_3D_GLTF_ERROR_UNSUPPORTED:
		return "unsupported";
	case SCENE_3D_GLTF_ERROR_DRACO_DECODER_UNAVAILABLE:
		return "draco_decoder_unavailable";
	case SCENE_3D_GLTF_ERROR_DRACO_DECODE_FAILED:
		return "draco_decode_failed";
	case SCENE_3D_GLTF_ERROR_ACCESSOR_DECODE_FAILED:
		return "accessor_decode_failed";
	default:
		return "unknown";
	}
}

void scene_3d_gltf_clear_error(struct scene_3d_gltf_error *error)
{
	if (!error)
		return;

	bfree(error->message);
	error->message = NULL;
	error->code = SCENE_3D_GLTF_SUCCESS;
}

void scene_3d_gltf_free_cpu_payload(struct scene_3d_cpu_payload *payload)
{
	size_t m;

	if (!payload)
		return;

	for (m = 0; m < payload->mesh_count; m++) {
		size_t p;
		struct scene_3d_cpu_mesh_payload *mesh = payload->meshes + m;

		bfree(mesh->name);
		for (p = 0; p < mesh->primitive_count; p++) {
			struct scene_3d_cpu_primitive_payload *prim = mesh->primitives + p;
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

bool scene_3d_gltf_load_cpu_payload(const char *model_path, struct scene_3d_cpu_payload *payload,
				    const struct scene_3d_gltf_load_options *options,
				    struct scene_3d_gltf_error *error)
{
	struct loader_ctx ctx;
	struct scene_3d_cpu_payload decoded;
	uint8_t *glb_bin = NULL;
	size_t glb_bin_size = 0;
	bool ok = false;

	if (error)
		scene_3d_gltf_clear_error(error);

	if (!model_path || !payload) {
		set_error(error, SCENE_3D_GLTF_ERROR_INVALID_ARGUMENT, "Invalid arguments");
		return false;
	}

	memset(&ctx, 0, sizeof(ctx));
	memset(&decoded, 0, sizeof(decoded));
	dstr_init(&ctx.base_dir);

	ctx.draco_enabled = true;
	ctx.draco_decoder = "auto";
	if (options) {
		ctx.draco_enabled = options->draco_enabled;
		if (options->draco_decoder && *options->draco_decoder)
			ctx.draco_decoder = options->draco_decoder;
	}

	scene_3d_gltf_free_cpu_payload(payload);
	build_base_dir(model_path, &ctx.base_dir);

	if (!parse_json_model(model_path, &ctx.root, &glb_bin, &glb_bin_size, error))
		goto cleanup;
	if (!resolve_buffers(&ctx, glb_bin, glb_bin_size, error))
		goto cleanup;
	if (!decode_meshes(&ctx, &decoded, error))
		goto cleanup;

	*payload = decoded;
	memset(&decoded, 0, sizeof(decoded));
	ok = true;

cleanup:
	scene_3d_gltf_free_cpu_payload(&decoded);
	free_raw_buffers(&ctx);
	obs_data_release(ctx.root);
	dstr_free(&ctx.base_dir);
	bfree(glb_bin);
	return ok;
}
