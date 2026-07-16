cmake_minimum_required(VERSION 3.10)
project(mmemu_sdk)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Library
add_library(mmemu_serial_monitor
    src/mmemu_serial_monitor.cpp
    include/mmemu_serial_monitor.h
)

target_include_directories(mmemu_serial_monitor PUBLIC include)

# Example executable
add_executable(memory_inspector
    examples/memory_inspector.cpp
)

target_link_libraries(memory_inspector mmemu_serial_monitor)

# Installation
install(TARGETS mmemu_serial_monitor
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES include/mmemu_serial_monitor.h
    DESTINATION include/mmemu
)

install(TARGETS memory_inspector
    RUNTIME DESTINATION bin
)
