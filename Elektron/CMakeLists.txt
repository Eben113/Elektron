cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    lib/ZMPT101B
    lib/sensor
    lib/publisher
    lib/TinyGSM
    lib/PubSubClient)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Elektron)