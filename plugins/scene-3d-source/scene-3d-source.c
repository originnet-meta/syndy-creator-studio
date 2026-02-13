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
    Modified: 2026-02-12
    Notes: Changes for Syndy Creator Studio.
******************************************************************************/

#include <obs-module.h>
#include <graphics/image-file.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>

#include "scene-3d-gltf-loader.h"
#include "scene-3d-source.h"

#define S_MODEL_PATH "model_path"
#define S_DRACO_ENABLED "draco_enabled"
#define S_DRACO_DECODER "draco_decoder"
#define S_DRACO_DECODER_AUTO "auto"
#define S_DRACO_DECODER_BUILTIN "builtin"
#define S_DRACO_DECODER_EXTERNAL "external"

struct scene_3d_source {
	obs_source_t *source;
	gs_effect_t *effect;
	gs_eparam_t *effect_base_color_param;
	char *model_path;
	char *draco_decoder;
	gs_vertbuffer_t *vertex_buffer;
	gs_indexbuffer_t *index_buffer;
	gs_image_file4_t base_color_image;
	size_t draw_vertex_count;
	size_t draw_index_count;
	bool base_color_image_valid;
	pthread_t worker_thread;
	pthread_mutex_t worker_mutex;
	os_event_t *worker_event;
	bool worker_thread_active;
	bool worker_mutex_valid;
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
	bool draco_enabled;
	bool active;
	bool showing;
	bool effect_load_attempted;
	uint32_t width;
	uint32_t height;
};

