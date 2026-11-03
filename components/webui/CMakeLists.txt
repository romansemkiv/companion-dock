# auth_logic.c, web_logic.c, ws_table.c and notify_logic.c are pure (also built by the host tests); the rest is ESP.
idf_component_register(SRCS "web.c" "web_logic.c" "ws_table.c" "methods.c" "auth.c" "auth_logic.c" "logbuf.c"
                            "notify.c" "notify_logic.c" "deckact.c"
                       INCLUDE_DIRS "include"
                       REQUIRES boxconfig esp_common netmgr surfaces
                       PRIV_REQUIRES rpc json esp_http_server app_update esp_app_format nvs_flash esp_hw_support
                                     esp_system esp_timer esp_netif freertos log lwip heap discovery
                                     satclient usbdeck c6fw elgdock breadcrumb feedback slotsync deckui)

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
add_custom_command(OUTPUT "${index_gz}"
    COMMAND "${python}" "${pack}" "${CMAKE_CURRENT_SOURCE_DIR}/www/${page}" "${index_gz}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/www/${page}" "${pack}"
    VERBATIM)
target_add_binary_data(${COMPONENT_LIB} "${index_gz}" BINARY)
# the page's size against its limit (pack_page.LIMIT), printed whenever the component is rebuilt
add_custom_command(TARGET ${COMPONENT_LIB} POST_BUILD
    COMMAND "${python}" -B -c "import os,sys;sys.path.insert(0,sys.argv[3]);import pack_page as p;g=os.path.getsize(sys.argv[2]);print('webui: index.html %d bytes, embedded gzip %d bytes (%.1f KB of %d KB)'%(os.path.getsize(sys.argv[1]),g,g/1024,p.LIMIT//1024))"
            "${CMAKE_CURRENT_SOURCE_DIR}/www/${page}" "${index_gz}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools"
    VERBATIM)

# PBKDF2 (10 000 HMAC rounds per password check) runs on the HTTP server's task: optimise this file
# even in -Og builds.
set_source_files_properties(auth_logic.c PROPERTIES COMPILE_OPTIONS "-O2")
