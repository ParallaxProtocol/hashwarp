set(SOURCES
 PoolURI.cpp PoolURI.h
 PoolClient.h
 PoolManager.h PoolManager.cpp
 PoolCheck.h PoolCheck.cpp
 Tls.h Tls.cpp
 testing/SimulateClient.h testing/SimulateClient.cpp
 stratum/EthStratumClient.h stratum/EthStratumClient.cpp
 getwork/EthGetworkClient.h getwork/EthGetworkClient.cpp
)

# hunter_add_package(OpenSSL) is in the top level CMakeLists.txt
# Hunter's static OpenSSL, even where the system has its own: the executable
# must not depend on the OpenSSL version installed
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)

add_library(poolprotocols ${SOURCES})
target_link_libraries(poolprotocols PRIVATE devcore hashwarp-buildinfo xhash::xhash Boost::system jsoncpp_static OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(poolprotocols PRIVATE ..)
