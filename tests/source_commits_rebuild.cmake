# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Run by vkc_source_commits_rebuild: cmake -DVKC_SOURCE_DIR=<core>
# -DVKC_WORK_DIR=<dir> -DVKC_GENERATOR=<generator> [-DVKC_MAKE_PROGRAM=<tool>]
# -DVKC_CXX_COMPILER=<compiler> -DVKC_GIT=<git> -P <this>. Copies
# source_commits/ into <dir>/project and makes it a git checkout, makes
# <dir>/dep a second one, configures once with dep as the package vkc_sc_dep's
# override, then builds again and again without reconfiguring. Every path goes
# through a symbolic link, as /tmp does on macOS, while git names a checkout's
# top by its real path. After each build, the program must print what git says
# of each checkout now:
#
# * nothing changed: the same, and the header is not rewritten, so nothing
#   recompiles;
# * the build run with a git hook's GIT_DIR, GIT_WORK_TREE and GIT_INDEX_FILE,
#   which name the project's checkout: the same, unchanged;
# * a tracked file touched, its text the same: unchanged, and the project's
#   index not rewritten either, as git status would to refresh it;
# * an ignored file: still clean, and not rewritten;
# * an untracked file: dirty; once it goes, clean again;
# * an edit to a tracked file in each checkout: both dirty;
# * a commit of the project's edit: its new commit, clean.
#
# local/, a folder inside the project's checkout, reads no commit throughout.
# Each build waits a second first: the make macOS ships compares times to the
# second.
cmake_minimum_required(VERSION 3.21)

foreach(_var IN ITEMS VKC_SOURCE_DIR VKC_WORK_DIR VKC_GENERATOR
                      VKC_CXX_COMPILER VKC_GIT)
  if("${${_var}}" STREQUAL "")
    message(FATAL_ERROR "source_commits_rebuild.cmake: ${_var} is not set")
  endif()
endforeach()

set(_link "${VKC_WORK_DIR}/link")
set(_project "${_link}/project")
set(_dep "${_link}/dep")
set(_build "${_link}/build")
set(_header "${_build}/generated/source_commits.h")
set(_stamp "${_build}/bin/stamp")
# A fresh copy each run: the last one edited and committed.
file(REMOVE_RECURSE "${VKC_WORK_DIR}")
file(MAKE_DIRECTORY "${VKC_WORK_DIR}/real")
file(CREATE_LINK "${VKC_WORK_DIR}/real" "${_link}" SYMBOLIC)
file(COPY "${CMAKE_CURRENT_LIST_DIR}/source_commits/" DESTINATION "${_project}")
file(WRITE "${_project}/.gitignore" "ignored.txt\n")
file(WRITE "${_dep}/file.txt" "the package's own file\n")

# Runs git in <dir> with an identity of its own, as CI has none; sets
# _vkc_git_out to what it printed.
set(_vkc_git_config user.name=vkc user.email=vkc@example.invalid
                    commit.gpgsign=false init.defaultBranch=main)
function(_vkc_git dir)
  set(_options)
  foreach(_setting IN LISTS _vkc_git_config)
    list(APPEND _options -c "${_setting}")
  endforeach()
  execute_process(
    COMMAND "${VKC_GIT}" -C "${dir}" ${_options} ${ARGN}
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error
    RESULT_VARIABLE _result
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "git ${ARGN} in '${dir}' failed:\n${_output}${_error}")
  endif()
  set(_vkc_git_out
      "${_output}"
      PARENT_SCOPE)
endfunction()

foreach(_dir "${_project}" "${_dep}")
  _vkc_git("${_dir}" init --quiet)
  _vkc_git("${_dir}" add --all)
  _vkc_git("${_dir}" commit --quiet -m "first")
endforeach()

set(_make_program)
if(DEFINED VKC_MAKE_PROGRAM)
  set(_make_program "-DCMAKE_MAKE_PROGRAM=${VKC_MAKE_PROGRAM}")
endif()
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -S "${_project}" -B "${_build}" -G "${VKC_GENERATOR}"
    ${_make_program} "-DCMAKE_CXX_COMPILER=${VKC_CXX_COMPILER}"
    -DCMAKE_BUILD_TYPE=Release "-DVKC_SOURCE_DIR=${VKC_SOURCE_DIR}"
    "-DFETCHCONTENT_SOURCE_DIR_VKC_SC_DEP=${_dep}" COMMAND_ERROR_IS_FATAL ANY)

