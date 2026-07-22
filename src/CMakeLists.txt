find_package(libobs REQUIRED)

add_library(mikhlink MODULE
    plugin-main.cpp
    network/NetworkAdapter.cpp
    network/NetworkAdapter.h
)

target_link_libraries(mikhlink PRIVATE
    OBS::libobs
    iphlpapi
    ws2_32
)

set_target_properties(mikhlink PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mikhlink"
)
