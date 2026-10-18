// Globals the libraries expect the executable to define (see hashwarp/main.cpp)

#include <boost/asio/io_service.hpp>

#include <atomic>

bool g_exitOnError = false;
std::atomic<int> g_exitCode = {0};
boost::asio::io_service g_io_service;
