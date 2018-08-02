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
#include <inttypes.h>
#include <Json/Json.hpp>
#include <SystemAbstractions/File.hpp>
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

    // Determine where to locate the static content.
    if (!configuration.Has("root")) {
        diagnosticMessageDelegate(
            "",
            SystemAbstractions::DiagnosticsSender::Levels::ERROR,
            "no 'root' URI in configuration"
        );
        return;
    }
    const std::string root = configuration["root"];

    // Register to handle requests for the space we're serving.
    const auto unregistrationDelegate = server->RegisterResource(
        space,
        [root](
            std::shared_ptr< Http::Request > request,
            std::shared_ptr< Http::Connection > connection,
            const std::string& trailer
        ){
            const auto path = SystemAbstractions::Join(
                {
                    root,
                    SystemAbstractions::Join(request->target.GetPath(), "/")
                },
                "/"
            );
            SystemAbstractions::File file(path);
            const auto response = std::make_shared< Http::Response >();
            if (
                file.IsExisting()
                && !file.IsDirectory()
            ) {
                if (file.Open()) {
                    SystemAbstractions::File::Buffer buffer(file.GetSize());
                    if (file.Read(buffer) == buffer.size()) {
                        // TODO: replace with something that gives
                        // a strong entity tag -- this one is weak.
                        uint32_t sum = 0;
                        for (auto b: buffer) {
                            sum += b;
                        }
                        const auto etag = SystemAbstractions::sprintf("%" PRIu32, sum);
                        if (
                            request->headers.HasHeader("If-None-Match")
                            && (request->headers.GetHeaderValue("If-None-Match") == etag)
                        ) {
                            response->statusCode = 304;
                            response->reasonPhrase = "Not Modified";
                        } else {
                            response->statusCode = 200;
                            response->reasonPhrase = "OK";
                            response->body.assign(
                                buffer.begin(),
                                buffer.end()
                            );
                        }
                        response->headers.AddHeader("Content-Type", "text/html");
                        response->headers.AddHeader("ETag", etag);
                    } else {
                        response->statusCode = 500;
                        response->reasonPhrase = "Unable to read file";
                        response->headers.AddHeader("Content-Type", "text/plain");
                        response->body = SystemAbstractions::sprintf(
                            "Error reading file '%s'",
                            path.c_str()
                        );
                    }
                } else {
                    response->statusCode = 500;
                    response->reasonPhrase = "Unable to open file";
                    response->headers.AddHeader("Content-Type", "text/plain");
                    response->body = SystemAbstractions::sprintf(
                        "Error opening file '%s'",
                        path.c_str()
                    );
                }
            } else {
                response->statusCode = 404;
                response->reasonPhrase = "Not Found";
                response->headers.AddHeader("Content-Type", "text/plain");
                response->body = SystemAbstractions::sprintf(
                    "File '%s' not found.",
                    path.c_str()
                );
            }
            response->headers.AddHeader("Content-Length", SystemAbstractions::sprintf("%zu", response->body.length()));
            return response;
        }
    );

    // Give back the delete to call just before this plug-in is unloaded.
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
