add_executable(Mikhlink
    main.cpp
    network/NetworkAdapter.cpp
    network/NetworkAdapter.h
)

target_link_libraries(Mikhlink PRIVATE
    iphlpapi
    ws2_32
)
