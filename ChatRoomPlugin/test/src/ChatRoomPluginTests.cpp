/**
 * @file ChatRoomPluginTests.cpp
 *
 * This module contains the unit tests of the
 * Chat Room web-server plugin.
 *
 * © 2018-2019 by Richard Walters
 */

#include <condition_variable>
#include <gtest/gtest.h>
#include <Json/Value.hpp>
#include <mutex>
#include <stdio.h>
#include <string>
#include <StringExtensions/StringExtensions.hpp>
#include <SystemAbstractions/File.hpp>
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
    Json::Value configuration,
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate,
    std::function< void() >& unloadDelegate
);

/*
 * These are back doors into the unit under test,
 * used to set up the conditions for the test.
 */
extern API std::vector< int > GetNextQuestionComponents();
extern API std::string GetNextQuestion();
extern API std::string GetNextAnswer();
extern API void SetNextAnswer(const std::string&);
extern API void SetAnsweredCorrectly();
extern API void AwaitNextQuestion();

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
     * This is a fake time-keeper which is used to test the server.
     */
    struct MockTimeKeeper
        : public Http::TimeKeeper
    {
        // Properties

        double currentTime = 0.0;

        // Methods

        // Http::TimeKeeper

        virtual double GetCurrentTime() override {
            return currentTime;
        }
    };

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

        /**
         * This is the time keeper used in the tests to simulate
         * the progress of time.
         */
        std::shared_ptr< MockTimeKeeper > timeKeeper = std::make_shared< MockTimeKeeper >();

        // Methods

        // IServer
    public:
        virtual SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate SubscribeToDiagnostics(
            SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
            size_t minLevel = 0
        ) override {
            return []{};
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

        virtual UnregistrationDelegate RegisterBanDelegate(
            BanDelegate banDelegate
        ) override {
            return []{};
        }

        virtual std::shared_ptr< Http::TimeKeeper > GetTimeKeeper() override {
            return timeKeeper;
        }

        virtual void Ban(
            const std::string& peerAddress,
            const std::string& reason
        ) override {
        }

        virtual void Unban(const std::string& peerAddress) override {
        }

        virtual std::set< std::string > GetBans() override {
            return {};
        }

        virtual void WhitelistAdd(const std::string& peerAddress) override {
        }

        virtual void WhitelistRemove(const std::string& peerAddress) override {
        }

        virtual std::set< std::string > GetWhitelist() override {
            return {};
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
         * This is the address to report for the peer of the connection.
         */
        std::string peerAddress;

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
        explicit MockConnection(
            const std::string& peerAddress,
            const std::string& peerId
        )
            : peerAddress(peerAddress)
            , peerId(peerId)
        {
        }

        void ReceiveData(const std::vector< uint8_t >& data) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            dataReceivedDelegate(data);
        }

        // Http::Connection

        virtual std::string GetPeerAddress() override {
            return peerAddress;
        }

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
    std::vector< Json::Value > messagesReceived[NUM_MOCK_CLIENTS];

    /**
     * These are the diagnostic messages that have been
     * received from the unit under test.
     */
    std::vector< std::string > diagnosticMessages;

    /**
     * This is the pool of nicknames from which the chat room allows
     * users to pick.
     */
    Json::Value availableNicknames;

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
            StringExtensions::sprintf(
                "mock-client-%zu",
                i
            ),
            StringExtensions::sprintf(
                "mock-client-%zu:7777",
                i
            )
        );
        serverConnection[i] = std::make_shared< MockConnection >(
            StringExtensions::sprintf(
                "mock-server-%zu",
                i
            ),
            StringExtensions::sprintf(
                "mock-server-%zu:5555",
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
        WebSockets::WebSocket::Delegates wsDelegates;
        wsDelegates.text = [this, i](const std::string& data){
            std::lock_guard< decltype(mutex) > lock(mutex);
            messagesReceived[i].push_back(Json::Value::FromEncoding(data));
            waitCondition.notify_all();
        };
        wsDelegates.close = [this, i](
            unsigned int code,
            const std::string& reason
        ){
            std::lock_guard< decltype(mutex) > lock(mutex);
            wsClosed[i] = true;
            waitCondition.notify_all();
        };
        ws[i].SetDelegates(std::move(wsDelegates));
    }

    // ::testing::Test

    virtual void SetUp() {
        availableNicknames = Json::Array({
            "Alice",
            "Bob",
            "BobaFett",
            "Carol",
            "Dan",
            "DarthVader",
            "HanSolo",
            "PePe",
        });
        const auto config = Json::Object({
            {"space", CHAT_ROOM_PATH},
            {"nicknames", availableNicknames},
            {"tellTimeout", 1.0},
            {
                "mathQuiz",
                Json::Object({
                    {"minCoolDown", 10.0},
                    {"maxCoolDown", 10.0},
                })
            },
            {
                "initialPoints",
                Json::Object({
                    {"Bob", 5},
                })
            },
        });
        LoadPlugin(
            &server,
            config,
            [this](
                std::string senderName,
                size_t level,
                std::string message
            ){
                diagnosticMessages.push_back(
                    StringExtensions::sprintf(
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
            Http::Request openRequest;
            openRequest.method = "GET";
            (void)openRequest.target.ParseFromString("/chat");
            ws[i].StartOpenAsClient(openRequest);
            const auto openResponse = server.registeredResourceDelegate(openRequest, serverConnection[i], "");
            ASSERT_TRUE(ws[i].FinishOpenAsClient(clientConnection[i], openResponse));
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

TEST_F(ChatRoomPluginTests, GetAvailableNickNames) {
    const auto message = Json::Object({
        {"Type", "GetAvailableNickNames"},
    });
    ws[0].SendText(message.ToEncoding());
    const auto expectedResponse = Json::Object({
        {"Type", "AvailableNickNames"},
        {"AvailableNickNames", availableNicknames},
        {"Time", 0.0},
    });
    ASSERT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
}

TEST_F(ChatRoomPluginTests, SetNickName) {
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "FeelsBadMan"},
    });
    ws[0].SendText(message.ToEncoding());
    message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());
    auto expectedResponse = Json::Object({
        {"Type", "SetNickNameResult"},
        {"Success", false},
        {"Time", 0.0},
    });
    auto expectedResponse2 = Json::Object({
        {"Type", "Join"},
        {"NickName", "Bob"},
        {"Time", 0.0},
    });
    auto expectedResponse3 = Json::Object({
        {"Type", "SetNickNameResult"},
        {"Success", true},
        {"Time", 0.0},
    });
    ASSERT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
            expectedResponse2,
            expectedResponse3,
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
    message = Json::Object({
        {"Type", "GetNickNames"},
    });
    ws[0].SendText(message.ToEncoding());
    expectedResponse = Json::Object({
        {"Type", "NickNames"},
        {"NickNames", Json::Array({"Bob"})},
        {"Time", 0.0},
    });
    ASSERT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();
}

TEST_F(ChatRoomPluginTests, SetNickNameTwice) {
    // Set nickname for first client.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());
    auto expectedResponse = Json::Object({
        {"Type", "Join"},
        {"NickName", "Bob"},
        {"Time", 0.0},
    });
    auto expectedResponse2 = Json::Object({
        {"Type", "SetNickNameResult"},
        {"Success", true},
        {"Time", 0.0},
    });
    ASSERT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();

    // Set nickname for second client, trying
    // to grab the same nickname as the first client.
    messagesReceived[1].clear();
    message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[1].SendText(message.ToEncoding());
    expectedResponse = Json::Object({
        {"Type", "SetNickNameResult"},
        {"Success", false},
        {"Time", 0.0},
    });
    ASSERT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[1]
    );
}

TEST_F(ChatRoomPluginTests, RoomClearedAfterUnload) {
    // Have Bob join the chat room.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());
    auto expectedResponse = Json::Object({
        {"Type", "Join"},
        {"NickName", "Bob"},
        {"Time", 0.0},
    });
    auto expectedResponse2 = Json::Object({
        {"Type", "SetNickNameResult"},
        {"Success", true},
        {"Time", 0.0},
    });
    ASSERT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();

    // Reload the chat room.
    TearDown();
    SetUp();

    // Verify the assigned nickname list is empty.
    message = Json::Object({
        {"Type", "GetNickNames"},
    });
    ws[0].SendText(message.ToEncoding());
    expectedResponse = Json::Object({
        {"Type", "NickNames"},
        {"NickNames", Json::Array({})},
        {"Time", 0.0},
    });
    ASSERT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();

    // Verify the available nickname list is full.
    message = Json::Object({
        {"Type", "GetAvailableNickNames"},
    });
    ws[0].SendText(message.ToEncoding());
    expectedResponse = Json::Object({
        {"Type", "AvailableNickNames"},
        {"AvailableNickNames", availableNicknames},
        {"Time", 0.0},
    });
    ASSERT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();
}

TEST_F(ChatRoomPluginTests, TellFromNonLurker) {
    // Bob joins the room.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());

    // Alice joins the room.
    message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Alice"},
    });
    ws[1].SendText(message.ToEncoding());

    // Bob peeks at the chat room member list.
    messagesReceived[0].clear();
    message = Json::Object({
        {"Type", "GetNickNames"},
    });
    ws[0].SendText(message.ToEncoding());
    auto expectedResponse = Json::Object({
        {"Type", "NickNames"},
        {"NickNames", Json::Array({"Alice", "Bob"})},
        {"Time", 0.0},
    });
    ASSERT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[0]
    );

    // Alice says something at 2.5 seconds.
    server.timeKeeper->currentTime = 2.5;
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    message = Json::Object({
        {"Type", "Tell"},
        {"Tell", "142"},
    });
    ws[1].SendText(message.ToEncoding());
    expectedResponse = Json::Object({
        {"Type", "Tell"},
        {"Sender", "Alice"},
        {"Tell", "142"},
        {"Time", 2.5},
    });
    EXPECT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();
    EXPECT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[1]
    );
    messagesReceived[1].clear();

    // Bob says something (but it's empty).
    message = Json::Object({
        {"Type", "Tell"},
        {"Tell", ""},
    });
    ws[0].SendText(message.ToEncoding());
    EXPECT_TRUE(messagesReceived[0].empty());
    EXPECT_TRUE(messagesReceived[1].empty());

    // Bob says something (but it's not a number).
    message = Json::Object({
        {"Type", "Tell"},
        {"Tell", "HeyGuys"},
    });
    ws[0].SendText(message.ToEncoding());
    EXPECT_TRUE(messagesReceived[0].empty());
    EXPECT_TRUE(messagesReceived[1].empty());
}

