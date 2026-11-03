idf_component_register(SRCS "c6fw.c" "c6fw_logic.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES breadcrumb feedback espressif__esp_hosted app_update esp_system esp_hw_support freertos log soc)

# The C6's ESP-Hosted firmware that this image carries (c6fw.c keeps the C6 on
# it), built first by tools/build_c6.sh into build-c6/; its version becomes
# c6fw_version.h. Without build-c6/ the build still succeeds with C6 updating
# disabled (C6FW_HAVE_IMAGE 0). The CONFIGURE_DEPENDS glob makes the next
# idf.py build reconfigure on its own once build_c6.sh has made (or a clean
# has removed) the image.
idf_build_get_property(project_dir PROJECT_DIR)
set(c6_dir "${project_dir}/build-c6")
file(GLOB c6_present CONFIGURE_DEPENDS "${c6_dir}/network_adapter.bin" "${c6_dir}/version.txt")
list(LENGTH c6_present c6_count)
if(c6_count EQUAL 2)
    file(READ "${c6_dir}/version.txt" c6_ver)
    string(STRIP "${c6_ver}" c6_ver)
    if(NOT c6_ver MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        message(FATAL_ERROR "build-c6/version.txt: no x.y.z version ('${c6_ver}'): run tools/build_c6.sh again")
    endif()
    set(c6_hdr_dir "${CMAKE_CURRENT_BINARY_DIR}/c6fw_gen")
    # CONFIGURE writes only when the content changed: no rebuild at every reconfigure.
    file(CONFIGURE OUTPUT "${c6_hdr_dir}/c6fw_version.h"
        CONTENT "#pragma once\n#define C6FW_MAJOR ${CMAKE_MATCH_1}\n#define C6FW_MINOR ${CMAKE_MATCH_2}\n#define C6FW_PATCH ${CMAKE_MATCH_3}\n"
        @ONLY)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${c6_hdr_dir}")
    target_add_binary_data(${COMPONENT_LIB} "${c6_dir}/network_adapter.bin" BINARY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE C6FW_HAVE_IMAGE=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${c6_dir}/version.txt")
    message(STATUS "c6fw: carrying the C6's ESP-Hosted ${c6_ver} firmware (build-c6/network_adapter.bin)")
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE C6FW_HAVE_IMAGE=0)
    message(WARNING "c6fw: build-c6/network_adapter.bin or version.txt is missing, so this image carries no C6 "
                    "firmware and C6 updating is disabled. Run tools/build_c6.sh, then idf.py build.")
endif()
