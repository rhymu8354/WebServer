/**
 * @file StaticContentPlugin.cpp
 *
 * This is a plug-in for the Excalibur web server, designed
 * to serve static content (e.g. files) in response to resource requests.
 *
 * © 2018 by Richard Walters
 */

#include <functional>
#include <Http/Server.hpp>
#include <Json/Json.hpp>
#include <SystemAbstractions/StringExtensions.hpp>
#include <WebServer/PluginEntryPoint.hpp>

#ifdef _WIN32
#define API __declspec(dllexport)
#else /* POSIX */
#define API
#endif /* _WIN32 / POSIX */

/**
 * This is the type expected for the entry point functions
 * for all server plug-ins.
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
    Http::Server& server,
    Json::Json configuration,
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate,
    std::function< void() >& unloadDelegate
) {
    Uri::Uri uri;
    if (!configuration.Has("space")) {
        diagnosticMessageDelegate(
            "",
            SystemAbstractions::DiagnosticsSender::Levels::ERROR,
            "no 'space' URI in configuration"
        );
        return;
    }
    if (!uri.ParseFromString(*configuration["space"])) {
        diagnosticMessageDelegate(
            "",
            SystemAbstractions::DiagnosticsSender::Levels::ERROR,
            "unable to parse 'space' URI in configuration"
        );
        return;
    }
    auto space = uri.GetPath();
    (void)space.erase(space.begin());
    const auto unregistrationDelegate = server.RegisterResource(
        space,
        [](
            std::shared_ptr< Http::Server::Request > request
        ){
            const auto response = std::make_shared< Http::Client::Response >();
            response->statusCode = 200;
            response->reasonPhrase = "OK";
            response->headers.AddHeader("Content-Type", "text/plain");
            response->body = "Coming soon...\r\n";
            response->headers.AddHeader("Content-Length", SystemAbstractions::sprintf("%zu", response->body.length()));
            return response;
        }
    );
    unloadDelegate = [unregistrationDelegate]{
        unregistrationDelegate();
    };
}

/**
 * This checks to make sure the plug-in entry point signature
 * matches the entry point type declared in the web server API.
 */
namespace {
    PluginEntryPoint EntryPoint = &LoadPlugin;
}