TEST_F(ChatRoomPluginTests, Join) {
    // Bob joins the room.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());

    // Alice joins the room.  Expect both Alice and Bob to see a message
    // about Alice joining.
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Alice"},
    });
    ws[1].SendText(message.ToEncoding());
    auto expectedResponse = Json::Object({
        {"Type", "Join"},
        {"NickName", "Alice"},
        {"Time", 0.0},
    });
    EXPECT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();
    auto expectedResponse2 = Json::Object({
        {"Type", "SetNickNameResult"},
        {"Success", true},
        {"Time", 0.0},
    });
    ASSERT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
            expectedResponse2,
        }),
        messagesReceived[1]
    );
    messagesReceived[1].clear();
}

TEST_F(ChatRoomPluginTests, Leave) {
    // Bob joins the room.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());
    messagesReceived[0].clear();

    // Alice joins the room.
    messagesReceived[1].clear();
    message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Alice"},
    });
    ws[1].SendText(message.ToEncoding());

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
        "Session #2[1]: Connection to mock-server-1:5555 closed by peer",
        diagnosticMessages[0]
    );

    // Bob peeks at the chat room member list.
    message = Json::Object({
        {"Type", "GetNickNames"},
    });
    ws[0].SendText(message.ToEncoding());
    ASSERT_EQ(
        (std::vector< Json::Value >{
            Json::Object({
                {"Type", "Leave"},
                {"NickName", "Alice"},
                {"Time", 0.0},
            }),
            Json::Object({
                {"Type", "NickNames"},
                {"NickNames", Json::Array({"Bob"})},
                {"Time", 0.0},
            }),
        }),
        messagesReceived[0]
    );
    messagesReceived[0].clear();
}