# Waits a second, builds without reconfiguring, with the environment assignments
# in ARGN on the build alone, and checks what the program prints:
# <project_dirty> and <dep_dirty> with each checkout's HEAD now, and local/ with
# no commit. Sets _vkc_header_time to the header's time.
function(_vkc_build_and_expect step project_dirty dep_dirty)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1 COMMAND_ERROR_IS_FATAL
                          ANY)
  set(_env)
  if(ARGN)
    set(_env "${CMAKE_COMMAND}" -E env ${ARGN})
  endif()
  execute_process(
    COMMAND ${_env} "${CMAKE_COMMAND}" --build "${_build}" --config Release
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _output)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${step}: the build in '${_build}' failed:\n${_output}")
  endif()
  _vkc_git("${_project}" rev-parse HEAD)
  set(_project_head "${_vkc_git_out}")
  _vkc_git("${_dep}" rev-parse HEAD)
  set(_dep_head "${_vkc_git_out}")
  string(CONCAT _expected "project [${_project_head}] ${project_dirty}\n"
                "dep [${_dep_head}] ${dep_dirty} [${_dep}]\n" "local [] 0 []\n")
  execute_process(COMMAND "${_stamp}"
                  OUTPUT_VARIABLE _printed COMMAND_ERROR_IS_FATAL ANY)
  if(NOT _printed STREQUAL _expected)
    message(FATAL_ERROR "${step}: the stamp reads\n${_printed}not\n${_expected}"
                        "The build printed:\n${_output}")
  endif()
  file(TIMESTAMP "${_header}" _time "%s")
  set(_vkc_header_time
      "${_time}"
      PARENT_SCOPE)
endfunction()

# Checks the last build left the header and the program as they were.
function(_vkc_expect_unchanged step header_time stamp_time)
  file(TIMESTAMP "${_stamp}" _time "%s")
  if(NOT _vkc_header_time STREQUAL header_time OR NOT _time STREQUAL stamp_time)
    message(FATAL_ERROR "${step}: an unchanged stamp rewrote '${_header}' or "
                        "rebuilt '${_stamp}'")
  endif()
endfunction()

_vkc_build_and_expect("the first build" 0 0)
set(_header_time "${_vkc_header_time}")
file(TIMESTAMP "${_stamp}" _stamp_time "%s")

_vkc_build_and_expect("a build with no change" 0 0)
_vkc_expect_unchanged("a build with no change" "${_header_time}"
                      "${_stamp_time}")

# Only the build carries them: this script's own git calls would follow them.
set(_hook "a build with a git hook's variables")
_vkc_build_and_expect(
  "${_hook}" 0 0 "GIT_DIR=${_project}/.git" "GIT_WORK_TREE=${_project}"
  "GIT_INDEX_FILE=${VKC_WORK_DIR}/no_such_index")
_vkc_expect_unchanged("${_hook}" "${_header_time}" "${_stamp_time}")

set(_index "${_project}/.git/index")
file(TIMESTAMP "${_index}" _index_time "%s")
file(TOUCH "${_project}/local/note.txt")
_vkc_build_and_expect("a tracked file touched" 0 0)
_vkc_expect_unchanged("a tracked file touched" "${_header_time}"
                      "${_stamp_time}")
file(TIMESTAMP "${_index}" _time "%s")
if(NOT _time STREQUAL _index_time)
  message(FATAL_ERROR "a tracked file touched: the stamp rewrote '${_index}', "
                      "taking the lock the user's own git command wants")
endif()

file(WRITE "${_project}/ignored.txt" "ignored\n")
_vkc_build_and_expect("an ignored file" 0 0)
_vkc_expect_unchanged("an ignored file" "${_header_time}" "${_stamp_time}")

file(WRITE "${_project}/untracked.txt" "untracked\n")
_vkc_build_and_expect("an untracked file" 1 0)
file(REMOVE "${_project}/untracked.txt")
_vkc_build_and_expect("the untracked file removed" 0 0)

file(APPEND "${_project}/local/note.txt" "An edit.\n")
file(APPEND "${_dep}/file.txt" "An edit.\n")
_vkc_build_and_expect("a tracked edit in each" 1 1)

_vkc_git("${_project}" commit --quiet --all -m "second")
_vkc_build_and_expect("the project's edit committed" 0 1)

message(STATUS "vkc_source_commits_rebuild: passed")
