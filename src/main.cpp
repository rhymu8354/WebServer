/**
 * @file main.cpp
 *
 * This module holds the main() function, which is the entrypoint
 * to the program.
 *
 * © 2018 by Richard Walters
 */

#include "Plugin.hpp"
#include "PluginLoader.hpp"
#include "TimeKeeper.hpp"

#include <chrono>
#include <memory>
#include <Http/Server.hpp>
#include <HttpNetworkTransport/HttpServerNetworkTransport.hpp>
#include <Json/Value.hpp>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <StringExtensions/StringExtensions.hpp>
#include <SystemAbstractions/DiagnosticsStreamReporter.hpp>
#include <SystemAbstractions/File.hpp>
#include <thread>
#include <TlsDecorator/TlsDecorator.hpp>
#include <vector>

namespace {

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

        /**
         * This is the path to the folder which will be monitored
         * for plug-ins to be added, changed, and removed.
         * These are the "originals" of the plug-ins, and will
         * be copied to another folder before loading them.
         */
        std::string pluginsImagePath = SystemAbstractions::File::GetExeParentDirectory();

        /**
         * This is the path to the folder where copies of all
         * plug-ins to be loaded will be made.
         */
        std::string pluginsRuntimePath = SystemAbstractions::File::GetExeParentDirectory() + "/runtime";
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
    Json::Value ReadConfiguration(const Environment& environment) {
        // Start with a default configuration, to be used if there are any
        // issues reading the actual configuration file.
        Json::Value configuration(Json::Value::Type::Object);

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
        configuration = Json::Value::FromEncoding(encodedConfig.data());
        return configuration;
    }


    /**
     * This function loads the contents of the file with the given path
     * into the given string.
     *
     * @param[in] filePath
     *     This is the path of the file to load.
     *
     * @param[in] fileDescription
     *     This is a description of the file being loaded, used in any
     *     diagnostic messages published by the function.
     *
     * @param[in] diagnosticMessageDelegate
     *     This is the function to call to publish any diagnostic messages.
     *
     * @param[out] fileContents
     *     This is where to store the file's contents.
     *
     * @return
     *     An indication of whether or not the function succeeded is returned.
     */
    bool LoadFile(
        const std::string& filePath,
        const std::string& fileDescription,
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate,
        std::string& fileContents
    ) {
        SystemAbstractions::File file(filePath);
        if (file.OpenReadOnly()) {
            std::vector< uint8_t > fileContentsAsVector(file.GetSize());
            if (file.Read(fileContentsAsVector) != fileContentsAsVector.size()) {
                diagnosticMessageDelegate(
                    "WebServer",
                    SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                    StringExtensions::sprintf(
                        "Unable to read %s file '%s'",
                        fileDescription.c_str(),
                        filePath.c_str()
                    )
                );
                return false;
            }
            (void)fileContents.assign(
                (const char*)fileContentsAsVector.data(),
                fileContentsAsVector.size()
            );
        } else {
            diagnosticMessageDelegate(
                "WebServer",
                SystemAbstractions::DiagnosticsSender::Levels::ERROR,
                StringExtensions::sprintf(
                    "Unable to open %s file '%s'",
                    fileDescription.c_str(),
                    filePath.c_str()
                )
            );
            return false;
        }
        return true;
    }

    /**
     * This function assembles the configuration of the server, and uses it
     * to start the server with the given transport layer.
     *
     * @param[in,out] server
     *     This is the server to configure and start.
     *
     * @param[in] configuration
     *     This holds all of the server's configuration items.
     *
     * @param[in] environment
     *     This contains variables set through the operating system
     *     environment or the command-line arguments.
     *
     * @param[in] diagnosticMessageDelegate
     *     This is the function to call to publish any diagnostic messages.
     *
     * @return
     *     An indication of whether or not the function succeeded is returned.
     */
    bool ConfigureAndStartServer(
        Http::Server& server,
        const Json::Value& configuration,
        const Environment& environment,
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate
    ) {
        auto transport = std::make_shared< HttpNetworkTransport::HttpServerNetworkTransport >();
        transport->SubscribeToDiagnostics(diagnosticMessageDelegate);
        if (
            configuration.Has("secure")
            && configuration["secure"]
        ) {
            std::string cert, key, passphrase;
            auto certPath = (std::string)configuration["sslCertificate"];
            if (!SystemAbstractions::File::IsAbsolutePath(certPath)) {
                certPath = SystemAbstractions::File::GetExeParentDirectory() + "/" + certPath;
            }
            if (!LoadFile(certPath, "SSL certificate", diagnosticMessageDelegate, cert)) {
                return false;
            }
            auto keyPath = (std::string)configuration["sslKey"];
            if (!SystemAbstractions::File::IsAbsolutePath(keyPath)) {
                keyPath = SystemAbstractions::File::GetExeParentDirectory() + "/" + keyPath;
            }
            if (!LoadFile(keyPath, "SSL private key", diagnosticMessageDelegate, key)) {
                return false;
            }
            passphrase = (std::string)configuration["sslKeyPassphrase"];
            transport->SetConnectionDecoratorFactory(
                [cert, key, passphrase, diagnosticMessageDelegate](
                    std::shared_ptr< SystemAbstractions::INetworkConnection > connection
                ){
                    const auto tlsDecorator = std::make_shared< TlsDecorator::TlsDecorator >();
                    tlsDecorator->ConfigureAsServer(
                        connection,
                        cert,
                        key,
                        passphrase
                    );
                    return tlsDecorator;
                }
            );
        }
        Http::Server::MobilizationDependencies deps;
        deps.transport = transport;
        deps.timeKeeper = std::make_shared< TimeKeeper >();
        for (const auto& key: configuration["server"].GetKeys()) {
            server.SetConfigurationItem(key, configuration["server"][key]);
        }
        if (!server.Mobilize(deps)) {
            return false;
        }
        return true;
    }

