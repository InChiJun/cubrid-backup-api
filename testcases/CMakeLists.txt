
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
