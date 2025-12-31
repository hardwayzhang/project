cmake_minimum_required(VERSION 3.14)
project(proto_converter VERSION 1.0.0 LANGUAGES CXX)

# C++17标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找Protobuf包
find_package(Protobuf REQUIRED)

# 输出找到的Protobuf信息
message(STATUS "Protobuf version: ${Protobuf_VERSION}")
message(STATUS "Protobuf include dirs: ${Protobuf_INCLUDE_DIRS}")
message(STATUS "Protobuf libraries: ${Protobuf_LIBRARIES}")

# 设置Proto文件路径
set(PROTO_DIR ${CMAKE_SOURCE_DIR}/proto)
set(PROTO_FILES
    ${PROTO_DIR}/options.proto
    ${PROTO_DIR}/messages.proto
)

# 生成Proto的输出目录
set(PROTO_OUTPUT_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_OUTPUT_DIR})

# 生成Proto C++文件
set(PROTO_SRCS)
set(PROTO_HDRS)

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    list(APPEND PROTO_SRCS ${PROTO_OUTPUT_DIR}/${PROTO_NAME}.pb.cc)
    list(APPEND PROTO_HDRS ${PROTO_OUTPUT_DIR}/${PROTO_NAME}.pb.h)
endforeach()

# 使用protoc编译proto文件
add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS --cpp_out=${PROTO_OUTPUT_DIR}
         --proto_path=${PROTO_DIR}
         --proto_path=${Protobuf_INCLUDE_DIRS}
         ${PROTO_FILES}
    DEPENDS ${PROTO_FILES}
    COMMENT "Running protoc on proto files"
)

# 创建生成的proto库
add_library(proto_generated STATIC
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

target_include_directories(proto_generated PUBLIC
    ${PROTO_OUTPUT_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(proto_generated PUBLIC
    ${Protobuf_LIBRARIES}
)

# 创建转换器库
add_library(proto_converter STATIC
    src/proto_converter.cpp
    src/reverse_converter.cpp
)

target_include_directories(proto_converter PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${PROTO_OUTPUT_DIR}
)

target_link_libraries(proto_converter PUBLIC
    proto_generated
)

# 创建测试可执行文件
add_executable(converter_test
    src/main.cpp
)

target_link_libraries(converter_test PRIVATE
    proto_converter
)

# 安装规则
install(TARGETS proto_converter
    ARCHIVE DESTINATION lib
)

install(FILES 
    include/proto_converter.h
    include/reverse_converter.h
    DESTINATION include
)

install(FILES ${PROTO_HDRS}
    DESTINATION include
)

# 输出构建信息
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Proto output dir: ${PROTO_OUTPUT_DIR}")
