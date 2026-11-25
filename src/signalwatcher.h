#pragma once

#include <QList>
#include <QObject>

class QSocketNotifier;

// Unix signals into the Qt event loop (self-pipe): the handler only
// write()s the signal number to a non-blocking O_CLOEXEC pipe; a
// QSocketNotifier reads it on the GUI thread and emits received().
//
// A self-pipe rather than signalfd: signalfd needs the signals blocked in
// every thread, and a blocked mask is inherited by launched apps across
// execve() (a game that can't receive SIGTERM). Handlers are reset in
// children anyway (QProcess ResetSignalHandlers).
//
// MightyOS uses (main.cpp):
//  - SIGTERM, SIGINT, SIGHUP: clean quit (logind ending the session, Ctrl+C
//    in a dev terminal, the tty hanging up): navstate / settings flushed,
//    the running app's scope stopped,
//  - SIGUSR1 / SIGUSR2 / SIGRTMIN+1: the labwc panic keys through
//    `mightyos --signal=guide|close|quit` (session/labwc/rc.xml.in): Guide
//    menu, close the running app, leave the session (MightyOS decides).
//
// One instance per process (the pipe and the handlers are process-wide).
// The destructor restores the previous dispositions.
class SignalWatcher : public QObject
{
    Q_OBJECT

public:
    explicit SignalWatcher(const QList<int> &signalNumbers, QObject *parent = nullptr);
    ~SignalWatcher() override;

    // False if the pipe or a handler could not be set up (logged).
    bool isActive() const { return m_active; }
    QList<int> watched() const { return m_signals; }

signals:
    void received(int signalNumber);

private:
    void drain();

    QList<int> m_signals;
    QSocketNotifier *m_notifier = nullptr;
    bool m_active = false;
};
