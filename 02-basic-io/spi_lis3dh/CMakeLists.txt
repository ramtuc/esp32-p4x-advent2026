cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 必要なコンポーネントだけをビルドする（main/CMakeLists.txt の PRIV_REQUIRES に書いた物）
idf_build_set_property(MINIMAL_BUILD ON)
project(spi_lis3dh)
