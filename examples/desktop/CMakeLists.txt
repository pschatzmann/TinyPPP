# Desktop demo: PPP endpoint on a pseudo terminal that answers ping
add_executable(ppp-pty ppp-pty.cpp)
target_link_libraries(ppp-pty PRIVATE tinyppp)
