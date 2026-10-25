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
        exports/hardcharger-odbc.def
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    )
endif()

add_library(hardcharger-odbc SHARED ${ODBC_ENTRY_SOURCES})

# Include paths
target_include_directories(hardcharger-odbc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/odbcabstraction/include
    ${CMAKE_SOURCE_DIR}/hardcharger/include
    ${CMAKE_SOURCE_DIR}/hardcharger
)

# Link the ODBC compatibility layer and native Warp SQL/Arrow libraries.
target_link_libraries(hardcharger-odbc PRIVATE
    hardcharger_odbc_spi_impl
    odbcabstraction
)

if(WIN32)
    target_link_libraries(hardcharger-odbc PRIVATE legacy_stdio_definitions)
endif()

# Symbol export control (macOS / Linux — Windows uses .def file)
if (APPLE)
    set_target_properties(hardcharger-odbc PROPERTIES
        LINK_FLAGS "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports/hardcharger-odbc.exports"
    )
elseif (UNIX)
    # -Bsymbolic-functions: resolve internal function calls to the library's
    # own definitions, preventing the ODBC Driver Manager from intercepting
    # calls between our exported ODBC functions (e.g. SQLAllocConnect calling
    # SQLAllocHandle internally).
    set_target_properties(hardcharger-odbc PROPERTIES
        LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports/hardcharger-odbc.map -Wl,-Bsymbolic-functions"
    )
endif()

# Output to the same lib directory as other targets
set_target_properties(hardcharger-odbc PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
)

