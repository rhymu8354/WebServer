/**
 * @file ChatRoomPluginTests.cpp
 *
 * This module contains the unit tests of the
 * Chat Room web-server plugin.
 *
 * © 2018 by Richard Walters
 */

#include <condition_variable>
#include <gtest/gtest.h>
#include <Json/Json.hpp>
#include <mutex>
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

        /**
         * This is used to synchronize access to this object's state.
         */
        std::recursive_mutex mutex;

        // Methods

        /**
         * This is the constructor for the structure.
         */
        explicit MockConnection(const std::string& peerId)
            : peerId(peerId)
        {
        }

        void ReceiveData(const std::vector< uint8_t >& data) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            dataReceivedDelegate(data);
        }

        // Http::Connection

        virtual std::string GetPeerId() override {
            return peerId;
        }

        virtual void SetDataReceivedDelegate(DataReceivedDelegate newDataReceivedDelegate) override {
            std::lock_guard< decltype(mutex) > lock(mutex);
            dataReceivedDelegate = newDataReceivedDelegate;
        }

        virtual void SetBrokenDelegate(BrokenDelegate newBrokenDelegate) override {
            std::lock_guard< decltype(mutex) > lock(mutex);
            brokenDelegate = newBrokenDelegate;
        }

        virtual void SendData(const std::vector< uint8_t >& data) override {
            std::lock_guard< decltype(mutex) > lock(mutex);
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
     * This is used to synchronize access to the wsClosed, messagesReceived,
     * and wsWaitCondition variables.
     */
    std::mutex mutex;

    /**
     * This is used to wait for, or signal, the condition of one or
     * more messagesReceived updates or wsClosed flags being set.
     */
    std::condition_variable waitCondition;

    /**
     * This is used to connect with the chat room and communicate with it.
     */
    WebSockets::WebSocket ws[NUM_MOCK_CLIENTS];

    /**
     * These flags indicate whether or not the corresponding WebSockets
     * have been closed.
     */
    bool wsClosed[NUM_MOCK_CLIENTS];

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

    /**
     * These are the diagnostic messages that have been
     * received from the unit under test.
     */
    std::vector< std::string > diagnosticMessages;

    // Methods

    /**
     * This method sets up the given client-side WebSocket
     * used to test the chat room.
     *
     * @param[in] i
     *     This is the index of the client-side WebSocket to set up.
     */
    void InitilizeClientWebSocket(size_t i) {
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
            serverConnection[i]->ReceiveData(data);
        };
        serverConnection[i]->sendDataDelegate = [this, i](
            const std::vector< uint8_t >& data
        ){
            clientConnection[i]->ReceiveData(data);
        };
        wsClosed[i] = false;
        ws[i].SetTextDelegate(
            [this, i](const std::string& data){
                std::lock_guard< decltype(mutex) > lock(mutex);
                messagesReceived[i].push_back(Json::Json::FromEncoding(data));
                waitCondition.notify_all();
            }
        );
        ws[i].SetCloseDelegate(
            [this, i](
                unsigned int code,
                const std::string& reason
            ){
                std::lock_guard< decltype(mutex) > lock(mutex);
                wsClosed[i] = true;
                waitCondition.notify_all();
            }
        );
    }

    // ::testing::Test

    virtual void SetUp() {
        Json::Json config(Json::Json::Type::Object);
        config.Set("space", CHAT_ROOM_PATH);
        LoadPlugin(
            &server,
            config,
            [this](
                std::string senderName,
                size_t level,
                std::string message
            ){
                diagnosticMessages.push_back(
                    SystemAbstractions::sprintf(
                        "%s[%zu]: %s",
                        senderName.c_str(),
                        level,
                        message.c_str()
                    )
                );
            },
            unloadDelegate
        );
        for (size_t i = 0; i < NUM_MOCK_CLIENTS; ++i) {
            InitilizeClientWebSocket(i);
            const auto openRequest = std::make_shared< Http::Request >();
            openRequest->method = "GET";
            (void)openRequest->target.ParseFromString("/chat");
            ws[i].StartOpenAsClient(*openRequest);
            const auto openResponse = server.registeredResourceDelegate(openRequest, serverConnection[i], "");
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
    expectedResponse.Set("Type", "Join");
    expectedResponse.Set("NickName", "Bob");
    Json::Json expectedResponse2(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "SetNickNameResult");
    expectedResponse2.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[0]
    );
    ASSERT_EQ(
        (std::vector< std::string >{
            "Session #1[1]: Nickname changed from '' to 'Bob'",
        }),
        diagnosticMessages
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
    expectedResponse.Set("Type", "Join");
    expectedResponse.Set("NickName", "Bob");
    Json::Json expectedResponse2(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "SetNickNameResult");
    expectedResponse2.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();

    // Set nickname for second client, trying
    // to grab the same nickname as the first client,
    // but with the wrong password.
    messagesReceived[1].clear();
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
    messagesReceived[2].clear();
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
    expectedResponse.Set("Type", "Join");
    expectedResponse.Set("NickName", "Bob");
    Json::Json expectedResponse2(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "SetNickNameResult");
    expectedResponse2.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
            expectedResponse2,
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
    expectedResponse.Set("Type", "Join");
    expectedResponse.Set("NickName", "Bob");
    Json::Json expectedResponse2(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "SetNickNameResult");
    expectedResponse2.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();

    // Alice joins the room.
    messagesReceived[1].clear();
    const std::string password2 = "FeelsBadMan";
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Alice");
    message.Set("Password", password2);
    ws[1].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "Join");
    expectedResponse.Set("NickName", "Alice");
    expectedResponse2 = Json::Json(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "SetNickNameResult");
    expectedResponse2.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[1]
    );
    messagesReceived[1].clear();

    // Bob peeks at the chat room member list.
    messagesReceived[0].clear();
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

