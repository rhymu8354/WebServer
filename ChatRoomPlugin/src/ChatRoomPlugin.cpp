/**
 * @file ChatRoomPlugin.cpp
 *
 * This is a plug-in for the Excalibur web server, designed
 * to demonstrate a simple chat-room type application.
 *
 * © 2018 by Richard Walters
 */

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <Http/Server.hpp>
#include <inttypes.h>
#include <Json/Json.hpp>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdlib.h>
#include <string>
#include <SystemAbstractions/StringExtensions.hpp>
#include <thread>
#include <time.h>
#include <vector>
#include <WebServer/PluginEntryPoint.hpp>
#include <WebSockets/WebSocket.hpp>

#ifdef _WIN32
#define API __declspec(dllexport)
#else /* POSIX */
#define API
#endif /* _WIN32 / POSIX */

namespace {

    /**
     * This is the number of milliseconds to wait between rounds of polling
     * in the worker thread of the chat room.
     */
    constexpr unsigned int WORKER_POLLING_PERIOD_MILLISECONDS = 50;

    /**
     * This represents one user in the chat room.
     */
    struct User {
        /**
         * This is the user's nickname.
         */
        std::string nickname;

        /**
         * This is the delegate representing the structure's subscription to
         * diagnostic messages published by the WebSocket.  When called,
         * it terminates the subscription.
         */
        SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate wsDiagnosticsUnsubscribeDelegate;

        /**
         * This is the sender name to use when publishing diagnostic messages
         * from this object.
         */
        std::string diagnosticsSenderName;

        /**
         * This is the WebSocket connection to the user.
         */
        std::shared_ptr< WebSockets::WebSocket > ws;

        /**
         * This flag indicates whether or not the WebSocket
         * connection to the user is still open.
         */
        bool open = true;

        /**
         * This is the time (according to the server's time keeper)
         * that the user last sent a tell.
         */
        double lastTell = std::numeric_limits< double >::lowest();

        /**
         * This is the user's current score.
         */
        int points = 0;
    };

    /**
     * This represents the state of the chat room.
     */
    struct Room {
        // Properties

        /**
         * This is used to synchronize access to the chat room.
         */
        std::recursive_mutex mutex;

        /**
         * This points back to the web server hosting the chat room.
         */
        Http::IServer* server = nullptr;

        /**
         * This is used by MathBot2000 to generate the math questions.
         */
        std::mt19937 generator;

        /**
         * This is the function to call to deliver diagnostic
         * messages generated by the plug-in.
         */
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate;

        /**
         * This is the amount of time, in seconds, that a user must wait
         * after sending a tell, before they'll be allowed to send their next
         * tell.
         */
        double tellTimeout = 1.0;

        /**
         * This is used to notify the worker thread about
         * any change that should cause it to wake up.
         */
        std::condition_variable_any workerWakeCondition;

        /**
         * This is used to perform housekeeping in the background.
         */
        std::thread workerThread;

        /**
         * This flag indicates whether or not the worker thread should stop.
         */
        bool stopWorker = false;

        /**
         * These are the nicknames that are currently unallocated
         * but ready for assignment to users.
         */
        std::set< std::string > availableNickNames;

        /**
         * This is used to assign the initial point score for a user
         * when they join the chat room.
         *
         * @note
         *     This is a back door used by the chat room's unit test
         *     framework, to make it easier to test the points system.
         *     Normally, users will start at zero points each time
         *     they connect.
         */
        std::map< std::string, int > initialPoints;

        /**
         * These are the users currently in the chat room,
         * keyed by session ID.
         */
        std::map< unsigned int, User > users;

        /**
         * This flag indicates whether or not there are users
         * in the chat room whose web sockets have closed.
         */
        bool usersHaveClosed = false;

        /**
         * This is the next session ID that may be assigned
         * to a new user.
         */
        unsigned int nextSessionId = 1;

        /**
         * This indicates whether or not a user has sent a tell
         * with the correct answer to the current math question.
         */
        bool answeredCorrectly = true;

        /**
         * This is the time (according to the time keeper) when
         * the next math question should be asked.
         */
        double nextQuestionTime = std::numeric_limits< double >::max();

        /**
         * This is the minimum cooldown time in seconds between
         * when two consecutive questions are asked.
         */
        double minQuestionCooldown = 10.0;

