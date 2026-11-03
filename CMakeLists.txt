cmake_minimum_required(VERSION 3.16)

# Firmware version. A build is "<version.txt>-dev+g<commit>[.dirty]",
# which sorts before the release it leads to, unless it is given a release version: -DCBX_VERSION=0.1.0
# (./build.sh --release does this) or CBX_VERSION=0.1.0 in the environment (CI). A given version is
# always announced (a CBX_VERSION left in the environment is a warning), and ./build.sh clears both
# for a development build. The version is the image's app version (Device.GetInfo.version). ESP-IDF
# keeps at most 31 characters.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" CBX_BASE_VERSION LIMIT_COUNT 1)
set(cbx_ver_from "")
if(DEFINED CBX_VERSION AND NOT "${CBX_VERSION}" STREQUAL "")
    set(PROJECT_VER "${CBX_VERSION}")
    set(cbx_ver_from "-DCBX_VERSION")
elseif(DEFINED ENV{CBX_VERSION} AND NOT "$ENV{CBX_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{CBX_VERSION}")
    set(cbx_ver_from "CBX_VERSION in the environment")
else()
    set(PROJECT_VER "${CBX_BASE_VERSION}-dev")
    execute_process(COMMAND git rev-parse --short=7 HEAD
                    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                    OUTPUT_VARIABLE cbx_commit OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE git_rc)
    if(git_rc EQUAL 0 AND cbx_commit)
        execute_process(COMMAND git status --porcelain --untracked-files=no
                        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                        OUTPUT_VARIABLE git_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(APPEND PROJECT_VER "+g${cbx_commit}")
        if(git_dirty)
            string(APPEND PROJECT_VER ".dirty")
        endif()
    endif()
endif()
string(LENGTH "${PROJECT_VER}" ver_len)
if(ver_len GREATER 31)
    message(FATAL_ERROR "firmware version '${PROJECT_VER}' is longer than 31 characters")
endif()
if(cbx_ver_from STREQUAL "-DCBX_VERSION")
    message(NOTICE "-- Companion Box firmware ${PROJECT_VER}: release version from -DCBX_VERSION, not from git")
elseif(cbx_ver_from)
    message(WARNING "Companion Box firmware ${PROJECT_VER}: release version from CBX_VERSION in the "
                    "environment, not from git. Unset CBX_VERSION for a development build.")
else()
    message(STATUS "Companion Box firmware ${PROJECT_VER}")
endif()
# The version is worked out at configure time: configure again when version.txt or the checked-out
# commit changes, not only on a clean build. HEAD's reflog changes at every commit, checkout and reset,
# also when the branch ref is packed; the git directories come from git itself, so a worktree (where
# .git is a file) works too. Uncommitted edits (".dirty") are seen only when CMake configures again:
# ./build.sh always does.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/version.txt")
execute_process(COMMAND git rev-parse --git-dir --git-common-dir
                WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                OUTPUT_VARIABLE cbx_git_dirs OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE git_rc)
if(git_rc EQUAL 0 AND cbx_git_dirs)
    string(REPLACE "\n" ";" cbx_git_dirs "${cbx_git_dirs}")
    list(GET cbx_git_dirs 0 cbx_git_dir)
    list(GET cbx_git_dirs -1 cbx_git_common)
    get_filename_component(cbx_git_dir "${cbx_git_dir}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    get_filename_component(cbx_git_common "${cbx_git_common}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    set(cbx_git_deps "${cbx_git_dir}/HEAD" "${cbx_git_dir}/logs/HEAD" "${cbx_git_common}/packed-refs")
    if(EXISTS "${cbx_git_dir}/HEAD")
        file(READ "${cbx_git_dir}/HEAD" git_head)
        if(git_head MATCHES "^ref: ([^\n]+)")
            list(APPEND cbx_git_deps "${cbx_git_common}/${CMAKE_MATCH_1}")
        endif()
    endif()
    foreach(f IN LISTS cbx_git_deps)
        if(EXISTS "${f}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${f}")
        endif()
    endforeach()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(companion_box)
