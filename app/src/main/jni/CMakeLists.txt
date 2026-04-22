cmake_minimum_required(VERSION 3.22.1)
project(vcam_injector)

set(FRIDA_CORE_DIR "${CMAKE_SOURCE_DIR}/../../../../third_party/frida-core-devkit")
if (NOT EXISTS "${FRIDA_CORE_DIR}/frida-core.h" OR NOT EXISTS "${FRIDA_CORE_DIR}/libfrida-core.a")
    message(FATAL_ERROR "Missing Frida Core devkit at ${FRIDA_CORE_DIR}")
endif()

add_library(frida-core STATIC IMPORTED)
set_target_properties(frida-core PROPERTIES
    IMPORTED_LOCATION "${FRIDA_CORE_DIR}/libfrida-core.a"
)

add_executable(injector_helper injector.cpp injector_main.cpp)
set_target_properties(injector_helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../assets"
)

add_library(test_inject SHARED test_inject.cpp)
set_target_properties(test_inject PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../assets"
    OUTPUT_NAME "test_inject"
)

find_library(log-lib log)
find_library(dl-lib dl)
find_library(m-lib m)

target_include_directories(injector_helper PRIVATE "${FRIDA_CORE_DIR}")
target_compile_definitions(injector_helper PRIVATE ANDROID=1)
target_compile_options(injector_helper PRIVATE -ffunction-sections -fdata-sections -pthread)
target_link_options(injector_helper PRIVATE -Wl,--export-dynamic -Wl,--gc-sections -pthread)

target_link_libraries(injector_helper frida-core ${log-lib} ${dl-lib} ${m-lib})
target_link_libraries(test_inject ${log-lib})
