#pragma once
#include <QDebug>
#include <QString>

// ---------------- debug logging (opt-in, off by default) ----------------
// Diagnostics stay silent in normal use. Set AUDIOSCOUT_DEBUG=1 in the
// environment to route the same messages through qDebug() on stderr only —
// no log file is ever created or appended to.
inline bool audioScoutDebugEnabled() {
    static const bool on = qEnvironmentVariableIsSet("AUDIOSCOUT_DEBUG");
    return on;
}

#define DBG(msg) do { \
    if (audioScoutDebugEnabled()) \
        qDebug().noquote() << "[audioscout]" << (msg); \
} while (0)
