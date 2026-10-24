find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(line-io-manager-tests
    line_io_manager_tests.cpp
)

target_link_libraries(line-io-manager-tests PRIVATE
    core
    Qt6::Test
)

add_test(NAME line-io-manager-tests COMMAND line-io-manager-tests)
