cmake_minimum_required(VERSION 3.0)
project(ffmpeg_input C)
enable_language(RC)

add_subdirectory(3rd/ovbase)
add_subdirectory(3rd/ovutil)

add_custom_target(${PROJECT_NAME}_generate_version_h COMMAND
  ${CMAKE_COMMAND}
  -Dlocal_dir="${CMAKE_CURRENT_SOURCE_DIR}"
  -Dinput_file="${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
  -Doutput_file="${CMAKE_CURRENT_BINARY_DIR}/version.h"
  -P "${ovutil_SOURCE_DIR}/src/cmake/version.cmake"
)

add_custom_target(copy_related_files
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../README.md" "${EXPORT_DIR}/ffmpeg_input.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${FFMPEG_DIR}" "${EXPORT_DIR}/ffmpeg${FFMPEG_INPUT_BITSIZE}"
)

find_program(CLANG_FORMAT_EXE clang-format)
file(GLOB sources "${CMAKE_CURRENT_SOURCE_DIR}/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
add_custom_target(${PROJECT_NAME}_format
  COMMAND ${CLANG_FORMAT_EXE} -style=file -i ${sources}
)

add_library(ffmpeg_input SHARED
  api.c
  audioidx.c
  audio.c
  bridgeclient.c
  bridgeserver.c
  config.c
  error.c
  ffmpeg.c
  ffmpeg_input.rc
  ipcclient.c
  ipccommon.c
  ipcserver.c
  main.c
  mapped.c
  process.c
  video.c
)
list(APPEND targets ffmpeg_input)
set_target_properties(ffmpeg_input PROPERTIES
  OUTPUT_NAME "ffmpeg_input.${FFMPEG_INPUT_DLLEXT}"
  PREFIX ""
  SUFFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${EXPORT_DIR}"
)
add_dependencies(ffmpeg_input ${PROJECT_NAME}_format ${PROJECT_NAME}_generate_version_h copy_related_files)
add_executable(ffmpeg_test mapped.c ffmpeg_test.c)
list(APPEND targets ffmpeg_test)
add_executable(ipc_test ipccommon.c ipcclient.c ipcserver.c ipc_test.c)
list(APPEND targets ipc_test)

set(AVCODEC_NAME avcodec-${AVCODEC_VERSION})
set(AVFORMAT_NAME avformat-${AVFORMAT_VERSION})
set(AVUTIL_NAME avutil-${AVUTIL_VERSION})
set(SWSCALE_NAME swscale-${SWSCALE_VERSION})
set(SWRESAMPLE_NAME swresample-${SWRESAMPLE_VERSION})
foreach(target ${targets})
  if(target MATCHES "_test$")
    add_test(NAME ${target} COMMAND ${target})
    target_compile_definitions(${target} PRIVATE
      FFMPEGDIR=L"${FFMPEG_DIR}/bin"
      TESTDATADIR=L"${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    )
  endif()
  target_include_directories(${target} PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" # for version.h
    "${FFMPEG_DIR}/include"
  )
  target_link_directories(${target} PRIVATE
    "${FFMPEG_DIR}/bin"
  )
  target_compile_definitions(${target} PRIVATE
    $<$<BOOL:${WIN32}>:_WIN32_WINNT=0x0600>
    _WINDOWS
    $<$<CONFIG:Release>:NDEBUG>
  )
  target_compile_options(${target} PRIVATE
    -mstackrealign
    -Wall
    -Wextra
    -Werror
    -Weverything
    -Wshadow
    -Werror=return-type
    -pedantic-errors
    -Wno-declaration-after-statement
    -Wno-padded
    -ffunction-sections
    -fdata-sections
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Release>:-O2>
    -flto
  )
  target_link_options(${target} PRIVATE
    -fuse-ld=lld
    -Wl,--gc-sections
    # -Wl,--print-gc-sections
    -Wl,--kill-at
    $<$<CONFIG:Release>:-s>
  )
  target_link_libraries(${target} PRIVATE
    ovbase
    ovutil
    ${AVCODEC_NAME} -Wl,-delayload,${AVCODEC_NAME}.dll
    ${AVFORMAT_NAME} -Wl,-delayload,${AVFORMAT_NAME}.dll
    ${AVUTIL_NAME} -Wl,-delayload,${AVUTIL_NAME}.dll
    ${SWSCALE_NAME} -Wl,-delayload,${SWSCALE_NAME}.dll
    ${SWRESAMPLE_NAME} -Wl,-delayload,${SWRESAMPLE_NAME}.dll
  )
endforeach(target)