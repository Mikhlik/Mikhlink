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

if(BUILD_TESTING)
    add_executable(moblink-server-tests
        tests/moblink-server-test.cpp
        moblink/MoblinkServer.cpp
        moblink/MoblinkServer.h
    )

    target_include_directories(moblink-server-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_link_libraries(moblink-server-tests PRIVATE
        Qt6::Core
        Qt6::Network
    )

    if(WIN32)
        add_custom_command(TARGET moblink-server-tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt6::Core>
                $<TARGET_FILE:Qt6::Network>
                $<TARGET_FILE_DIR:moblink-server-tests>
            COMMENT "Deploying Qt runtime for Moblink protocol test"
        )
    endif()

    add_test(NAME moblink-server-protocol COMMAND moblink-server-tests)
endif()