    /**
     * This function is called from the main function, once the web server
     * is up and running.  It monitors the plug-ins folder and performs
     * any plugin loading/unloading.  It returns once the user has signaled
     * to end the program.
     *
     * @param[in,out] server
     *     This is the server to monitor.
     *
     * @param[in] configuration
     *     This holds all of the server's configuration items.
     *
     * @param[in] environment
     *     This contains variables set through the operating system
     *     environment or the command-line arguments.
     *
     * @param[in] diagnosticMessageDelegate
     *     This is the function to call to publish any diagnostic messages.
     */
    void MonitorServer(
        Http::Server& server,
        const Json::Value& configuration,
        const Environment& environment,
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate
    ) {
        std::string pluginsImagePath = environment.pluginsImagePath;
        if (configuration.Has("plugins-image")) {
            pluginsImagePath = (std::string)configuration["plugins-image"];
            if (!SystemAbstractions::File::IsAbsolutePath(pluginsImagePath)) {
                pluginsImagePath = SystemAbstractions::File::GetExeParentDirectory() + "/" + pluginsImagePath;
            }
        }
        std::string pluginsRuntimePath = environment.pluginsRuntimePath;
        if (configuration.Has("plugins-runtime")) {
            pluginsRuntimePath = (std::string)configuration["plugins-runtime"];
            if (!SystemAbstractions::File::IsAbsolutePath(pluginsRuntimePath)) {
                pluginsRuntimePath = SystemAbstractions::File::GetExeParentDirectory() + "/" + pluginsRuntimePath;
            }
        }
        const auto pluginsEntries = configuration["plugins"];
        const auto pluginsEnabled = configuration["plugins-enabled"];
        std::string modulePrefix, moduleExtension;
#if _WIN32
        modulePrefix = "";
        moduleExtension = ".dll";
#elif defined(APPLE)
        modulePrefix = "lib";
        moduleExtension = ".dylib";
#else
        modulePrefix = "lib";
        moduleExtension = ".so";
#endif
        std::map< std::string, std::shared_ptr< Plugin > > plugins;
        for (size_t i = 0; i < pluginsEnabled.GetSize(); ++i) {
            const std::string pluginName = pluginsEnabled[i];
            if (pluginsEntries.Has(pluginName)) {
                const auto pluginEntry = pluginsEntries[pluginName];
                const std::string pluginModule = pluginEntry["module"];
                const auto plugin = plugins[pluginName] = std::make_shared< Plugin >(
                    pluginsImagePath + "/" + modulePrefix + pluginModule + moduleExtension,
                    pluginsRuntimePath + "/" + modulePrefix + pluginModule + moduleExtension
                );
                plugin->moduleName = pluginModule;
                plugin->configuration = pluginEntry["configuration"];
                plugin->lastModifiedTime = plugin->imageFile.GetLastModifiedTime();
            }
        }
        PluginLoader pluginLoader(
            server,
            plugins,
            pluginsImagePath,
            pluginsRuntimePath,
            diagnosticMessageDelegate
        );
        pluginLoader.Scan();
        pluginLoader.StartBackgroundScanning();
        while (!shutDown) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        pluginLoader.StopBackgroundScanning();
        for (auto& plugin: plugins) {
            plugin.second->Unload(
                plugin.first,
                diagnosticMessageDelegate
            );
        }
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
    Http::Server server;
    (void)setbuf(stdout, NULL);
    const auto diagnosticsPublisher = SystemAbstractions::DiagnosticsStreamReporter(stdout, stderr);
    const auto diagnosticsSubscription = server.SubscribeToDiagnostics(diagnosticsPublisher);
    const auto configuration = ReadConfiguration(environment);
    if (!ConfigureAndStartServer(server, configuration, environment, diagnosticsPublisher)) {
        return EXIT_FAILURE;
    }
    diagnosticsPublisher("WebServer", 3, "Web server up and running.");
    MonitorServer(server, configuration, environment, diagnosticsPublisher);
    (void)signal(SIGINT, previousInterruptHandler);
    diagnosticsPublisher("WebServer", 3, "Exiting...");
    return EXIT_SUCCESS;
}
