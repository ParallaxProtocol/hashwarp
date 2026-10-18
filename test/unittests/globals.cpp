// Globals the libraries expect the executable to define (see hashwarp/main.cpp)

#include <boost/asio/io_service.hpp>

bool g_exitOnError = false;
boost::asio::io_service g_io_service;
