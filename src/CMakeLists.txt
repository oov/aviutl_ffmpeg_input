cmake_minimum_required(VERSION 3.10)
project(ffmpeg_input C)
enable_language(RC)

option(FORMAT_SOURCES "execute clang-format" ON)

if(FORMAT_SOURCES)
  file(GLOB_RECURSE sources LIST_DIRECTORIES false CONFIGURE_DEPENDS "*.h" "*.c")
  list(FILTER sources EXCLUDE REGEX "${CMAKE_CURRENT_SOURCE_DIR}/3rd")
  find_program(CLANG_FORMAT_EXE clang-format)
  add_custom_target(${PROJECT_NAME}-format ALL
    COMMAND ${CLANG_FORMAT_EXE} -style=file -i ${sources}
  )
endif()

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

set(AVCODEC_NAME avcodec-${AVCODEC_VERSION})
set(AVFORMAT_NAME avformat-${AVFORMAT_VERSION})
set(AVUTIL_NAME avutil-${AVUTIL_VERSION})
set(SWSCALE_NAME swscale-${SWSCALE_VERSION})
set(SWRESAMPLE_NAME swresample-${SWRESAMPLE_VERSION})
set(is_clang "$<C_COMPILER_ID:Clang>")
set(v16_or_later "$<VERSION_GREATER_EQUAL:$<C_COMPILER_VERSION>,16>")
add_library(ffmpeg_input_intf INTERFACE)
target_include_directories(ffmpeg_input_intf INTERFACE
  "${CMAKE_CURRENT_BINARY_DIR}" # for version.h
  "${FFMPEG_DIR}/include"
)
target_link_directories(ffmpeg_input_intf INTERFACE
  "${FFMPEG_DIR}/bin"
)
target_compile_definitions(ffmpeg_input_intf INTERFACE
  $<$<BOOL:${WIN32}>:_WIN32_WINNT=0x0600>
  _WINDOWS
  $<$<CONFIG:Release>:NDEBUG>
)
target_compile_options(ffmpeg_input_intf INTERFACE
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
  $<$<AND:${is_clang},${v16_or_later}>:-Wno-unsafe-buffer-usage>
  -ffunction-sections
  -fdata-sections
  $<$<CONFIG:Debug>:-O0>
  $<$<CONFIG:Release>:-O2>
  -flto
)
target_link_options(ffmpeg_input_intf INTERFACE
  -fuse-ld=lld
  -Wl,--gc-sections
  # -Wl,--print-gc-sections
  -Wl,--kill-at
  $<$<CONFIG:Release>:-s>
)
target_link_libraries(ffmpeg_input_intf INTERFACE
  ovbase
  ovutil
  psapi
  ${AVCODEC_NAME} -Wl,-delayload,${AVCODEC_NAME}.dll
  ${AVFORMAT_NAME} -Wl,-delayload,${AVFORMAT_NAME}.dll
  ${AVUTIL_NAME} -Wl,-delayload,${AVUTIL_NAME}.dll
  ${SWSCALE_NAME} -Wl,-delayload,${SWSCALE_NAME}.dll
  ${SWRESAMPLE_NAME} -Wl,-delayload,${SWRESAMPLE_NAME}.dll
)

add_library(ffmpeg_input_test_intf INTERFACE)
target_compile_definitions(ffmpeg_input_test_intf INTERFACE
  FFMPEGDIR=L"${FFMPEG_DIR}/bin"
  TESTDATADIR=L"${CMAKE_CURRENT_SOURCE_DIR}/testdata"
)

add_library(ffmpeg_input SHARED
  api.c
  audioidx.c
  audio.c
  aviutl.c
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
  now.c
  process.c
  progress.c
  stream.c
  video.c
)
set_target_properties(ffmpeg_input PROPERTIES
  OUTPUT_NAME "ffmpeg_input.${FFMPEG_INPUT_DLLEXT}"
  PREFIX ""
  SUFFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${EXPORT_DIR}"
)
add_dependencies(ffmpeg_input ${PROJECT_NAME}-format ${PROJECT_NAME}_generate_version_h copy_related_files)
target_link_libraries(ffmpeg_input PRIVATE ffmpeg_input_intf)

add_executable(ffmpeg_test mapped.c ffmpeg_test.c)
target_link_libraries(ffmpeg_test PRIVATE ffmpeg_input_intf ffmpeg_input_test_intf)
add_test(NAME ffmpeg_test COMMAND ffmpeg_test)

add_executable(ipc_test ipccommon.c ipcclient.c ipcserver.c ipc_test.c)
target_link_libraries(ipc_test PRIVATE ffmpeg_input_intf)
add_test(NAME ipc_test COMMAND ipc_test)
