
cmake_minimum_required(VERSION 3.14)
project(commitgen LANGUAGES CXX)

# --------------------
# C++ settings
# --------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INSTALL_RPATH "/usr/local/lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

# --------------------
# Force static libraries everywhere
# --------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLAMA_STATIC ON CACHE BOOL "" FORCE)

# --------------------
# llama.cpp options
# --------------------
option(LLAMA_METAL   "Enable Metal support" ON)
option(LLAMA_CUBLAS "Enable CUDA support"  OFF)
option(LLAMA_VULKAN "Enable Vulkan support" OFF)

# --------------------
# Add llama.cpp
# IMPORTANT: this must come AFTER the options above
# --------------------

# --------------------
# Common sources
# --------------------
set(COMMON_SOURCES
    commitgen.cpp
)

# --------------------
# Server executable
# --------------------
add_executable(commitgen-server
    server.cpp
    ${COMMON_SOURCES}
)

target_compile_definitions(commitgen-server PRIVATE SERVER_MODE)

add_executable(commitgen
    client.cpp
    ${COMMON_SOURCES}
)

target_link_libraries(commitgen-server PRIVATE
    llama
    ggml
)

target_link_libraries(commitgen PRIVATE
    llama
    ggml
)
