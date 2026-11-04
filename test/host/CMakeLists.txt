cmake_minimum_required(VERSION 3.16)
project(companion_box_host_tests C)
enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(UNITY_DIR $ENV{IDF_PATH}/components/unity/unity)
set(CJSON_DIR $ENV{IDF_PATH}/components/json/cJSON)
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Third-party sources are built without -Werror.
add_library(unity STATIC ${UNITY_DIR}/src/unity.c)
target_include_directories(unity PUBLIC ${UNITY_DIR}/src)
add_library(cjson STATIC ${CJSON_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${CJSON_DIR})

# Every component's public include dir is on every test's include path.
file(GLOB COMPONENT_INCLUDE_DIRS LIST_DIRECTORIES true ${REPO_ROOT}/components/*/include)

# add_host_test(name SOURCES a.c b.c ... [WITH_CJSON])
# Creates executable test_<name>, registered with ctest. Sources are relative
# to this directory (or absolute). Components' include dirs are added.
function(add_host_test name)
  cmake_parse_arguments(T "WITH_CJSON" "" "SOURCES" ${ARGN})
  set(target test_${name})
  add_executable(${target} ${T_SOURCES})
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  target_include_directories(${target} PRIVATE ${COMPONENT_INCLUDE_DIRS})
  target_link_libraries(${target} PRIVATE unity)
  if(T_WITH_CJSON)
    target_link_libraries(${target} PRIVATE cjson)
  endif()
  add_test(NAME ${name} COMMAND ${target} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

add_host_test(smoke SOURCES test_smoke.c)

add_host_test(codec SOURCES test_codec.c
  ../../components/satproto/codec.c ../../components/satproto/b64.c WITH_CJSON)
target_compile_definitions(test_codec PRIVATE
  CORPUS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/satellite-protocol-corpus.json")

add_host_test(session SOURCES test_session.c
  ../../components/satproto/session.c ../../components/satproto/lineasm.c
  ../../components/satproto/codec.c ../../components/satproto/b64.c)

add_host_test(model SOURCES test_model.c
  ../../components/deckmodel/model.c ../../components/deckmodel/manifest.c WITH_CJSON)

add_host_test(deckproto SOURCES test_deckproto.c
  ../../components/deckproto/images.c ../../components/deckproto/features.c
  ../../components/deckproto/input.c ../../components/deckmodel/model.c)

add_host_test(deckproto_parse SOURCES test_deckproto_parse.c
  ../../components/deckproto/images.c ../../components/deckproto/parse.c ../../components/deckmodel/model.c)

add_host_test(imgproc SOURCES test_imgproc.c
  ../../components/imgproc/transform.c ../../components/imgproc/bmp.c)

# lodepng (third party, zlib licence) built as the firmware builds it: the decoder only, without -Werror
add_library(lodepng STATIC ${REPO_ROOT}/components/imgproc/lodepng/lodepng.c)
target_include_directories(lodepng PUBLIC ${REPO_ROOT}/components/imgproc/lodepng)
target_compile_definitions(lodepng PUBLIC LODEPNG_NO_COMPILE_ENCODER LODEPNG_NO_COMPILE_DISK LODEPNG_NO_COMPILE_CPP
  LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS LODEPNG_NO_COMPILE_ERROR_TEXT)
add_host_test(png SOURCES test_png.c ../../components/imgproc/png.c)
target_link_libraries(test_png PRIVATE lodepng)
target_compile_definitions(test_png PRIVATE PNG_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/png")

add_host_test(statusui SOURCES test_statusui.c statusui_fixtures.c
  ../../components/statusui/layout.c ../../components/statusui/render.c ../../components/statusui/deck_font.c
  ../../components/deckmodel/model.c)
target_link_libraries(test_statusui PRIVATE m) # WCAG contrast (pow)

# Not a test: PNG renders of every deck screen on every key size (build-host/statusui_renders <out dir>).
find_package(ZLIB)
if(ZLIB_FOUND)
  add_executable(statusui_renders statusui_renders.c
    ../../components/statusui/layout.c ../../components/statusui/render.c ../../components/statusui/deck_font.c
    ../../components/deckmodel/model.c)
  target_compile_options(statusui_renders PRIVATE -Wall -Wextra -Werror)
  target_include_directories(statusui_renders PRIVATE ${COMPONENT_INCLUDE_DIRS})
  target_link_libraries(statusui_renders PRIVATE ZLIB::ZLIB)
endif()

# The deck maintenance UI engine; qrcodegen is third-party (built without -Werror).
add_library(qrcodegen STATIC ${REPO_ROOT}/components/deckui/qrcodegen/qrcodegen.c)
set(DECKUI_SRCS ../../components/deckui/screen.c ../../components/deckui/plan.c ../../components/deckui/align.c
  ../../components/deckui/text.c ../../components/deckui/raster.c ../../components/deckui/icons.c ../../components/deckui/phosphor.c
  ../../components/deckui/draw.c ../../components/deckui/qr.c ../../components/deckui/screens.c
  ../../components/deckui/nav.c ../../components/deckui/fp.c ../../components/deckui/pad.c ../../components/deckui/pin.c ../../components/deckui/ipedit.c
  ../../components/statusui/layout.c ../../components/statusui/deck_font.c ../../components/deckmodel/model.c)
add_host_test(deckui SOURCES test_deckui.c deckui_fixtures.c ${DECKUI_SRCS})
target_link_libraries(test_deckui PRIVATE qrcodegen m)
add_host_test(deckui_screens SOURCES test_deckui_screens.c ${DECKUI_SRCS})
target_link_libraries(test_deckui_screens PRIVATE qrcodegen m)
add_host_test(deckui_nav SOURCES test_deckui_nav.c ${DECKUI_SRCS})
target_link_libraries(test_deckui_nav PRIVATE qrcodegen m)
add_host_test(deckui_pin SOURCES test_deckui_pin.c ${DECKUI_SRCS})
target_link_libraries(test_deckui_pin PRIVATE qrcodegen m)
add_host_test(deckui_ipedit SOURCES test_deckui_ipedit.c ${DECKUI_SRCS})
target_link_libraries(test_deckui_ipedit PRIVATE qrcodegen m)
# The maintenance UI's glue in surfaces: the snapshot, who gets a press, input as navigation events.
add_host_test(maintmap SOURCES test_maintmap.c ../../components/surfaces/maintmap.c ${DECKUI_SRCS})
target_include_directories(test_maintmap PRIVATE ${REPO_ROOT}/components/surfaces)
target_link_libraries(test_maintmap PRIVATE qrcodegen m)

# Not a test: PNG renders of the maintenance UI's screens on every model (build-host/deckui_renders <out dir>).
if(ZLIB_FOUND)
  add_executable(deckui_renders deckui_renders.c deckui_fixtures.c ${DECKUI_SRCS})
  target_compile_options(deckui_renders PRIVATE -Wall -Wextra -Werror)
  target_include_directories(deckui_renders PRIVATE ${COMPONENT_INCLUDE_DIRS})
  target_link_libraries(deckui_renders PRIVATE qrcodegen ZLIB::ZLIB m)
endif()

add_host_test(config SOURCES test_config.c
  ../../components/boxconfig/config_json.c ../../components/boxconfig/led_pins.c WITH_CJSON)

add_host_test(netpolicy SOURCES test_netpolicy.c ../../components/netmgr/policy.c ../../components/netmgr/wififo_logic.c
  ../../components/netmgr/scan_logic.c)
add_host_test(wifiscan SOURCES test_wifiscan.c ../../components/netmgr/scan_logic.c)
add_host_test(wififo SOURCES test_wififo.c ../../components/netmgr/wififo_logic.c)

add_host_test(select SOURCES test_select.c ../../components/discovery/select.c)

add_host_test(surfaces SOURCES test_surfaces.c
  ../../components/surfaces/registry.c ../../components/surfaces/coalesce.c
  ../../components/surfaces/inputmap.c ../../components/surfaces/message.c
  ../../components/deckmodel/model.c ../../components/satproto/b64.c)

add_host_test(c6fw_logic SOURCES test_c6fw_logic.c ../../components/c6fw/c6fw_logic.c)
target_include_directories(test_c6fw_logic PRIVATE ${REPO_ROOT}/components/c6fw)

add_host_test(quickreset_logic SOURCES test_quickreset_logic.c ../../main/quickreset_logic.c)
target_include_directories(test_quickreset_logic PRIVATE ${REPO_ROOT}/main)

add_host_test(svcbtn_logic SOURCES test_svcbtn_logic.c ../../main/svcbtn_logic.c)
target_include_directories(test_svcbtn_logic PRIVATE ${REPO_ROOT}/main)

add_host_test(rpc_core SOURCES test_rpc_core.c ../../components/rpc/rpc_core.c WITH_CJSON)

add_host_test(auth_logic SOURCES test_auth_logic.c ../../components/webui/auth_logic.c)
target_include_directories(test_auth_logic PRIVATE ${REPO_ROOT}/components/webui)
target_compile_options(test_auth_logic PRIVATE -O2) # as on the box (webui/CMakeLists.txt)

add_host_test(web_logic SOURCES test_web_logic.c ../../components/webui/web_logic.c
  ../../components/boxconfig/config_json.c ../../components/boxconfig/led_pins.c WITH_CJSON)
target_include_directories(test_web_logic PRIVATE ${REPO_ROOT}/components/webui)

add_host_test(usbdeck_policy SOURCES test_usbdeck_policy.c ../../components/usbdeck/usbdeck_policy.c)
target_include_directories(test_usbdeck_policy PRIVATE ${REPO_ROOT}/components/usbdeck)

add_host_test(corner SOURCES test_corner.c ../../components/deckproto/corner.c ../../components/deckmodel/model.c)

add_host_test(ingate SOURCES test_ingate.c ../../components/elgdock/ingate.c)
target_include_directories(test_ingate PRIVATE ${REPO_ROOT}/components/elgdock)

add_host_test(dockcache SOURCES test_dockcache.c ../../components/surfaces/dockcache.c
  ../../components/deckproto/images.c ../../components/deckproto/parse.c ../../components/deckproto/features.c
  ../../components/deckmodel/model.c)
target_include_directories(test_dockcache PRIVATE ${REPO_ROOT}/components/surfaces)

add_host_test(surf_problems SOURCES test_surf_problems.c ../../components/surfaces/problems.c)
target_include_directories(test_surf_problems PRIVATE ${REPO_ROOT}/components/surfaces)

add_host_test(ws_table SOURCES test_ws_table.c ../../components/webui/ws_table.c)
target_include_directories(test_ws_table PRIVATE ${REPO_ROOT}/components/webui)

add_host_test(notify_logic SOURCES test_notify_logic.c ../../components/webui/notify_logic.c WITH_CJSON)
target_include_directories(test_notify_logic PRIVATE ${REPO_ROOT}/components/webui)

add_host_test(cora SOURCES test_cora.c ../../components/elgdock/cora.c)
target_include_directories(test_cora PRIVATE ${REPO_ROOT}/components/elgdock)

add_host_test(dockproto SOURCES test_dockproto.c
  ../../components/elgdock/dockproto.c ../../components/elgdock/cora.c)
target_include_directories(test_dockproto PRIVATE ${REPO_ROOT}/components/elgdock)

add_host_test(breadcrumb_logic SOURCES test_breadcrumb_logic.c ../../components/breadcrumb/breadcrumb_logic.c)
target_include_directories(test_breadcrumb_logic PRIVATE ${REPO_ROOT}/components/breadcrumb)

add_host_test(satclient_logic SOURCES test_satclient_logic.c ../../components/satclient/satclient_logic.c)
target_include_directories(test_satclient_logic PRIVATE ${REPO_ROOT}/components/satclient)

add_host_test(supervisor_logic SOURCES test_supervisor_logic.c ../../main/supervisor_logic.c)
target_include_directories(test_supervisor_logic PRIVATE ${REPO_ROOT}/main)

add_host_test(feedback_logic SOURCES test_feedback_logic.c ../../components/feedback/fbstatus.c)
target_include_directories(test_feedback_logic PRIVATE ${REPO_ROOT}/components/feedback)

add_host_test(feedback_sound SOURCES test_feedback_sound.c ../../components/feedback/fbsound.c
  ../../components/feedback/tones.c ../../components/feedback/tonegen.c)
target_include_directories(test_feedback_sound PRIVATE ${REPO_ROOT}/components/feedback)

add_host_test(slotsync_logic SOURCES test_slotsync_logic.c ../../components/slotsync/slotsync_logic.c)
target_include_directories(test_slotsync_logic PRIVATE ${REPO_ROOT}/components/slotsync)
