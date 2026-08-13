cmake_minimum_required(VERSION 3.16)

project(monero-solo-stratum VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)

option(MSS_BUILD_TESTS "Build the monero-solo-stratum test suite" ON)
option(MSS_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MSS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
set(MSS_GIT_COMMIT "0000000000000000000000000000000000000000" CACHE STRING
    "Exact commit for runtime provenance; all zeroes means an uncommitted source tree")
set(MSS_SYSTEMD_UNIT_DIR "lib/systemd/system" CACHE PATH
    "Install destination for the systemd service unit")
string(LENGTH "${MSS_GIT_COMMIT}" MSS_GIT_COMMIT_LENGTH)
if(NOT MSS_GIT_COMMIT_LENGTH EQUAL 40 OR
   NOT MSS_GIT_COMMIT MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "MSS_GIT_COMMIT must be exactly 40 lowercase hexadecimal characters")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

foreach(required_dependency IN ITEMS
        third_party/monero-stratum-pow-verifier/CMakeLists.txt
        third_party/monero-stratum-pow-verifier/third_party/RandomX/CMakeLists.txt
        third_party/nlohmann-json/CMakeLists.txt
        third_party/sqlite-amalgamation/sqlite3.c)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${required_dependency}")
        message(FATAL_ERROR
            "Required submodule content is missing: ${required_dependency}\n"
            "For a Git clone, run: git submodule update --init --recursive\n"
            "GitHub's automatic source ZIP/tarball omits submodule contents; "
            "use a recursive clone or the project's flattened release archive.")
    endif()
endforeach()

# A Git build must use the audited dependency checkouts advertised by the
# binary and documentation. Flattened release archives have no .git metadata;
# their complete dependency bytes are instead covered by SHA256SUMS.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    find_package(Git REQUIRED)
    function(mss_require_git_revision relative_path expected_revision)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}"
            RESULT_VARIABLE revision_result
            OUTPUT_VARIABLE actual_revision
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT revision_result EQUAL 0 OR
           NOT actual_revision STREQUAL expected_revision)
            message(FATAL_ERROR
                "Dependency revision mismatch: ${relative_path}\n"
                "Expected: ${expected_revision}\n"
                "Found: ${actual_revision}\n"
                "Run: git submodule update --init --recursive")
        endif()
    endfunction()
    mss_require_git_revision(
        third_party/monero-stratum-pow-verifier
        856c015de433a23fe45d88a18dc08c821e50f1cb)
    mss_require_git_revision(
        third_party/monero-stratum-pow-verifier/third_party/RandomX
        6c4340ba4561aec9a3611c1aedf9931239777fb3)
    mss_require_git_revision(
        third_party/nlohmann-json
        9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03)
    mss_require_git_revision(
        third_party/sqlite-amalgamation
        15d0ff10ebc7e7225eced1de84bb52137000899b)
endif()

