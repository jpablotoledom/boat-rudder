cmake_minimum_required(VERSION 3.10)
project(boat-rudder C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_definitions(-D_XOPEN_SOURCE=700)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Debug build: enabling AddressSanitizer")
    add_compile_options(-fsanitize=address -g -O0)
    add_link_options(-fsanitize=address)
endif()

# ── Dependencies ─────────────────────────────────────────────────────────────

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MONGOC REQUIRED IMPORTED_TARGET libmongoc-1.0)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
# libcurl: outgoing mail over SMTP (utils/mailer.c).
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

# Optional: MaxMind GeoLite2 for country detection in analytics (src/modules/
# analytics/geoip.c). Install libmaxminddb-dev and place a GeoLite2-Country.mmdb
# file at ./data/GeoLite2-Country.mmdb to enable - without it, every visit's
# country buckets under "Unknown" instead of failing to build.
pkg_check_modules(MAXMINDDB libmaxminddb)
if(MAXMINDDB_FOUND)
    message(STATUS "libmaxminddb found - GeoIP country detection enabled")
    add_definitions(-DHAVE_MAXMINDDB)
    include_directories(${MAXMINDDB_INCLUDE_DIRS})
else()
    message(STATUS "libmaxminddb not found - GeoIP country detection disabled (every visit shows Unknown)")
endif()

# ── Sources ───────────────────────────────────────────────────────────────────

set(SOURCES
    src/main.c

    # Web server core
    src/web_server/server_listener.c
    src/web_server/connection.c
    src/web_server/connection_thread.c
    src/web_server/tls_context.c
    src/web_server/http_router.c
    src/web_server/http_request_parser.c
    src/web_server/proxy_protocol.c

    # Web server utilities
    src/web_server/utils/url_parser.c
    src/web_server/utils/static_file_server.c

    # General utilities
    src/utils/config_loader.c
    src/utils/image_size.c
    src/utils/category_tags.c
    src/utils/code_highlight.c
    src/html_builder/page_layout.c
    src/utils/log.c
    src/utils/http_utils.c
    src/utils/detect_epoch.c
    src/utils/css_compat.c
    src/utils/markdown_lite.c
    src/utils/read_file.c
    src/utils/generate_url_theme.c
    src/utils/template_utils.c
    src/utils/build_epoch_response.c
    src/utils/json_utils.c
    src/utils/qr_generator/qr_generator.c
    src/utils/wbmp_writer.c
    src/utils/image_convert/image_convert.c
    src/utils/request_charset.c
    src/utils/request_code_lines.c
    src/utils/request_wml.c
    src/wap_gateway/wap_gateway.c
    src/wap_gateway/wbxml.c
    src/utils/request_lang.c
    src/utils/request_theme.c
    src/utils/request_site.c
    src/utils/theme_catalog.c
    src/utils/request_user.c
    src/utils/ua_parser.c

    # Retro-compatible CMS: HTML builder
    src/html_builder/orchestrator.c
    src/modules/menu/menu.c
    src/modules/mainbanner/mainbanner.c
    src/modules/home_content/home_content.c
    src/modules/blog_list/blog_list.c
    src/modules/category_menu/category_menu.c
    src/modules/entry_page/entry_page.c
    src/modules/language_page/language_page.c
    src/modules/site_page/site_page.c
    src/modules/theme_page/theme_page.c
    src/modules/login/login.c
    src/modules/dashboard/dashboard.c
    src/modules/error/error.c
    src/modules/categories_admin/categories_admin.c
    src/modules/languages_admin/languages_admin.c
    src/modules/entries_admin/entries_admin.c
    src/modules/menu_admin/menu_admin.c
    src/modules/users_admin/users_admin.c
    src/modules/entry_editor/entry_editor.c
    src/modules/entry_editor/entry_editor_blocks.c
    src/modules/media_admin/media_admin.c
    src/modules/site_settings_admin/site_settings_admin.c
    src/modules/browser_detection_admin/browser_detection_admin.c
    src/modules/updates_admin/updates_admin.c
    src/modules/extensions_admin/extensions_admin.c
    src/modules/search_page/search_page.c
    src/modules/comments/comments.c
    src/modules/comments_admin/comments_admin.c
    src/modules/fonts_admin/fonts_admin.c
    src/modules/analytics/analytics.c
    src/modules/analytics/geoip.c
    src/modules/analytics_view/analytics_view.c
    src/modules/analytics_view/analytics_charts.c
    src/modules/analytics_view/country_continent.c

    # Web server utilities (multipart)
    src/web_server/utils/multipart_parser.c

    # Database / auth / sessions
    src/db/mongodb_manager.c
    src/db/auth.c
    src/db/bson_lang.c
    src/db/cms_entries.c
    src/db/cms_entries_admin.c
    src/db/cms_menu.c
    src/db/cms_categories.c
    src/db/cms_languages.c
    src/db/cms_users_admin.c
    src/db/cms_media.c
    src/db/cms_media_galleries.c
    src/db/short_links.c
    src/db/cms_site_settings.c
    src/db/browser_detection_bson.c
    src/db/cms_browser_detection.c
    src/db/cms_themes.c
    src/db/cms_extensions.c
    src/db/cms_search.c
    src/db/cms_comments.c
    src/utils/mailer.c
    src/utils/captcha.c
    src/extensions/extensions.c
    src/db/cms_fonts.c
    src/db/language_catalog.c
    src/db/session_manager.c
)