TEST_F(ChatRoomPluginTests, Join) {
    // Bob joins the room.
    const std::string password1 = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password1);
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "Join");
    expectedResponse.Set("NickName", "Bob");
    Json::Json expectedResponse2(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "SetNickNameResult");
    expectedResponse2.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();

    // Alice joins the room.  Expect both Alice and Bob to see a message
    // about Alice joining.
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    const std::string password2 = "FeelsBadMan";
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Alice");
    message.Set("Password", password2);
    ws[1].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "Join");
    expectedResponse.Set("NickName", "Alice");
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();
    expectedResponse2 = Json::Json(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "SetNickNameResult");
    expectedResponse2.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[1]
    );
    messagesReceived[1].clear();
}

TEST_F(ChatRoomPluginTests, Leave) {
    // Bob joins the room.
    const std::string password1 = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password1);
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "Join");
    expectedResponse.Set("NickName", "Bob");
    Json::Json expectedResponse2(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "SetNickNameResult");
    expectedResponse2.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();

    // Alice joins the room.
    messagesReceived[1].clear();
    const std::string password2 = "FeelsBadMan";
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Alice");
    message.Set("Password", password2);
    ws[1].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "Join");
    expectedResponse.Set("NickName", "Alice");
    expectedResponse2 = Json::Json(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "SetNickNameResult");
    expectedResponse2.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[1]
    );
    messagesReceived[1].clear();

    // Alice leaves the room.
    messagesReceived[0].clear();
    diagnosticMessages.clear();
    ws[1].Close();
    {
        std::unique_lock< decltype(mutex) > lock(mutex);
        ASSERT_TRUE(
            waitCondition.wait_for(
                lock,
                std::chrono::seconds(1),
                [this]{ return wsClosed[1] && !messagesReceived[0].empty(); }
            )
        );
    }
    ASSERT_EQ(
        (std::vector< std::string >{
            "Session #2[1]: Connection to mock-server-1 closed ()",
            "Session #2[1]: Connection to mock-server-1 closed by peer",
        }),
        diagnosticMessages
    );

    // Bob peeks at the chat room member list.
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "GetNickNames");
    ws[0].SendText(message.ToEncoding());
    std::vector< Json::Json > expectedResponses;
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "Leave");
    expectedResponse.Set("NickName", "Alice");
    expectedResponses.push_back(expectedResponse);
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "NickNames");
    expectedResponse.Set("NickNames", {"Bob"});
    expectedResponses.push_back(expectedResponse);
    ASSERT_EQ(expectedResponses, messagesReceived[0]);
    messagesReceived[0].clear();
}

