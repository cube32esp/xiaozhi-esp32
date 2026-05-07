# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Board-specific sdkconfig fragment loading.
# Pass -DBOARD=<board-name> to idf.py to have the fragment loaded automatically.
# Example: idf.py -DBOARD=cube32-s3 set-target esp32s3
# For each entry in SDKCONFIG_DEFAULTS, ESP-IDF also auto-loads entry.<target>
# (e.g. sdkconfig.defaults.esp32s3), so that file is still applied.
if(DEFINED BOARD AND NOT BOARD STREQUAL "")
    file(GLOB_RECURSE _board_fragment
        "${CMAKE_CURRENT_LIST_DIR}/main/boards/*/*/sdkconfig.defaults.${BOARD}")
    if(_board_fragment)
        list(GET _board_fragment 0 _board_fragment_file)
        set(SDKCONFIG_DEFAULTS
            "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
            "${_board_fragment_file}")
        message(STATUS "[board] '${BOARD}': loading sdkconfig fragment ${_board_fragment_file}")
    else()
        message(WARNING "[board] '${BOARD}': no sdkconfig fragment found under main/boards/")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.2.6")
project(xiaozhi)
