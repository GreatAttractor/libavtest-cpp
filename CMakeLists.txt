project("libavtest-cpp")

cmake_minimum_required(VERSION 3.1)

include(FindPkgConfig)

add_executable(libavtest WIN32
    src/main.cpp
)

set_property(TARGET libavtest PROPERTY CXX_STANDARD 17)
set_property(TARGET libavtest PROPERTY CXX_STANDARD_REQUIRED ON)

pkg_check_modules(LIBAVFORMAT libavformat REQUIRED)
target_include_directories(libavtest PRIVATE ${LIBAVFORMAT_INCLUDE_DIRS})
target_link_libraries(libavtest PRIVATE ${LIBAVFORMAT_LIBRARIES})

pkg_check_modules(LIBAVCODEC libavcodec REQUIRED)
target_include_directories(libavtest PRIVATE ${LIBAVCODEC_INCLUDE_DIRS})
target_link_libraries(libavtest PRIVATE ${LIBAVCODEC_LIBRARIES})

pkg_check_modules(LIBAVUTIL libavutil REQUIRED)
target_include_directories(libavtest PRIVATE ${LIBAVUTIL_INCLUDE_DIRS})
target_link_libraries(libavtest PRIVATE ${LIBAVUTIL_LIBRARIES})

pkg_check_modules(LIBSWSCALE libswscale REQUIRED)
target_include_directories(libavtest PRIVATE ${LIBSWSCALE_INCLUDE_DIRS})
target_link_libraries(libavtest PRIVATE ${LIBSWSCALE_LIBRARIES})
