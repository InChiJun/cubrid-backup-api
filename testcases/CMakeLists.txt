
cmake_minimum_required(VERSION 2.8)

project(cubrid-backup-api-testcases)

set(CMAKE_C_COMPILER gcc)
#add_compile_options(-g -Wall)

set(CUBRID_BACKUP_API_LIB "libcubridbackupapi.so")

include_directories(${CMAKE_SOURCE_DIR}/cubrid-backup-api)
link_directories(${CMAKE_SOURCE_DIR}/cubrid-backup-api)

# testcases for backup
add_executable(backup_tc01 backup_tc01.c)
target_link_libraries(backup_tc01 ${CUBRID_BACKUP_API_LIB} pthread)

add_executable(backup_tc02 backup_tc02.c)
target_link_libraries(backup_tc02 ${CUBRID_BACKUP_API_LIB} pthread)

add_executable(backup_tc03 backup_tc03.c)
target_link_libraries(backup_tc03 ${CUBRID_BACKUP_API_LIB} pthread)

add_executable(backup_tc04 backup_tc04.c)
target_link_libraries(backup_tc04 ${CUBRID_BACKUP_API_LIB} pthread)

# slow-consumer / tiered-buffer stress
add_executable(backup_tc05 backup_tc05.c)
target_link_libraries(backup_tc05 ${CUBRID_BACKUP_API_LIB} pthread)

# testcases for restore
add_executable(restore_tc01 restore_tc01.c)
target_link_libraries(restore_tc01 ${CUBRID_BACKUP_API_LIB} pthread)

add_executable(restore_tc02 restore_tc02.c)
target_link_libraries(restore_tc02 ${CUBRID_BACKUP_API_LIB} pthread)

add_executable(restore_tc03 restore_tc03.c)
target_link_libraries(restore_tc03 ${CUBRID_BACKUP_API_LIB} pthread)

# ── deterministic unit tests for the observational log-phase parser + rings ──
# parser_ut.c #includes ../src/backup_core.c to reach its static functions, so it
# COMPILES the API TUs directly and must NOT link the .so (that would duplicate
# the static symbols). Registered with ctest; the binary exits non-zero on any
# failed check. Verified standalone build:
#   gcc -std=gnu11 -D_GNU_SOURCE -I ../src/include parser_ut.c \
#       ../src/backup_manager.c ../src/handle_manager.c -lpthread
enable_testing()
add_executable(parser_ut parser_ut.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/backup_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/handle_manager.c)
target_include_directories(parser_ut PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/include)
target_compile_definitions(parser_ut PRIVATE _GNU_SOURCE)
set_target_properties(parser_ut PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_link_libraries(parser_ut pthread)
add_test(NAME parser_ut COMMAND parser_ut)
