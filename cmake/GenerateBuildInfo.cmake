cmake_minimum_required(VERSION 3.16)

foreach(required IN ITEMS
        MSS_SOURCE_DIR
        MSS_VERSION_BASE
        MSS_GIT_COMMIT_INPUT
        MSS_BUILD_REVISION_INPUT
        MSS_BUILD_TIMESTAMP_INPUT
        MSS_VERSION_HEADER
        MSS_TIMESTAMP_HEADER)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "GenerateBuildInfo.cmake requires ${required}")
    endif()
endforeach()

set(zero_commit "0000000000000000000000000000000000000000")
set(have_git_checkout FALSE)
set(git_dirty FALSE)
set(head_commit "")

if(EXISTS "${MSS_SOURCE_DIR}/.git")
    if(NOT DEFINED MSS_GIT_EXECUTABLE OR
       MSS_GIT_EXECUTABLE STREQUAL "" OR
       MSS_GIT_EXECUTABLE MATCHES "-NOTFOUND$")
        message(FATAL_ERROR "Git is required to derive source provenance")
    endif()
    set(have_git_checkout TRUE)
    execute_process(
        COMMAND "${MSS_GIT_EXECUTABLE}" rev-parse --verify HEAD
        WORKING_DIRECTORY "${MSS_SOURCE_DIR}"
        RESULT_VARIABLE head_result
        OUTPUT_VARIABLE head_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    string(LENGTH "${head_commit}" head_commit_length)
    if(NOT head_result EQUAL 0 OR
       NOT head_commit_length EQUAL 40 OR
       NOT head_commit MATCHES "^[0-9a-f][0-9a-f]*$")
        message(FATAL_ERROR "Could not resolve the source Git commit")
    endif()

    execute_process(
        COMMAND "${MSS_GIT_EXECUTABLE}" diff-index --quiet
            --ignore-submodules=none HEAD --
        WORKING_DIRECTORY "${MSS_SOURCE_DIR}"
        RESULT_VARIABLE dirty_result
        ERROR_QUIET)
    if(dirty_result EQUAL 1)
        set(git_dirty TRUE)
    elseif(NOT dirty_result EQUAL 0)
        message(FATAL_ERROR "Could not inspect the source Git worktree")
    endif()

    # Untracked build inputs are not reported by diff-index. Restrict the
    # query to source/package inputs so ordinary ignored build directories do
    # not make an otherwise exact checkout appear dirty.
    execute_process(
        COMMAND "${MSS_GIT_EXECUTABLE}" ls-files --others --exclude-standard --
            CMakeLists.txt README.md SOURCE_MANIFEST.md THIRD_PARTY_NOTICES.md
            LICENSE cmake config.example.json docs include packaging scripts
            src tests
        WORKING_DIRECTORY "${MSS_SOURCE_DIR}"
        RESULT_VARIABLE untracked_result
        OUTPUT_VARIABLE untracked_inputs
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT untracked_result EQUAL 0)
        message(FATAL_ERROR "Could not inspect untracked source inputs")
    endif()
    if(NOT untracked_inputs STREQUAL "")
        set(git_dirty TRUE)
    endif()
endif()

if(MSS_GIT_COMMIT_INPUT STREQUAL "AUTO")
    if(have_git_checkout AND NOT git_dirty)
        set(resolved_commit "${head_commit}")
    else()
        set(resolved_commit "${zero_commit}")
    endif()
else()
    string(LENGTH "${MSS_GIT_COMMIT_INPUT}" commit_length)
    if(NOT commit_length EQUAL 40 OR
       NOT MSS_GIT_COMMIT_INPUT MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "MSS_GIT_COMMIT must be AUTO or exactly 40 lowercase hexadecimal characters")
    endif()
    if(have_git_checkout AND git_dirty AND
       NOT MSS_GIT_COMMIT_INPUT STREQUAL zero_commit)
        message(FATAL_ERROR
            "A tracked-dirty source tree cannot claim an exact MSS_GIT_COMMIT")
    endif()
    if(have_git_checkout AND NOT git_dirty AND
       NOT MSS_GIT_COMMIT_INPUT STREQUAL zero_commit AND
       NOT MSS_GIT_COMMIT_INPUT STREQUAL head_commit)
        message(FATAL_ERROR
            "MSS_GIT_COMMIT does not match the checked-out source commit")
    endif()
    set(resolved_commit "${MSS_GIT_COMMIT_INPUT}")
endif()

if(MSS_BUILD_REVISION_INPUT STREQUAL "AUTO")
    if(have_git_checkout)
        execute_process(
            COMMAND "${MSS_GIT_EXECUTABLE}" rev-parse --is-shallow-repository
            WORKING_DIRECTORY "${MSS_SOURCE_DIR}"
            RESULT_VARIABLE shallow_result
            OUTPUT_VARIABLE is_shallow
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT shallow_result EQUAL 0)
            message(FATAL_ERROR "Could not inspect Git history depth")
        endif()
        if(is_shallow STREQUAL "true")
            message(FATAL_ERROR
                "Automatic build revisions require full Git history; unshallow the clone or set MSS_BUILD_REVISION")
        endif()
        execute_process(
            COMMAND "${MSS_GIT_EXECUTABLE}" rev-list --count HEAD
            WORKING_DIRECTORY "${MSS_SOURCE_DIR}"
            RESULT_VARIABLE revision_result
            OUTPUT_VARIABLE resolved_revision
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT revision_result EQUAL 0)
            message(FATAL_ERROR "Could not derive the build revision")
        endif()
    else()
        set(resolved_revision "0")
    endif()
else()
    set(resolved_revision "${MSS_BUILD_REVISION_INPUT}")
endif()
if(NOT resolved_revision MATCHES "^(0|[1-9][0-9]*)$")
    message(FATAL_ERROR
        "MSS_BUILD_REVISION must be AUTO or a nonnegative integer without leading zeroes")
endif()

set(provenance_suffix "")
if(resolved_commit STREQUAL zero_commit OR
   (NOT have_git_checkout AND MSS_BUILD_REVISION_INPUT STREQUAL "AUTO"))
    set(provenance_suffix ".unknown")
endif()
if(git_dirty)
    set(provenance_suffix ".dirty")
endif()
set(full_version
    "${MSS_VERSION_BASE}+rev.${resolved_revision}${provenance_suffix}")

if(MSS_BUILD_TIMESTAMP_INPUT STREQUAL "AUTO")
    string(TIMESTAMP resolved_timestamp "%Y-%m-%d %H:%M:%S UTC" UTC)
else()
    set(resolved_timestamp "${MSS_BUILD_TIMESTAMP_INPUT}")
endif()
if(NOT resolved_timestamp MATCHES
   "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9] [0-9][0-9]:[0-9][0-9]:[0-9][0-9] UTC$")
    message(FATAL_ERROR
        "MSS_BUILD_TIMESTAMP must be AUTO or YYYY-MM-DD HH:MM:SS UTC")
endif()

function(write_if_different destination content)
    get_filename_component(destination_directory "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${destination_directory}")
    set(temporary "${destination}.tmp")
    file(WRITE "${temporary}" "${content}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${temporary}" "${destination}"
        RESULT_VARIABLE copy_result)
    file(REMOVE "${temporary}")
    if(NOT copy_result EQUAL 0)
        message(FATAL_ERROR "Could not update ${destination}")
    endif()
endfunction()

write_if_different(
    "${MSS_VERSION_HEADER}"
    "#pragma once\n#define MSS_VERSION \"${full_version}\"\n#define MSS_GIT_COMMIT \"${resolved_commit}\"\n")
write_if_different(
    "${MSS_TIMESTAMP_HEADER}"
    "#pragma once\n#define MSS_BUILD_TIMESTAMP \"${resolved_timestamp}\"\n")

message(STATUS
    "Building monero-solo-stratum ${full_version} at ${resolved_timestamp}")
