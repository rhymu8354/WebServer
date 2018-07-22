/**
 * @file main.cpp
 *
 * This module holds the main() function, which is the entrypoint
 * to the program.
 *
 * © 2018 by Richard Walters
 */

#include <chrono>
#include <memory>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <Http/Server.hpp>
#include <HttpNetworkTransport/HttpServerNetworkTransport.hpp>
#include <Json/Json.hpp>
#include <stdio.h>
#include <SystemAbstractions/DiagnosticsStreamReporter.hpp>
#include <SystemAbstractions/File.hpp>
#include <thread>
#include <vector>

namespace {

    /**
     * This is the default port number on which to listen for
     * connections from web clients.
     */
    constexpr uint16_t DEFAULT_PORT = 8080;

    /**
     * This flag indicates whether or not the web server should shut down.
     */
    bool shutDown = false;

    /**
     * This function is set up to be called when the SIGINT signal is
     * received by the program.  It just sets the "shutDown" flag
     * and relies on the program to be polling the flag to detect
     * when it's been set.
     *
     * @param[in] sig
     *     This is the signal for which this function was called.
     */
    void InterruptHandler(int) {
        shutDown = true;
    }

    /**
     * This function opens and reads the server's configuration file,
     * returning it.  The configuration is formatted as a JSON object.
     *
     * @return
     *     The server's configuration is returned as a JSON object.
     */
    Json::Json ReadConfiguration() {
        // Start with a default configuration, to be used if there are any
        // issues reading the actual configuration file.
        Json::Json configuration(Json::Json::Type::Object);
        configuration.Set("port", DEFAULT_PORT);

        // Open the configuration file.
        const auto configFile = std::shared_ptr< FILE >(
            fopen(
                (SystemAbstractions::File::GetExeParentDirectory() + "/config.json").c_str(),
                "rb"
            ),
            [](FILE* f){
                if (f != NULL) {
                    (void)fclose(f);
                }
            }
        );
        if (configFile == NULL) {
            fprintf(stderr, "error: unable to open configuration file\n");
            return configuration;
        }

        // Determine the size of the configuration file.
        if (fseek(configFile.get(), 0, SEEK_END) != 0) {
            fprintf(stderr, "error: unable to seek to end of configuration file\n");
            return configuration;
        }
        const auto configSize = ftell(configFile.get());
        if (configSize == EOF) {
            fprintf(stderr, "error: unable to determine end of configuration file\n");
            return configuration;
        }
        if (fseek(configFile.get(), 0, SEEK_SET) != 0) {
            fprintf(stderr, "error: unable to seek to beginning of configuration file\n");
            return configuration;
        }

        // Read the configuration file into memory.
        std::vector< char > encodedConfig(configSize + 1);
        const auto readResult = fread(encodedConfig.data(), configSize, 1, configFile.get());
        if (readResult != 1) {
            fprintf(stderr, "error: unable to read configuration file\n");
            return configuration;
        }

        // Decode the configuration file.
        configuration = Json::Json::FromEncoding(encodedConfig.data());
        return configuration;
    }

}

/**
 * This function is the entrypoint of the program.
 * It just sets up the web server and then waits for
 * the SIGINT signal to know when the web server should
 * be shut down and program terminated.
 *
 * @param[in] argc
 *     This is the number of command-line arguments given to the program.
 *
 * @param[in] argv
 *     This is the array of command-line arguments given to the program.
 */
int main(int argc, char* argv[]) {
#ifdef _WIN32
    //_crtBreakAlloc = 18;
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif /* _WIN32 */
    auto transport = std::make_shared< HttpNetworkTransport::HttpServerNetworkTransport >();
    Http::Server server;
    const auto diagnosticsSubscription = server.SubscribeToDiagnostics(
        SystemAbstractions::DiagnosticsStreamReporter(stdout, stderr)
    );
    const auto configuration = ReadConfiguration();
    uint16_t port = 0;
    if (configuration.Has("port")) {
        port = (int)*configuration["port"];
    }
    if (port == 0) {
        port = DEFAULT_PORT;
    }
    if (!server.Mobilize(transport, port)) {
        return EXIT_FAILURE;
    }
    const auto previousInterruptHandler = signal(SIGINT, InterruptHandler);
    printf("Web server up and running.\n");
    while (!shutDown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    (void)signal(SIGINT, previousInterruptHandler);
    printf("Exiting...\n");
    return EXIT_SUCCESS;
}
