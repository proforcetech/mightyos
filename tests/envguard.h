#pragma once

#include <QByteArray>
#include <QtGlobal>

// Sets (or unsets) an environment variable for the lifetime of the guard
// and restores the previous state afterwards, even if a QVERIFY returns
// early from the test function.
class EnvGuard
{
public:
    EnvGuard(const char *name, const QByteArray &value)
        : m_name(name), m_wasSet(qEnvironmentVariableIsSet(name)), m_old(qgetenv(name))
    {
        qputenv(name, value);
    }
    ~EnvGuard()
    {
        if (m_wasSet)
            qputenv(m_name.constData(), m_old);
        else
            qunsetenv(m_name.constData());
    }
    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;

private:
    QByteArray m_name;
    bool m_wasSet;
    QByteArray m_old;
};
