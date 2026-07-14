
cmake_minimum_required(VERSION 2.8)

project(cubridbackupapi)

# Version info
if(EXISTS ${CMAKE_SOURCE_DIR}/../VERSION)
    set(VERSION_FILE ../VERSION)
else()
    message(FATAL_ERROR "Could not find a VERSION file")
endif()

file(STRINGS ${VERSION_FILE} VERSION_STR)

if(VERSION_STR MATCHES "[0-9]+\\.[0-9]+")
    string(REGEX MATCHALL "[0-9]+" VERSION_MATCHES ${VERSION_STR})
    list(GET VERSION_MATCHES 0 CUBRID_BACKUP_API_MAJOR_VER)
    list(GET VERSION_MATCHES 1 CUBRID_BACKUP_API_MINOR_VER)
else()
    message(FATAL_ERROR "Failed to parse a version string from ${VERSION_FILE} file")
endif()

set(CMAKE_C_COMPILER "gcc")

if(CMAKE_BUILD_TYPE MATCHES "Debug")
    add_compile_options(-g -Wall)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

# tiered buffer: drain thread (pthread) + Linux/glibc syscalls
# (F_SETPIPE_SZ, fallocate, eventfd) require _GNU_SOURCE.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
add_compile_definitions(_GNU_SOURCE)

set(CUBRID_BACKUP_API_SRCS
    ${CMAKE_SOURCE_DIR}/backup_api.c
    ${CMAKE_SOURCE_DIR}/backup_core.c
    ${CMAKE_SOURCE_DIR}/backup_manager.c
    ${CMAKE_SOURCE_DIR}/handle_manager.c)

add_library(${PROJECT_NAME} SHARED ${CUBRID_BACKUP_API_SRCS})
set_target_properties(${PROJECT_NAME}
                      PROPERTIES
                          SOVERSION "${CUBRID_BACKUP_API_MAJOR_VER}.${CUBRID_BACKUP_API_MINOR_VER}"
                          PUBLIC_HEADER "${CMAKE_SOURCE_DIR}/include/backup_api.h"
                          # C11 required: the vendored backup-format header pins the
                          # on-wire layout with _Static_assert (ABI-drift firewall).
                          C_STANDARD 11
                          C_STANDARD_REQUIRED ON)

# The library itself now spawns the drain thread, so link pthread explicitly
# instead of relying on the loading executable to pull it in.
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

install(TARGETS ${PROJECT_NAME}
        LIBRARY
            DESTINATION ${CMAKE_INSTALL_PREFIX}
            COMPONENT Libraries
        PUBLIC_HEADER
            RENAME "cubrid_backup_api.h"
            DESTINATION ${CMAKE_INSTALL_PREFIX}
            COMPONENT Header)
