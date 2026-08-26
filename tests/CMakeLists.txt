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