        /**
         * This is the maximum cooldown time in seconds between
         * when two consecutive questions are asked.
         */
        double maxQuestionCooldown = 30.0;

        /**
         * These are the components of the current math question.
         */
        std::vector< int > questionComponents;

        /**
         * This is the current math question.
         */
        std::string question;

        /**
         * This is the correct answer to the current math question.
         */
        std::string answer;

        /**
         * This is used to issue notifications when the current
         * math question has changed.
         */
        std::condition_variable_any answerChangedCondition;

        // Methods

        /**
         * This is called just before the chat room is connected
         * into the web server, in order to prepare it for operation.
         */
        void Start() {
            if (workerThread.joinable()) {
                return;
            }
            stopWorker = false;
            generator.seed((int)time(NULL));
            nextQuestionTime = server->GetTimeKeeper()->GetCurrentTime();
            CooldownNextQuestion();
            workerThread = std::thread(&Room::Worker, this);
        }

        /**
         * This is called just after the chat room is disconnection
         * from the web server, in order to cleanly shut it down.
         */
        void Stop() {
            if (!workerThread.joinable()) {
                return;
            }
            {
                std::lock_guard< decltype(mutex) > lock(mutex);
                stopWorker = true;
                workerWakeCondition.notify_all();
            }
            workerThread.join();
        }

        /**
         * This method sets the time the next math question will be asked.
         */
        void CooldownNextQuestion() {
            nextQuestionTime += std::uniform_real_distribution<>(
                minQuestionCooldown,
                maxQuestionCooldown
            )(generator);
        }

        /**
         * This function sends the given message to a specific user.
         *
         * @param[in] user
         *     This is the user to whom to send the message.
         *
         * @param[in] message
         *     This is the message to send to the user.
         */
        void SendToUser(
            const User& user,
            Json::Json message
        ) {
            message.Set("Time", server->GetTimeKeeper()->GetCurrentTime());
            user.ws->SendText(message.ToEncoding());
        }

        /**
         * This function sends the given text message to the given
         * set of users.
         *
         * @param[in] users
         *     These are the users to whom to send the message.
         *
         * @param[in] message
         *     This is the message to send to all users.
         */
        void SendToUsers(
            const std::map< unsigned int, User >& users,
            const Json::Json& message
        ) {
            for (auto& user: users) {
                SendToUser(user.second, message);
            }
        }

        /**
         * This function sends the given message to all users.
         *
         * @param[in] message
         *     This is the message to send to all users.
         */
        void SendToAll(const Json::Json& message) {
            SendToUsers(users, message);
        }

        /**
         * This function sends the given tell as a text message
         * to all users.
         *
         * @param[in] tell
         *     This is the tell to send as a text mesage to all users.
         *
         * @param[in] sender
         *     This is the sender of the tell.
         */
        void SendTell(
            const std::string& tell,
            const std::string& sender
        ) {
            std::unique_lock< decltype(mutex) > lock(mutex);
            const auto post = Json::JsonObject({
                {"Type", "Tell"},
                {"Sender", sender},
                {"Tell", tell},
            });
            auto usersCopy = users;
            lock.unlock();
            SendToUsers(usersCopy, post);
            lock.lock();
        }

        /**
         * This function is called in a separate thread to perform
         * housekeeping in the background for the chat room.
         */
        void Worker() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            while (!stopWorker) {
                workerWakeCondition.wait_for(
                    lock,
                    std::chrono::milliseconds(WORKER_POLLING_PERIOD_MILLISECONDS),
                    [this]{ return stopWorker || usersHaveClosed; }
                );
                if (usersHaveClosed) {
                    std::vector< User > closedUsers;
                    for (
                        auto userEntry = users.begin();
                        userEntry != users.end();
                    ) {
                        if (userEntry->second.open) {
                            ++userEntry;
                        } else {
                            const auto nickname = userEntry->second.nickname;
                            userEntry->second.wsDiagnosticsUnsubscribeDelegate();
                            closedUsers.push_back(std::move(userEntry->second));
                            userEntry = users.erase(userEntry);
                            if (!nickname.empty()) {
                                (void)availableNickNames.insert(nickname);
                                const auto response = Json::JsonObject({
                                    {"Type", "Leave"},
                                    {"NickName", nickname},
                                });
                                auto usersCopy = users;
                                lock.unlock();
                                SendToUsers(usersCopy, response);
                                lock.lock();
                            }
                        }
                    }
                    usersHaveClosed = false;
                    {
                        lock.unlock();
                        closedUsers.clear();
                        lock.lock();
                    }
                }
                const auto now = server->GetTimeKeeper()->GetCurrentTime();
                if (now >= nextQuestionTime) {
                    const auto lastAnswer = answer;
                    do {
                        questionComponents.resize(3);
                        questionComponents[0] = std::uniform_int_distribution<>(2, 10)(generator);
                        questionComponents[1] = std::uniform_int_distribution<>(2, 10)(generator);
                        questionComponents[2] = std::uniform_int_distribution<>(2, 97)(generator);
                        question = SystemAbstractions::sprintf(
                            "What is %d * %d + %d?",
                            questionComponents[0],
                            questionComponents[1],
                            questionComponents[2]
                        );
                        answer = SystemAbstractions::sprintf(
                            "%d",
                            questionComponents[0] * questionComponents[1] + questionComponents[2]
                        );
                    } while (answer == lastAnswer);
                    answeredCorrectly = false;
                    CooldownNextQuestion();
                    lock.unlock();
                    SendTell(question, "MathBot2000");
                    lock.lock();
                    answerChangedCondition.notify_all();
                }
            }
        }

