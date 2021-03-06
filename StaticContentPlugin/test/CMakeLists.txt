# CMakeLists.txt for StaticContentPluginTests
#
# © 2018 by Richard Walters

cmake_minimum_required(VERSION 3.8)
set(This StaticContentPluginTests)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:StaticContentPlugin>)

set(Sources
    src/StaticContentPluginTests.cpp
)

add_executable(${This} ${Sources})
set_target_properties(${This} PROPERTIES
    FOLDER Tests
)

target_include_directories(${This} PRIVATE ..)
target_include_directories(${This} PRIVATE $<TARGET_PROPERTY:WebServer,INCLUDE_DIRECTORIES>)

target_link_libraries(${This} PUBLIC
    gtest_main
    Hash
    StaticContentPlugin
)

add_custom_command(TARGET ${This} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:StaticContentPlugin> $<TARGET_FILE_DIR:${This}>
)

add_test(
    NAME ${This}
    COMMAND ${This}
)