TEST_F(ChatRoomPluginTests, SetNickNameInTrailer) {
    // Reopen a WebSocket connection with part of a
    // "SetNickName" message captured in the trailer.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
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
    Http::Request openRequest;
    openRequest.method = "GET";
    (void)openRequest.target.ParseFromString("/chat");
    ws[0] = WebSockets::WebSocket();
    InitilizeClientWebSocket(0);
    ws[0].StartOpenAsClient(openRequest);
    const auto openResponse = server.registeredResourceDelegate(openRequest, serverConnection[0], frameFirstHalf);
    EXPECT_FALSE(ws[0].FinishOpenAsClient(clientConnection[0], openResponse));
    EXPECT_EQ(400, openResponse.statusCode);
}

TEST_F(ChatRoomPluginTests, ConnectionNotUpgraded) {
    const auto connection = std::make_shared< MockConnection >("mock-client", "mock-client:5555");
    std::string responseText;
    connection->sendDataDelegate = [&responseText](
        const std::vector< uint8_t >& data
    ){
        responseText += std::string(
            data.begin(),
            data.end()
        );
    };
    Http::Request request;
    request.method = "GET";
    (void)request.target.ParseFromString("");
    const auto response = server.registeredResourceDelegate(request, connection, "");
    ASSERT_EQ(200, response.statusCode);
    ASSERT_EQ("text/plain", response.headers.GetHeaderValue("Content-Type"));
    ASSERT_EQ("Try again, but next time use a WebSocket.  Kthxbye!", response.body);
}

