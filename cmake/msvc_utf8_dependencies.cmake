# SPDX-FileCopyrightText: 2026 VectorText contributors
# SPDX-License-Identifier: GPL-3.0-only
# /utf-8 makes localized cl diagnostics UTF-8. Some CMake versions decode
# their /showIncludes probe using the console's legacy code page, so Ninja
# cannot recognize the prefix and silently loses header dependencies.
if(CMAKE_GENERATOR MATCHES "Ninja")
    if(NOT DEFINED VT_UTF8_INCLUDES_PREFIX)
        set(probe "${CMAKE_BINARY_DIR}/CMakeFiles/VectorTextShowIncludes")
        file(MAKE_DIRECTORY "${probe}")
        file(WRITE "${probe}/foo.h" "\n")
        file(WRITE "${probe}/main.c" "#include \"foo.h\"\n")
        execute_process(COMMAND "${CMAKE_C_COMPILER}" /nologo /utf-8 /showIncludes /c main.c
            WORKING_DIRECTORY "${probe}" OUTPUT_VARIABLE output ERROR_VARIABLE errors
            RESULT_VARIABLE result ENCODING UTF-8)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "MSVC include-dependency probe failed: ${output}${errors}")
        endif()
        string(REGEX MATCH "(^|\n)([^\r\n]+: +)[A-Za-z]:[^\r\n]*foo\\.h" match "${output}")
        if(NOT match)
            message(FATAL_ERROR "Cannot detect MSVC UTF-8 include-dependency prefix: ${output}")
        endif()
        set(VT_UTF8_INCLUDES_PREFIX "${CMAKE_MATCH_2}" CACHE INTERNAL "MSVC UTF-8 dependency prefix")
    endif()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${VT_UTF8_INCLUDES_PREFIX}")
endif()
