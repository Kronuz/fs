# fs: filesystem helpers (path normalization, recursive mkdir, glob delete/quarantine,
# directory copy/move) extracted from Xapiand.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
#
# A compiled static library (fs.cc) plus a header (fs.hh). Depends on a few Kronuz
# libraries for the real work (io for EINTR-safe file ops, strings/split/stringified for
# path handling); logging is an optional header seam (FS_TRACE_HEADER) that defaults to a
# no-op, so error-string / repr helpers are only needed if a host opts into real logging.

cmake_minimum_required(VERSION 3.16)
project(fs LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# --- Dependencies (the Kronuz family, tracked at tip) ---
#   io          -> io::open/close/read/write/mkdtemp (io.hh)
#   strings     -> strings::startswith / endswith    (strings.hh)
#   split       -> Split<>                            (split.h)
#   stringified -> stringified                        (stringified.hh)
FetchContent_Declare(io          GIT_REPOSITORY https://github.com/Kronuz/io.git          GIT_TAG main)
FetchContent_Declare(strings     GIT_REPOSITORY https://github.com/Kronuz/strings.git     GIT_TAG main)
FetchContent_Declare(split       GIT_REPOSITORY https://github.com/Kronuz/split.git       GIT_TAG main)
FetchContent_Declare(stringified GIT_REPOSITORY https://github.com/Kronuz/stringified.git GIT_TAG main)
FetchContent_MakeAvailable(io strings split stringified)

# --- Library target ---
add_library(fs STATIC fs.cc)
target_include_directories(fs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(fs PUBLIC cxx_std_20)
target_link_libraries(fs PUBLIC io::io strings::strings split stringified::stringified)
add_library(fs::fs ALIAS fs)

# Build the test only when this repo is the top-level project, so consumers
# (e.g. via FetchContent) don't build it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	enable_testing()
	add_executable(fs_test test/test.cc)
	target_link_libraries(fs_test PRIVATE fs)
	add_test(NAME fs COMMAND fs_test)

	# A realistic usage example that mirrors Xapiand's database directory layout:
	# normalize a data path, create the .xapiand tree, receive files into a
	# mkdtemp switch dir, move them into place, quarantine WAL files, and delete
	# shard file globs. It self-checks and is also registered as a ctest test.
	add_executable(fs_db_layout examples/db_layout.cc)
	target_link_libraries(fs_db_layout PRIVATE fs)
	add_test(NAME fs_db_layout COMMAND fs_db_layout)
endif()
