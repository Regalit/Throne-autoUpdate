# Release version: prefer an explicit -DINPUT_VERSION=... cache variable, then env.
if (NOT DEFINED INPUT_VERSION)
    set(INPUT_VERSION "$ENV{INPUT_VERSION}")
endif ()
set(NKR_VERSION "${INPUT_VERSION}")

# Shadowlos: one flag switches every dev-only feature, and the version is always given
# explicitly: X.Y.Z for prod, X.Y.Z-dev for dev. The updater compares versions and picks
# its channel from these, so a Windows build never guesses either.
option(SHADOWLOS_DEV "Shadowlos dev build: dev-only features and the dev update channel" OFF)
if (WIN32)
    if (NOT NKR_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-dev)?$")
        message(FATAL_ERROR "Version '${NKR_VERSION}' must be X.Y.Z or X.Y.Z-dev (pass -DINPUT_VERSION=... or env INPUT_VERSION)")
    endif ()
    if (CMAKE_MATCH_1 GREATER 99 OR CMAKE_MATCH_2 GREATER 99 OR CMAKE_MATCH_3 GREATER 99)
        message(FATAL_ERROR "Version '${NKR_VERSION}': each part must be 0..99")
    endif ()
    math(EXPR SHADOWLOS_VERSION_CODE "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2} * 100 + ${CMAKE_MATCH_3}")
    if (SHADOWLOS_DEV AND NOT CMAKE_MATCH_4)
        message(FATAL_ERROR "SHADOWLOS_DEV=ON needs a -dev version, got '${NKR_VERSION}'")
    elseif (NOT SHADOWLOS_DEV AND CMAKE_MATCH_4)
        message(FATAL_ERROR "Version '${NKR_VERSION}' is a dev version; build it with -DSHADOWLOS_DEV=ON")
    endif ()
else ()
    set(SHADOWLOS_VERSION_CODE 0)
endif ()
if (SHADOWLOS_DEV)
    set(SHADOWLOS_DEV_INT 1)
    set(SHADOWLOS_CHANNEL "dev")
    set(SHADOWLOS_EXE_NAME "ShadowLosThroneDev")
else ()
    set(SHADOWLOS_DEV_INT 0)
    set(SHADOWLOS_CHANNEL "prod")
    set(SHADOWLOS_EXE_NAME "ShadowLosThrone")
endif ()
message(STATUS "Shadowlos: channel=${SHADOWLOS_CHANNEL} version_code=${SHADOWLOS_VERSION_CODE}")

# Derive numeric parts for Windows PE VERSIONINFO (X.Y.Z.W) from NKR_VERSION.
# Examples:
#   1.2.3       -> 1.2.3.0
#   1.2.3-beta -> 1.2.3.0
#   v1.2.3      -> 1.2.3.0
set(NKR_VERSION_NUMERIC_STR "${NKR_VERSION}")
string(REGEX REPLACE "^[vV]" "" NKR_VERSION_NUMERIC_STR "${NKR_VERSION_NUMERIC_STR}")
string(REGEX REPLACE "-.*$" "" NKR_VERSION_NUMERIC_STR "${NKR_VERSION_NUMERIC_STR}")

# Defaults match generate_product_version() current fallback: 1.0.0.0
set(NKR_VERSION_MAJOR 1)
set(NKR_VERSION_MINOR 0)
set(NKR_VERSION_PATCH 0)
set(NKR_VERSION_REVISION 0)

# CMake's regex engine has no {n,m} quantifier -- braces match literally, so the
# part count is bounded by the list(GET) guards below rather than by the pattern.
if (NOT NKR_VERSION_NUMERIC_STR MATCHES "^[0-9]+(\\.[0-9]+)*$")
    if (NKR_VERSION MATCHES "([0-9]+\\.[0-9]+(\\.[0-9]+)*)")
        set(NKR_VERSION_NUMERIC_STR "${CMAKE_MATCH_1}")
    endif ()
endif ()

if (NKR_VERSION_NUMERIC_STR MATCHES "^[0-9]+(\\.[0-9]+)*$")
    string(REPLACE "." ";" _nkr_parts "${NKR_VERSION_NUMERIC_STR}")

    list(LENGTH _nkr_parts _nkr_len)
    if (_nkr_len GREATER_EQUAL 1)
        list(GET _nkr_parts 0 NKR_VERSION_MAJOR)
    endif ()
    if (_nkr_len GREATER_EQUAL 2)
        list(GET _nkr_parts 1 NKR_VERSION_MINOR)
    endif ()
    if (_nkr_len GREATER_EQUAL 3)
        list(GET _nkr_parts 2 NKR_VERSION_PATCH)
    endif ()
    if (_nkr_len GREATER_EQUAL 4)
        list(GET _nkr_parts 3 NKR_VERSION_REVISION)
    endif ()
endif ()

message(STATUS "NKR_VERSION='${NKR_VERSION}' -> ${NKR_VERSION_MAJOR}.${NKR_VERSION_MINOR}.${NKR_VERSION_PATCH}.${NKR_VERSION_REVISION}")

configure_file("${CMAKE_SOURCE_DIR}/cmake/NkrVersion.h.in" "${CMAKE_BINARY_DIR}/NkrVersion.h" @ONLY)

# Debug
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DNKR_CPP_DEBUG")

# Func
function(nkr_add_compile_definitions arg)
    message("[add_compile_definitions] ${ARGV}")
    add_compile_definitions(${ARGV})
endfunction()
