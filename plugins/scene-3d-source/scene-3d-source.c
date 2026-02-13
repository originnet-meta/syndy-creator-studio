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

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <string.h>

#include "scene-3d-source.h"

#define S_MODEL_PATH "model_path"
#define S_DRACO_ENABLED "draco_enabled"
#define S_DRACO_DECODER "draco_decoder"
#define S_DRACO_DECODER_AUTO "auto"
#define S_DRACO_DECODER_BUILTIN "builtin"
#define S_DRACO_DECODER_EXTERNAL "external"

struct scene_3d_source {
	obs_source_t *source;
	char *model_path;
	char *draco_decoder;
	bool draco_enabled;
	uint32_t width;
	uint32_t height;
};

static const char *scene_3d_source_log_name(const struct scene_3d_source *context)
{
	if (context && context->source)
		return obs_source_get_name(context->source);

	return "scene_3d_source";
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
	context->width = 1920;
	context->height = 1080;
	scene_3d_source_update(context, settings);
	return context;
}

static void scene_3d_source_destroy(void *data)
{
	struct scene_3d_source *context = data;

	bfree(context->model_path);
	bfree(context->draco_decoder);
	bfree(context);
}

static void scene_3d_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(effect);
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
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = scene_3d_source_get_name,
	.create = scene_3d_source_create,
	.destroy = scene_3d_source_destroy,
	.update = scene_3d_source_update,
	.get_defaults = scene_3d_source_defaults,
	.get_properties = scene_3d_source_properties,
	.video_render = scene_3d_source_render,
	.get_width = scene_3d_source_get_width,
	.get_height = scene_3d_source_get_height,
};