TEST_F(ChatRoomPluginTests, SetNickNameInTrailer) {
    // Reopen a WebSocket connection with part of a
    // "SetNickName" message captured in the trailer.
    const std::string password = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password);
    const auto messageEncoding = message.ToEncoding();
    const char mask[4] = {0x12, 0x34, 0x56, 0x78};
    std::string frame = "\x81";
    frame += (char)(0x80 + messageEncoding.length());
    frame += std::string(mask, 4);
    for (size_t i = 0; i < messageEncoding.length(); ++i) {
        frame += messageEncoding[i] ^ mask[i % 4];
    }
    const auto frameFirstHalf = frame.substr(0, frame.length() / 2);
    const auto frameSecondHalf = frame.substr(frame.length() / 2);
    ws[0].Close();
    {
        std::unique_lock< decltype(mutex) > lock(mutex);
        ASSERT_TRUE(
            waitCondition.wait_for(
                lock,
                std::chrono::seconds(1),
                [this]{ return wsClosed[0]; }
            )
        );
    }
    const auto openRequest = std::make_shared< Http::Request >();
    openRequest->method = "GET";
    (void)openRequest->target.ParseFromString("/chat");
    ws[0] = WebSockets::WebSocket();
    InitilizeClientWebSocket(0);
    ws[0].StartOpenAsClient(*openRequest);
    const auto openResponse = server.registeredResourceDelegate(openRequest, serverConnection[0], frameFirstHalf);
    ASSERT_TRUE(ws[0].FinishOpenAsClient(clientConnection[0], *openResponse));
    serverConnection[0]->dataReceivedDelegate(
        std::vector< uint8_t >(
            frameSecondHalf.begin(),
            frameSecondHalf.end()
        )
    );
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "Join");
    expectedResponse.Set("NickName", "Bob");
    Json::Json expectedResponse2(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "SetNickNameResult");
    expectedResponse2.Set("Success", true);
    ASSERT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
            expectedResponse2,
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

TEST_F(ChatRoomPluginTests, ConnectionNotUpgraded) {
    const auto connection = std::make_shared< MockConnection >("mock-client");
    std::string responseText;
    connection->sendDataDelegate = [this, &responseText](
        const std::vector< uint8_t >& data
    ){
        responseText += std::string(
            data.begin(),
            data.end()
        );
    };
    const auto request = std::make_shared< Http::Request >();
    request->method = "GET";
    (void)request->target.ParseFromString("");
    const auto response = server.registeredResourceDelegate(request, connection, "");
    ASSERT_EQ(200, response->statusCode);
    ASSERT_EQ("text/plain", response->headers.GetHeaderValue("Content-Type"));
    ASSERT_EQ("Try again, but next time use a WebSocket.  Kthxbye!", response->body);
}