TEST_F(ChatRoomPluginTests, ChangeNickNameSingleConnectionNonLurkerToNonLurkerNotAlreadyInRoom) {
    // Set nickname to "Bob" initially on one connection.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());

    // Change nickname to "PePe".
    diagnosticMessages.clear();
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    messagesReceived[2].clear();
    message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "PePe"},
    });
    ws[0].SendText(message.ToEncoding());
    auto expectedResponse1 = Json::Object({
        {"Type", "Leave"},
        {"NickName", "Bob"},
        {"Time", 0.0},
    });
    auto expectedResponse2 = Json::Object({
        {"Type", "Join"},
        {"NickName", "PePe"},
        {"Time", 0.0},
    });
    auto expectedResponse3 = Json::Object({
        {"Type", "SetNickNameResult"},
        {"Success", true},
        {"Time", 0.0},
    });
    EXPECT_EQ(
        (std::vector< Json::Value >{
            expectedResponse1,
            expectedResponse2,
            expectedResponse3,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Value >{
            expectedResponse1,
            expectedResponse2,
        }),
        messagesReceived[1]
    );
    EXPECT_EQ(
        (std::vector< Json::Value >{
            expectedResponse1,
            expectedResponse2,
        }),
        messagesReceived[2]
    );
    EXPECT_EQ(
        (std::vector< std::string >{
            "Session #1[1]: Nickname changed from 'Bob' to 'PePe'",
        }),
        diagnosticMessages
    );

    // Verify nickname of "Bob" really did change to "PePe".
    messagesReceived[1].clear();
    message = Json::Object({
        {"Type", "Tell"},
        {"Tell", "142"},
    });
    ws[0].SendText(message.ToEncoding());
    EXPECT_EQ(
        (std::vector< Json::Value >{
            Json::Object({
                {"Type", "Tell"},
                {"Sender", "PePe"},
                {"Tell", "142"},
                {"Time", 0.0},
            }),
        }),
        messagesReceived[1]
    );

    // Verify Bob is now an available nickname, but PePe isn't.
    auto nicknames = Json::Value(Json::Value::Type::Array);
    for (size_t i = 0; i < availableNicknames.GetSize(); ++i) {
        if (availableNicknames[i] != "PePe") {
            nicknames.Add(availableNicknames[i]);
        }
    }
    messagesReceived[1].clear();
    message = Json::Object({
        {"Type", "GetAvailableNickNames"},
    });
    ws[1].SendText(message.ToEncoding());
    EXPECT_EQ(
        (std::vector< Json::Value >{
            Json::Object({
                {"Type", "AvailableNickNames"},
                {"AvailableNickNames", nicknames},
                {"Time", 0.0},
            }),
        }),
        messagesReceived[1]
    );
}