        /**
         * This method handles the "SetNickName" message from
         * users in the chat room.
         *
         * @param[in] message
         *     This is the content of the user message.
         *
         * @param[in] userEntry
         *     This is the entry of the user who sent the message.
         */
        void SetNickName(
            const Json::Json& message,
            std::map< unsigned int, User >::iterator userEntry
        ) {
            const auto oldNickname = userEntry->second.nickname;
            const std::string newNickname = message["NickName"];
            const std::string password = message["Password"];
            auto setNickNameResult = Json::JsonObject({
                {"Type", "SetNickNameResult"},
            });
            if (newNickname.empty()) {
                userEntry->second.nickname.clear();
                setNickNameResult.Set("Success", true);
                if (!oldNickname.empty()) {
                    diagnosticMessageDelegate(
                        userEntry->second.diagnosticsSenderName,
                        1,
                        SystemAbstractions::sprintf(
                            "Nickname changed from '%s' to '%s'",
                            oldNickname.c_str(),
                            newNickname.c_str()
                        )
                    );
                    (void)availableNickNames.insert(oldNickname);
                    const auto response = Json::JsonObject({
                        {"Type", "Leave"},
                        {"NickName", oldNickname},
                    });
                    SendToAll(response);
                }
            } else if (oldNickname == newNickname) {
                setNickNameResult.Set("Success", true);
            } else {
                auto availableNicknameEntry = availableNickNames.find(newNickname);
                if (availableNicknameEntry == availableNickNames.end()) {
                    setNickNameResult.Set("Success", false);
                } else {
                    (void)availableNickNames.erase(availableNicknameEntry);
                    userEntry->second.nickname = newNickname;
                    userEntry->second.points = initialPoints[newNickname];
                    if (!oldNickname.empty()) {
                        (void)availableNickNames.insert(oldNickname);
                        const auto response = Json::JsonObject({
                            {"Type", "Leave"},
                            {"NickName", oldNickname},
                        });
                        SendToAll(response);
                    }
                    const auto response = Json::JsonObject({
                        {"Type", "Join"},
                        {"NickName", newNickname},
                    });
                    SendToAll(response);
                    setNickNameResult.Set("Success", true);
                    diagnosticMessageDelegate(
                        userEntry->second.diagnosticsSenderName,
                        1,
                        SystemAbstractions::sprintf(
                            "Nickname changed from '%s' to '%s'",
                            oldNickname.c_str(),
                            newNickname.c_str()
                        )
                    );
                }
            }
            SendToUser(
                userEntry->second,
                setNickNameResult
            );
        }

        /**
         * This method handles the "GetNickNames" message from
         * users in the chat room.
         *
         * @param[in] message
         *     This is the content of the user message.
         *
         * @param[in] userEntry
         *     This is the entry of the user who sent the message.
         */
        void GetNickNames(
            const Json::Json& message,
            std::map< unsigned int, User >::iterator userEntry
        ) {
            auto response = Json::JsonObject({
                {"Type", "NickNames"},
            });
            std::set< std::string > nicknameSet;
            for (const auto& user: users) {
                if (!user.second.nickname.empty()) {
                    (void)nicknameSet.insert(user.second.nickname);
                }
            }
            Json::Json nicknames(Json::Json::Type::Array);
            for (const auto& nickname: nicknameSet) {
                nicknames.Add(nickname);
            }
            response.Set("NickNames", nicknames);
            SendToUser(
                userEntry->second,
                response
            );
        }

