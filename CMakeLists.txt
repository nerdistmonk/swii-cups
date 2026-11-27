cmake_minimum_required(VERSION 3.12)

project(swii-cups C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Cups REQUIRED)

set(SWII_CUPS_SERVERBIN "/usr/lib/cups" CACHE PATH
    "CUPS server binary directory; filter/ and backend/ are installed below it")
set(SWII_CUPS_MODELDIR "/usr/share/cups/model" CACHE PATH
    "Directory where the PPD files are installed")

add_compile_definitions(_DEFAULT_SOURCE)
add_compile_options(-Wall -Wextra -Wpedantic)

add_library(swii_encode STATIC src/stylewriter_encode.c)
target_include_directories(swii_encode PUBLIC src)

add_executable(rastertostylewriter src/rastertostylewriter.c)
target_include_directories(rastertostylewriter PRIVATE ${CUPS_INCLUDE_DIR})
target_link_libraries(rastertostylewriter PRIVATE swii_encode ${CUPS_LIBRARIES})

add_executable(stylewriter src/stylewriter_backend.c)
target_include_directories(stylewriter PRIVATE ${CUPS_INCLUDE_DIR})
target_link_libraries(stylewriter PRIVATE swii_encode ${CUPS_LIBRARIES})

enable_testing()
add_executable(test_encode tests/test_encode.c)
target_link_libraries(test_encode PRIVATE swii_encode)
add_test(NAME test_encode COMMAND test_encode)

install(TARGETS rastertostylewriter DESTINATION ${SWII_CUPS_SERVERBIN}/filter)
install(TARGETS stylewriter DESTINATION ${SWII_CUPS_SERVERBIN}/backend
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE)
install(FILES stylewriter-bw.ppd stylewriter-color.ppd
    DESTINATION ${SWII_CUPS_MODELDIR})