set(MSPV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MSPV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MSPV_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(MSPV_BUILD_FAST_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/monero-stratum-pow-verifier)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
add_subdirectory(third_party/nlohmann-json EXCLUDE_FROM_ALL)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_library(mss_sqlite STATIC third_party/sqlite-amalgamation/sqlite3.c)
target_include_directories(mss_sqlite PUBLIC third_party/sqlite-amalgamation)
target_compile_definitions(mss_sqlite PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_ENABLE_COLUMN_METADATA=1
    SQLITE_ENABLE_DBSTAT_VTAB=1
    SQLITE_ENABLE_FTS5=1
    SQLITE_ENABLE_JSON1=1
    SQLITE_ENABLE_MATH_FUNCTIONS=1
    SQLITE_ENABLE_RTREE=1
    SQLITE_ENABLE_STAT4=1
    SQLITE_OMIT_DEPRECATED=1
    SQLITE_THREADSAFE=1
    SQLITE_USE_URI=1
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mss_sqlite PRIVATE -w)
endif()

file(GLOB_RECURSE MSS_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(FILTER MSS_CORE_SOURCES EXCLUDE REGEX "/main\\.cpp$")

add_library(monero_solo_core STATIC ${MSS_CORE_SOURCES})
add_library(monero_solo::core ALIAS monero_solo_core)
target_include_directories(monero_solo_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_definitions(monero_solo_core PUBLIC
    MSS_VERSION="${PROJECT_VERSION}"
    MSS_GIT_COMMIT="${MSS_GIT_COMMIT}"
    MSS_MSPV_COMMIT="856c015de433a23fe45d88a18dc08c821e50f1cb"
)
target_link_libraries(monero_solo_core
    PUBLIC
        mspv::verifier
        nlohmann_json::nlohmann_json
    PRIVATE
        CURL::libcurl
        OpenSSL::Crypto
        Threads::Threads
        mss_sqlite
        ${CMAKE_DL_LIBS}
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(monero_solo_core PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        -Wshadow -Wformat=2 -Wundef -Wnull-dereference
        -Werror
    )
endif()

if(MSS_ENABLE_SANITIZERS AND MSS_ENABLE_TSAN)
    message(FATAL_ERROR "MSS_ENABLE_SANITIZERS and MSS_ENABLE_TSAN are mutually exclusive")
endif()
if(MSS_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(monero_solo_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(monero_solo_core PUBLIC -fsanitize=address,undefined)
endif()
if(MSS_ENABLE_TSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(monero_solo_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(monero_solo_core PUBLIC -fsanitize=thread)
endif()

add_executable(monero-solo-stratum src/main.cpp)
target_link_libraries(monero-solo-stratum PRIVATE monero_solo::core)

install(TARGETS monero-solo-stratum RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS scripts/watch-status.sh
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME mss-watch-status)
# The generated service uses CMAKE_INSTALL_FULL_SYSCONFDIR. Install the
# example into that same configure-time location so the GNUInstallDirs `/usr`
# special case (`/etc`, not `/usr/etc`) cannot make the unit and package tree
# disagree. DESTDIR still supplies a package-staging root.
install(FILES config.example.json
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/monero-solo-stratum)
install(DIRECTORY docs DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES README.md SOURCE_MANIFEST.md THIRD_PARTY_NOTICES.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES third_party/nlohmann-json/LICENSE.MIT
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        RENAME nlohmann-json-LICENSE.MIT)
install(FILES third_party/nlohmann-json/LICENSES/Apache-2.0.txt
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        RENAME nlohmann-json-Apache-2.0.txt)
install(FILES third_party/sqlite-amalgamation/LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        RENAME sqlite-amalgamation-LICENSE)
configure_file(packaging/monero-solo-stratum.service.in
               packaging/monero-solo-stratum.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/packaging/monero-solo-stratum.service
        DESTINATION ${MSS_SYSTEMD_UNIT_DIR})

if(BUILD_TESTING AND MSS_BUILD_TESTS)
    find_program(MSS_BASH_EXECUTABLE bash REQUIRED)
    find_program(MSS_JQ_EXECUTABLE jq REQUIRED)
    file(GLOB MSS_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/*_tests.cpp"
    )
    foreach(test_source IN LISTS MSS_TEST_SOURCES)
        get_filename_component(test_name "${test_source}" NAME_WE)
        add_executable("${test_name}" "${test_source}")
        target_include_directories("${test_name}" PRIVATE
            tests
            third_party/sqlite-amalgamation
        )
        target_link_libraries("${test_name}" PRIVATE monero_solo::core)
        add_test(NAME "${test_name}" COMMAND "${test_name}")
        set_tests_properties("${test_name}" PROPERTIES TIMEOUT 180)
    endforeach()
    # The rental fan-in regression has generous functional deadlines inside
    # the test; its observed wall time and RSS are diagnostics, not benchmarks.
    set_tests_properties(rental_fanin_tests PROPERTIES
        LABELS "integration;stratum;fan-in"
        TIMEOUT 120
    )
    add_test(NAME watch_status_tests
             COMMAND "${MSS_BASH_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/watch_status_tests.sh"
                     "${CMAKE_CURRENT_SOURCE_DIR}/scripts/watch-status.sh")
    set_tests_properties(watch_status_tests PROPERTIES TIMEOUT 30)
endif()
