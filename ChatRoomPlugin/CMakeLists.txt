# CMakeLists.txt for ChatRoomPlugin
#
# © 2018 by Richard Walters

cmake_minimum_required(VERSION 3.8)
set(This ChatRoomPlugin)

set(Sources
    src/ChatRoomPlugin.cpp
)

add_library(${This} SHARED ${Sources})
set_target_properties(${This} PROPERTIES
    FOLDER "Web Server Plugins"
)

target_include_directories(${This} PRIVATE $<TARGET_PROPERTY:WebServer,INCLUDE_DIRECTORIES>)

target_link_libraries(${This} PUBLIC
    Http
    Json
    SystemAbstractions
    Uri
    WebSockets
)

add_subdirectory(test)
