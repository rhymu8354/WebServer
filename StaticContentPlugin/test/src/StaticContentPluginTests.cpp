/**
 * @file StaticContentPluginTests.cpp
 *
 * This module contains the unit tests of the
 * Static Content web-server plugin.
 *
 * © 2018 by Richard Walters
 */

#include <gtest/gtest.h>
#include <stdio.h>
#include <SystemAbstractions/File.hpp>
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
 * This is the test fixture for these tests, providing common
 * setup and teardown for each test.
 */
struct StaticContentPluginTests
    : public ::testing::Test
{
    // Properties

    /**
     * This is the temporary directory to use to test
     * the File class.
     */
    std::string testAreaPath;

    // Methods

    // ::testing::Test

    virtual void SetUp() {
        testAreaPath = SystemAbstractions::File::GetExeParentDirectory() + "/TestArea";
        ASSERT_TRUE(SystemAbstractions::File::CreateDirectory(testAreaPath));
    }

    virtual void TearDown() {
        ASSERT_TRUE(SystemAbstractions::File::DeleteDirectory(testAreaPath));
    }
};

TEST_F(StaticContentPluginTests, Load) {
    MockServer server;
    std::function< void() > unloadDelegate;
    Json::Json config(Json::Json::Type::Object);
    config.Set("space", "/");
    config.Set("root", testAreaPath);
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
    ASSERT_FALSE(unloadDelegate == nullptr);
    ASSERT_FALSE(server.registeredResourceDelegate == nullptr);
}

TEST_F(StaticContentPluginTests, ServeTestFile) {
    SystemAbstractions::File testFile(testAreaPath + "/foo.txt");
    (void)testFile.Create();
    (void)testFile.Write("Hello!", 6);
    testFile.Close();
    MockServer server;
    std::function< void() > unloadDelegate;
    Json::Json config(Json::Json::Type::Object);
    config.Set("space", "/");
    config.Set("root", testAreaPath);
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
    const auto request = std::make_shared< Http::Request >();
    request->target.SetPath({"foo.txt"});
    const auto response = server.registeredResourceDelegate(request, nullptr);
    ASSERT_EQ("Hello!", response->body);
}

TEST_F(StaticContentPluginTests, ConditionalGetWithMatchingEntityTagHitsCache) {
    // Create test file.
    SystemAbstractions::File testFile(testAreaPath + "/foo.txt");
    (void)testFile.Create();
    (void)testFile.Write("Hello!", 6);
    testFile.Close();

    // Configure plug-in.
    MockServer server;
    std::function< void() > unloadDelegate;
    Json::Json config(Json::Json::Type::Object);
    config.Set("space", "/");
    config.Set("root", testAreaPath);
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

    // Send initial request to get the entity tag
    // of the test file.
    auto request = std::make_shared< Http::Request >();
    request->target.SetPath({"foo.txt"});
    auto response = server.registeredResourceDelegate(request, nullptr);
    ASSERT_EQ(200, response->statusCode);
    ASSERT_TRUE(response->headers.HasHeader("ETag"));
    const auto etag = response->headers.GetHeaderValue("ETag");

    // Send second conditional request for the test
    // file, this time expecting "304 Not Modified" response.
    request = std::make_shared< Http::Request >();
    request->target.SetPath({"foo.txt"});
    request->headers.SetHeader("If-None-Match", etag);
    response = server.registeredResourceDelegate(request, nullptr);
    EXPECT_EQ(304, response->statusCode);
    EXPECT_EQ("Not Modified", response->reasonPhrase);
    EXPECT_TRUE(response->body.empty());
}
