# CMakeLists.txt for WebServer
#
# © 2018 by Richard Walters

cmake_minimum_required(VERSION 3.8)
set(This WebServer)

set(Sources
    src/main.cpp
)

add_executable(${This} ${Sources})
set_target_properties(${This} PROPERTIES
    FOLDER Applications
)

target_link_libraries(${This} PUBLIC
    Json
    Http
    HttpNetworkTransport
    SystemAbstractions
)

add_subdirectory(StaticContentPlugin)
