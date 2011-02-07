#include <cstdlib>

#include <QMutexLocker>
#include <QString>
#include <QStringList>

#include "mythsystemevent.h"
#include "mythevent.h"
#include "mythsocket.h"
#include "handler.h"
#include "mythverbose.h"
#include "mainserver.h"
#include "mythcorecontext.h"

ProtoSocketHandler::ProtoSocketHandler(MainServer *parent, MythSocket *sock,
                                       QString hostname) :
    m_parent(parent),
    m_sock(sock),
    m_hostname(hostname),
    m_refCount(0)
{
    m_sock->UpRef();
}

ProtoSocketHandler::ProtoSocketHandler(ProtoSocketHandler &other) :
    m_parent(other.m_parent),
    m_sock(other.m_sock),
    m_hostname(other.m_hostname),
    m_refCount(other.m_refCount)
{
    m_sock->UpRef();
}

ProtoSocketHandler::~ProtoSocketHandler()
{
    m_sock->DownRef();
}

void ProtoSocketHandler::UpRef(void)
{
    QMutexLocker locker(&m_refLock);
    m_refCount++;
    VERBOSE(VB_SOCKET, QString("ProtoSocketHandler(): UpRef -> %1").arg(m_refCount));
}

bool ProtoSocketHandler::DownRef(void)
{
    QMutexLocker locker(&m_refLock);

    m_refCount--;
    VERBOSE(VB_SOCKET, QString("ProtoSocketHandler(): DownRef -> %1").arg(m_refCount));
    if (m_refCount < 0)
    {
//        m_parent->DeletePBS(this);
        return true;
    }
    return false;
}

bool ProtoSocketHandler::wantsEvents(void) const
{
    return (m_eventsMode != kPBSEvents_None);
}

bool ProtoSocketHandler::wantsNonSystemEvents(void) const
{
    return ((m_eventsMode == kPBSEvents_Normal) ||
            (m_eventsMode == kPBSEvents_NonSystem));
}

bool ProtoSocketHandler::wantsSystemEvents(void) const
{
    return ((m_eventsMode == kPBSEvents_Normal) ||
            (m_eventsMode == kPBSEvents_SystemOnly));
}

bool ProtoSocketHandler::wantsOnlySystemEvents(void) const
{
    return (m_eventsMode == kPBSEvents_SystemOnly);
}


bool ProtoSocketHandler::SendReceiveStringlist(QStringList &strlist,
                                               uint min_reply_length)
{
    bool ok = false;

    m_sock->Lock();
    m_sock->UpRef();

    {
//        QMutexLocker locker(&m_sockLock);
        m_parent->ExpectingResponse(m_sock, true);

        m_sock->writeStringList(strlist);
        ok = m_sock->readStringList(strlist);

        while (ok && strlist[0] == "BACKEND_MESSAGE")
        {
            // oops, not for us
            if (strlist.size() >= 2)
            {
                QString message = strlist[1];
                strlist.pop_front();
                strlist.pop_front();
                MythEvent me(message, strlist);
                gCoreContext->dispatch(me);
            }

            ok = m_sock->readStringList(strlist);
        }

        m_parent->ExpectingResponse(m_sock, false);
    }

    m_sock->Unlock();
    m_sock->DownRef();

    if (!ok)
    {
        VERBOSE(VB_IMPORTANT,
                "PlaybackSock::SendReceiveStringList(): No response.");
        return false;
    }

    if (min_reply_length && ((uint)strlist.size() < min_reply_length))
    {
        VERBOSE(VB_IMPORTANT,
                "PlaybackSock::SendReceiveStringList(): Response too short");
        return false;
    }

    return true;
}