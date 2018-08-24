#ifndef PLUGIN_HPP
#define PLUGIN_HPP

/**
 * @file Plugin.hpp
 *
 * This module declares the Plugin structure.
 *
 * © 2018 by Richard Walters
 */

#include <functional>
#include <Http/Server.hpp>
#include <memory>
#include <Json/Value.hpp>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <SystemAbstractions/DynamicLibrary.hpp>
#include <SystemAbstractions/File.hpp>
#include <string>
#include <time.h>

/**
 * This is the information tracked for each plug-in.
 */
struct Plugin {
    // Properties

    /**
     * This flag indicates whether or not the web server has
     * determined that the plug-in is a candidate to be loaded.
     */
    bool loadable = true;

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
    Json::Value configuration;

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
    );

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
    );

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
     *
     * @param[in] pluginName
     *     This is the name identifying the plug-in in any diagnostic
     *     messages relating to the plug-in.
     *
     * @param[in] diagnosticMessageDelegate
     *     This is the function to call to publish any diagnostic messages.
     */
    void Unload(
        const std::string& pluginName,
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate
    );
};

#endif /* PLUGIN_HPP */
