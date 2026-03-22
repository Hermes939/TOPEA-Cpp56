cmake_minimum_required(VERSION 3.20)
project(top4ea VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimisations production
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined")

# Sources
add_executable(top4ea
    src/main.cpp
    src/mt4_bridge.cpp
    src/grid_engine.cpp
    src/basket_manager.cpp
    src/recovery_engine.cpp
    src/top4ea_engine.cpp
)

target_include_directories(top4ea PRIVATE include)

# Threads
find_package(Threads REQUIRED)
target_link_libraries(top4ea PRIVATE Threads::Threads)

# Windows Winsock
if(WIN32)
    target_link_libraries(top4ea PRIVATE ws2_32)
endif()

# Warnings
target_compile_options(top4ea PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

install(TARGETS top4ea DESTINATION bin)
install(FILES ../top4ea.conf DESTINATION etc OPTIONAL)
