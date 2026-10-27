# Ship of Harkinian for the PlayStation 5 - what the console adds to Shipwright's own build.
# (Derived from griches/PS5_Ghostship's ps5-game.cmake.)
#
# tools/build.sh configures vendor/ghostship itself with the payload SDK's
# toolchain, and names this file as CMAKE_PROJECT_Ship_INCLUDE: CMake
# includes it right after the project() call, before the build reads its
# options and looks for its dependencies. It provides:
#
#   SDL2     .deps/SDL2, patched (tools/setup-sdl.sh), with the drivers in ps5/sdl
#   Vulkan   RADV's headers (the driver itself is linked by tools/link-title.sh)
#   shaderc  built from source: libultraship compiles its shaders at run time
#   zlib     the headers of the zlib inside the RADV archive
#   options  the console's choices among Ghostship's own
#
# and makes the executable's link PS5_Vulkan's (tools/link-title.sh, with RADV).
# Only the top-level project gets this (CMAKE_PROJECT_<name>_INCLUDE).
#
# SPDX-License-Identifier: MIT

foreach(required PS5_PORT_DIR PS5_VULKAN_DIR SDL_DIR)
	if(NOT ${required})
		message(FATAL_ERROR "ps5-game.cmake: ${required} is not set (use ps5/tools/build.sh)")
	endif()
endforeach()
set(ps5 ${PS5_PORT_DIR}/ps5)
include(FetchContent)

# -fno-omit-frame-pointer: the console's crash backtraces are frame-pointer walks.
add_compile_options(-g1 -ffunction-sections -fdata-sections -fno-omit-frame-pointer)
# The console's CPU is a Zen 2: the toolchain's default is the first x86-64.
add_compile_options(-march=znver2)
# An init array, in the order the source has: tools/cc-init-array.sh has the reason
set(CMAKE_C_COMPILER_LAUNCHER sh ${ps5}/tools/cc-init-array.sh)
set(CMAKE_CXX_COMPILER_LAUNCHER sh ${ps5}/tools/cc-init-array.sh)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF)
# no C++ modules anywhere: the toolchain has no dependency scanner
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# The platform archive, by its file and not its target: it is linked whole into the title (below)
set(ps5_platform_archive ${CMAKE_BINARY_DIR}/libps5_platform.a)

# zlib is in the RADV archive already (Mesa's own): its headers, and nothing to link
set(ZLIB_INCLUDE_DIR ${PS5_VULKAN_DIR}/.deps/native/zlib/root/usr/include CACHE PATH "" FORCE)
set(ZLIB_LIBRARY ${ps5_platform_archive} CACHE FILEPATH "" FORCE)

# The console: klog, the pad, and the libc names a title cannot import
add_library(ps5_platform STATIC ${ps5}/src/platform.c ${ps5}/src/shims.c ${ps5}/src/startup.c
	${ps5}/src/devloop_shot.c)
target_include_directories(ps5_platform PUBLIC ${ps5}/src)
# The crash handler (every build: it writes /data/soh/logs/crash-*.txt, no network)
target_sources(ps5_platform PRIVATE ${ps5}/src/crash.c)

# Developer tools (DEVTOOLS.md): a debug server for scripted tests. Only a dev build
# (PS5_DEVTOOLS=1 ps5/tools/build.sh) compiles it in; a release has none of its code, and
# a dev build only starts it when /data/soh/devtools.enable exists.
option(PS5_DEVTOOLS "build the developer tools (debug server) into the title: never for a release" OFF)
if(PS5_DEVTOOLS)
	add_compile_definitions(PS5_DEVTOOLS=1)
	message(STATUS "ps5-game.cmake: PS5_DEVTOOLS=ON (a dev build, not for release)")
