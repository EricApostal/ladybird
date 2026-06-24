# Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
#
# SPDX-License-Identifier: BSD-2-Clause
#

#
# Copy resources into tarball for inclusion in /assets of APK
#
set(LADYBIRD_RESOURCE_ROOT "${LADYBIRD_SOURCE_DIR}/Base/res")

function(copy_res_folder folder)
    set(destination_dir "asset-bundle/${folder}")
    set(stamp_file "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/copy-${folder}.stamp")

    file(GLOB_RECURSE resource_inputs CONFIGURE_DEPENDS "${LADYBIRD_RESOURCE_ROOT}/${folder}/*")

    add_custom_command(
        OUTPUT "${stamp_file}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${LADYBIRD_RESOURCE_ROOT}/${folder}" "${destination_dir}"
        COMMAND ${CMAKE_COMMAND} -E touch "${stamp_file}"
        DEPENDS ${resource_inputs}
        VERBATIM
    )

    add_custom_target(copy-${folder} DEPENDS "${stamp_file}")
    add_dependencies(archive-assets copy-${folder})
endfunction()

set(LADYBIRD_ASSET_BUNDLE_ZIP "${CMAKE_CURRENT_BINARY_DIR}/ladybird-assets.zip")

add_custom_command(
    OUTPUT "${LADYBIRD_ASSET_BUNDLE_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory asset-bundle
    COMMAND ${CMAKE_COMMAND} -E chdir asset-bundle zip -r ../ladybird-assets.zip ./
    DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/copy-ladybird.stamp"
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/copy-fonts.stamp"
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/copy-icons.stamp"
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/copy-themes.stamp"
    VERBATIM
)

add_custom_target(archive-assets DEPENDS "${LADYBIRD_ASSET_BUNDLE_ZIP}")

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

add_custom_command(
    OUTPUT "${LADYBIRD_ANDROID_ASSETS_OUTPUT_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LADYBIRD_ASSET_BUNDLE_ZIP}" "${LADYBIRD_ANDROID_ASSETS_OUTPUT_PATH}"
    DEPENDS "${LADYBIRD_ASSET_BUNDLE_ZIP}"
    VERBATIM
)

add_custom_target(copy-assets DEPENDS "${LADYBIRD_ANDROID_ASSETS_OUTPUT_PATH}")
add_dependencies(ladybird copy-assets)