add_executable(boat-rudder ${SOURCES})

# The version a build reports (`boat-rudder --version`, the startup log line):
# the first line of ./VERSION, which the updater also installs and compares
# (develop_docs/plans/update-system-plan.md). Changing the file reconfigures.
set(BOAT_RUDDER_VERSION "unknown")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BOAT_RUDDER_VERSION LIMIT_COUNT 1)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
target_compile_definitions(boat-rudder PRIVATE BOAT_RUDDER_VERSION="${BOAT_RUDDER_VERSION}")

target_include_directories(boat-rudder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
)

# libqrencode: retro epochs render YouTube blocks as a scannable QR code
# (no iframe before HTML5), so the encoder is a hard dependency. Ships no
# pkg-config file; qrencode_minimal.h declares the two symbols we use.
find_library(QRENCODE_LIBRARY NAMES qrencode libqrencode.so.4)
if(NOT QRENCODE_LIBRARY)
    message(FATAL_ERROR "libqrencode not found - install libqrencode4 (runtime) or libqrencode-dev")
endif()

target_link_libraries(boat-rudder
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    PkgConfig::MONGOC
    PkgConfig::SODIUM
    PkgConfig::CURL
    ${QRENCODE_LIBRARY}
    m
)

# Link libmaxminddb when available (see the pkg_check_modules(MAXMINDDB ...)
# gate above).
if(MAXMINDDB_FOUND)
    target_link_libraries(boat-rudder ${MAXMINDDB_LIBRARIES})
endif()

install(TARGETS boat-rudder RUNTIME DESTINATION bin)

# ── Tests ─────────────────────────────────────────────────────────────────────
# Unit tests (no server, no MongoDB connection), built with the server and run with
# `ctest --test-dir build/debug`. Not installed.

enable_testing()

add_executable(detect_epoch_test tests/detect_epoch_test.c src/utils/detect_epoch.c)
target_link_libraries(detect_epoch_test Threads::Threads)
add_test(NAME detect_epoch COMMAND detect_epoch_test)

add_executable(browser_detection_bson_test tests/browser_detection_bson_test.c
               src/db/browser_detection_bson.c src/utils/detect_epoch.c)
target_link_libraries(browser_detection_bson_test PkgConfig::MONGOC Threads::Threads)
add_test(NAME browser_detection_bson COMMAND browser_detection_bson_test)

add_executable(captcha_test tests/captcha_test.c src/utils/captcha.c)
target_link_libraries(captcha_test PkgConfig::SODIUM m)
add_test(NAME captcha COMMAND captcha_test)

add_executable(proxy_protocol_test tests/proxy_protocol_test.c src/web_server/proxy_protocol.c)
target_link_libraries(proxy_protocol_test Threads::Threads)
add_test(NAME proxy_protocol COMMAND proxy_protocol_test)

add_executable(css_compat_test tests/css_compat_test.c src/utils/css_compat.c src/utils/detect_epoch.c)
target_link_libraries(css_compat_test Threads::Threads)
add_test(NAME css_compat COMMAND css_compat_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
