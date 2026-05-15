#项目名称
project (sunnect)
#CMake最低版本号要求
cmake_minimum_required (VERSION 2.8)
#头文件目录
include_directories(include)
include_directories(./3rd/lua-5.3.5/src)
#查找./src目录下所有源文件，保存到DIR_SRCS变量
aux_source_directory(./src DIR_SRCS)
#用c++11
add_definitions(-std=c++11)
#指定生成目标文件
add_executable(sunnet ${DIR_SRCS})

#指定库文件搜索路径
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rd/lua-5.3.5/src/)

#给目标 sunnet 链接 liblua.a 这个库
target_link_libraries(sunnet liblua.a)

target_link_libraries(sunnet dl)