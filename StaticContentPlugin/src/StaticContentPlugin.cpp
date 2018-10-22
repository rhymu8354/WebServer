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
#include <Json/Value.hpp>
#include <regex>
#include <Hash/Sha1.hpp>
#include <Hash/Templates.hpp>
#include <SystemAbstractions/File.hpp>
#include <SystemAbstractions/StringExtensions.hpp>
#include <WebServer/PluginEntryPoint.hpp>

#ifdef _WIN32
#define API __declspec(dllexport)
#else /* POSIX */
#define API
#endif /* _WIN32 / POSIX */

namespace {

    /**
     * This represents one space of server resources and how they
     * should be mapped to the file system.
     */
    struct SpaceMapping {
        /**
         * This is the path to the resource space in the server.
         */
        std::vector< std::string > space;

        /**
         * This is the file system path to the files to be served.
         */
        std::string root;

        /**
         * This is the function to call in order to unregister
         * the plug-in as handling this server resource space.
         */
        Http::IServer::UnregistrationDelegate unregistrationDelegate;
    };

    /**
     * This function configures the given space mapping from
     * the given configuration items.
     *
     * @param[in,out] spaceMapping
     *     This is the space mapping to configure.
     *
     * @parma[in] configuration
     *     This contains the items used to configure the space mapping.
     *
     * @param[in] diagnosticMessageDelegate
     *     This is the function to call to deliver diagnostic
     *     messages generated by the plug-in.
     *
     * @return
     *     An indication of whether or not the space mapping
     *     was successfully configured is returned.
     */
    bool ConfigureSpaceMapping(
        SpaceMapping& spaceMapping,
        Json::Value configuration,
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate
    ) {
        // Determine the resource space we're serving.
        Uri::Uri uri;
        if (!configuration.Has("space")) {
            diagnosticMessageDelegate(
                "",
                SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                "no 'space' URI in configuration"
            );
            return false;
        }
        if (!uri.ParseFromString(configuration["space"])) {
            diagnosticMessageDelegate(
                "",
                SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                "unable to parse 'space' URI in configuration"
            );
            return false;
        }
        spaceMapping.space = uri.GetPath();
        (void)spaceMapping.space.erase(spaceMapping.space.begin());

        // Determine where to locate the static content.
        if (!configuration.Has("root")) {
            diagnosticMessageDelegate(
                "",
                SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                "no 'root' URI in configuration"
            );
            return false;
        }
        spaceMapping.root = (std::string)configuration["root"];
        if (!SystemAbstractions::File::IsAbsolutePath(spaceMapping.root)) {
            spaceMapping.root = SystemAbstractions::File::GetExeParentDirectory() + "/" + spaceMapping.root;
        }
        return true;
    }

}

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
 * @param[in] diagnosticMessageDelegate
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
    Json::Value configuration,
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate,
    std::function< void() >& unloadDelegate
) {
    // If multiple spaces are specified, configure each of them.
    // Otherwise, expect a single space/root configuration item pair.
    std::vector< SpaceMapping > spaceMappings;
    if (
        configuration.Has("spaces")
        && (configuration["spaces"].GetType() == Json::Value::Type::Array)
    ) {
        const auto spaces = configuration["spaces"];
        for (size_t i = 0; i < spaces.GetSize(); ++i) {
            SpaceMapping spaceMapping;
            if (
                !ConfigureSpaceMapping(
                    spaceMapping,
                    spaces[i],
                    diagnosticMessageDelegate
                )
            ) {
                return;
            }
            spaceMappings.push_back(std::move(spaceMapping));
        }
    } else {
        SpaceMapping spaceMapping;
        if (
            !ConfigureSpaceMapping(
                spaceMapping,
                configuration,
                diagnosticMessageDelegate
            )
        ) {
            return;
        }
        spaceMappings.push_back(std::move(spaceMapping));
    }

    // Register to handle requests for the space we're serving.
    for (auto& spaceMapping: spaceMappings) {
        auto root = spaceMapping.root;
        spaceMapping.unregistrationDelegate = server->RegisterResource(
            spaceMapping.space,
            [root](
                const Http::Request& request,
                std::shared_ptr< Http::Connection > connection,
                const std::string& trailer
            ){
                const auto path = SystemAbstractions::Join(
                    {
                        root,
                        SystemAbstractions::Join(request.target.GetPath(), "/")
                    },
                    "/"
                );
                SystemAbstractions::File file(path);
                Http::Response response;
                if (
                    file.IsExisting()
                    && !file.IsDirectory()
                ) {
                    if (file.Open()) {
                        SystemAbstractions::File::Buffer buffer(file.GetSize());
                        if (file.Read(buffer) == buffer.size()) {
                            auto etag = Hash::BytesToString< Hash::Sha1 >(buffer);
                            if (
                                request.headers.HasHeader("If-None-Match")
                                && (request.headers.GetHeaderValue("If-None-Match") == etag)
                            ) {
                                response.statusCode = 304;
                                response.reasonPhrase = "Not Modified";
                            } else {
                                response.statusCode = 200;
                                response.reasonPhrase = "OK";
                                response.body.assign(
                                    buffer.begin(),
                                    buffer.end()
                                );
                            }
                            bool isWorthyOfBeingGzipped = false;
                            if (
                                (path.length() >= 5)
                                && (path.substr(path.length() - 5) == ".html")
                            ) {
                                response.headers.AddHeader("Content-Type", "text/html");
                                isWorthyOfBeingGzipped = true;
                            } else if (
                                (path.length() >= 3)
                                && (path.substr(path.length() - 3) == ".js")
                            ) {
                                response.headers.AddHeader("Content-Type", "application/javascript");
                                isWorthyOfBeingGzipped = true;
                            } else if (
                                (path.length() >= 4)
                                && (path.substr(path.length() - 4) == ".css")
                            ) {
                                response.headers.AddHeader("Content-Type", "text/css");
                                isWorthyOfBeingGzipped = true;
                            } else if (
                                (path.length() >= 4)
                                && (path.substr(path.length() - 4) == ".txt")
                            ) {
                                response.headers.AddHeader("Content-Type", "text/plain");
                                isWorthyOfBeingGzipped = true;
                            } else if (
                                (path.length() >= 4)
                                && (path.substr(path.length() - 4) == ".ico")
                            ) {
                                response.headers.AddHeader("Content-Type", "image/x-icon");
                            } else {
                                response.headers.AddHeader("Content-Type", "text/plain");
                            }
                            if (
                                (request.headers.HasHeaderToken("Accept-Encoding", "gzip"))
                                && isWorthyOfBeingGzipped
                            )  {
                                response.headers.SetHeader("Content-Encoding", "gzip");
                                etag += "-gzip";
                            }
                            response.headers.AddHeader("ETag", etag);
                        } else {
                            response.statusCode = 500;
                            response.reasonPhrase = "Unable to read file";
                            response.headers.AddHeader("Content-Type", "text/plain");
                            response.body = SystemAbstractions::sprintf(
                                "Error reading file '%s'",
                                path.c_str()
                            );
                        }
                    } else {
                        response.statusCode = 500;
                        response.reasonPhrase = "Unable to open file";
                        response.headers.AddHeader("Content-Type", "text/plain");
                        response.body = SystemAbstractions::sprintf(
                            "Error opening file '%s'",
                            path.c_str()
                        );
                    }
                } else {
                    response.statusCode = 404;
                    response.reasonPhrase = "Not Found";
                    response.headers.AddHeader("Content-Type", "text/plain");
                    response.body = SystemAbstractions::sprintf(
                        "File '%s' not found.",
                        path.c_str()
                    );
                }
                response.headers.AddHeader("Content-Length", SystemAbstractions::sprintf("%zu", response.body.length()));
                return response;
            }
        );
    }

    // Give back the delete to call just before this plug-in is unloaded.
    unloadDelegate = [spaceMappings]{
        for (const auto& spaceMapping: spaceMappings) {
            spaceMapping.unregistrationDelegate();
        }
    };
}

/**
 * This checks to make sure the plug-in entry point signature
 * matches the entry point type declared in the web server API.
 */
namespace {
    PluginEntryPoint EntryPoint = &LoadPlugin;
}
