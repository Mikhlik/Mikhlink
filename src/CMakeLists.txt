find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)

add_library(mikhlink MODULE
    plugin-main.cpp
    moblink/MoblinkServer.cpp
    moblink/MoblinkServer.h
    network/NetworkAdapter.cpp
    network/NetworkAdapter.h
)

target_link_libraries(mikhlink PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
    iphlpapi
    ws2_32
)

set_target_properties(mikhlink PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mikhlink"
)
