##
# @file CMakeLists.txt
# @brief Tuya T5AI Pixel CS100A distance display
#/

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)

aux_source_directory(${APP_PATH}/src APP_SRC)
set(APP_INC ${APP_PATH}/include)
set(APP_OPTIONS "")

add_library(${EXAMPLE_LIB})
message(STATUS "EXAMPLE_LIB:${APP_PATH}")

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRC}
)

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
)

target_compile_options(${EXAMPLE_LIB}
    PRIVATE
        ${APP_OPTIONS}
)
