cmake_minimum_required(VERSION 3.18)
project(a64 C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -Wall")
add_library(a64 SHARED native-lib.cpp arm64.c a64demo.c machine.c macho.c inspect.c img4.c zipinfo.c attempt.c
    lzfse/lzfse_decode.c lzfse/lzfse_decode_base.c lzfse/lzfse_fse.c lzfse/lzvn_decode_base.c)
find_library(log-lib log)
find_library(z-lib z)
target_link_libraries(a64 ${log-lib} ${z-lib})