TEST_F(ChatRoomPluginTests, ChangeNickNameTwoConnectionNonLurkerToNonLurkerNotAlreadyInRoom) {
    // Set nickname to "Bob" initially on two separate connections.
    const std::string password = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());
    ws[2].SendText(message.ToEncoding());

    // Change nickname to "Bobby".
    diagnosticMessages.clear();
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    messagesReceived[2].clear();
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bobby");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    Json::Json expectedResponse2(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "Join");
    expectedResponse2.Set("NickName", "Bobby");
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse2,
            expectedResponse,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse2,
        }),
        messagesReceived[1]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse2,
        }),
        messagesReceived[2]
    );
    EXPECT_EQ(
        (std::vector< std::string >{
            "Session #1[1]: Nickname changed from 'Bob' to 'Bobby'",
        }),
        diagnosticMessages
    );

    // Verify nickname of "Bob" is changed only for one connection,
    // not the other.
    messagesReceived[1].clear();
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "Tell");
    message.Set("Tell", "Howdy");
    ws[0].SendText(message.ToEncoding());
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "Tell");
    message.Set("Tell", "HeyGuys");
    ws[2].SendText(message.ToEncoding());
    expectedResponse = Json::Json(Json::Json::Type::Object);
    expectedResponse.Set("Type", "Tell");
    expectedResponse.Set("Sender", "Bobby");
    expectedResponse.Set("Tell", "Howdy");
    expectedResponse2 = Json::Json(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "Tell");
    expectedResponse2.Set("Sender", "Bob");
    expectedResponse2.Set("Tell", "HeyGuys");
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[1]
    );
}

TEST_F(ChatRoomPluginTests, ChangeNickNameSingleConnectionNonLurkerToNonLurkerNotAlreadyInRoom) {
    // Set nickname to "Bob" initially on one connection.
    const std::string password = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());

    // Change nickname to "Bobby".
    diagnosticMessages.clear();
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    messagesReceived[2].clear();
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bobby");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse1(Json::Json::Type::Object);
    expectedResponse1.Set("Type", "Leave");
    expectedResponse1.Set("NickName", "Bob");
    Json::Json expectedResponse2(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "Join");
    expectedResponse2.Set("NickName", "Bobby");
    Json::Json expectedResponse3(Json::Json::Type::Object);
    expectedResponse3.Set("Type", "SetNickNameResult");
    expectedResponse3.Set("Success", true);
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse1,
            expectedResponse2,
            expectedResponse3,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse1,
            expectedResponse2,
        }),
        messagesReceived[1]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse1,
            expectedResponse2,
        }),
        messagesReceived[2]
    );
    EXPECT_EQ(
        (std::vector< std::string >{
            "Session #1[1]: Nickname changed from 'Bob' to 'Bobby'",
        }),
        diagnosticMessages
    );

    // Verify nickname of "Bob" really did change to "Bobby".
    messagesReceived[1].clear();
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "Tell");
    message.Set("Tell", "Howdy");
    ws[0].SendText(message.ToEncoding());
    expectedResponse1 = Json::Json(Json::Json::Type::Object);
    expectedResponse1.Set("Type", "Tell");
    expectedResponse1.Set("Sender", "Bobby");
    expectedResponse1.Set("Tell", "Howdy");
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse1,
        }),
        messagesReceived[1]
    );
}

TEST_F(ChatRoomPluginTests, ChangeNickNameFoldTwoConnections) {
    // Set nickname to "Bob" initially on one connection.
    const std::string password = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());

    // Set nickname to "Bobby" initially on second connection.
    const std::string password2 = "PogChamp";
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bobby");
    message.Set("Password", password2);
    ws[1].SendText(message.ToEncoding());

    // Change nickname from "Bob" to "Bobby".
    diagnosticMessages.clear();
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    messagesReceived[2].clear();
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bobby");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    Json::Json expectedResponse2(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "Leave");
    expectedResponse2.Set("NickName", "Bob");
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse2,
            expectedResponse,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse2,
        }),
        messagesReceived[1]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse2,
        }),
        messagesReceived[2]
    );
    EXPECT_EQ(
        (std::vector< std::string >{
            "Session #1[1]: Nickname changed from 'Bob' to 'Bobby'",
        }),
        diagnosticMessages
    );
}

