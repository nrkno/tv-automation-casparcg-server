cmake_minimum_required (VERSION 2.6)
project (poco)

set(SOURCES
		consumer/poco_consumer.cpp

		poco.cpp
)
set(HEADERS
		consumer/poco_consumer.h

		poco.h
)

add_library(poco ${SOURCES} ${HEADERS})

include_directories(..)
include_directories(../..)
include_directories(${BOOST_INCLUDE_PATH})
include_directories(${RXCPP_INCLUDE_PATH})
include_directories(${POCO_INCLUDE_PATH})
include_directories(${TBB_INCLUDE_PATH})

set_target_properties(poco PROPERTIES FOLDER modules)
source_group(sources\\consumer consumer/*)
source_group(sources ./*)

target_link_libraries(poco common core)

casparcg_add_include_statement("modules/poco/poco.h")
casparcg_add_init_statement("poco::init" "poco")
casparcg_add_module_project("poco")
