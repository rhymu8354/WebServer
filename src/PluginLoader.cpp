/**
 * @file PluginLoader.cpp
 *
 * This module contains the implementation of the PluginLoader class.
 *
 * © 2018 by Richard Walters
 */

#include "PluginLoader.hpp"

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <SystemAbstractions/DirectoryMonitor.hpp>
#include <SystemAbstractions/StringExtensions.hpp>
#include <thread>

/**
 * This contains the private properties of the PluginLoader class.
 */
struct PluginLoader::Impl {
    // Properties

    /**
     * This is the server for which to handle plug-in loading.
     */
    Http::Server& server;

    /**
     * This is the dictionary of plug-ins that are known and configured
     * for use by the web server.
     */
    std::map< std::string, std::shared_ptr< Plugin > >& plugins;

    /**
     * This is the path to the folder which will be monitored
     * for plug-ins to be added, changed, and removed.
     * These are the "originals" of the plug-ins, and will
     * be copied to another folder before loading them.
     */
    std::string imagePath;

    /**
     * This is the path to the folder where copies of all
     * plug-ins to be loaded will be made.
     */
    std::string runtimePath;

    /**
     * This is the function to call to publish any diagnostic messages.
     */
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate;

    /**
     * This is used to receive callbacks from the operating system
     * when any changes are detected to the plug-in image folder
     * or the files it contains.
     */
    SystemAbstractions::DirectoryMonitor directoryMonitor;

    /**
     * This thread is used to perform delayed plug-in loading and unloading
     * in response to changes made to the plug-in image folder.
     */
    std::thread worker;

    /**
     * This is used to signal the worker thread to wake up.
     */
    std::condition_variable wakeCondition;

    /**
     * This is used to synchronize access to the state shared
     * with the worker thread.
     */
    std::mutex mutex;

    /**
     * This flag indicates whether or not the worker thread
     * should perform a plug-in scan.  It's set when any changes
     * are detected to the plug-in image folder or the files
     * is contains.
     */
    bool scan = false;

    /**
     * This flag indicates whether or not the worker thread
     * should exit.
     */
    bool stop = false;

    // Methods

    /**
     * This is the constructor of the implementation structure.
     *
     * @param[in,out] server
     *     This is the server for which to handle plug-in loading.
     *
     * @param[in,out] plugins
     *     This is the dictionary of plug-ins that are known and configured
     *     for use by the web server.
     */
    Impl(
        Http::Server& server,
        std::map< std::string, std::shared_ptr< Plugin > >& plugins
    )
        : server(server)
        , plugins(plugins)
    {
    }

    /**
     * This method is called to perform manual scanning of the plug-in
     * image folder, looking for plug-ins to load.
     */
    void Scan() {
        for (auto& plugin: plugins) {
            if (!plugin.second->imageFile.IsExisting()) {
                continue;
            }
            if (plugin.second->unloadDelegate != nullptr) {
                continue;
            }
            if (!plugin.second->needsLoading) {
                const auto lastModifiedTime = plugin.second->imageFile.GetLastModifiedTime();
                if (plugin.second->lastModifiedTime != lastModifiedTime) {
                    diagnosticMessageDelegate("PluginLoader", 0, SystemAbstractions::sprintf("plugin '%s' appears to have changed", plugin.first.c_str()));
                    plugin.second->needsLoading = true;
                    plugin.second->lastModifiedTime = lastModifiedTime;
                }
            }
            if (plugin.second->needsLoading) {
                plugin.second->Load(
                    plugin.first,
                    runtimePath,
                    server,
                    diagnosticMessageDelegate
                );
                if (
                    (plugin.second->unloadDelegate == nullptr)
                    && plugin.second->needsLoading
                ) {
                    diagnosticMessageDelegate(
                        "PluginLoader",
                        SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                        SystemAbstractions::sprintf(
                            "plugin '%s' failed to copy...will attempt to copy and load again soon",
                            plugin.first.c_str()
                        )
                    );
                    scan = true;
                }
            }
        }
    }

    /**
     * This function is called in its own worker thread.
     * It will initiate a plug-in scan whenever the
     * scan flag remains clear after a short wait period after
     * being set.
     */
    void Run() {
        std::unique_lock< std::mutex > lock(mutex);
        diagnosticMessageDelegate("PluginLoader", 0, "starting");
        while (!stop) {
            diagnosticMessageDelegate("PluginLoader", 0, "sleeping");
            (void)wakeCondition.wait(
                lock,
                [this]{ return scan || stop; }
            );
            diagnosticMessageDelegate("PluginLoader", 0, "waking");
            if (stop) {
                break;
            }
            if (scan) {
                diagnosticMessageDelegate("PluginLoader", 0, "need scan...waiting");
                scan = false;
                if (
                    wakeCondition.wait_for(
                        lock,
                        std::chrono::milliseconds(100),
                        [this] { return scan || stop; }
                    )
                ) {
                    diagnosticMessageDelegate("PluginLoader", 0, "need scan, but updates still happening; backing off");
                } else {
                    diagnosticMessageDelegate("PluginLoader", 0, "scanning");
                    Scan();
                }
            }
        }
        diagnosticMessageDelegate("PluginLoader", 0, "stopping");
    }

};

PluginLoader::~PluginLoader() {
    StopBackgroundScanning();
}

PluginLoader::PluginLoader(
    Http::Server& server,
    std::map< std::string, std::shared_ptr< Plugin > >& plugins,
    std::string imagePath,
    std::string runtimePath,
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate
)
    : impl_(new Impl(server, plugins))
{
    impl_->imagePath = imagePath;
    impl_->runtimePath = runtimePath;
    impl_->diagnosticMessageDelegate = diagnosticMessageDelegate;
}

void PluginLoader::Scan() {
    std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
    if (!impl_->worker.joinable()) {
        impl_->Scan();
    }
}

void PluginLoader::StartBackgroundScanning() {
    if (impl_->worker.joinable()) {
        return;
    }
    const auto imagePathChangedDelegate = [this]{
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        impl_->scan = true;
        impl_->wakeCondition.notify_all();
    };
    if (!impl_->directoryMonitor.Start(imagePathChangedDelegate, impl_->imagePath)) {
        fprintf(stderr, "warning: unable to monitor plug-ins image directory (%s)\n", impl_->imagePath.c_str());
        return;
    }
    impl_->stop = false;
    impl_->worker = std::thread(&Impl::Run, impl_.get());
}

void PluginLoader::StopBackgroundScanning() {
    if (!impl_->worker.joinable()) {
        return;
    }
    impl_->directoryMonitor.Stop();
    {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        impl_->stop = true;
        impl_->wakeCondition.notify_all();
    }
    impl_->worker.join();
}
