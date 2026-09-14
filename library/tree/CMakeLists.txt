add_library(treelib STATIC
    src/resolver.cpp
    src/ast.cpp
    src/diagnostic.cpp
    src/lexer.cpp
    src/parser.cpp
    src/span.cpp
    src/token.cpp
)

target_link_libraries(treelib PRIVATE)

target_include_directories(treelib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${TREE_CONFIG_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src
)

target_compile_options(treelib PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wunused -Wconversion -Wsign-conversion -Wmisleading-indentation -Werror>
)