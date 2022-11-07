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

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(FFMPEG_INPUT_BITSIZE 64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(FFMPEG_INPUT_BITSIZE 32)
endif()

add_custom_target(extract_ffmpeg COMMAND
  ${CMAKE_COMMAND}
  -Dlocal_dir="${PROJECT_BINARY_DIR}"
  -Durl="https://github.com/oov/ffmpeg-openh264-win/releases/download/rel.2022-10-15/ffmpeg-n5.1.2-4-gf5455889fd-win${FFMPEG_INPUT_BITSIZE}-lgpl-shared-5.1.zip"
  -Ddir="ffmpeg${FFMPEG_INPUT_BITSIZE}"
  -P "${CMAKE_SOURCE_DIR}/src/cmake/extract-ffmpeg.cmake"
)

add_custom_target(copy_related_files
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../README.md" "${EXPORT_DIR}/ffmpeg_input.txt"
)

find_program(CLANG_FORMAT_EXE clang-format)
file(GLOB sources "${CMAKE_CURRENT_SOURCE_DIR}/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
add_custom_target(${PROJECT_NAME}_format
  COMMAND ${CLANG_FORMAT_EXE} -style=file -i ${sources}
)

add_library(ffmpeg_input SHARED
  api.c
  audio.c
  bridgeclient.c
  bridgeserver.c
  error.c
  ffmpeg.c
  ffmpeg_input.rc
  file.c
  ipcclient.c
  ipcserver.c
  main.c
  process.c
  video.c
)
list(APPEND targets ffmpeg_input)
set_target_properties(ffmpeg_input PROPERTIES
  OUTPUT_NAME "ffmpeg_input.${FFMPEG_INPUT_BITSIZE}aui"
  PREFIX ""
  SUFFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${EXPORT_DIR}"
)
add_dependencies(ffmpeg_input ${PROJECT_NAME}_format extract_ffmpeg ${PROJECT_NAME}_generate_version_h copy_related_files)
set(AVCODEC_NAME avcodec-59)
set(AVFORMAT_NAME avformat-59)
set(AVUTIL_NAME avutil-57)
set(SWSCALE_NAME swscale-6)
set(SWRESAMPLE_NAME swresample-4)
target_link_libraries(ffmpeg_input PRIVATE
  ${AVCODEC_NAME} -Wl,-delayload,${AVCODEC_NAME}.dll
  ${AVFORMAT_NAME} -Wl,-delayload,${AVFORMAT_NAME}.dll
  ${AVUTIL_NAME} -Wl,-delayload,${AVUTIL_NAME}.dll
  ${SWSCALE_NAME} -Wl,-delayload,${SWSCALE_NAME}.dll
  ${SWRESAMPLE_NAME} -Wl,-delayload,${SWRESAMPLE_NAME}.dll
)
target_compile_definitions(ffmpeg_input PRIVATE
  "AVCODEC_DLL=L\"${AVCODEC_NAME}.dll\""
  "AVFORMAT_DLL=L\"${AVFORMAT_NAME}.dll\""
  "AVUTIL_DLL=L\"${AVUTIL_NAME}.dll\""
  "SWSCALE_DLL=L\"${SWSCALE_NAME}.dll\""
  "SWRESAMPLE_DLL=L\"${SWRESAMPLE_NAME}.dll\""
)

add_executable(ipc_test file.c ipcclient.c ipcserver.c ipc_test.c)
list(APPEND targets ipc_test)

foreach(target ${targets})
  if(target MATCHES "_test$")
    add_test(NAME ${target} COMMAND ${target})
  endif()
  target_include_directories(${target} PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" # for version.h
    "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg${FFMPEG_INPUT_BITSIZE}/include"
  )
  target_link_directories(${target} PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg${FFMPEG_INPUT_BITSIZE}/bin"
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
  )
endforeach(target)