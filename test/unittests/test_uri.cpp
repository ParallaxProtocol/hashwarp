// Pool URI parsing and redaction

#include <libpoolprotocols/PoolURI.h>

#include <gtest/gtest.h>

using dev::URI;

TEST(uri, password_is_redacted)
{
    URI u("stratum+tcp://0xabc.rig1:secret@pool.example.com:4444");
    EXPECT_EQ(u.User(), "0xabc");
    EXPECT_EQ(u.Workername(), "rig1");
    EXPECT_EQ(u.Pass(), "secret");
    EXPECT_EQ(u.strRedacted(), "stratum+tcp://0xabc.rig1:***@pool.example.com:4444");
    EXPECT_EQ(u.strRedacted().find("secret"), std::string::npos);
}

TEST(uri, without_password_is_unchanged)
{
    for (const char* s : {"stratum+tcp://0xabc.rig1@pool.example.com:4444",
             "stratum+ssl://0xabc@pool.example.com:5555", "http://127.0.0.1:8545"})
        EXPECT_EQ(URI(s).strRedacted(), s);
}

TEST(uri, getwork_family)
{
    URI u("http://127.0.0.1:8545");
    EXPECT_EQ(u.Family(), dev::ProtocolFamily::GETWORK);
    EXPECT_EQ(u.Port(), 8545);
    EXPECT_TRUE(u.IsLoopBack());
    EXPECT_FALSE(u.Websocket());
}

TEST(uri, websocket)
{
    URI ws("ws://127.0.0.1:8546");
    EXPECT_EQ(ws.Family(), dev::ProtocolFamily::GETWORK);
    EXPECT_TRUE(ws.Websocket());
    EXPECT_EQ(ws.SecLevel(), dev::SecureLevel::NONE);
    EXPECT_EQ(ws.Port(), 8546);
    URI wss("wss://node.example.com:443");
    EXPECT_TRUE(wss.Websocket());
    EXPECT_EQ(wss.SecLevel(), dev::SecureLevel::TLS);
    EXPECT_FALSE(URI("stratum+tcp://0xabc@pool.example.com:4444").Websocket());
}
