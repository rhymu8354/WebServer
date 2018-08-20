/**
 * @file EchoPlugin.cpp
 *
 * This is a plug-in for the Excalibur web server, designed
 * to generate an HTML page that just tells you what was in
 * your own request headers.
 *
 * © 2018 by Richard Walters
 */

#include <functional>
#include <Http/Server.hpp>
#include <inttypes.h>
#include <Json/Json.hpp>
#include <sstream>
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

    // Register to handle requests for the space we're serving.
    const auto unregistrationDelegate = server->RegisterResource(
        space,
        [](
            const Http::Request& request,
            std::shared_ptr< Http::Connection > connection,
            const std::string& trailer
        ){
            Http::Response response;
            response.statusCode = 200;
            response.reasonPhrase = "OK";
            response.headers.AddHeader("Content-Type", "text/html");
            std::ostringstream reportRows;
            for (const auto& header: request.headers.GetAll()) {
                reportRows
                    << "<tr>"
                    << "<td>" << header.name << "</td>"
                    << "<td>" << header.value << "</td>"
                    << "</tr>";
            }
            response.body = (
                "<!DOCTYPE html>"
                "<html>"
                "<head>"
                "<meta charset=\"UTF-8\">"
                "<title>Excalibur - Request Echo</title>"
                "</head>"
                ""
                "<body>"
                "<table><thead><tr><th>Header</th><th>Value</th></tr></thead>"
                "<tbody>"
                + reportRows.str()
                + "</tbody></table>"
                "</body>"
                ""
                "</html>"
            );
            response.headers.AddHeader("Content-Length", SystemAbstractions::sprintf("%zu", response.body.length()));
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