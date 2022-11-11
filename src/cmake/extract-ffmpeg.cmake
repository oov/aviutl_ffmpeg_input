cmake_minimum_required(VERSION 3.0.0)

string(REGEX MATCH "[^/]+$" FFMPEG_ARCHIVE_NAME "${ffmpeg}")
set(FFMPEG_ARCHIVE_PATH "${local_dir}/${FFMPEG_ARCHIVE_NAME}")
if(NOT EXISTS "${FFMPEG_ARCHIVE_PATH}")
  file(DOWNLOAD "${ffmpeg}" "${FFMPEG_ARCHIVE_PATH}")
endif()

string(REGEX MATCH "[^/]+$" OPENH264_ARCHIVE_NAME "${openh264}")
set(OPENH264_ARCHIVE_PATH "${local_dir}/${OPENH264_ARCHIVE_NAME}")
if(NOT EXISTS "${OPENH264_ARCHIVE_PATH}")
  file(DOWNLOAD "${openh264}" "${OPENH264_ARCHIVE_PATH}")
endif()

string(REGEX MATCH "[^/]+$" OPENH264_LICENSE "${openh264_license}")
set(OPENH264_LICENSE_PATH "${local_dir}/${OPENH264_LICENSE}")
if(NOT EXISTS "${OPENH264_LICENSE_PATH}")
  file(DOWNLOAD "${openh264_license}" "${OPENH264_LICENSE_PATH}")
endif()

if(NOT EXISTS "${local_dir}/${dir}")
  find_program(BUNZIP2 bunzip2)
  string(REGEX REPLACE "\\.[^.]+$" "" FFMPEG_ARCHIVE_NOEXT "${FFMPEG_ARCHIVE_NAME}")
  string(REGEX MATCH "[^/]+\\.dll" OPENH264_DLLNAME "${openh264}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar xzvf ${FFMPEG_ARCHIVE_PATH}
    COMMAND ${BUNZIP2} -d -k ${OPENH264_ARCHIVE_PATH}
    WORKING_DIRECTORY "${local_dir}"
  )
  file(RENAME "${local_dir}/${FFMPEG_ARCHIVE_NOEXT}" "${local_dir}/${dir}")
  file(RENAME "${local_dir}/${OPENH264_DLLNAME}" "${local_dir}/${dir}/bin/${OPENH264_DLLNAME}")
  file(RENAME "${local_dir}/${dir}/LICENSE.txt" "${local_dir}/${dir}/FFMPEG_LICENSE.txt")
  file(RENAME "${local_dir}/${OPENH264_LICENSE}" "${local_dir}/${dir}/OPENH264_BINARY_LICENSE.txt")
endif()
