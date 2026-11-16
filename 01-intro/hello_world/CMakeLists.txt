cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 必要なコンポーネントだけをビルドする（ESP-IDF の hello_world と同じ）
idf_build_set_property(MINIMAL_BUILD ON)
project(hello_world)
