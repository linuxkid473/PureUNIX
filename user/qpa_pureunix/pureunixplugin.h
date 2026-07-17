// user/qpa_pureunix/pureunixplugin.h — the plugin entry point itself.
// PureUnix has no dynamic plugin loading at all (see docs/qt-port.md
// section 2), so this is always statically linked and registered via
// Q_IMPORT_PLUGIN(QPureUnixIntegrationPlugin) in the consuming program's
// own source, exactly like the vendored qminimal/qoffscreen plugins
// already are (user/qtguitest.cpp) -- the real, standard Qt mechanism for
// a statically-built platform plugin, not a PureUnix-specific shortcut.
#ifndef PUREUNIX_QPA_PLUGIN_H
#define PUREUNIX_QPA_PLUGIN_H

#include <qpa/qplatformintegrationplugin.h>

class QPureUnixIntegrationPlugin : public QPlatformIntegrationPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "pureunix.json")
public:
    QPlatformIntegration *create(const QString &system, const QStringList &paramList) override;
};

#endif
