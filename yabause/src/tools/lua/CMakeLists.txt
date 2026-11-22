cmake_minimum_required(VERSION 3.1)

# Lua 5.4.7 (MIT licence, see LICENSE), embedded as a static library for the
# Qt port's script console. Interpreter (lua.c) and compiler (luac.c) are not built.
project(kronos_lua C)

file(GLOB KRONOS_LUA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_library(kronos-lua STATIC ${KRONOS_LUA_SOURCES})
target_include_directories(kronos-lua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(NOT MSVC)
  target_link_libraries(kronos-lua PUBLIC m)
endif()
