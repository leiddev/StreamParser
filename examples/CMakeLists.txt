# Example programs. They are part of the repository -- CI builds and the e2e
# tests run them -- but they are never installed: A55A55AAParser.h and
# StartEndParser.h are meant to be copied into the downstream project, not
# linked against.

add_executable(SerialDemo serial_main.cc)
target_link_libraries(SerialDemo PRIVATE sp::transport sp::warnings)

add_executable(TcpServerDemo tcp_server_main.cc)
target_link_libraries(TcpServerDemo PRIVATE sp::transport sp::warnings)

add_executable(TcpClientDemo tcp_client_main.cc)
target_link_libraries(TcpClientDemo PRIVATE sp::transport sp::warnings)

add_executable(UdpDemo udp_main.cc)
target_link_libraries(UdpDemo PRIVATE sp::transport sp::warnings)

# Transport facade demo: no asio in the program itself.
add_executable(TransportDemo transport_main.cc)
target_link_libraries(TransportDemo PRIVATE sp::transport sp::warnings)
