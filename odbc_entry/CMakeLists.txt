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
        exports/chargewire-odbc.def
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    )
endif()

add_library(chargewire-odbc SHARED ${ODBC_ENTRY_SOURCES})

# Include paths
target_include_directories(chargewire-odbc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CHARGEWIRE_ODBC_SOURCE_DIR}/odbcabstraction/include
    ${CHARGEWIRE_ODBC_SOURCE_DIR}/chargewire/include
    ${CHARGEWIRE_ODBC_SOURCE_DIR}/chargewire
)

# Link the ODBC compatibility layer and native Chargewire/Arrow libraries.
target_link_libraries(chargewire-odbc PRIVATE
    chargewire_odbc_spi_impl
    odbcabstraction
)

if(WIN32)
    target_link_libraries(chargewire-odbc PRIVATE legacy_stdio_definitions)
endif()

# Symbol export control (macOS / Linux — Windows uses .def file)
if (APPLE)
    set_target_properties(chargewire-odbc PROPERTIES
        LINK_FLAGS "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports/chargewire-odbc.exports"
    )
elseif (UNIX)
    # -Bsymbolic-functions: resolve internal function calls to the library's
    # own definitions, preventing the ODBC Driver Manager from intercepting
    # calls between our exported ODBC functions (e.g. SQLAllocConnect calling
    # SQLAllocHandle internally).
    set_target_properties(chargewire-odbc PROPERTIES
        LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports/chargewire-odbc.map -Wl,-Bsymbolic-functions"
    )
endif()

# Output to the same lib directory as other targets
set_target_properties(chargewire-odbc PROPERTIES OUTPUT_NAME ${CHARGEWIRE_ODBC_LIBRARY_NAME})
set_target_properties(chargewire-odbc PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
)