        /**
         * This method handles the "Tell" message from
         * users in the chat room.
         *
         * @param[in] message
         *     This is the content of the user message.
         *
         * @param[in] userEntry
         *     This is the entry of the user who sent the message.
         */
        void Tell(
            const Json::Json& message,
            std::map< unsigned int, User >::iterator userEntry
        ) {
            if (userEntry->second.nickname.empty()) {
                return;
            }
            const auto now = server->GetTimeKeeper()->GetCurrentTime();
            if (now - userEntry->second.lastTell < tellTimeout) {
                return;
            }
            const std::string tell = message["Tell"];
            if (tell.empty()) {
                return;
            }
            intmax_t tellAsNumber;
            if (
                SystemAbstractions::ToInteger(tell, tellAsNumber)
                != SystemAbstractions::ToIntegerResult::Success
            ) {
                return;
            }
            userEntry->second.lastTell = now;
            SendTell(tell, userEntry->second.nickname);
            if (!answeredCorrectly) {
                if (tell == answer) {
                    answeredCorrectly = true;
                    ++userEntry->second.points;
                    const auto response = Json::JsonObject({
                        {"Type", "Award"},
                        {"Subject", userEntry->second.nickname},
                        {"Award", 1},
                        {"Points", userEntry->second.points},
                    });
                    SendToAll(response);
                } else {
                    --userEntry->second.points;
                    const auto response = Json::JsonObject({
                        {"Type", "Penalty"},
                        {"Subject", userEntry->second.nickname},
                        {"Penalty", 1},
                        {"Points", userEntry->second.points},
                    });
                    SendToAll(response);
                }
            }
        }

        /**
         * This method handles the "GetAvailableNickNames" message from
         * users in the chat room.
         *
         * @param[in] message
         *     This is the content of the user message.
         *
         * @param[in] userEntry
         *     This is the entry of the user who sent the message.
         */
        void GetAvailableNickNames(
            const Json::Json& message,
            std::map< unsigned int, User >::iterator userEntry
        ) {
            auto availableNickNamesAsJson = Json::JsonArray({});
            for (const auto& nickname: availableNickNames) {
                availableNickNamesAsJson.Add(nickname);
            }
            const auto response = Json::JsonObject({
                {"Type", "AvailableNickNames"},
                {"AvailableNickNames", availableNickNamesAsJson},
            });
            SendToAll(response);
        }

        /**
         * This method handles the "GetUsers" message from
         * users in the chat room.
         *
         * @param[in] message
         *     This is the content of the user message.
         *
         * @param[in] userEntry
         *     This is the entry of the user who sent the message.
         */
        void GetUsers(
            const Json::Json& message,
            std::map< unsigned int, User >::iterator userEntry
        ) {
            auto response = Json::JsonObject({
                {"Type", "Users"},
            });
            auto usersJson = Json::JsonArray({});
            for (const auto& user: users) {
                if (!user.second.nickname.empty()) {
                    usersJson.Add(
                        Json::JsonObject({
                            {"Nickname", user.second.nickname},
                            {"Points", user.second.points},
                        })
                    );
                }
            }
            response.Set("Users", usersJson);
            SendToUser(
                userEntry->second,
                response
            );
        }

