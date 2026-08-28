add_executable (
    navteq_test
    navteq_test.c
    ../src/navteq.c
)
target_include_directories (
    navteq_test PRIVATE
    ../include
    ../src
)
add_test (NAME navteq_test COMMAND navteq_test)

add_executable (
    alert_c_test
    alert_c_test.c
    ../src/alert_c.c
)
target_include_directories (alert_c_test PRIVATE ../include ../src)
add_test (NAME alert_c_test COMMAND alert_c_test)

add_executable (
    location_table_test
    location_table_test.c
    ../src/location_table.c
)
target_include_directories (location_table_test PRIVATE ../include)
target_compile_definitions (
    location_table_test PRIVATE
    LOCATION_TABLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/location_table.tsv"
    INVALID_LOCATION_TABLE_PATH="${PROJECT_SOURCE_DIR}/README.md"
)
add_test (NAME location_table_test COMMAND location_table_test)

add_executable (
    ttn_tpeg_test
    ttn_tpeg_test.c
    ../src/ttn_tpeg.c
    ../src/location_table.c
)
target_include_directories (ttn_tpeg_test PRIVATE ../include ../src ${ZLIB_INCLUDE_DIRS})
target_link_libraries (ttn_tpeg_test PRIVATE ${ZLIB_LIBRARIES})
if (ZLIB_BUILTIN)
    add_dependencies (ttn_tpeg_test ${ZLIB_BUILTIN})
endif ()
add_test (NAME ttn_tpeg_test COMMAND ttn_tpeg_test)

add_executable (
    here_tpeg_test
    here_tpeg_test.c
    ../src/here_tpeg.c
)
target_include_directories (here_tpeg_test PRIVATE ../include ../src)
add_test (NAME here_tpeg_test COMMAND here_tpeg_test)
