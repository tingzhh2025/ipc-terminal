# # 设置库的输出目录
# set(LIB_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/lib")

# # 创建 gpio 动态库
# file(GLOB GPIO_SRC "gpio/gpio.c")
# add_library(gpio SHARED ${GPIO_SRC})
# set_target_properties(gpio PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${LIB_OUTPUT_DIR})
# target_include_directories(gpio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# target_compile_options(gpio PRIVATE -g -Wall)

# # 创建 led 动态库
# file(GLOB LED_SRC "led/led.c")
# add_library(led SHARED ${LED_SRC})
# set_target_properties(led PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${LIB_OUTPUT_DIR})
# target_include_directories(led PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# target_compile_options(led PRIVATE -g -Wall)

# # 设置 led 库的附加属性
# set_target_properties(led
#     PROPERTIES
#     INSTALL_RPATH "$ORIGIN"  # 设置共享库的运行时路径
#     BUILD_WITH_INSTALL_RPATH TRUE  # 在构建时使用安装路径
# )

# # 将 gpio 库链接到 led 库
# target_link_libraries(led PUBLIC gpio)

# # 创建 framebuffer 动态库
# file(GLOB FB_SRC "display/framebuffer.c")
# add_library(fb SHARED ${FB_SRC})
# set_target_properties(fb PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${LIB_OUTPUT_DIR})
# target_include_directories(fb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
# target_compile_options(fb PRIVATE -g -Wall)

# # 创建 video 动态库
# file(GLOB VIDEO_SRC "video/video.cpp" "video/luckfox_mpi.cpp")
# add_library(video SHARED ${VIDEO_SRC})
# set_target_properties(video PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${LIB_OUTPUT_DIR})
# target_include_directories(video PRIVATE
#     ${CMAKE_CURRENT_SOURCE_DIR}/../include
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/include/rkaiq
#     ${CMAKE_SOURCE_DIR}/include/rkaiq/uAPI2
#     ${CMAKE_SOURCE_DIR}/include/rkaiq/common
#     ${CMAKE_SOURCE_DIR}/include/rkaiq/xcore
#     ${CMAKE_SOURCE_DIR}/include/rkaiq/algos
#     ${CMAKE_SOURCE_DIR}/include/rkaiq/iq_parser
#     ${CMAKE_SOURCE_DIR}/include/rkaiq/iq_parser_v2
#     ${CMAKE_SOURCE_DIR}/include/rkaiq/smartIr
# )

# # # Set the SDK path and cross-compilers
# # set(SDK_PATH "/home/xiaolan/luckfox-pico")

# # # Add definition for specific drivers
# # add_definitions(-DRV1106_1103)

# target_compile_options(video PRIVATE -g -Wall)

# # 设置链接目录
# link_directories(${CMAKE_CURRENT_SOURCE_DIR}../../lib)
# # message(STATUS "Library directories: ${LIB_DIR}")

# # set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${CMAKE_SOURCE_DIR}/lib:/usr/lib")

# # 将必要的库链接到 video 库
# target_link_libraries(video PUBLIC 
#     rockiva
#     sample_comm
#     rockit
#     rockchip_mpp
#     rkaiq
#     pthread
#     # rtsp
# )