static const char *scene_3d_source_log_name(const struct scene_3d_source *context)
{
	if (context && context->source)
		return obs_source_get_name(context->source);

	return "scene_3d_source";
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

static void scene_3d_source_unload_effect(struct scene_3d_source *context)
{
	if (!context || !context->effect)
		return;

	obs_enter_graphics();
	gs_effect_destroy(context->effect);
	obs_leave_graphics();

	context->effect = NULL;
	context->effect_base_color_param = NULL;
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
	if (context->effect)
		context->effect_base_color_param = gs_effect_get_param_by_name(context->effect, "effect_base_color_param");
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

	if (context->base_color_image_valid) {
		gs_image_file4_free(&context->base_color_image);
		context->base_color_image_valid = false;
	}

	context->draw_vertex_count = 0;
	context->draw_index_count = 0;
}

static const struct scene_3d_cpu_primitive_payload *scene_3d_source_pick_draw_primitive(const struct scene_3d_cpu_payload *payload)
{
	size_t mesh_idx;

	if (!payload)
		return NULL;

	for (mesh_idx = 0; mesh_idx < payload->mesh_count; mesh_idx++) {
		const struct scene_3d_cpu_mesh_payload *mesh = payload->meshes + mesh_idx;
		size_t primitive_idx;

		for (primitive_idx = 0; primitive_idx < mesh->primitive_count; primitive_idx++) {
			const struct scene_3d_cpu_primitive_payload *primitive = mesh->primitives + primitive_idx;

			if (primitive->positions && primitive->vertex_count > 0)
				return primitive;
		}
	}

	return NULL;
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

static struct gs_vb_data *scene_3d_source_build_vb_data(const struct scene_3d_cpu_primitive_payload *primitive)
{
	struct gs_vb_data *vb_data;
	struct vec3 *points;
	struct vec2 *uvs;
	size_t i;

	if (!primitive || !primitive->positions || primitive->vertex_count == 0)
		return NULL;

	vb_data = gs_vbdata_create();
	vb_data->num = primitive->vertex_count;
	vb_data->points = bmalloc(sizeof(struct vec3) * primitive->vertex_count);
	vb_data->num_tex = 1;
	vb_data->tvarray = bzalloc(sizeof(struct gs_tvertarray));
	vb_data->tvarray[0].width = 2;
	vb_data->tvarray[0].array = bmalloc(sizeof(struct vec2) * primitive->vertex_count);

	points = vb_data->points;
	uvs = vb_data->tvarray[0].array;

	for (i = 0; i < primitive->vertex_count; i++) {
		const float *src_pos = primitive->positions + (i * 3);

		points[i].x = src_pos[0];
		points[i].y = src_pos[1];
		points[i].z = src_pos[2];

		if (primitive->texcoords) {
			const float *src_uv = primitive->texcoords + (i * 2);

			uvs[i].x = src_uv[0];
			uvs[i].y = src_uv[1];
		} else {
			uvs[i].x = 0.0f;
			uvs[i].y = 0.0f;
		}
	}

	return vb_data;
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
	const struct scene_3d_cpu_primitive_payload *primitive;
	struct gs_vb_data *vb_data = NULL;
	gs_vertbuffer_t *new_vertex_buffer = NULL;
	gs_indexbuffer_t *new_index_buffer = NULL;

	if (!context || !payload)
		return;

	primitive = scene_3d_source_pick_draw_primitive(payload);
	if (!primitive) {
		obs_enter_graphics();
		scene_3d_source_release_gpu_resources(context);
		obs_leave_graphics();
		return;
	}

	vb_data = scene_3d_source_build_vb_data(primitive);
	if (!vb_data) {
		blog(LOG_WARNING, "[scene-3d-source: '%s'] Failed to build vertex buffer input data.",
		     scene_3d_source_log_name(context));
		return;
	}

	obs_enter_graphics();

	new_vertex_buffer = gs_vertexbuffer_create(vb_data, 0);
	if (!new_vertex_buffer)
		gs_vbdata_destroy(vb_data);

	if (new_vertex_buffer && primitive->indices && primitive->index_count > 0) {
		new_index_buffer =
			gs_indexbuffer_create(GS_UNSIGNED_LONG, primitive->indices, primitive->index_count, GS_DUP_BUFFER);
		if (!new_index_buffer) {
			gs_vertexbuffer_destroy(new_vertex_buffer);
			new_vertex_buffer = NULL;
		}
	}

	if (new_vertex_buffer) {
		if (decoded_image_valid)
			gs_image_file4_init_texture(decoded_image);

		scene_3d_source_release_gpu_resources(context);

		context->vertex_buffer = new_vertex_buffer;
		context->index_buffer = new_index_buffer;
		context->draw_vertex_count = primitive->vertex_count;
		context->draw_index_count = primitive->index_count;

		if (decoded_image_valid && decoded_image->image3.image2.image.texture) {
			context->base_color_image = *decoded_image;
			memset(decoded_image, 0, sizeof(*decoded_image));
			context->base_color_image_valid = true;
		}
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

	if (!scene_3d_source_take_pending_upload(context, &payload, &decoded_image, &decoded_image_valid))
		return;

	scene_3d_source_upload_pending_payload(context, &payload, &decoded_image, decoded_image_valid);
	scene_3d_gltf_free_cpu_payload(&payload);

	if (decoded_image_valid)
		gs_image_file4_free(&decoded_image);
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

	scene_3d_source_queue_load_job(context);
	scene_3d_source_refresh_size(context);
}

static obs_properties_t *scene_3d_source_properties(void *unused)
{
	obs_property_t *draco_decoder;
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, S_MODEL_PATH, obs_module_text("Scene3D.ModelFile"), OBS_PATH_FILE,
				obs_module_text("Scene3D.ModelFile.Filter"), NULL);
	obs_properties_add_bool(props, S_DRACO_ENABLED, obs_module_text("Scene3D.Draco.Enable"));

	draco_decoder = obs_properties_add_list(props, S_DRACO_DECODER, obs_module_text("Scene3D.Draco.Decoder"),
						OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(draco_decoder, obs_module_text("Scene3D.Draco.Decoder.Auto"), S_DRACO_DECODER_AUTO);
	obs_property_list_add_string(draco_decoder, obs_module_text("Scene3D.Draco.Decoder.Builtin"),
				     S_DRACO_DECODER_BUILTIN);
	obs_property_list_add_string(draco_decoder, obs_module_text("Scene3D.Draco.Decoder.External"),
				     S_DRACO_DECODER_EXTERNAL);

	UNUSED_PARAMETER(unused);
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

	context->source = source;
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

	scene_3d_source_refresh_size(context);
	scene_3d_source_update(context, settings);
	scene_3d_source_load_effect(context);
	return context;
}

static void scene_3d_source_destroy(void *data)
{
	struct scene_3d_source *context = data;

	scene_3d_source_stop_worker(context);
	scene_3d_source_free_worker_job(context);
	scene_3d_source_release_pending_upload(context);

	obs_enter_graphics();
	scene_3d_source_release_gpu_resources(context);
	obs_leave_graphics();

	scene_3d_source_unload_effect(context);
	bfree(context->model_path);
	bfree(context->draco_decoder);
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

	scene_3d_source_process_pending_upload(context);

	if (!context->effect && !context->effect_load_attempted)
		scene_3d_source_load_effect(context);
}

static void scene_3d_source_render(void *data, gs_effect_t *effect)
{
	struct scene_3d_source *context = data;
	bool rendered = false;
	const bool previous_srgb = gs_framebuffer_srgb_enabled();

	UNUSED_PARAMETER(effect);

	if (!context)
		return;

	/* BaseColor is sampled as sRGB and shaded in linear space before output. */
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (context->effect && context->vertex_buffer) {
		gs_texture_t *base_color_texture = NULL;

		if (context->base_color_image_valid)
			base_color_texture = context->base_color_image.image3.image2.image.texture;

		if (context->effect_base_color_param) {
			if (base_color_texture)
				gs_effect_set_texture_srgb(context->effect_base_color_param, base_color_texture);
			else
				gs_effect_set_texture(context->effect_base_color_param, NULL);
		}

		gs_load_vertexbuffer(context->vertex_buffer);
		gs_load_indexbuffer(context->index_buffer);

		while (gs_effect_loop(context->effect, "Draw"))
			gs_draw(GS_TRIS, 0, context->index_buffer ? 0 : (uint32_t)context->draw_vertex_count);

		gs_load_indexbuffer(NULL);
		gs_load_vertexbuffer(NULL);
		rendered = true;
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
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
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
	.video_tick = scene_3d_source_video_tick,
	.video_render = scene_3d_source_render,
	.get_width = scene_3d_source_get_width,
	.get_height = scene_3d_source_get_height,
	.icon_type = OBS_ICON_TYPE_CUSTOM,
	.video_get_color_space = scene_3d_source_get_color_space,
};
