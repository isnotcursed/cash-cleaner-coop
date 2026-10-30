set(TARGET CashCleanerCoop)

add_library(${TARGET} SHARED src/dllmain.cpp)
target_link_libraries(${TARGET} PUBLIC UE4SS ccscoop_protocol)

set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME "main"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/package/CashCleanerCoop/dlls"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/package/CashCleanerCoop/dlls"
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/enabled.txt"
    "${CMAKE_BINARY_DIR}/package/CashCleanerCoop/enabled.txt"
    COPYONLY
)
