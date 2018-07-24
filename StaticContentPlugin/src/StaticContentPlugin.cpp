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

#ifdef _WIN32
#define API __declspec(dllexport)
#else /* POSIX */
#define API
#endif /* _WIN32 / POSIX */

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
