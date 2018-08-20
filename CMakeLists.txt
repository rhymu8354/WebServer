# CMakeLists.txt for WebServer
#
# © 2018 by Richard Walters

cmake_minimum_required(VERSION 3.8)
set(This WebServer)

set(Headers
    include/WebServer/PluginEntryPoint.hpp
)

set(Sources
    src/main.cpp
    src/Plugin.cpp
    src/Plugin.hpp
    src/PluginLoader.cpp
    src/PluginLoader.hpp
    src/TimeKeeper.cpp
    src/TimeKeeper.hpp
)

add_executable(${This} ${Sources} ${Headers})
set_target_properties(${This} PROPERTIES
    FOLDER Applications
)

target_include_directories(${This} PUBLIC include)

target_link_libraries(${This} PUBLIC
    Json
    Http
    HttpNetworkTransport
    SystemAbstractions
)

if(UNIX)
    target_link_libraries(${This} PRIVATE
        -static-libstdc++
    )
endif(UNIX)

add_subdirectory(ChatRoomPlugin)
add_subdirectory(EchoPlugin)
add_subdirectory(StaticContentPlugin)