        /**
         * This is called whenever a text message is received from
         * a user in the chat room.
         *
         * @param[in] sessionId
         *     This is the session ID of the user who sent the message.
         *
         * @param[in] data
         *     This is the content of the message received from the user.
         */
        void ReceiveMessage(
            unsigned int sessionId,
            const std::string& data
        ) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            const auto userEntry = users.find(sessionId);
            if (userEntry == users.end()) {
                return;
            }
            const auto message = Json::Json::FromEncoding(data);
            if (message["Type"] == "SetNickName") {
                SetNickName(message, userEntry);
            } else if (message["Type"] == "GetNickNames") {
                GetNickNames(message, userEntry);
            } else if (message["Type"] == "Tell") {
                Tell(message, userEntry);
            } else if (message["Type"] == "GetAvailableNickNames") {
                GetAvailableNickNames(message, userEntry);
            } else if (message["Type"] == "GetUsers") {
                GetUsers(message, userEntry);
            }
        }

        /**
         * This is called whenever the WebSocket to a user has
         * been closed, in order to remove the user from the room.
         *
         * @param[in] sessionId
         *     This is the session ID of the user who left the room.
         *
         * @param[in] code
         *     This is the WebSocket close status code.
         *
         * @param[in] reason
         *     This is the payload data from the WebSocket close.
         */
        void RemoveUser(
            unsigned int sessionId,
            unsigned int code,
            const std::string& reason
        ) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            const auto userEntry = users.find(sessionId);
            if (userEntry == users.end()) {
                return;
            }
            userEntry->second.ws->Close(code, reason);
            userEntry->second.open = false;
            usersHaveClosed = true;
            workerWakeCondition.notify_all();
        }

        /**
         * This method is called whenever a new user tries
         * to connect to the chat room.
         *
         * @param[in] request
         *     This is the request to connect to the chat room.
         *
         * @param[in] connection
         *     This is the connection on which the request was made.
         *
         * @param[in] trailer
         *     This holds any characters that have already been received
         *     by the server but come after the end of the current
         *     request.  A handler that upgrades the connection might want
         *     to interpret these characters within the context of the
         *     upgraded connection.
         *
         * @return
         *     The response to be returned to the client is returned.
         */
        Http::Response AddUser(
            const Http::Request& request,
            std::shared_ptr< Http::Connection > connection,
            const std::string& trailer
        ) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            Http::Response response;
            const auto sessionId = nextSessionId++;
            auto& user = users[sessionId];
            user.ws = std::make_shared< WebSockets::WebSocket >();
            const auto diagnosticsSenderName = SystemAbstractions::sprintf(
                "Session #%zu", sessionId
            );
            user.diagnosticsSenderName = diagnosticsSenderName;
            user.wsDiagnosticsUnsubscribeDelegate = user.ws->SubscribeToDiagnostics(
                [this, diagnosticsSenderName](
                    std::string senderName,
                    size_t level,
                    std::string message
                ){
                    diagnosticMessageDelegate(
                        diagnosticsSenderName,
                        level,
                        message
                    );
                }
            );
            user.ws->SetTextDelegate(
                [this, sessionId](const std::string& data){ ReceiveMessage(sessionId, data); }
            );
            user.ws->SetCloseDelegate(
                [this, sessionId](
                    unsigned int code,
                    const std::string& reason
                ){
                    RemoveUser(sessionId, code, reason);
                }
            );
            if (
                !user.ws->OpenAsServer(
                    connection,
                    request,
                    response,
                    trailer
                )
            ) {
                (void)users.erase(sessionId);
                response.statusCode = 200;
                response.headers.SetHeader("Content-Type", "text/plain");
                response.body = "Try again, but next time use a WebSocket.  Kthxbye!";
            }
            return response;
        }
    } room;

}

/**
 * This is the entry point function of the plug-in.
 *
 * @param[in,out] server
 *     This is the server to which to add the plug-in.
 *
 * @param[in] configuration
 *     This holds the configuration items of the plug-in.
 *
 * @param[in] delegate
 *     This is the function to call to deliver diagnostic
 *     messages generated by the plug-in.
 *
 * @param[out] unloadDelegate
 *     This is where the plug-in should store a function object
 *     that the server should call to stop and clean up the plug-in
 *     just prior to unloading it.
 *
 *     If this is set to nullptr on return, it means the plug-in
 *     was unable to load successfully.
 */
