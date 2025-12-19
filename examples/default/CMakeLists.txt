cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS "../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fpr)

add_compile_options(-Wall -Wextra -Werror)