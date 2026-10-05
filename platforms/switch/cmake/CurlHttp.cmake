# aleks2: real HTTP on Switch, in place of the stub.
#
# borealis already has a curl backend, and devkitPro ships libcurl as a portlib,
# so nothing had to be written -- only wired up. What made it look impossible is
# that TLS on a console usually means building mbedtls or OpenSSL: devkitPro's
# libcurl instead talks to **Horizon's own ssl service** (it references
# sslInitialize, sslCreateContext and nifmInitialize), so HTTPS needs no TLS
# library shipped with us at all.
#
# borealis cannot build this target itself, which is why it is here: its http
# needs borealis::io, and on Switch borealis::io is one of the replacements
# defined in this directory -- after extern/borealis has already been added. So
# the target is built from borealis' own sources, the way borealis would.
#
# Sockets are the one thing libcurl does not start; src/dusk/switch_net.cpp does.

if (NOT ALEKS2_SWITCH_HTTP)
    return ()
endif ()
if (TARGET borealis_http)
    return ()
endif ()

set(_borealis_dir "${CMAKE_SOURCE_DIR}/extern/borealis")
if (NOT TARGET borealis_io)
    message(FATAL_ERROR "CurlHttp.cmake must be included after borealis::io is defined")
endif ()

find_package(CURL 7.47 QUIET)
if (NOT CURL_FOUND)
    # A hard stop rather than a quiet fallback. There is no stub behind this any
    # more -- the online mod browser, mod updates, the guide downloader and the
    # update check all go through it -- so a build without it would look finished
    # and be unable to reach anything.
    message(FATAL_ERROR
            "aleks2: libcurl is needed for HTTP on Switch and was not found. "
            "Install it with `dkp-pacman -S switch-curl`, or configure with "
            "-DALEKS2_SWITCH_HTTP=OFF to build deliberately offline.")
endif ()

add_library(borealis_http STATIC
        "${_borealis_dir}/src/http.cpp"
        "${_borealis_dir}/src/http_download.cpp"
        "${_borealis_dir}/src/http/curl.cpp")
add_library(borealis::http ALIAS borealis_http)
target_include_directories(borealis_http
        PUBLIC "${_borealis_dir}/include"
        PRIVATE "${_borealis_dir}/src")
target_link_libraries(borealis_http
        PUBLIC borealis::core borealis::task borealis::url
        PRIVATE borealis::io borealis::log nlohmann_json::nlohmann_json CURL::libcurl)
target_compile_features(borealis_http PUBLIC cxx_std_20)
target_compile_options(borealis_http PRIVATE ${DUSK_SWITCH_OPT_FLAGS})

message(STATUS "aleks2: HTTP on Switch through libcurl ${CURL_VERSION_STRING} "
        "(TLS by Horizon's ssl service)")
