# SPDX-FileCopyrightText: 2026 VectorText contributors
# SPDX-License-Identifier: GPL-3.0-only
get_filename_component(work "${EXECUTABLE}" DIRECTORY)
configure_file("${SOURCE_INI}" "${work}/VectorText.ini" COPYONLY)
configure_file("${SOURCE_DLL}" "${work}/VectorText.dll" COPYONLY)
execute_process(COMMAND "${EXECUTABLE}" ${ARGS} WORKING_DIRECTORY "${work}" RESULT_VARIABLE result)
configure_file("${SOURCE_INI}" "${work}/VectorText.ini" COPYONLY)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "${EXECUTABLE} failed: ${result}")
endif()
