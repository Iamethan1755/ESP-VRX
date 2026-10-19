cmake_minimum_required(VERSION 3.16)
set(C5VRX4_EXPERIMENT ON)
if(DEFINED C5VRX4_VERSION)
    set(PROJECT_VER "${C5VRX4_VERSION}")
else()
    set(PROJECT_VER "4.0.0-alpha-dev")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.base.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
# The existing main workflow needs no edits: configuration verifies the local
# source snapshot and generated three-bundle programs before cross-compiling.
find_program(C5VRX4_HOST_PYTHON NAMES python3 REQUIRED)
execute_process(COMMAND "${C5VRX4_HOST_PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/verify.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}" RESULT_VARIABLE C5VRX4_VERIFY_RESULT)
if(NOT C5VRX4_VERIFY_RESULT EQUAL 0)
    message(FATAL_ERROR "C5VRX-4 integration verification failed")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c5vrx4)
