cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# main/CMakeLists.txt の PRIV_REQUIRES に書いたコンポーネントだけをビルドする
idf_build_set_property(MINIMAL_BUILD ON)
project(ppa_bench)