extern "C" API void LoadPlugin(
    Http::IServer* server,
    Json::Json configuration,
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate,
    std::function< void() >& unloadDelegate
) {
    // Determine the resource space we're serving.
    Uri::Uri uri;
    if (!configuration.Has("space")) {
        diagnosticMessageDelegate(
            "",
            SystemAbstractions::DiagnosticsSender::Levels::ERROR,
            "no 'space' URI in configuration"
        );
        return;
    }
    if (!uri.ParseFromString(configuration["space"])) {
        diagnosticMessageDelegate(
            "",
            SystemAbstractions::DiagnosticsSender::Levels::ERROR,
            "unable to parse 'space' URI in configuration"
        );
        return;
    }
    auto space = uri.GetPath();
    (void)space.erase(space.begin());

    // Get available nicknames from configuration.
    const auto availableNickNamesJson = configuration["nicknames"];
    if (availableNickNamesJson.GetType() == Json::Json::Type::Array) {
        for (size_t i = 0; i < availableNickNamesJson.GetSize(); ++i) {
            (void)room.availableNickNames.insert(availableNickNamesJson[i]);
        }
    }

    // Allow math question cooldown period range
    // to be configured.
    const auto mathQuiz = configuration["mathQuiz"];
    if (mathQuiz.GetType() == Json::Json::Type::Object) {
        const auto minCooldownJson = mathQuiz["minCoolDown"];
        if (minCooldownJson.GetType() == Json::Json::Type::FloatingPoint) {
            room.minQuestionCooldown = minCooldownJson;
        }
        const auto maxCooldownJson = mathQuiz["maxCoolDown"];
        if (maxCooldownJson.GetType() == Json::Json::Type::FloatingPoint) {
            room.maxQuestionCooldown = maxCooldownJson;
        }
    }
    if (room.minQuestionCooldown > room.maxQuestionCooldown) {
        std::swap(room.minQuestionCooldown, room.maxQuestionCooldown);
    }

    // Get initial points from configuration.
    const auto initialPointsJson = configuration["initialPoints"];
    if (initialPointsJson.GetType() == Json::Json::Type::Object) {
        for (const auto nickname: initialPointsJson.GetKeys()) {
            room.initialPoints[nickname] = initialPointsJson[nickname];
        }
    }

    // Allow tell timeout to be configured.
    const auto tellTimeoutJson = configuration["tellTimeout"];
    if (tellTimeoutJson.GetType() == Json::Json::Type::FloatingPoint) {
        room.tellTimeout = tellTimeoutJson;
    }

    // Register to handle requests for the space we're serving.
    room.server = server;
    room.diagnosticMessageDelegate = diagnosticMessageDelegate;
    room.Start();
    const auto unregistrationDelegate = server->RegisterResource(
        space,
        [](
            const Http::Request& request,
            std::shared_ptr< Http::Connection > connection,
            const std::string& trailer
        ){
            return room.AddUser(request, connection, trailer);
        }
    );

    // Give back the delete to call just before this plug-in is unloaded.
    unloadDelegate = [unregistrationDelegate]{
        unregistrationDelegate();
        room.Stop();
        room.users.clear();
        room.usersHaveClosed = false;
        room.answeredCorrectly = true;
        room.nextSessionId = 1;
        room.diagnosticMessageDelegate = nullptr;
        room.availableNickNames.clear();
        room.server = nullptr;
    };
}

/**
 * This is a back door used during testing, to get the next math question.
 *
 * @return
 *     The next math question is returned.
 */
extern API std::vector< int > GetNextQuestionComponents() {
    return room.questionComponents;
}

/**
 * This is a back door used during testing, to get the next math question.
 *
 * @return
 *     The next math question is returned.
 */
extern API std::string GetNextQuestion() {
    return room.question;
}

/**
 * This is a back door used during testing, to get the answer
 * to the next math question.
 *
 * @return
 *     The answer to the next math question is returned.
 */
extern API std::string GetNextAnswer() {
    return room.answer;
}

/**
 * This is a back door used during testing, to set the answer
 * to the next math question.
 *
 * @param[in] answer
 *     This is the answer to the next math question.
 */
extern API void SetNextAnswer(const std::string& answer) {
    std::lock_guard< decltype(room.mutex) > lock(room.mutex);
    room.answer = answer;
    room.answeredCorrectly = false;
    room.answerChangedCondition.notify_all();
}

/**
 * This is a back door used during testing, to set the flag
 * which indicates that the current math question was answered correctly.
 */
extern API void SetAnsweredCorrectly() {
    std::lock_guard< decltype(room.mutex) > lock(room.mutex);
    room.answeredCorrectly = true;
}

/**
 * This is a back door used during testing, to wait for the
 * next math question to be asked.
 */
extern API void AwaitNextQuestion() {
    std::unique_lock< decltype(room.mutex) > lock(room.mutex);
    Room* roomPtr = &room;
    (void)room.answerChangedCondition.wait_for(
        lock,
        std::chrono::seconds(1),
        [roomPtr]{
            return !roomPtr->answeredCorrectly;
        }
    );
}

/**
 * This checks to make sure the plug-in entry point signature
 * matches the entry point type declared in the web server API.
 */
namespace {
    PluginEntryPoint EntryPoint = &LoadPlugin;
}
