cmake_minimum_required(VERSION 3.0.0)

string(REGEX MATCH "[^/]+$" FILENAME "${url}")
set(ZIP_PATH "${local_dir}/${FILENAME}")
if(NOT EXISTS "${ZIP_PATH}")
  file(DOWNLOAD "${url}" "${ZIP_PATH}")
endif()
if(NOT EXISTS "${local_dir}/${dir}")
  string(REGEX REPLACE "\\.[^.]+$" "" FILENAME_NOEXT "${FILENAME}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar xzvf ${ZIP_PATH}
    WORKING_DIRECTORY "${local_dir}"
  )
  file(RENAME "${local_dir}/${FILENAME_NOEXT}" "${local_dir}/${dir}")
endif()
