/**
 * @file EchoPluginTests.cpp
 *
 * This module contains the unit tests of the
 * Echo web-server plugin.
 *
 * © 2018 by Richard Walters
 */

#include <gtest/gtest.h>
#include <regex>
#include <stdio.h>
#include <string>
#include <SystemAbstractions/File.hpp>
#include <SystemAbstractions/StringExtensions.hpp>
#include <vector>
#include <WebServer/PluginEntryPoint.hpp>

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
    const std::string ECHO_PATH = "/echo";

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

        virtual std::shared_ptr< Http::TimeKeeper > GetTimeKeeper() override {
            return timeKeeper;
        }
    };

}

/**
 * This is the test fixture for these tests, providing common
 * setup and teardown for each test.
 */
struct EchoPluginTests
    : public ::testing::Test
{
    // Properties

    /**
     * This is used to simulate the web server hosting the plug-in.
     */
    MockServer server;

    /**
     * This is the function to call to unload the plug-in.
     */
    std::function< void() > unloadDelegate;

    /**
     * These are the diagnostic messages that have been
     * received from the unit under test.
     */
    std::vector< std::string > diagnosticMessages;

    // Methods

    // ::testing::Test

    virtual void SetUp() {
        const auto config = Json::JsonObject({
            {"space", ECHO_PATH},
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
    }

    virtual void TearDown() {
        unloadDelegate();
    }
};

TEST_F(EchoPluginTests, Load) {
    ASSERT_FALSE(unloadDelegate == nullptr);
    ASSERT_FALSE(server.registeredResourceDelegate == nullptr);
    ASSERT_EQ(
        (std::vector< std::string >{
            "echo",
        }),
        server.registeredResourceSubspacePath
    );
}

TEST_F(EchoPluginTests, EchoHeaders) {
    Http::Request request;
    (void)request.target.ParseFromString("/echo");
    request.headers.SetHeader("X-Foo", "Bar");
    request.headers.SetHeader("X-Hello", "World");
    const auto response = server.registeredResourceDelegate(request, nullptr, "");
    EXPECT_EQ(200, response.statusCode);
    static std::regex pattern("<td>(.*?)</td><td>(.*?)</td>");
    EXPECT_EQ("text/html", response.headers.GetHeaderValue("Content-Type"));
    std::vector< std::pair< std::string, std::string> > rows;
    for (
        auto it = std::sregex_iterator(response.body.begin(), response.body.end(), pattern);
        it != std::sregex_iterator();
        ++it
    ) {
        const auto match = *it;
        rows.push_back({match[1], match[2]});
    }
    EXPECT_EQ(
        (std::vector< std::pair< std::string, std::string> >{
            {"X-Foo", "Bar"},
            {"X-Hello", "World"},
        }),
        rows
    );
}

TEST_F(EchoPluginTests, ProperHeadersTableHeaderRow) {
    Http::Request request;
    (void)request.target.ParseFromString("/echo");
    request.headers.SetHeader("X-Foo", "Bar");
    request.headers.SetHeader("X-Hello", "World");
    const auto response = server.registeredResourceDelegate(request, nullptr, "");
    static std::regex pattern("<table><thead><tr><th>Header</th><th>Value</th></tr></thead>");
    EXPECT_TRUE(std::regex_search(response.body, pattern));
}