TEST_F(ChatRoomPluginTests, ChangeNickNameOneConnectionNonLurkerToLurker) {
    // Set nickname to "Bob" initially on one connection.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());

    // Change nickname to "" (become a lurker).
    diagnosticMessages.clear();
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    messagesReceived[2].clear();
    message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", ""},
    });
    ws[0].SendText(message.ToEncoding());
    auto expectedResponse = Json::Object({
        {"Type", "SetNickNameResult"},
        {"Success", true},
        {"Time", 0.0},
    });
    auto expectedResponse2 = Json::Object({
        {"Type", "Leave"},
        {"NickName", "Bob"},
        {"Time", 0.0},
    });
    EXPECT_EQ(
        (std::vector< Json::Value >{
            expectedResponse2,
            expectedResponse,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Value >{
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
    //
    // Note that we expect to timeout here, because we need to make
    // sure a message is NOT received by the second client, but if
    // a message IS received, it's done in a separate thread which
    // we may end up racing if we don't wait.
    messagesReceived[1].clear();
    diagnosticMessages.clear();
    ws[0].Close();
    {
        std::unique_lock< decltype(mutex) > lock(mutex);
        EXPECT_FALSE(
            waitCondition.wait_for(
                lock,
                std::chrono::seconds(1),
                [this]{ return wsClosed[0] && !messagesReceived[1].empty(); }
            )
        );
    }
    EXPECT_EQ(
        (std::vector< Json::Value >{
        }),
        messagesReceived[1]
    );

    // Verify Bob is now an available nickname.
    messagesReceived[1].clear();
    message = Json::Object({
        {"Type", "GetAvailableNickNames"},
    });
    ws[1].SendText(message.ToEncoding());
    EXPECT_EQ(
        (std::vector< Json::Value >{
            Json::Object({
                {"Type", "AvailableNickNames"},
                {"AvailableNickNames", availableNicknames},
                {"Time", 0.0},
            }),
        }),
        messagesReceived[1]
    );
}

TEST_F(ChatRoomPluginTests, ChangeNickNameLurkerToLurker) {
    // Change nickname to "" (which it already was).
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", ""},
    });
    ws[0].SendText(message.ToEncoding());
    EXPECT_EQ(
        (std::vector< Json::Value >{
            Json::Object({
                {"Type", "SetNickNameResult"},
                {"Success", true},
                {"Time", 0.0},
            }),
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Value >{
        }),
        messagesReceived[1]
    );
    EXPECT_EQ(
        (std::vector< Json::Value >{
        }),
        messagesReceived[2]
    );
    EXPECT_EQ(
        (std::vector< std::string >{
        }),
        diagnosticMessages
    );
}

TEST_F(ChatRoomPluginTests, TellFromLurker) {
    auto message = Json::Object({
        {"Type", "Tell"},
        {"Tell", "142"},
    });
    ws[0].SendText(message.ToEncoding());
    EXPECT_EQ(
        (std::vector< Json::Value >{
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Value >{
        }),
        messagesReceived[1]
    );
}

TEST_F(ChatRoomPluginTests, TwoTellsTooQuickly) {
    // Register as "Bob" and then say something.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    message = Json::Object({
        {"Type", "Tell"},
        {"Tell", "142"},
    });
    ws[0].SendText(message.ToEncoding());
    auto expectedResponse = Json::Object({
        {"Type", "Tell"},
        {"Sender", "Bob"},
        {"Tell", "142"},
        {"Time", 0.0},
    });
    EXPECT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[1]
    );

    // Advance time one half second, which is
    // half the tell cool-down time.
    server.timeKeeper->currentTime += 0.5;

    // Say something else, and expect it to be ignored.
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    ws[0].SendText(message.ToEncoding());
    EXPECT_EQ(
        (std::vector< Json::Value >{
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Value >{
        }),
        messagesReceived[1]
    );

    // Advance time another half second, which will take
    // Bob's tell off cool-down.
    server.timeKeeper->currentTime += 0.5;

    // Say something else, and expect it to be sent.
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    ws[0].SendText(message.ToEncoding());
    expectedResponse = Json::Object({
        {"Type", "Tell"},
        {"Sender", "Bob"},
        {"Tell", "142"},
        {"Time", 1.0},
    });
    EXPECT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[0]
    );
    EXPECT_EQ(
        (std::vector< Json::Value >{
            expectedResponse,
        }),
        messagesReceived[1]
    );
}

