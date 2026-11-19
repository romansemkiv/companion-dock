# auth_logic.c, web_logic.c, ws_table.c, notify_logic.c, perf_logic.c and cfgext_logic.c are pure (also built by the
# host tests); the rest is ESP.
idf_component_register(SRCS "web.c" "web_logic.c" "ws_table.c" "methods.c" "auth.c" "auth_logic.c" "logbuf.c"
                            "notify.c" "notify_logic.c" "deckact.c" "perf.c" "perf_logic.c" "cfgext_logic.c"
                       INCLUDE_DIRS "include"
                       REQUIRES boxconfig esp_common json netmgr surfaces
                       PRIV_REQUIRES rpc esp_http_server app_update esp_app_format nvs_flash esp_hw_support
                                     esp_system esp_timer esp_netif freertos log lwip heap discovery
                                     satclient usbdeck c6fw elgdock breadcrumb feedback slotsync deckui esp_driver_tsens)

# The page (www/index.html) is embedded gzip-compressed and served with Content-Encoding: gzip.
# tools/pack_page.py strips the comments, indentation and
# blank lines of the embedded copy (the source keeps them), compresses it (gzip -9, mtime=0 keeps the output
# reproducible), and fails the build when it is over its limit, 40 KB gzipped (LIMIT in tools/pack_page.py, the
# one place it is set). tools/check_pages.js (run by tools/test-host.sh) checks that the scripts of the source and
# of the stripped copy compile and run.
idf_build_get_property(python PYTHON)
set(page "index.html")
set(pack "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/pack_page.py")
set(index_gz "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
# Optional: a JS file whose text goes in at the page's /*@ext*/ marker (sections and navigation another build
# adds), and the size limit in bytes to use instead of pack_page.LIMIT. Both empty: the page as the source has it.
set(WEBUI_PAGE_EXT "" CACHE STRING "A JS file packed into the page at its /*@ext*/ marker")
set(WEBUI_PAGE_LIMIT "" CACHE STRING "The page's gzipped size limit in bytes (empty: pack_page.LIMIT)")
set(pack_args "")
set(pack_deps "")
if(WEBUI_PAGE_EXT)
    list(APPEND pack_args --ext "${WEBUI_PAGE_EXT}")
    list(APPEND pack_deps "${WEBUI_PAGE_EXT}")
endif()
if(WEBUI_PAGE_LIMIT)
    list(APPEND pack_args --limit "${WEBUI_PAGE_LIMIT}")
endif()
add_custom_command(OUTPUT "${index_gz}"
    COMMAND "${python}" "${pack}" "${CMAKE_CURRENT_SOURCE_DIR}/www/${page}" "${index_gz}" ${pack_args}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/www/${page}" "${pack}" ${pack_deps}
    VERBATIM)
target_add_binary_data(${COMPONENT_LIB} "${index_gz}" BINARY)
# the page's size against its limit (pack_page.LIMIT, or WEBUI_PAGE_LIMIT), printed whenever the component is rebuilt
add_custom_command(TARGET ${COMPONENT_LIB} POST_BUILD
    COMMAND "${python}" -B -c "import os,sys;sys.path.insert(0,sys.argv[3]);import pack_page as p;g=os.path.getsize(sys.argv[2]);print('webui: index.html %d bytes, embedded gzip %d bytes (%.1f KB of %d KB)'%(os.path.getsize(sys.argv[1]),g,g/1024,int(sys.argv[4] or p.LIMIT)//1024))"
            "${CMAKE_CURRENT_SOURCE_DIR}/www/${page}" "${index_gz}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools"
            "${WEBUI_PAGE_LIMIT}"
    VERBATIM)

# PBKDF2 (10 000 HMAC rounds per password check) runs on the HTTP server's task: optimise this file
# even in -Og builds.
set_source_files_properties(auth_logic.c PROPERTIES COMPILE_OPTIONS "-O2")
