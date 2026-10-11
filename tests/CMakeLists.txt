add_executable(test_tinyppp test_tinyppp.cpp)
target_compile_options(test_tinyppp PRIVATE -Wall -Wextra)
target_link_libraries(test_tinyppp PRIVATE tinyppp)
add_test(NAME test_tinyppp COMMAND test_tinyppp)
