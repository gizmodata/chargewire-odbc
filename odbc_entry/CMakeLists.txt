# Copyright (C) 2026 GizmoData LLC
#
# See "LICENSE" for license information.

cmake_minimum_required(VERSION 3.11)
set(CMAKE_CXX_STANDARD 17)

set(ODBC_ENTRY_SOURCES
    driver_singleton.h
    driver_singleton.cc
    odbc_entry.cc
)

# On Windows, add the .def file for symbol exports and version resource
if (WIN32)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
        @ONLY
    )
    list(APPEND ODBC_ENTRY_SOURCES
        exports/warpsql-odbc.def
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    )
endif()

add_library(warpsql-odbc SHARED ${ODBC_ENTRY_SOURCES})

# Include paths
target_include_directories(warpsql-odbc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WARP_ODBC_SOURCE_DIR}/odbcabstraction/include
    ${WARP_ODBC_SOURCE_DIR}/warpsql/include
    ${WARP_ODBC_SOURCE_DIR}/warpsql
)

# Link the ODBC compatibility layer and native Warp SQL/Arrow libraries.
target_link_libraries(warpsql-odbc PRIVATE
    warpsql_odbc_spi_impl
    odbcabstraction
)

if(WIN32)
    target_link_libraries(warpsql-odbc PRIVATE legacy_stdio_definitions)
endif()

# Symbol export control (macOS / Linux — Windows uses .def file)
if (APPLE)
    set_target_properties(warpsql-odbc PROPERTIES
        LINK_FLAGS "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports/warpsql-odbc.exports"
    )
elseif (UNIX)
    # -Bsymbolic-functions: resolve internal function calls to the library's
    # own definitions, preventing the ODBC Driver Manager from intercepting
    # calls between our exported ODBC functions (e.g. SQLAllocConnect calling
    # SQLAllocHandle internally).
    set_target_properties(warpsql-odbc PROPERTIES
        LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports/warpsql-odbc.map -Wl,-Bsymbolic-functions"
    )
endif()

# Output to the same lib directory as other targets
set_target_properties(warpsql-odbc PROPERTIES OUTPUT_NAME ${WARP_ODBC_LIBRARY_NAME})
set_target_properties(warpsql-odbc PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
)