endif()
file(READ ${ps5}/sce_sys/param.json param_json)
string(JSON PS5_TITLE_NAME GET "${param_json}" localizedParameters en-US titleName)
target_compile_definitions(ps5_platform PRIVATE "PS5_TITLE_NAME=\"${PS5_TITLE_NAME}\"")
string(JSON PS5_TITLE_ID GET "${param_json}" titleId)
add_compile_definitions("PS5_TITLE_ID=\"${PS5_TITLE_ID}\"")
target_include_directories(ps5_platform PRIVATE ${ZLIB_INCLUDE_DIR})
set_target_properties(ps5_platform PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# SDL2: threads, time and files as a FreeBSD has them; video, input and audio from ps5/sdl
set(SDL_PS5_PORT_DIR ${ps5}/sdl CACHE INTERNAL "")
set(SDL_PS5_PLATFORM_DIR ${ps5}/src CACHE INTERNAL "")
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
foreach(off SDL_SHARED SDL_TEST SDL_TESTS
		SDL_X11 SDL_WAYLAND SDL_KMSDRM SDL_RPI SDL_VIVANTE SDL_OFFSCREEN SDL_DUMMYVIDEO SDL_DIRECTFB
		SDL_OPENGL SDL_OPENGLES SDL_VULKAN
		SDL_OSS SDL_ALSA SDL_JACK SDL_ESD SDL_ARTS SDL_NAS SDL_PIPEWIRE SDL_PULSEAUDIO SDL_SNDIO SDL_FUSIONSOUND
		SDL_DISKAUDIO SDL_DUMMYAUDIO SDL_LIBSAMPLERATE
		SDL_DBUS SDL_IBUS SDL_LIBUDEV SDL_HIDAPI SDL_HIDAPI_LIBUSB SDL_HIDAPI_JOYSTICK SDL_VIRTUAL_JOYSTICK
		SDL_SYSTEM_ICONV SDL_LIBICONV SDL_RPATH SDL_SENSOR SDL_HAPTIC SDL_POWER)
	set(${off} OFF CACHE BOOL "" FORCE)
endforeach()
# (SDL_LOADSO stays on: without it SDL drops its Vulkan support. SDL_VULKAN is SDL's switch for a desktop's loader: ps5/patches/sdl.patch turns
# Vulkan on for the console itself)
add_subdirectory(${SDL_DIR} ${CMAKE_BINARY_DIR}/SDL2 EXCLUDE_FROM_ALL)
target_include_directories(ps5_platform PRIVATE ${SDL_DIR}/include)
if(NOT TARGET SDL2::SDL2)
	add_library(SDL2::SDL2 ALIAS SDL2-static)
endif()
# <SDL.h> and <SDL2/SDL.h> both: the build tree has its headers under include/SDL2
set(SDL2_INCLUDE_DIRS ${CMAKE_BINARY_DIR}/SDL2/include ${CMAKE_BINARY_DIR}/SDL2/include/SDL2
	${CMAKE_BINARY_DIR}/SDL2/include-config-release)
include_directories(${SDL2_INCLUDE_DIRS})

# Vulkan: RADV's headers. The driver is in the RADV archive the link adds.
set(Vulkan_INCLUDE_DIR ${PS5_VULKAN_DIR}/.deps/native/radv-release/include CACHE PATH "" FORCE)
set(Vulkan_LIBRARY ${ps5_platform_archive} CACHE FILEPATH "" FORCE)
include_directories(SYSTEM ${Vulkan_INCLUDE_DIR})

# ... and no loader: volk fetches every command from RADV's entry point, so nothing
# may declare the commands as functions
add_compile_definitions(VK_NO_PROTOTYPES)
add_library(volk STATIC ${ps5}/third_party/volk/volk.c)
target_include_directories(volk PUBLIC ${ps5}/third_party/volk)

# shaderc (with glslang and SPIRV-Tools): GLSL to SPIR-V on the console
set(SHADERC_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
# (SHADERC_SKIP_INSTALL stays off: on, shaderc hands glslang a generator expression as a boolean)
set(SHADERC_SKIP_INSTALL OFF CACHE BOOL "" FORCE)
set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "" FORCE)
set(SHADERC_ENABLE_SHARED_CRT OFF CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SPIRV_HEADERS_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SHADERC_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SHADERC_DIR "" CACHE PATH "shaderc with its third_party checkouts (tools/setup-shaderc.sh)")
if(NOT SHADERC_DIR)
	message(FATAL_ERROR "ps5-game.cmake: SHADERC_DIR is not set (use ps5/tools/build.sh)")
endif()
add_subdirectory(${SHADERC_DIR} ${CMAKE_BINARY_DIR}/shaderc EXCLUDE_FROM_ALL)
# libultraship links whichever of these exists
add_library(Vulkan::shaderc_combined INTERFACE IMPORTED GLOBAL)
target_link_libraries(Vulkan::shaderc_combined INTERFACE shaderc)
target_include_directories(Vulkan::shaderc_combined INTERFACE ${SHADERC_DIR}/libshaderc/include)

# libzip's checks link against the SDK's stubs, which name functions the console's
# C library does not declare
foreach(absent HAVE_MEMCPY_S HAVE_STRNCPY_S HAVE_STRERROR_S HAVE_STRERRORLEN_S)
	set(${absent} "" CACHE INTERNAL "" FORCE)
endforeach()

# Shipwright's dependencies, from source: the console has no packages to find
# (the toolchain says FreeBSD, so libultraship includes no platform dependency file)
set(tinyxml2_BUILD_TESTING OFF)
FetchContent_Declare(tinyxml2 GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git GIT_TAG 10.0.0 GIT_SHALLOW TRUE OVERRIDE_FIND_PACKAGE)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.12.0 GIT_SHALLOW TRUE OVERRIDE_FIND_PACKAGE)
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.16.0 GIT_SHALLOW TRUE OVERRIDE_FIND_PACKAGE)
foreach(off BUILD_TOOLS BUILD_REGRESS BUILD_EXAMPLES BUILD_DOC BUILD_OSSFUZZ ENABLE_BZIP2 ENABLE_LZMA ENABLE_ZSTD
		ENABLE_COMMONCRYPTO ENABLE_GNUTLS ENABLE_MBEDTLS ENABLE_OPENSSL ENABLE_WINDOWS_CRYPTO)
	set(${off} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(libzip GIT_REPOSITORY https://github.com/nih-at/libzip.git GIT_TAG v1.11.4 GIT_SHALLOW TRUE OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(tinyxml2 nlohmann_json spdlog libzip)
if(NOT TARGET libzip::zip)
	add_library(libzip::zip ALIAS zip)
endif()
include_directories(${libzip_SOURCE_DIR}/lib ${libzip_BINARY_DIR})

# SDL2_net (Shipwright's network features include it unconditionally): BSD sockets, from source
set(SDL2NET_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2NET_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2NET_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2_net GIT_REPOSITORY https://github.com/libsdl-org/SDL_net.git GIT_TAG release-2.2.0 GIT_SHALLOW TRUE OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(SDL2_net)
if(NOT TARGET SDL2_net::SDL2_net-static)
	add_library(SDL2_net::SDL2_net-static ALIAS SDL2_net)
endif()
# <SDL2/SDL_net.h>
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/sdl2net-include/SDL2)
file(COPY ${sdl2_net_SOURCE_DIR}/SDL_net.h DESTINATION ${CMAKE_BINARY_DIR}/sdl2net-include/SDL2)
include_directories(${CMAKE_BINARY_DIR}/sdl2net-include)

# The audio codecs SoH's sample importer uses (ogg, vorbis, opus, opusfile)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OP_DISABLE_HTTP ON CACHE BOOL "" FORCE)
set(OP_DISABLE_DOCS ON CACHE BOOL "" FORCE)
set(OP_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
FetchContent_Declare(Ogg GIT_REPOSITORY https://github.com/xiph/ogg.git GIT_TAG v1.3.6 GIT_SHALLOW TRUE OVERRIDE_FIND_PACKAGE)
FetchContent_Declare(Vorbis GIT_REPOSITORY https://github.com/xiph/vorbis.git GIT_TAG v1.3.7 GIT_SHALLOW TRUE OVERRIDE_FIND_PACKAGE)
FetchContent_Declare(Opus GIT_REPOSITORY https://github.com/xiph/opus.git GIT_TAG v1.5.2 GIT_SHALLOW TRUE OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(Ogg Opus)
if(NOT TARGET Ogg::ogg)
	add_library(Ogg::ogg ALIAS ogg)
endif()
if(NOT TARGET Opus::opus)
	add_library(Opus::opus ALIAS opus)
endif()
FetchContent_MakeAvailable(Vorbis)
FetchContent_Declare(OpusFile GIT_REPOSITORY https://github.com/xiph/opusfile.git GIT_TAG v0.12 GIT_SHALLOW TRUE)
FetchContent_GetProperties(OpusFile)
if(NOT opusfile_POPULATED)
	FetchContent_Populate(OpusFile)
endif()
# opusfile, without its http and its own CMake (which wants installed packages)
add_library(opusfile STATIC ${opusfile_SOURCE_DIR}/src/info.c ${opusfile_SOURCE_DIR}/src/internal.c
	${opusfile_SOURCE_DIR}/src/opusfile.c ${opusfile_SOURCE_DIR}/src/stream.c)
target_include_directories(opusfile PUBLIC ${opusfile_SOURCE_DIR}/include)
target_link_libraries(opusfile PUBLIC ogg opus)
target_compile_definitions(opusfile PRIVATE OP_DISABLE_HTTP OP_DISABLE_FLOAT_API=0)
include_directories(${opusfile_SOURCE_DIR}/include ${opus_SOURCE_DIR}/include ${ogg_SOURCE_DIR}/include
	${ogg_BINARY_DIR}/include ${vorbis_SOURCE_DIR}/include)

# Shipwright's options, as the console has them
set(BUILD_REMOTE_CONTROL OFF CACHE BOOL "" FORCE)
set(USE_NETWORKING OFF CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(DISABLE_DLL_LOADER ON CACHE BOOL "" FORCE)

# The link: every object and archive CMake would hand the linker goes to
# PS5_Vulkan's recipe instead (RADV, the platform layer, the CRT, the fake SELF)
set(CMAKE_CXX_LINK_EXECUTABLE
	"bash ${ps5}/tools/link-title.sh ${CMAKE_BINARY_DIR} ${PS5_VULKAN_DIR} ${PS5_PAYLOAD_SDK} --whole-archive ${ps5_platform_archive} --no-whole-archive <OBJECTS> <LINK_LIBRARIES>")
# (the start-up constructor in the platform archive has no caller: hence whole)

# The platform archive is named by its file in the link, so the game does not
# depend on it by itself (the game's target exists once its CMakeLists.txt has run)
function(ps5_late_fixes)
	add_dependencies(soh ps5_platform)
	# what the crash report says about the game (scene, Link): every build
	target_sources(soh PRIVATE ${ps5}/src/crash_context.c)
	target_include_directories(soh PRIVATE ${ps5}/src)
	if(PS5_DEVTOOLS)
		# the debug server reads the game's own state: built with the game's includes
		target_sources(soh PRIVATE ${ps5}/src/devtools/devtools_server.cpp ${ps5}/src/devtools/devtools_game.cpp)
		target_include_directories(soh PRIVATE ${ps5}/src/devtools)
	endif()
	set_property(TARGET soh APPEND PROPERTY LINK_DEPENDS ${ps5_platform_archive})
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL ps5_late_fixes)
