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
    diagnosticMessageDelegate("", 0, SystemAbstractions::sprintf("Copying plug-in '%s'", pluginName.c_str()));
    if (imageFile.Copy(runtimeFile.GetPath())) {
        diagnosticMessageDelegate("", 0, SystemAbstractions::sprintf("Linking plug-in '%s'", pluginName.c_str()));
        if (runtimeLibrary.Load(pluginsRuntimePath, moduleName)) {
            diagnosticMessageDelegate("", 0, SystemAbstractions::sprintf("Locating plug-in '%s' entrypoint", pluginName.c_str()));
            const auto loadPlugin = (
                void(*)(
                    Http::Server& server,
                    Json::Json configuration,
                    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate,
                    std::function< void() >& unloadDelegate
                )
            )runtimeLibrary.GetProcedure("LoadPlugin");
            if (loadPlugin != nullptr) {
                diagnosticMessageDelegate("", 0, SystemAbstractions::sprintf("Loading plug-in '%s'", pluginName.c_str()));
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
                    needsLoading = false;
                } else {
                    diagnosticMessageDelegate("", 1, SystemAbstractions::sprintf("Plug-in '%s' loaded", pluginName.c_str()));
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
                needsLoading = false;
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
            needsLoading = false;
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

void Plugin::Unload(
    const std::string& pluginName,
    SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate diagnosticMessageDelegate
) {
    if (unloadDelegate == nullptr) {
        return;
    }
    diagnosticMessageDelegate("", 0, SystemAbstractions::sprintf("Unloading plug-in '%s'", pluginName.c_str()));
    unloadDelegate();
    unloadDelegate = nullptr;
    runtimeLibrary.Unload();
    diagnosticMessageDelegate("", 1, SystemAbstractions::sprintf("Plug-in '%s' unloaded", pluginName.c_str()));
}