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

# The normalize_path micro-bench (bench/normalize_path_bench.cc), off by default: with the library's
# loop and the two it replaced frozen in the file, three binaries that ab_compare.py can pair.
option(FS_BUILD_BENCH "Build the normalize_path micro-bench" OFF)

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
FetchContent_Declare(io          GIT_REPOSITORY https://github.com/Kronuz/io.git          GIT_TAG 93ddcaf3953e9e2abd0ea0615313fa1accb57c16)
FetchContent_Declare(strings     GIT_REPOSITORY https://github.com/Kronuz/strings.git     GIT_TAG 09d4d1bfbd7dd475cc4c532338fd19beca19dcbe)
FetchContent_Declare(split       GIT_REPOSITORY https://github.com/Kronuz/split.git       GIT_TAG 4b3010cb7b01958c045d9a738a5087887215e099)
FetchContent_Declare(stringified GIT_REPOSITORY https://github.com/Kronuz/stringified.git GIT_TAG dea1ceba684e7fc35b54fa389579e8147ca62051)
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
	find_package(Threads REQUIRED)   # the EINTR fault-injection test runs a reader thread
	add_executable(fs_test test/test.cc)
	target_link_libraries(fs_test PRIVATE fs Threads::Threads)
	add_test(NAME fs COMMAND fs_test)

	# The file identity copy_file refuses on (device and inode), checked on hand-made stat
	# results: it includes fs.cc for the file-local predicate, so it links the dependencies
	# and not the fs library.
	add_executable(fs_identity_test test/identity_test.cc)
	target_include_directories(fs_identity_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
	target_compile_features(fs_identity_test PRIVATE cxx_std_20)
	target_link_libraries(fs_identity_test PRIVATE io::io strings::strings split stringified::stringified)
	add_test(NAME fs_identity COMMAND fs_identity_test)

	# normalize_path against the loop it replaced, frozen, over every short string and random ones, on
	# exact-size blocks (an AddressSanitizer build fails on a byte read or written past them).
	add_executable(fs_normalize_test test/normalize_test.cc)
	target_link_libraries(fs_normalize_test PRIVATE fs)
	add_test(NAME fs_normalize COMMAND fs_normalize_test)

	# The expected values of test/test.cc's normalize_path tests (the digest and the table) are
	# posixpath.normpath's: tools/normpath_oracle.py regenerates them, and this checks the file.
	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(Python3_Interpreter_FOUND)
		add_test(NAME fs_normpath_oracle COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/normpath_oracle.py" --check "${CMAKE_CURRENT_SOURCE_DIR}/test/test.cc")
	endif()

	# bench/normalize_path_bench.cc: the library's loop, the loop of 8934dbc and the one of ce16118.
	if(FS_BUILD_BENCH)
		add_executable(normalize_path_bench bench/normalize_path_bench.cc)
		target_link_libraries(normalize_path_bench PRIVATE fs)
		foreach(frozen HEAD START)
			string(TOLOWER ${frozen} lower)
			add_executable(normalize_path_bench_${lower} bench/normalize_path_bench.cc)
			target_include_directories(normalize_path_bench_${lower} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
			target_compile_definitions(normalize_path_bench_${lower} PRIVATE NP_FROZEN_${frozen})
		endforeach()
	endif()

	# A realistic usage example that mirrors Xapiand's database directory layout:
	# normalize a data path, create the .xapiand tree, receive files into a
	# mkdtemp switch dir, move them into place, quarantine WAL files, and delete
	# shard file globs. It self-checks and is also registered as a ctest test.
	add_executable(fs_db_layout examples/db_layout.cc)
	target_link_libraries(fs_db_layout PRIVATE fs)
	add_test(NAME fs_db_layout COMMAND fs_db_layout)
endif()
