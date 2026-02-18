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
#include <util/platform.h>

#include "vspace-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("vspace-source", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "OBS scene 3D source";
}

static void verify_data_paths(void)
{
	char *effect_path = obs_module_file("effects/vspace.effect");

	if (!effect_path) {
		blog(LOG_WARNING, "[vspace-source] Could not resolve effects/vspace.effect.");
		return;
	}

	if (!os_file_exists(effect_path))
		blog(LOG_WARNING, "[vspace-source] Effect file missing: %s", effect_path);
	else
		blog(LOG_DEBUG, "[vspace-source] Effect file: %s", effect_path);

	bfree(effect_path);
}

bool obs_module_load(void)
{
	verify_data_paths();
	obs_register_source(&vspace_source_info);
	return true;
}
