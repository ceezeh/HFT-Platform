# Sources and Headers
# Library
set(LIBRARY_NAME "lmax_disruptor")
add_library(${LIBRARY_NAME} INTERFACE)
target_include_directories(lmax_disruptor INTERFACE include)

target_link_libraries(
    ${LIBRARY_NAME} 
    INTERFACE stdc++) 

if(${ENABLE_LTO})
    target_enable_lto(
        TARGET
        ${LIBRARY_NAME}
        ENABLE
        ON)
endif()

if(${ENABLE_CLANG_TIDY})
    add_clang_tidy_to_target(${LIBRARY_NAME})
endif()

add_subdirectory(tests)