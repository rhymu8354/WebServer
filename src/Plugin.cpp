/**
 * @file Plugin.cpp
 *
 * This module contains the implementations of the Plugin
 * structure methods.
 *
 * © 2018 by Richard Walters
 */

#include "Plugin.hpp"

#include <SystemAbstractions/StringExtensions.hpp>
#include <WebServer/PluginEntryPoint.hpp>

Plugin::Plugin(
    const std::string& imageFileName,
    const std::string& runtimeFileName
)
    : imageFile(imageFileName)
    , runtimeFile(runtimeFileName)
{
}

void Plugin::Load(
    const std::string& pluginName,
    const std::string& pluginsRuntimePath,
    Http::Server& server,
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate
) {
    diagnosticMessageDelegate("WebServer", 0, SystemAbstractions::sprintf("Copying plug-in '%s'", pluginName.c_str()));
    if (imageFile.Copy(runtimeFile.GetPath())) {
        diagnosticMessageDelegate("WebServer", 0, SystemAbstractions::sprintf("Linking plug-in '%s'", pluginName.c_str()));
        if (runtimeLibrary.Load(pluginsRuntimePath, moduleName)) {
            diagnosticMessageDelegate("WebServer", 0, SystemAbstractions::sprintf("Locating plug-in '%s' entrypoint", pluginName.c_str()));
            const auto loadPlugin = (PluginEntryPoint)runtimeLibrary.GetProcedure("LoadPlugin");
            if (loadPlugin != nullptr) {
                diagnosticMessageDelegate("WebServer", 0, SystemAbstractions::sprintf("Loading plug-in '%s'", pluginName.c_str()));
                loadPlugin(
                    &server,
                    configuration,
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
                    loadable = false;
                } else {
                    diagnosticMessageDelegate("WebServer", 1, SystemAbstractions::sprintf("Plug-in '%s' loaded", pluginName.c_str()));
                }
            } else {
                diagnosticMessageDelegate(
                    "WebServer",
                    SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                    SystemAbstractions::sprintf(
                        "unable to find plugin '%s' entrypoint",
                        pluginName.c_str()
                    )
                );
                loadable = false;
            }
            if (unloadDelegate == nullptr) {
                runtimeLibrary.Unload();
            }
        } else {
            diagnosticMessageDelegate(
                "WebServer",
                SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                SystemAbstractions::sprintf(
                    "unable to link plugin '%s' library",
                    pluginName.c_str()
                )
            );
            loadable = false;
        }
        if (unloadDelegate == nullptr) {
            runtimeFile.Destroy();
        }
    } else {
        diagnosticMessageDelegate(
            "WebServer",
            SystemAbstractions::DiagnosticsSender::Levels::WARNING,
            SystemAbstractions::sprintf(
                "unable to copy plugin '%s' library",
                pluginName.c_str()
            )
        );
    }
}

void Plugin::Unload(
    const std::string& pluginName,
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate
) {
    if (unloadDelegate == nullptr) {
        return;
    }
    diagnosticMessageDelegate("WebServer", 0, SystemAbstractions::sprintf("Unloading plug-in '%s'", pluginName.c_str()));
    unloadDelegate();
    unloadDelegate = nullptr;
    runtimeLibrary.Unload();
    diagnosticMessageDelegate("WebServer", 1, SystemAbstractions::sprintf("Plug-in '%s' unloaded", pluginName.c_str()));
}
