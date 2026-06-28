cmake_minimum_required(VERSION 3.15)
project(osm_map_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MACOSX_RPATH ON)

find_package(glfw3 REQUIRED)
find_package(CURL REQUIRED)

add_executable(osm_map_cpp
    src/main.cpp
    vendor/imgui.cpp
    vendor/imgui_draw.cpp
    vendor/imgui_tables.cpp
    vendor/imgui_widgets.cpp
    vendor/imgui_impl_glfw.cpp
    vendor/imgui_impl_opengl3.cpp
)

target_include_directories(osm_map_cpp PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/vendor/backends
)

target_link_libraries(osm_map_cpp PRIVATE
    glfw
    CURL::libcurl
    "-framework OpenGL"
    "-framework Cocoa"
    "-framework IOKit"
    "-framework CoreVideo"
)


