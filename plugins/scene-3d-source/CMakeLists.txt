#[[
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
]]

cmake_minimum_required(VERSION 3.28...3.30)

add_library(scene-3d-source MODULE)
add_library(OBS::scene-3d-source ALIAS scene-3d-source)

target_sources(
  scene-3d-source
  PRIVATE
    scene-3d-plugin.c
    scene-3d-source.c
    scene-3d-source.h
    scene-3d-gltf-loader.c
    scene-3d-gltf-loader.h
)

target_link_libraries(scene-3d-source PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in scene-3d-source.rc)
  target_sources(scene-3d-source PRIVATE scene-3d-source.rc)
endif()

set_target_properties_obs(scene-3d-source PROPERTIES FOLDER plugins PREFIX "")
