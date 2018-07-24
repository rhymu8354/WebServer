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
     * This contains variables set through the operating system environment
     * or the command-line arguments.
     */
    struct Environment {
        /**
         * This is the path to the configuration file to use when
         * configuring the server.
         */
        std::string configFilePath;
    };

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
     * This function updates the program environment to incorporate
     * any applicable command-line arguments.
     *
     * @param[in] argc
     *     This is the number of command-line arguments given to the program.
     *
     * @param[in] argv
     *     This is the array of command-line arguments given to the program.
     *
     * @param[in,out] environment
     *     This is the environment to update.
     *
     * @return
     *     An indication of whether or not the function succeeded is returned.
     */
    bool ProcessCommandLineArguments(
        int argc,
        char* argv[],
        Environment& environment
    ) {
        size_t state = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            switch (state) {
                case 0: { // next argument
                    if ((arg == "-c") || (arg == "--config")) {
                        state = 1;
                    } else {
                        fprintf(stderr, "error: unrecognized option: '%s'\n", arg.c_str());
                        return false;
                    }
                } break;

                case 1: { // -c|--config
                    if (!environment.configFilePath.empty()) {
                        fprintf(stderr, "error: multiple configuration file paths given\n");
                        return false;
                    }
                    environment.configFilePath = arg;
                    state = 0;
                } break;
            }
        }
        switch (state) {
            case 1: { // -c|--config
                fprintf(stderr, "error: configuration file path expected\n");
            } return false;
        }
        return true;
    }

    /**
     * This function opens and reads the server's configuration file,
     * returning it.  The configuration is formatted as a JSON object.
     *
     * @param[in] environment
     *     This contains variables set through the operating system
     *     environment or the command-line arguments.
     *
     * @return
     *     The server's configuration is returned as a JSON object.
     */
    Json::Json ReadConfiguration(const Environment& environment) {
        // Start with a default configuration, to be used if there are any
        // issues reading the actual configuration file.
        Json::Json configuration(Json::Json::Type::Object);
        configuration.Set("port", DEFAULT_PORT);

        // Open the configuration file.
        std::vector< std::string > possibleConfigFilePaths = {
            "config.json",
            SystemAbstractions::File::GetExeParentDirectory() + "/config.json",
        };
        if (!environment.configFilePath.empty()) {
            possibleConfigFilePaths.insert(
                possibleConfigFilePaths.begin(),
                environment.configFilePath
            );
        }
        std::shared_ptr< FILE > configFile;
        for (const auto& possibleConfigFilePath: possibleConfigFilePaths) {
            configFile = std::shared_ptr< FILE >(
                fopen(possibleConfigFilePath.c_str(), "rb"),
                [](FILE* f){
                    if (f != NULL) {
                        (void)fclose(f);
                    }
                }
            );
            if (configFile != NULL) {
                break;
            }
        }
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

    /**
     * This function assembles the configuration of the server, and uses it
     * to start server with the given transport layer.
     *
     * @param[in,out] server
     *     This is the server to configure and start.
     *
     * @param[in] transport
     *     This is the transport layer to give to the server for interfacing
     *     with the network.
     *
     * @param[in] environment
     *     This contains variables set through the operating system
     *     environment or the command-line arguments.
     *
     * @return
     *     An indication of whether or not the function succeeded is returned.
     */
    bool ConfigureAndStartServer(
        Http::Server& server,
        std::shared_ptr< Http::ServerTransport > transport,
        const Environment& environment
    ) {
        const auto configuration = ReadConfiguration(environment);
        uint16_t port = 0;
        if (configuration.Has("port")) {
            port = (int)*configuration["port"];
        }
        if (port == 0) {
            port = DEFAULT_PORT;
        }
        if (!server.Mobilize(transport, port)) {
            return false;
        }
        return true;
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
    const auto previousInterruptHandler = signal(SIGINT, InterruptHandler);
    Environment environment;
    if (!ProcessCommandLineArguments(argc, argv, environment)) {
        return EXIT_FAILURE;
    }
    auto transport = std::make_shared< HttpNetworkTransport::HttpServerNetworkTransport >();
    Http::Server server;
    const auto diagnosticsSubscription = server.SubscribeToDiagnostics(
        SystemAbstractions::DiagnosticsStreamReporter(stdout, stderr)
    );
    if (!ConfigureAndStartServer(server, transport, environment)) {
        return EXIT_FAILURE;
    }
    printf("Web server up and running.\n");
    while (!shutDown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    (void)signal(SIGINT, previousInterruptHandler);
    printf("Exiting...\n");
    return EXIT_SUCCESS;
}
