# Upstream no longer ships an Android app. An embedder supplies its own UI process, but the rest of the build still
# expects the UI framework to declare a `ladybird` target, so declare an empty one.
set(ladybird_stub "${CMAKE_CURRENT_BINARY_DIR}/ladybird_stub.cpp")
file(WRITE "${ladybird_stub}" "// Intentionally empty.\n")

add_library(ladybird SHARED "${ladybird_stub}")
target_link_libraries(ladybird PRIVATE LibFileSystem jnigraphics android)
