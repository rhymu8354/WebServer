#ifndef PLUGIN_LOADER_HPP
#define PLUGIN_LOADER_HPP

/**
 * @file PluginLoader.hpp
 *
 * This module declares the PluginLoader class.
 *
 * © 2018 by Richard Walters
 */

#include "Plugin.hpp"

#include <Http/Server.hpp>
#include <map>
#include <memory>
#include <string>
#include <SystemAbstractions/DiagnosticsSender.hpp>

/**
 * This class monitors the directory containing the image files
 * for the plug-ins of the web server.  For any image files
 * that are found to match a known plug-in, the class will attempt
 * to load the plug-in, or reload it if the image file changes.
 */
class PluginLoader {
    // Lifecycle Methods
public:
    ~PluginLoader() noexcept;
    PluginLoader(const PluginLoader&) = delete;
    PluginLoader(PluginLoader&&) noexcept = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;
    PluginLoader& operator=(PluginLoader&&) noexcept = delete;

    // Public Methods
public:
    /**
     * This is the constructor of the class.
     *
     * @param[in,out] server
     *     This is the server for which to handle plug-in loading.
     *
     * @param[in,out] plugins
     *     This is the dictionary of plug-ins that are known and configured
     *     for use by the web server.
     *
     * @param[in] imagePath
     *     This is the path to the folder which will be monitored
     *     for plug-ins to be added, changed, and removed.
     *     These are the "originals" of the plug-ins, and will
     *     be copied to another folder before loading them.
     *
     * @param[in] runtimePath
     *     This is the path to the folder where copies of all
     *     plug-ins to be loaded will be made.
     *
     * @param[in] diagnosticMessageDelegate
     *     This is the function to call to publish any diagnostic messages.
     */
    PluginLoader(
        Http::Server& server,
        std::map< std::string, std::shared_ptr< Plugin > >& plugins,
        std::string imagePath,
        std::string runtimePath,
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate
    );

    /**
     * This method is called to perform manual scanning of the plug-in
     * image folder, looking for plug-ins to load.
     */
    void Scan();

    /**
     * This method starts a worker thread which will monitor the image
     * folder for plug-ins, loading, reloading, or unloading plug-ins
     * as necessary.
     */
    void StartBackgroundScanning();

    /**
     * This method stops the worker thread which monitors the image folder.
     */
    void StopBackgroundScanning();

    // Private properties
private:
    /**
     * This is the type of structure that contains the private
     * properties of the instance.  It is defined in the implementation
     * and declared here to ensure that it is scoped inside the class.
     */
    struct Impl;

    /**
     * This contains the private properties of the instance.
     */
    std::unique_ptr< struct Impl > impl_;
};

#endif /* PLUGIN_LOADER_HPP */
