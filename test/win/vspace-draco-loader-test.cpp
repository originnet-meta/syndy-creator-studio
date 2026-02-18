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

extern "C" {
#include "../../plugins/vspace-source/vspace-gltf-loader.h"
}

#include <stdio.h>

#include <string>

#ifndef VSPACE_DRACO_FIXTURE_DIR
#define VSPACE_DRACO_FIXTURE_DIR "test/test-input/data/vspace"
#endif

static bool expect(bool condition, const char *message)
{
	if (!condition)
		fprintf(stderr, "vspace-draco-loader-test: %s\n", message);

	return condition;
}

static std::string fixture_path(const char *name)
{
	std::string path = VSPACE_DRACO_FIXTURE_DIR;

	if (!path.empty() && path.back() != '/' && path.back() != '\\')
		path.push_back('/');

	path += name;
	return path;
}

static bool run_success_case(const char *label, const char *fixture_name, const struct vspace_gltf_load_options *options,
			     bool expect_draco_extension, enum vspace_decode_path expect_decode_path)
{
	struct vspace_cpu_payload payload = {0};
	struct vspace_gltf_error error = {0};
	const struct vspace_cpu_primitive_payload *primitive = NULL;
	const std::string path = fixture_path(fixture_name);
	bool ok = true;

	if (!vspace_gltf_load_cpu_payload(path.c_str(), &payload, options, &error)) {
		fprintf(stderr, "vspace-draco-loader-test: %s failed: %s (%s)\n", label,
			vspace_gltf_error_to_string(error.code), error.message ? error.message : "no details");
		vspace_gltf_clear_error(&error);
		return false;
	}

	ok &= expect(payload.mesh_count == 1, "expected one mesh");
	ok &= expect(payload.meshes != NULL, "mesh array must exist");
	if (!ok)
		goto cleanup;

	ok &= expect(payload.meshes[0].primitive_count == 1, "expected one primitive");
	ok &= expect(payload.meshes[0].primitives != NULL, "primitive array must exist");
	if (!ok)
		goto cleanup;

	primitive = &payload.meshes[0].primitives[0];

	ok &= expect(primitive->decode_path == expect_decode_path, "unexpected decode path");
	ok &= expect(primitive->used_draco_extension == expect_draco_extension, "unexpected draco extension flag");
	ok &= expect(primitive->positions != NULL && primitive->vertex_count == 3, "position payload mismatch");
	ok &= expect(primitive->indices != NULL && primitive->index_count == 3, "index payload mismatch");
	ok &= expect(primitive->texcoords != NULL, "texcoord payload mismatch");

cleanup:
	vspace_gltf_free_cpu_payload(&payload);
	vspace_gltf_clear_error(&error);
	return ok;
}

static bool run_failure_case(const char *label, const char *fixture_name, const struct vspace_gltf_load_options *options,
			     enum vspace_gltf_error_code expect_error)
{
	struct vspace_cpu_payload payload = {0};
	struct vspace_gltf_error error = {0};
	const std::string path = fixture_path(fixture_name);
	bool matched = false;

	if (vspace_gltf_load_cpu_payload(path.c_str(), &payload, options, &error)) {
		vspace_gltf_free_cpu_payload(&payload);
		vspace_gltf_clear_error(&error);
		fprintf(stderr, "vspace-draco-loader-test: %s unexpectedly succeeded\n", label);
		return false;
	}

	if (!expect(error.code == expect_error, "unexpected error code"))
		fprintf(stderr, "vspace-draco-loader-test: %s got %s\n", label,
			vspace_gltf_error_to_string(error.code));
	matched = error.code == expect_error;

	vspace_gltf_free_cpu_payload(&payload);
	vspace_gltf_clear_error(&error);
	return matched;
}

int main()
{
	bool ok = true;
	struct vspace_gltf_load_options draco_auto = {0};
	struct vspace_gltf_load_options draco_disabled = {0};

	if (!obs_startup("en-US", NULL, NULL)) {
		fprintf(stderr, "vspace-draco-loader-test: obs_startup failed\n");
		return 1;
	}

	draco_auto.draco_enabled = true;
	draco_auto.draco_decoder = "auto";
	draco_disabled.draco_enabled = false;
	draco_disabled.draco_decoder = "auto";

	/* Smoke: Draco extension exists but decoder is unavailable -> accessor fallback remains stable. */
	{
		const bool passed = run_success_case("smoke-draco-fallback", "draco-fallback.gltf", &draco_auto, true,
						     VSPACE_DECODE_PATH_ACCESSOR);
		if (!passed)
			fprintf(stderr, "vspace-draco-loader-test: smoke-draco-fallback failed\n");
		ok &= passed;
	}

	/* Regression: Explicitly disabling Draco keeps extension-annotated assets loadable. */
	{
		const bool passed =
			run_success_case("regression-draco-disabled", "draco-fallback.gltf", &draco_disabled, true,
					 VSPACE_DECODE_PATH_ACCESSOR);
		if (!passed)
			fprintf(stderr, "vspace-draco-loader-test: regression-draco-disabled failed\n");
		ok &= passed;
	}

	/* Regression: Non-Draco assets still decode through accessor path. */
	{
		const bool passed = run_success_case("regression-accessor", "accessor-only.gltf", &draco_auto, false,
						     VSPACE_DECODE_PATH_ACCESSOR);
		if (!passed)
			fprintf(stderr, "vspace-draco-loader-test: regression-accessor failed\n");
		ok &= passed;
	}

	/* Negative guard: extension-only payload must fail with deterministic decoder error. */
	{
		const bool passed = run_failure_case("guard-draco-requires-decoder", "draco-requires-decoder.gltf",
						     &draco_auto, VSPACE_GLTF_ERROR_DRACO_DECODER_UNAVAILABLE);
		if (!passed)
			fprintf(stderr, "vspace-draco-loader-test: guard-draco-requires-decoder failed\n");
		ok &= passed;
	}

	obs_shutdown();

	if (!ok)
		return 1;

	printf("vspace-draco-loader-test: success\n");
	return 0;
}
