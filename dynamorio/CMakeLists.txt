cmake_minimum_required(VERSION 2.6)

# Hack: When building against a DR build dir, it doesn't set up an include dir
# for extension headers, so we go ahead and add an include path for the source
# dir of the extensions we use.
#include_directories("dr/ext/drutil")

find_package(DynamoRIO)
if (NOT DynamoRIO_FOUND)
  message(FATAL_ERROR "DynamoRIO package required to build")
endif(NOT DynamoRIO_FOUND)

add_library(dr_asan SHARED dr_asan.cpp)
configure_DynamoRIO_client(dr_asan)

use_DynamoRIO_extension(dr_asan drutil)
