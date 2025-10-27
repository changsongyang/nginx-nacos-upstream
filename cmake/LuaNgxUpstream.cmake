
if (NOT LUA_NGX_DIR)
    message(FATAL_ERROR "Must set LUA_NGX_DIR to build luajit with CMake")
endif ()

# 搜索 src 目录下所有 .c 文件
file(GLOB LUA_NGX_SOURCES
        ${LUA_NGX_DIR}/src/*.c
)

list(APPEND NGX_C_FILES ${LUA_NGX_SOURCES})