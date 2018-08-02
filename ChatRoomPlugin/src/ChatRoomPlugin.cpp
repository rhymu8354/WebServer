/**
 * @file ChatRoomPlugin.cpp
 *
 * This is a plug-in for the Excalibur web server, designed
 * to demonstrate a simple chat-room type application.
 *
 * © 2018 by Richard Walters
 */

#include <condition_variable>
#include <functional>
#include <Http/Server.hpp>
#include <inttypes.h>
#include <Json/Json.hpp>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <SystemAbstractions/StringExtensions.hpp>
#include <thread>
#include <WebServer/PluginEntryPoint.hpp>
#include <WebSockets/WebSocket.hpp>

#ifdef _WIN32
#define API __declspec(dllexport)
#else /* POSIX */
#define API
#endif /* _WIN32 / POSIX */

namespace {

    /**
     * This represents a registered user of the chat room.
     */
    struct Account {
        /**
         * This is the string that the user needs to present
         * for the value of "password" when setting their nickname
         * in order to link the user to the account.
         */
        std::string password;
    };

    /**
     * This represents one user in the chat room.
     */
    struct User {
        /**
         * This is the user's nickname.
         */
        std::string nickname;

        /**
         * This is the WebSocket connection to the user.
         */
        WebSockets::WebSocket ws;

        /**
         * This flag indicates whether or not the WebSocket
         * connection to the user is still open.
         */
        bool open = true;
    };

    /**
     * This represents the state of the chat room.
     */
    struct Room {
        // Properties

        /**
         * This is used to synchronize access to the chat room.
         */
        std::mutex mutex;

        /**
         * This is used to notify the worker thread about
         * any change that should cause it to wake up.
         */
        std::condition_variable workerWakeCondition;

        /**
         * This is used to perform housekeeping in the background.
         */
        std::thread workerThread;

        /**
         * This flag indicates whether or not the worker thread should stop.
         */
        bool stopWorker = false;

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
         * These are the registered users of the chat room,
         * keyed by nickname.
         */
        std::map< std::string, Account > accounts;

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
         * This function is called in a separate thread to perform
         * housekeeping in the background for the chat room.
         */
        void Worker() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            while (!stopWorker) {
                workerWakeCondition.wait(
                    lock,
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
                            closedUsers.push_back(std::move(userEntry->second));
                            userEntry = users.erase(userEntry);
                        }
                    }
                    usersHaveClosed = false;
                    {
                        lock.unlock();
                        closedUsers.clear();
                        lock.lock();
                    }
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
            const std::string nickname = message["NickName"];
            const std::string password = message["Password"];
            Json::Json response(Json::Json::Type::Object);
            response.Set("Type", "SetNickNameResult");
            auto accountEntry = accounts.find(nickname);
            if (
                !nickname.empty()
                && (
                    (accountEntry == accounts.end())
                    || (accountEntry->second.password == password)
                )
            ) {
                userEntry->second.nickname = message["NickName"];
                auto& account = accounts[nickname];
                account.password = password;
                response.Set("Success", true);
            } else {
                response.Set("Success", false);
            }
            userEntry->second.ws.SendText(response.ToEncoding());
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
            Json::Json response(Json::Json::Type::Object);
            response.Set("Type", "NickNames");
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
            userEntry->second.ws.SendText(response.ToEncoding());
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
            const std::string tell = message["Tell"];
            if (tell.empty()) {
                return;
            }
            Json::Json response(Json::Json::Type::Object);
            response.Set("Type", "Tell");
            response.Set("Tell", tell);
            response.Set("Sender", userEntry->second.nickname);
            const auto responseEncoding = response.ToEncoding();
            for (auto& user: users) {
                user.second.ws.SendText(responseEncoding);
            }
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
            }
        }

        /**
         * This is called whenever the WebSocket to a user has
         * been closed, in order to remove the user from the room.
         *
         * @param[in] sessionId
         *     This is the session ID of the user who left the room.
         */
        void RemoveUser(unsigned int sessionId) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            const auto userEntry = users.find(sessionId);
            if (userEntry == users.end()) {
                return;
            }
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
         * @return
         *     The response to be returned to the client is returned.
         */
        std::shared_ptr< Http::Response > AddUser(
            std::shared_ptr< Http::Request > request,
            std::shared_ptr< Http::Connection > connection
        ) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            const auto response = std::make_shared< Http::Response >();
            const auto sessionId = nextSessionId++;
            auto& user = users[sessionId];
            user.ws.SetTextDelegate(
                [this, sessionId](const std::string& data){ ReceiveMessage(sessionId, data); }
            );
            user.ws.SetCloseDelegate(
                [this, sessionId](
                    unsigned int code,
                    const std::string& reason
                ){
                    RemoveUser(sessionId);
                }
            );
            if (
                !user.ws.OpenAsServer(
                    connection,
                    *request,
                    *response
                )
            ) {
                (void)users.erase(sessionId);
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

    // Register to handle requests for the space we're serving.
    room.Start();
    const auto unregistrationDelegate = server->RegisterResource(
        space,
        [](
            std::shared_ptr< Http::Request > request,
            std::shared_ptr< Http::Connection > connection
        ){
            return room.AddUser(request, connection);
        }
    );

    // Give back the delete to call just before this plug-in is unloaded.
    unloadDelegate = [unregistrationDelegate]{
        unregistrationDelegate();
        room.Stop();
        room.users.clear();
        room.accounts.clear();
        room.usersHaveClosed = false;
    };
}

/**
 * This checks to make sure the plug-in entry point signature
 * matches the entry point type declared in the web server API.
 */
namespace {
    PluginEntryPoint EntryPoint = &LoadPlugin;
}
