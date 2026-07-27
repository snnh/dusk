# Applies the project-maintained Aurora compatibility patch before Aurora is added
# to the build.  This lets an ordinary recursive clone configure locally, while
# treating a patch already applied by CI as a no-op.
function(dusk_apply_aurora_patch aurora_dir patch_file)
    get_filename_component(_aurora_dir "${aurora_dir}" ABSOLUTE)
    get_filename_component(_patch_file "${patch_file}" ABSOLUTE)

    if (NOT EXISTS "${_aurora_dir}/.git")
        message(FATAL_ERROR
                "Aurora submodule is unavailable at '${_aurora_dir}'. "
                "Initialize it with: git submodule update --init --recursive")
    endif ()
    if (NOT EXISTS "${_patch_file}")
        message(FATAL_ERROR "Aurora compatibility patch is missing: '${_patch_file}'")
    endif ()

    find_package(Git REQUIRED)
    # Explicitly trust this checked-out submodule. This also allows CMake to be
    # run by a different local user than the one that initialized the checkout.
    set(_aurora_git "${GIT_EXECUTABLE}" "-c" "safe.directory=${_aurora_dir}" "-C" "${_aurora_dir}")

    # A reverse check succeeds only when every hunk is already present, which
    # makes a CI pre-application or a previous CMake configure idempotent.
    execute_process(
            COMMAND ${_aurora_git} apply --check --reverse --whitespace=nowarn "${_patch_file}"
            RESULT_VARIABLE _reverse_result
            OUTPUT_VARIABLE _reverse_stdout
            ERROR_VARIABLE _reverse_stderr)
    if (_reverse_result EQUAL 0)
        message(STATUS "dusklight: Aurora compatibility patch is already applied")
        return()
    endif ()

    # Never alter an incomplete or independently modified Aurora worktree.
    # This protects developer changes and makes any required manual resolution
    # explicit instead of silently mixing them with the compatibility patch.
    execute_process(
            COMMAND ${_aurora_git} status --porcelain --untracked-files=no
            RESULT_VARIABLE _status_result
            OUTPUT_VARIABLE _status_output
            ERROR_VARIABLE _status_stderr)
    if (NOT _status_result EQUAL 0)
        message(FATAL_ERROR
                "Could not inspect the Aurora worktree before applying the compatibility patch.\n"
                "${_status_stderr}")
    endif ()
    if (NOT _status_output STREQUAL "")
        message(FATAL_ERROR
                "Aurora needs the Dusklight compatibility patch, but its worktree is modified. "
                "Resolve or stash the Aurora changes, then configure again.\n${_status_output}")
    endif ()

    execute_process(
            COMMAND ${_aurora_git} apply --check --whitespace=nowarn "${_patch_file}"
            RESULT_VARIABLE _check_result
            OUTPUT_VARIABLE _check_stdout
            ERROR_VARIABLE _check_stderr)
    if (NOT _check_result EQUAL 0)
        message(FATAL_ERROR
                "The Aurora compatibility patch cannot be applied to this submodule revision.\n"
                "${_check_stdout}${_check_stderr}")
    endif ()

    execute_process(
            COMMAND ${_aurora_git} apply --whitespace=nowarn "${_patch_file}"
            RESULT_VARIABLE _apply_result
            OUTPUT_VARIABLE _apply_stdout
            ERROR_VARIABLE _apply_stderr)
    if (NOT _apply_result EQUAL 0)
        message(FATAL_ERROR
                "Failed to apply the Aurora compatibility patch.\n${_apply_stdout}${_apply_stderr}")
    endif ()

    message(STATUS "dusklight: Applied Aurora compatibility patch")
endfunction()
