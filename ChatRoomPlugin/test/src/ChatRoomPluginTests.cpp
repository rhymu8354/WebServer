/**
 * @file ChatRoomPluginTests.cpp
 *
 * This module contains the unit tests of the
 * Chat Room web-server plugin.
 *
 * © 2018 by Richard Walters
 */

#include <gtest/gtest.h>
#include <stdio.h>
#include <string>
#include <SystemAbstractions/File.hpp>
#include <WebServer/PluginEntryPoint.hpp>
#include <WebSockets/WebSocket.hpp>

#ifdef _WIN32
#define API __declspec(dllimport)
#else /* POSIX */
#define API
#endif /* _WIN32 / POSIX */
extern "C" API void LoadPlugin(
    Http::IServer* server,
    Json::Json configuration,
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate,
    std::function< void() >& unloadDelegate
);

namespace {

    /**
     * This is the path in the server at which to place the plug-in.
     */
    const std::string CHAT_ROOM_PATH = "/chat";

    /**
     * This simulates the actual web server hosting the chat room.
     */
    struct MockServer
        : public Http::IServer
    {
        // Properties

        /**
         * This is the resource subspace path that the unit under
         * test has registered.
         */
        std::vector< std::string > registeredResourceSubspacePath;

        /**
         * This is the delegate that the unit under test has registered
         * to be called to handle resource requests.
         */
        ResourceDelegate registeredResourceDelegate;

        // Methods

        // IServer
    public:
        virtual SystemAbstractions::DiagnosticsSender::SubscriptionToken SubscribeToDiagnostics(
            SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
            size_t minLevel = 0
        ) override {
            return 0;
        }

        virtual void UnsubscribeFromDiagnostics(SystemAbstractions::DiagnosticsSender::SubscriptionToken subscriptionToken) override {
        }

        virtual std::string GetConfigurationItem(const std::string& key) override {
            return "";
        }

        virtual void SetConfigurationItem(
            const std::string& key,
            const std::string& value
        ) override {
        }

        virtual UnregistrationDelegate RegisterResource(
            const std::vector< std::string >& resourceSubspacePath,
            ResourceDelegate resourceDelegate
        ) override {
            registeredResourceSubspacePath = resourceSubspacePath;
            registeredResourceDelegate = resourceDelegate;
            return []{};
        }
    };

    /**
     * This is a fake connection which is used with both ends of
     * the WebSockets going between the chat room and the test framework.
     */
    struct MockConnection
        : public Http::Connection
    {
        // Properties

        /**
         * This is the delegate to call whenever data is recevied
         * from the remote peer.
         */
        DataReceivedDelegate dataReceivedDelegate;

        /**
         * This is the delegate to call whenever the connection
         * has been broken.
         */
        BrokenDelegate brokenDelegate;

        /**
         * This holds onto a copy of all data received from the remote peer.
         */
        std::vector< uint8_t > dataReceived;

        /**
         * This flag is set if the remote peer breaks the connection.
         */
        bool broken = false;

        /**
         * This is the identifier to report for the peer of the connection.
         */
        std::string peerId;

        // Methods

        /**
         * This is the constructor for the structure.
         */
        explicit MockConnection(const std::string& peerId)
            : peerId(peerId)
        {
        }

        // Http::Connection

        virtual std::string GetPeerId() override {
            return peerId;
        }

        virtual void SetDataReceivedDelegate(DataReceivedDelegate newDataReceivedDelegate) override {
            dataReceivedDelegate = newDataReceivedDelegate;
        }

        virtual void SetBrokenDelegate(BrokenDelegate newBrokenDelegate) override {
            brokenDelegate = newBrokenDelegate;
        }

        virtual void SendData(const std::vector< uint8_t >& data) override {
            (void)dataReceived.insert(
                dataReceived.end(),
                data.begin(),
                data.end()
            );
        }

        virtual void Break(bool clean) override {
            broken = true;
        }
    };

}

/**
 * This is the test fixture for these tests, providing common
 * setup and teardown for each test.
 */
struct ChatRoomPluginTests
    : public ::testing::Test
{
    // Properties

    /**
     * This simulates the actual web server hosting the chat room.
     */
    MockServer server;

    /**
     * This is the function to call to unload the chat room plug-in.
     */
    std::function< void() > unloadDelegate;

    /**
     * This is used to connect with the chat room and communicate with it.
     */
    WebSockets::WebSocket ws;

    /**
     * This is used to simulate the client side of the HTTP connection
     * between the client and the chat room.
     */
    std::shared_ptr< MockConnection > clientConnection = std::make_shared< MockConnection >("mock-client");

    /**
     * This is used to simulate the server side of the HTTP connection
     * between the client and the chat room.
     */
    std::shared_ptr< MockConnection > serverConnection = std::make_shared< MockConnection >("mock-server");

    // Methods

    // ::testing::Test

    virtual void SetUp() {
        Json::Json config(Json::Json::Type::Object);
        config.Set("space", CHAT_ROOM_PATH);
        LoadPlugin(
            &server,
            config,
            [](
                std::string senderName,
                size_t level,
                std::string message
            ){
                printf(
                    "[%s:%zu] %s\n",
                    senderName.c_str(),
                    level,
                    message.c_str()
                );
            },
            unloadDelegate
        );
        const auto openRequest = std::make_shared< Http::Request >();
        openRequest->method = "GET";
        (void)openRequest->target.ParseFromString("/chat");
        ws.StartOpenAsClient(*openRequest);
        const auto openResponse = server.registeredResourceDelegate(openRequest, serverConnection);
        ASSERT_TRUE(ws.FinishOpenAsClient(clientConnection, *openResponse));
    }

    virtual void TearDown() {
        unloadDelegate();
    }
};

TEST_F(ChatRoomPluginTests, Load) {
    ASSERT_FALSE(unloadDelegate == nullptr);
    ASSERT_FALSE(server.registeredResourceDelegate == nullptr);
    ASSERT_EQ(
        (std::vector< std::string >{
            "chat",
        }),
        server.registeredResourceSubspacePath
    );
}

TEST_F(ChatRoomPluginTests, Connect) {
    
}