TEST_F(ChatRoomPluginTests, ChangeNickNameNonLurkerToNonLurkerAlreadyInRoom) {
    // Set nickname to "Bob" initially on two separate connections.
    const std::string password = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());
    ws[1].SendText(message.ToEncoding());

    // Set nickname to "Bobby" initially on third connection.
    const std::string password2 = "PogChamp";
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bobby");
    message.Set("Password", password2);
    ws[2].SendText(message.ToEncoding());

    // Change nickname from "Bob" to "Bobby".
    diagnosticMessages.clear();
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    messagesReceived[2].clear();
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bobby");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
        }),
        messagesReceived[1]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
        }),
        messagesReceived[2]
    );
    EXPECT_EQ(
        (std::vector< std::string >{
            "Session #1[1]: Nickname changed from 'Bob' to 'Bobby'",
        }),
        diagnosticMessages
    );
}

TEST_F(ChatRoomPluginTests, ChangeNickNameTwoConnectionsNonLurkerToLurker) {
    // Set nickname to "Bob" initially on two connections.
    const std::string password = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());
    ws[1].SendText(message.ToEncoding());

    // Change nickname to "" on one connection (become a lurker).
    diagnosticMessages.clear();
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    messagesReceived[2].clear();
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "");
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
        }),
        messagesReceived[1]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
        }),
        messagesReceived[2]
    );
    EXPECT_EQ(
        (std::vector< std::string >{
            "Session #1[1]: Nickname changed from 'Bob' to ''",
        }),
        diagnosticMessages
    );
}

TEST_F(ChatRoomPluginTests, ChangeNickNameOneConnectionNonLurkerToLurker) {
    // Set nickname to "Bob" initially on one connection.
    const std::string password = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());

    // Change nickname to "" (become a lurker).
    diagnosticMessages.clear();
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    messagesReceived[2].clear();
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "");
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    Json::Json expectedResponse2(Json::Json::Type::Object);
    expectedResponse2.Set("Type", "Leave");
    expectedResponse2.Set("NickName", "Bob");
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse2,
            expectedResponse,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse2,
        }),
        messagesReceived[1]
    );
    EXPECT_EQ(
        (std::vector< std::string >{
            "Session #1[1]: Nickname changed from 'Bob' to ''",
        }),
        diagnosticMessages
    );

    // Disconnect lurker formerly known as "Bob", and verify
    // no "Leave" is published.
    messagesReceived[1].clear();
    diagnosticMessages.clear();
    ws[0].Close();
    {
        std::unique_lock< decltype(mutex) > lock(mutex);
        ASSERT_FALSE(
            waitCondition.wait_for(
                lock,
                std::chrono::seconds(1),
                [this]{ return wsClosed[0] && !messagesReceived[1].empty(); }
            )
        );
    }
    EXPECT_EQ(
        (std::vector< Json::Json >{
        }),
        messagesReceived[1]
    );
}

TEST_F(ChatRoomPluginTests, ChangeNickNameNonLurkerNotChanged) {
    // Set nickname to "Bob" initially on two connections.
    const std::string password = "PogChamp";
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());
    ws[1].SendText(message.ToEncoding());

    // Change nickname to "Bob" on one connection (not actually a change).
    diagnosticMessages.clear();
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    messagesReceived[2].clear();
    message = Json::Json(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "Bob");
    message.Set("Password", password);
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
        }),
        messagesReceived[1]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
        }),
        messagesReceived[2]
    );
    EXPECT_EQ(
        (std::vector< std::string >{
        }),
        diagnosticMessages
    );
}

TEST_F(ChatRoomPluginTests, ChangeNickNameLurkerToLurker) {
    // Change nickname to "" (which it already was).
    Json::Json message(Json::Json::Type::Object);
    message.Set("Type", "SetNickName");
    message.Set("NickName", "");
    ws[0].SendText(message.ToEncoding());
    Json::Json expectedResponse(Json::Json::Type::Object);
    expectedResponse.Set("Type", "SetNickNameResult");
    expectedResponse.Set("Success", true);
    EXPECT_EQ(
        (std::vector< Json::Json >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
        }),
        messagesReceived[1]
    );
    EXPECT_EQ(
        (std::vector< Json::Json >{
        }),
        messagesReceived[2]
    );
    EXPECT_EQ(
        (std::vector< std::string >{
        }),
        diagnosticMessages
    );
}
