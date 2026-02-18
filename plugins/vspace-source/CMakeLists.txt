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

find_package(jansson REQUIRED)

add_library(vspace-source MODULE)
add_library(OBS::vspace-source ALIAS vspace-source)

target_sources(
  vspace-source
  PRIVATE
    vspace-plugin.c
    vspace-source.c
    vspace-source.h
    vspace-gltf-loader.c
    vspace-gltf-loader.h
)

target_link_libraries(
  vspace-source
  PRIVATE
    OBS::libobs
    $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>
    jansson::jansson
)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in vspace-source.rc)
  target_sources(vspace-source PRIVATE vspace-source.rc)
endif()

set_target_properties_obs(vspace-source PROPERTIES FOLDER plugins PREFIX "")
