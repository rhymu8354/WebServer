/**
 * @file ChatRoomPluginTests.cpp
 *
 * This module contains the unit tests of the
 * Chat Room web-server plugin.
 *
 * © 2018 by Richard Walters
 */

#include <gtest/gtest.h>
#include <Json/Json.hpp>
#include <stdio.h>
#include <string>
#include <SystemAbstractions/File.hpp>
#include <SystemAbstractions/StringExtensions.hpp>
#include <vector>
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
     * This is the number of mock clients to connect to the chat room.
     */
    constexpr size_t NUM_MOCK_CLIENTS = 3;

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
         * This is the delegate to call whenever data is to be sent
         * to the remote peer.
         */
        DataReceivedDelegate sendDataDelegate;

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
            sendDataDelegate(data);
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
    WebSockets::WebSocket ws[NUM_MOCK_CLIENTS];

    /**
     * This is used to simulate the client side of the HTTP connection
     * between the client and the chat room.
     */
    std::shared_ptr< MockConnection > clientConnection[NUM_MOCK_CLIENTS];

    /**
     * This is used to simulate the server side of the HTTP connection
     * between the client and the chat room.
     */
    std::shared_ptr< MockConnection > serverConnection[NUM_MOCK_CLIENTS];

    /**
     * This stores all text messages received from the chat room.
     */
    std::vector< Json::Json > messagesReceived[NUM_MOCK_CLIENTS];

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
        for (size_t i = 0; i < NUM_MOCK_CLIENTS; ++i) {
             clientConnection[i] = std::make_shared< MockConnection >(
                 SystemAbstractions::sprintf(
                     "mock-client-%zu",
                     i
                 )
             );
             serverConnection[i] = std::make_shared< MockConnection >(
                 SystemAbstractions::sprintf(
                     "mock-server-%zu",
                     i
                 )
             );
            clientConnection[i]->sendDataDelegate = [this, i](
                const std::vector< uint8_t >& data
            ){
                serverConnection[i]->dataReceivedDelegate(data);
            };
            serverConnection[i]->sendDataDelegate = [this, i](
                const std::vector< uint8_t >& data
            ){
                clientConnection[i]->dataReceivedDelegate(data);
            };
            ws[i].SetTextDelegate(
                [this, i](const std::string& data){
                    messagesReceived[i].push_back(Json::Json::FromEncoding(data));
                }
            );
            const auto openRequest = std::make_shared< Http::Request >();
            openRequest->method = "GET";
            (void)openRequest->target.ParseFromString("/chat");
            ws[i].StartOpenAsClient(*openRequest);
            const auto openResponse = server.registeredResourceDelegate(openRequest, serverConnection[i]);
            ASSERT_TRUE(ws[i].FinishOpenAsClient(clientConnection[i], *openResponse));
        }
    }

    virtual void TearDown() {
        unloadDelegate();
    }
};

TEST_F(ChatRoomPluginTests, LoadAndConnect) {
    ASSERT_FALSE(unloadDelegate == nullptr);
    ASSERT_FALSE(server.registeredResourceDelegate == nullptr);
    ASSERT_EQ(
        (std::vector< std::string >{
            "chat",
        }),
        server.registeredResourceSubspacePath
    );
}

TEST_F(ChatRoomPluginTests, SetNickName) {
    const std::string password = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "GetNickNames");
    ws[0].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "NickNames");
    expectedResponse.Set("NickNames", {"Bob"});
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();
}

TEST_F(ChatRoomPluginTests, SetNickNameTwice) {
    // Set nickname for first client.
    const std::string password1 = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password1);
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();

    // Set nickname for second client, trying
    // to grab the same nickname as the first client,
    // but with the wrong password.
    const std::string password2 = "Poggers";
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password2);
    ws[1].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", false);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[1]
    );
    messagesReceived[1].clear();

    // Set nickname for third client, trying
    // to grab the same nickname as the first client,
    // with the correct password.  This is allowed, and
    // a real-world use case might be the same user joining
    // the chat room from two separate browser windows.
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password1);
    ws[2].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[2]
    );
    messagesReceived[2].clear();

    // Have the second client get the nickname list.
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "GetNickNames");
    ws[1].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "NickNames");
    expectedResponse.Set("NickNames", {"Bob"});
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[1]
    );
    messagesReceived[1].clear();
}

TEST_F(ChatRoomPluginTests, RoomClearedAfterUnload) {
    // Have Bob join the chat room.
    const std::string password = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();

    // Reload the chat room.
    TearDown();
    SetUp();

    // Verify the nickname list is empty.
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "GetNickNames");
    ws[0].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "NickNames");
    expectedResponse.Set("NickNames", Json::Json(Json::Json::Type::Array));
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();
}

TEST_F(ChatRoomPluginTests, Tell) {
    // Bob joins the room.
    const std::string password1 = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password1);
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();

    // Alice joins the room.
    const std::string password2 = "FeelsBadMan";
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Alice");
    message.Set("Password", password2);
    ws[1].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[1]
    );
    messagesReceived[1].clear();

    // Bob peeks at the chat room member list.
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "GetNickNames");
    ws[0].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "NickNames");
    expectedResponse.Set("NickNames", {"Alice", "Bob"});
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();

    // Alice says something.
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "Tell");
    message.Set("Tell", "HeyGuys");
    ws[1].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "Tell");
    expectedResponse.Set("Sender", "Alice");
    expectedResponse.Set("Tell", "HeyGuys");
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[1]
    );
    messagesReceived[1].clear();

    // Bob says something (but it's empty).
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "Tell");
    message.Set("Tell", "");
    ws[0].SendText(message.ToEncoding());
    EXPECT_TRUE(messagesReceived[0].empty());
    EXPECT_TRUE(messagesReceived[1].empty());
}