TEST_F(ChatRoomPluginTests, GetUsers) {
    // Bob and Alice join the room.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());
    message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Alice"},
    });
    ws[1].SendText(message.ToEncoding());

    // Get the user list and verify point totals
    // are as expected.
    messagesReceived[0].clear();
    message = Json::Object({
        {"Type", "GetUsers"},
    });
    ws[0].SendText(message.ToEncoding());
    ASSERT_EQ(
        (std::vector< Json::Value >{
            Json::Object({
                {"Type", "Users"},
                {"Users", Json::Array({
                    Json::Object({
                        {"Nickname", "Bob"},
                        {"Points", 5},
                    }),
                    Json::Object({
                        {"Nickname", "Alice"},
                        {"Points", 0},
                    }),
                })},
                {"Time", 0.0},
            }),
        }),
        messagesReceived[0]
    );
}

TEST_F(ChatRoomPluginTests, FirstAnswerScores) {
    // Bob and Alice join the room.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());
    message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Alice"},
    });
    ws[1].SendText(message.ToEncoding());

    // Post the next challenge in the room.
    SetNextAnswer("42");

    // Lurker tries to answer the current question.
    message = Json::Object({
        {"Type", "Tell"},
        {"Tell", "42"},
    });
    ws[2].SendText(message.ToEncoding());

    // Bob answers the current question.
    server.timeKeeper->currentTime = 1.5;
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    ws[0].SendText(message.ToEncoding());

    // Alice answers the current question, but a little too late.
    server.timeKeeper->currentTime = 1.6;
    ws[1].SendText(message.ToEncoding());

    // Expect both tells, but only Bob awarded a point.
    EXPECT_EQ(
        (std::vector< Json::Value >{
            Json::Object({
                {"Type", "Tell"},
                {"Sender", "Bob"},
                {"Tell", "42"},
                {"Time", 1.5},
            }),
            Json::Object({
                {"Type", "Award"},
                {"Subject", "Bob"},
                {"Award", 1},
                {"Points", 6},
                {"Time", 1.5},
            }),
            Json::Object({
                {"Type", "Tell"},
                {"Sender", "Alice"},
                {"Tell", "42"},
                {"Time", 1.6},
            }),
        }),
        messagesReceived[0]
    );

    // Get the user list and verify point totals
    // are as expected.
    messagesReceived[0].clear();
    message = Json::Object({
        {"Type", "GetUsers"},
    });
    ws[0].SendText(message.ToEncoding());
    ASSERT_EQ(
        (std::vector< Json::Value >{
            Json::Object({
                {"Type", "Users"},
                {"Users", Json::Array({
                    Json::Object({
                        {"Nickname", "Bob"},
                        {"Points", 6},
                    }),
                    Json::Object({
                        {"Nickname", "Alice"},
                        {"Points", 0},
                    }),
                })},
                {"Time", 1.6},
            }),
        }),
        messagesReceived[0]
    );
}

