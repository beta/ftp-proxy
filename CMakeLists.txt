cmake_minimum_required(VERSION 3.5)
project(FTP_Proxy)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

set(SOURCE_FILES main.c)
add_executable(FTP_Proxy ${SOURCE_FILES})