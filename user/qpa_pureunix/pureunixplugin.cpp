#include "pureunixplugin.h"
#include "pureunixintegration.h"

QPlatformIntegration *QPureUnixIntegrationPlugin::create(const QString &system, const QStringList &paramList)
{
    if (system.compare(QLatin1String("pureunix"), Qt::CaseInsensitive) == 0) {
        return new QPureUnixIntegration(paramList);
    }
    return nullptr;
}

#include "moc_pureunixplugin.cpp"