TEST_F(ChatRoomPluginTests, IncorrectAnswersPenalizedBeforeCorrectAnswer) {
    // Bob and Alice join the room.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());
    message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Alice"},
    });
    ws[1].SendText(message.ToEncoding());

    // Post the next challenge in the room.
    SetNextAnswer("42");

    // Bob answers the current question incorrectly.
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    message = Json::Object({
        {"Type", "Tell"},
        {"Tell", "41"},
    });
    ws[0].SendText(message.ToEncoding());

    // Alice answers the current question correctly.
    server.timeKeeper->currentTime = 1.0;
    message = Json::Object({
        {"Type", "Tell"},
        {"Tell", "42"},
    });
    ws[1].SendText(message.ToEncoding());

    // Bob answers the last question correctly, but a little too late.
    server.timeKeeper->currentTime = 1.1;
    message = Json::Object({
        {"Type", "Tell"},
        {"Tell", "42"},
    });
    ws[0].SendText(message.ToEncoding());

    // Expect both tells, but only Bob awarded a point.
    EXPECT_EQ(
        (std::vector< Json::Value >{
            Json::Object({
                {"Type", "Tell"},
                {"Sender", "Bob"},
                {"Tell", "41"},
                {"Time", 0.0},
            }),
            Json::Object({
                {"Type", "Penalty"},
                {"Subject", "Bob"},
                {"Penalty", 1},
                {"Points", 4},
                {"Time", 0.0},
            }),
            Json::Object({
                {"Type", "Tell"},
                {"Sender", "Alice"},
                {"Tell", "42"},
                {"Time", 1.0},
            }),
            Json::Object({
                {"Type", "Award"},
                {"Subject", "Alice"},
                {"Award", 1},
                {"Points", 1},
                {"Time", 1.0},
            }),
            Json::Object({
                {"Type", "Tell"},
                {"Sender", "Bob"},
                {"Tell", "42"},
                {"Time", 1.1},
            }),
        }),
        messagesReceived[0]
    );

    // Get the user list and verify point totals
    // are as expected.
    messagesReceived[0].clear();
    message = Json::Object({
        {"Type", "GetUsers"},
    });
    ws[0].SendText(message.ToEncoding());
    ASSERT_EQ(
        (std::vector< Json::Value >{
            Json::Object({
                {"Type", "Users"},
                {"Users", Json::Array({
                    Json::Object({
                        {"Nickname", "Bob"},
                        {"Points", 4},
                    }),
                    Json::Object({
                        {"Nickname", "Alice"},
                        {"Points", 1},
                    }),
                })},
                {"Time", 1.1},
            }),
        }),
        messagesReceived[0]
    );
}

TEST_F(ChatRoomPluginTests, MathQuestionPostedWhenNotOnCooldown) {
    // Bob joins the room.
    auto message = Json::Object({
        {"Type", "SetNickName"},
        {"NickName", "Bob"},
    });
    ws[0].SendText(message.ToEncoding());

    // Advance the time so that the next math question will
    // have been posted.
    messagesReceived[0].clear();
    messagesReceived[1].clear();
    server.timeKeeper->currentTime = 10.0;
    AwaitNextQuestion();
    server.timeKeeper->currentTime = 11.0;

    // Bob answers the current question.
    const auto question = GetNextQuestion();
    const auto answer = GetNextAnswer();
    message = Json::Object({
        {"Type", "Tell"},
        {"Tell", answer},
    });
    ws[0].SendText(message.ToEncoding());
    EXPECT_EQ(
        (std::vector< Json::Value >{
            Json::Object({
                {"Type", "Tell"},
                {"Sender", "MathBot2000"},
                {"Tell", question},
                {"Time", 10.0},
            }),
            Json::Object({
                {"Type", "Tell"},
                {"Sender", "Bob"},
                {"Tell", answer},
                {"Time", 11.0},
            }),
            Json::Object({
                {"Type", "Award"},
                {"Subject", "Bob"},
                {"Award", 1},
                {"Points", 6},
                {"Time", 11.0},
            }),
        }),
        messagesReceived[0]
    );
}

TEST_F(ChatRoomPluginTests, DifferentAnswersEachMathQuestion) {
    std::string lastQuestion, lastAnswer;
    for (size_t i = 0; i < 10; ++i) {
        server.timeKeeper->currentTime = (i + 1) * 10.0 + 1.0;
        AwaitNextQuestion();
        const auto questionComponents = GetNextQuestionComponents();
        const auto question = GetNextQuestion();
        const auto answer = GetNextAnswer();
        ASSERT_EQ(3, questionComponents.size());
        ASSERT_EQ(
            StringExtensions::sprintf(
                "What is %d * %d + %d?",
                questionComponents[0],
                questionComponents[1],
                questionComponents[2]
            ),
            question
        );
        ASSERT_NE(lastQuestion, question) << i;
        ASSERT_NE(lastAnswer, answer);
        lastQuestion = question;
        lastAnswer = answer;
        SetAnsweredCorrectly();
    }
}
