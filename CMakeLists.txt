# CMake build for the goradio SA-MP plugin. The Makefile alongside this
# covers Linux; this is here for Windows/MSVC and for anyone whose
# toolchain is already CMake-shaped.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#
# SA-MP itself is a 32-bit process, so the default is a 32-bit build.
# Pass -DGORADIO_32BIT=OFF for open.mp or for host-side testing.
cmake_minimum_required(VERSION 3.5)
project(goradio CXX)

option(GORADIO_32BIT "Build a 32-bit binary (required by samp03svr)" ON)
option(GORADIO_TLS "Link OpenSSL so https:// audio server URLs work" OFF)

# Baked into the binary and reported in the server log. Release builds
# pass -DGORADIO_VERSION=v1.2.3; a local build says "dev".
set(GORADIO_VERSION "dev" CACHE STRING "Version string reported by the plugin")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

file(GLOB GORADIO_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(goradio SHARED ${GORADIO_SOURCES})
target_include_directories(goradio PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/sdk
	${CMAKE_CURRENT_SOURCE_DIR}/src)

# The server looks for "goradio.so" / "goradio.dll" -- no "lib" prefix.
set_target_properties(goradio PROPERTIES PREFIX "" OUTPUT_NAME "goradio")
target_compile_definitions(goradio PRIVATE GORADIO_VERSION="${GORADIO_VERSION}")

if(GORADIO_32BIT AND NOT MSVC)
	target_compile_options(goradio PRIVATE -m32)
	target_link_options(goradio PRIVATE -m32)
endif()

if(NOT MSVC)
	target_compile_options(goradio PRIVATE -W -Wall -Wextra -Wno-unused-parameter)
	# Old servers run old runtimes; static-link ours so one build travels.
	target_link_options(goradio PRIVATE -static-libgcc -static-libstdc++)
endif()

if(UNIX AND NOT APPLE)
	# Keep the statically linked C++ runtime out of the dynamic symbol
	# table, so this plugin can't shadow another plugin's copy.
	target_link_options(goradio PRIVATE
		"-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/sdk/goradio.ver")
endif()

find_package(Threads REQUIRED)
target_link_libraries(goradio PRIVATE Threads::Threads)

if(WIN32)
	target_link_libraries(goradio PRIVATE ws2_32)
	if(MSVC)
		# The entry points are __stdcall; the .def re-exports them
		# undecorated, which is how the server resolves them.
		set_target_properties(goradio PROPERTIES
			LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/sdk/goradio.def\"")
	endif()
endif()

if(GORADIO_TLS)
	# Static by default: a plugin that needs a matching libssl on the
	# target box is not much use to a SA-MP server running an older
	# OpenSSL than the build machine. A release binary carries its own.
	set(OPENSSL_USE_STATIC_LIBS ON)
	find_package(OpenSSL REQUIRED)
	target_compile_definitions(goradio PRIVATE GORADIO_TLS)
	target_link_libraries(goradio PRIVATE OpenSSL::SSL OpenSSL::Crypto)
	if(WIN32)
		# Static OpenSSL on Windows pulls in the platform crypto and
		# registry APIs that its own code calls.
		target_link_libraries(goradio PRIVATE crypt32 advapi32 user32)
	endif()
endif()
