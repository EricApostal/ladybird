# Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
#
# SPDX-License-Identifier: BSD-2-Clause
#

#
# Copy resources into tarball for inclusion in /assets of APK
#
set(LADYBIRD_RESOURCE_ROOT "${LADYBIRD_SOURCE_DIR}/Base/res")
macro(copy_res_folder folder)
    add_custom_target(copy-${folder}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${LADYBIRD_RESOURCE_ROOT}/${folder}"
            "asset-bundle/${folder}"
    )
    add_dependencies(archive-assets copy-${folder})
endmacro()
add_custom_target(archive-assets COMMAND ${CMAKE_COMMAND} -E chdir asset-bundle zip -r ../ladybird-assets.zip ./ )
copy_res_folder(ladybird)
copy_res_folder(fonts)
copy_res_folder(icons)
copy_res_folder(themes)
set(LADYBIRD_ANDROID_ASSETS_OUTPUT_PATH "${CMAKE_SOURCE_DIR}/UI/Android/src/main/assets/ladybird-assets.zip")
if (DEFINED LADYBIRD_ANDROID_ASSETS_OUTPUT_PATH_OVERRIDE)
    set(LADYBIRD_ANDROID_ASSETS_OUTPUT_PATH "${LADYBIRD_ANDROID_ASSETS_OUTPUT_PATH_OVERRIDE}")
endif()

get_filename_component(LADYBIRD_ANDROID_ASSETS_OUTPUT_DIR "${LADYBIRD_ANDROID_ASSETS_OUTPUT_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${LADYBIRD_ANDROID_ASSETS_OUTPUT_DIR}")

add_custom_target(copy-assets COMMAND ${CMAKE_COMMAND} -E copy_if_different ladybird-assets.zip "${LADYBIRD_ANDROID_ASSETS_OUTPUT_PATH}")
add_dependencies(copy-assets archive-assets)
add_dependencies(ladybird copy-assets)
