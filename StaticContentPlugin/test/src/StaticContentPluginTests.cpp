/**
 * @file StaticContentPluginTests.cpp
 *
 * This module contains the unit tests of the
 * Static Content web-server plugin.
 *
 * © 2018 by Richard Walters
 */

#include <gtest/gtest.h>
#include <map>
#include <Sha1/Sha1.hpp>
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

namespace {

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
         * These are the delegates that the unit under test has registered
         * to be called to handle resource requests.  They are keyed
         * by the first element of the registered path.
         */
        std::map< std::string, ResourceDelegate > registeredResourceDelegates;

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
            if (resourceSubspacePath.size() > 0) {
                registeredResourceDelegates[resourceSubspacePath[0]] = resourceDelegate;
            }
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
    Http::Request request;
    request.target.SetPath({"foo.txt"});
    const auto response = server.registeredResourceDelegate(request, nullptr, "");
    ASSERT_EQ("Hello!", response.body);
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
    Http::Request request;
    request.target.SetPath({"foo.txt"});
    auto response = server.registeredResourceDelegate(request, nullptr, "");
    ASSERT_EQ(200, response.statusCode);
    ASSERT_TRUE(response.headers.HasHeader("ETag"));
    const auto etag = response.headers.GetHeaderValue("ETag");

    // Send second conditional request for the test
    // file, this time expecting "304 Not Modified" response.
    request = Http::Request();
    request.target.SetPath({"foo.txt"});
    request.headers.SetHeader("If-None-Match", etag);
    response = server.registeredResourceDelegate(request, nullptr, "");
    EXPECT_EQ(304, response.statusCode);
    EXPECT_EQ("Not Modified", response.reasonPhrase);
    EXPECT_TRUE(response.body.empty());
}

TEST_F(StaticContentPluginTests, EntityTagComputedFromSha1) {
    // Create test file.
    SystemAbstractions::File testFile(testAreaPath + "/foo.txt");
    (void)testFile.Create();
    const std::string testFileContent = "Hello!";
    (void)testFile.Write(testFileContent.data(), testFileContent.length());
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

    // Send request to get the entity tag
    // of the test file.
    Http::Request request;
    request.target.SetPath({"foo.txt"});
    auto response = server.registeredResourceDelegate(request, nullptr, "");
    ASSERT_EQ(200, response.statusCode);
    ASSERT_TRUE(response.headers.HasHeader("ETag"));
    const auto actualEtag = response.headers.GetHeaderValue("ETag");

    // Verify entity tag was computed using SHA-1.
    const auto expectedEtag = Sha1::Sha1String(testFileContent);
    EXPECT_EQ(expectedEtag, actualEtag);
}

TEST_F(StaticContentPluginTests, ServeMultipleResourceSpaces) {
    const std::string fooTestAreaPath = testAreaPath + "/foo";
    const std::string barTestAreaPath = testAreaPath + "/bar";
    ASSERT_TRUE(SystemAbstractions::File::CreateDirectory(fooTestAreaPath));
    ASSERT_TRUE(SystemAbstractions::File::CreateDirectory(barTestAreaPath));
    SystemAbstractions::File testFile1(testAreaPath + "/foo/hello.txt");
    SystemAbstractions::File testFile2(testAreaPath + "/bar/hello.txt");
    (void)testFile1.Create();
    (void)testFile2.Create();
    (void)testFile1.Write("Hello!", 6);
    (void)testFile2.Write("World!", 6);
    testFile1.Close();
    testFile2.Close();
    MockServer server;
    std::function< void() > unloadDelegate;
    Json::Json config(Json::Json::Type::Object);
    config.Set(
        "spaces",
        Json::JsonArray(
            {
                Json::JsonObject({
                    {"space", "/foo"},
                    {"root", fooTestAreaPath},
                }),
                Json::JsonObject({
                    {"space", "/bar"},
                    {"root", barTestAreaPath},
                }),
            }
        )
    );
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
    ASSERT_FALSE(server.registeredResourceDelegates.find("foo") == server.registeredResourceDelegates.end());
    ASSERT_FALSE(server.registeredResourceDelegates.find("bar") == server.registeredResourceDelegates.end());
    Http::Request request;
    request.target.SetPath({"hello.txt"});
    auto response = server.registeredResourceDelegates["foo"](request, nullptr, "");
    EXPECT_EQ("Hello!", response.body);
    request = Http::Request();
    request.target.SetPath({"hello.txt"});
    response = server.registeredResourceDelegates["bar"](request, nullptr, "");
    EXPECT_EQ("World!", response.body);
}
