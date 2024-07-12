cmake_minimum_required(VERSION 3.22.1)
project(rdma_test)

set(CMAKE_CXX_STANDARD 17)

add_executable(server
        server.cc
        server.h
        rdma_common.cc
        rdma_common.h
)

add_executable(client
        client.cc
        client.h
        rdma_common.cc
        rdma_common.h
)

target_link_libraries(server ibverbs)
target_link_libraries(client ibverbs)
