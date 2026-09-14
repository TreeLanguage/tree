add_executable(tests
    unit/lexer_test.cpp
    unit/parser_test.cpp
    unit/resolver_test.cpp
    main.cpp)

target_link_libraries(tests PRIVATE treelib)

target_include_directories(tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(tests PRIVATE cxx_std_20)

add_test(NAME tests COMMAND tests)
