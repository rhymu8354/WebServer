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
#include <SystemAbstractions/DirectoryMonitor.hpp>
#include <SystemAbstractions/DynamicLibrary.hpp>
#include <SystemAbstractions/File.hpp>
#include <SystemAbstractions/StringExtensions.hpp>
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
     * This is the information tracked for each plug-in.
     */
    struct Plugin {
        // Properties

        /**
         * This is the time that the plug-in image was last modified.
         */
        time_t lastModifiedTime = 0;

        /**
         * This is the plug-in image file.
         */
        SystemAbstractions::File imageFile;

        /**
         * This is the plug-in runtime file.
         */
        SystemAbstractions::File runtimeFile;

        /**
         * This is the name of the plug-in runtime file,
         * without the file extension (if any).
         */
        std::string moduleName;

        /**
         * This is the configuration object to give to the plug-in when
         * it's loaded.
         */
        std::shared_ptr< Json::Json > configuration;

        /**
         * This is used to dynamically link with the run-time copy
         * of the plug-in image.
         */
        SystemAbstractions::DynamicLibrary runtimeLibrary;

        /**
         * If the plug-in is currently loaded, this is the function to
         * call in order to unload it.
         */
        std::function< void() > unloadDelegate;

        // Methods

        /**
         * This is the constructor for the structure.
         *
         * @param[in] imageFileName
         *     This is the plug-in image file name.
         *
         * @param[in] runtimeFileName
         *     This is the plug-in runtime file name.
         */
        Plugin(
            const std::string& imageFileName,
            const std::string& runtimeFileName
        )
            : imageFile(imageFileName)
            , runtimeFile(runtimeFileName)
        {
        }

        /**
         * This method cleanly loads the plug-in, following this
         * general procedure:
         * 1. Make a copy of the plug-in code, from the image folder
         *    to the runtime folder.
         * 2. Link the plug-in code.
         * 3. Locate the plug-in entrypoint function, "LoadPlugin".
         * 4. Call the entrypoint function, providing the plug-in
         *    with access to the server.  The plug-in will return
         *    a function the server can call later to unload the plug-in.
         *    The plug-in can signal a "failure to load" or other
         *    kind of fatal error simply by leaving the "unload"
         *    delegate as a nullptr.
         *
         * @param[in] pluginName
         *     This is the name identifying the plug-in in any diagnostic
         *     messages relating to the plug-in.
         *
         * @param[in] pluginsRuntimePath
         *     This is the path to where a copy of the plug-in code
         *     is made, in order to link the code without locking
         *     the original code image.  This is so that the original
         *     code image can be updated even while the plug-in is loaded.
         *
         * @param[in,out] server
         *     This is the server for which to load the plug-in.
         *
         * @param[in] diagnosticMessageDelegate
         *     This is the function to call to publish any diagnostic messages.
         */
        void Load(
            const std::string& pluginName,
            const std::string& pluginsRuntimePath,
            Http::Server& server,
            SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate
        ) {
            if (imageFile.Copy(runtimeFile.GetPath())) {
                if (runtimeLibrary.Load(pluginsRuntimePath, moduleName)) {
                    const auto loadPlugin = (
                        void(*)(
                            Http::Server& server,
                            Json::Json configuration,
                            SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate,
                            std::function< void() >& unloadDelegate
                        )
                    )runtimeLibrary.GetProcedure("LoadPlugin");
                    if (loadPlugin != nullptr) {
                        loadPlugin(
                            server,
                            *configuration,
                            [diagnosticMessageDelegate, pluginName](
                                std::string senderName,
                                size_t level,
                                std::string message
                            ) {
                                if (senderName.empty()) {
                                    diagnosticMessageDelegate(
                                        pluginName,
                                        level,
                                        message
                                    );
                                } else {
                                    diagnosticMessageDelegate(
                                        SystemAbstractions::sprintf(
                                            "%s/%s",
                                            pluginName.c_str(),
                                            senderName.c_str()
                                        ),
                                        level,
                                        message
                                    );
                                }
                            },
                            unloadDelegate
                        );
                        if (unloadDelegate == nullptr) {
                            diagnosticMessageDelegate(
                                "",
                                SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                                SystemAbstractions::sprintf(
                                    "plugin '%s' failed to load",
                                    pluginName.c_str()
                                )
                            );
                        }
                    } else {
                        diagnosticMessageDelegate(
                            "",
                            SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                            SystemAbstractions::sprintf(
                                "unable to find plugin '%s' entrypoint",
                                pluginName.c_str()
                            )
                        );
                    }
                    if (unloadDelegate == nullptr) {
                        runtimeLibrary.Unload();
                    }
                } else {
                    diagnosticMessageDelegate(
                        "",
                        SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                        SystemAbstractions::sprintf(
                            "unable to link plugin '%s' library",
                            pluginName.c_str()
                        )
                    );
                }
                if (unloadDelegate == nullptr) {
                    runtimeFile.Destroy();
                }
            } else {
                diagnosticMessageDelegate(
                    "",
                    SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                    SystemAbstractions::sprintf(
                        "unable to copy plugin '%s' library",
                        pluginName.c_str()
                    )
                );
            }
        }

        /**
         * This method cleanly unloads the plug-in, following this
         * general procedure:
         * 1. Call the "unload" delegate provided by the plug-in.
         *    Typically this delegate will turn around and revoke
         *    its registrations with the web server.
         * 2. Release the unload delegate (by assigning nullptr
         *    to the variable holding onto it.  Typically this
         *    will cause any state captured in the unload delegate
         *    to be destroyed/freed.  After this, it should be
         *    safe to unlink the plug-in code.
         * 3. Unlink the plug-in code.
         */
        void Unload() {
            if (unloadDelegate == nullptr) {
                return;
            }
            unloadDelegate();
            unloadDelegate = nullptr;
            runtimeLibrary.Unload();
        }
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
     * @param[in] configuration
     *     This holds all of the server's configuration items.
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
        const Json::Json& configuration,
        const Environment& environment
    ) {
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
        const Json::Json& configuration,
        const Environment& environment,
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate
    ) {
        SystemAbstractions::DirectoryMonitor directoryMonitor;
        std::string pluginsImagePath = environment.pluginsImagePath;
        if (configuration.Has("plugins-image")) {
            pluginsImagePath = *configuration["plugins-image"];
        }
        std::string pluginsRuntimePath = environment.pluginsRuntimePath;
        if (configuration.Has("plugins-runtime")) {
            pluginsRuntimePath = *configuration["plugins-runtime"];
        }
        std::map< std::string, std::shared_ptr< Plugin > > plugins;
        const auto pluginsEntries = configuration["plugins"];
        const auto pluginsEnabled = configuration["plugins-enabled"];
        std::string moduleExtension;
#if _WIN32
        moduleExtension = ".dll";
#elif defined(APPLE)
        moduleExtension = ".dylib";
#else
        moduleExtension = ".so";
#endif
        for (size_t i = 0; i < pluginsEnabled->GetSize(); ++i) {
            const std::string pluginName = *(*pluginsEnabled)[i];
            if (pluginsEntries->Has(pluginName)) {
                const auto pluginEntry = (*pluginsEntries)[pluginName];
                const std::string pluginModule = *(*pluginEntry)["module"];
                const auto plugin = plugins[pluginName] = std::make_shared< Plugin >(
                    pluginsImagePath + "/" + pluginModule + moduleExtension,
                    pluginsRuntimePath + "/" + pluginModule + moduleExtension
                );
                plugin->moduleName = pluginModule;
                plugin->configuration = (*pluginEntry)["configuration"];
                plugin->lastModifiedTime = plugin->imageFile.GetLastModifiedTime();
            }
        }
        const auto pluginScanDelegate = [
            &server,
            &plugins,
            configuration,
            diagnosticMessageDelegate,
            pluginsImagePath,
            pluginsRuntimePath
        ]{
            for (auto& plugin: plugins) {
                if (
                    (plugin.second->unloadDelegate == nullptr)
                    && plugin.second->imageFile.IsExisting()
                ) {
                    plugin.second->Load(
                        plugin.first,
                        pluginsRuntimePath,
                        server,
                        diagnosticMessageDelegate
                    );
                }
            }
        };
        pluginScanDelegate();
        if (!directoryMonitor.Start(pluginScanDelegate, pluginsImagePath)) {
            fprintf(stderr, "warning: unable to monitor plug-ins image directory (%s)\n", pluginsImagePath.c_str());
        }
        while (!shutDown) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        directoryMonitor.Stop();
        for (auto& plugin: plugins) {
            plugin.second->Unload();
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
    auto transport = std::make_shared< HttpNetworkTransport::HttpServerNetworkTransport >();
    Http::Server server;
    const auto diagnosticsPublisher = SystemAbstractions::DiagnosticsStreamReporter(stdout, stderr);
    const auto diagnosticsSubscription = server.SubscribeToDiagnostics(diagnosticsPublisher);
    const auto configuration = ReadConfiguration(environment);
    if (!ConfigureAndStartServer(server, transport, configuration, environment)) {
        return EXIT_FAILURE;
    }
    printf("Web server up and running.\n");
    MonitorServer(server, configuration, environment, diagnosticsPublisher);
    (void)signal(SIGINT, previousInterruptHandler);
    printf("Exiting...\n");
    return EXIT_SUCCESS;
}
