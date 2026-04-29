cmake_minimum_required(VERSION 3.25)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(gba_esp32_test)
