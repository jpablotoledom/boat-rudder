cmake_minimum_required(VERSION 3.10)
project(base-http-server C)

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

    # Web server utilities
    src/web_server/utils/url_parser.c
    src/web_server/utils/static_file_server.c

    # General utilities
    src/utils/config_loader.c
    src/utils/log.c
    src/utils/http_utils.c
    src/utils/detect_epoch.c
    src/utils/read_file.c
    src/utils/generate_url_theme.c
    src/utils/template_utils.c
    src/utils/build_epoch_response.c

    # Retro-compatible CMS: HTML builder
    src/html_builder/orchestrator.c
    src/modules/container/container.c
    src/modules/menu/menu.c
    src/modules/slider/slider.c
    src/modules/home_content/home_content.c
    src/modules/home_blog/home_blog.c
    src/modules/login/login.c
    src/modules/dashboard/dashboard.c
    src/modules/error/error.c

    # Database / auth / sessions
    src/db/mongodb_manager.c
    src/db/auth.c
    src/db/session_manager.c
)

add_executable(base-http-server ${SOURCES})

target_include_directories(base-http-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(base-http-server
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    PkgConfig::MONGOC
    PkgConfig::SODIUM
)

install(TARGETS base-http-server RUNTIME DESTINATION bin)
