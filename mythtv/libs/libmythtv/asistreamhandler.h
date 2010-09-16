// -*- Mode: c++ -*-

#ifndef _ASISTREAMHANDLER_H_
#define _ASISTREAMHANDLER_H_

#include <vector>
using namespace std;

#include <QMutex>
#include <QMap>

#include "streamhandler.h"
#include "util.h"

class QString;
class ASIStreamHandler;
class DTVSignalMonitor;
class ASIChannel;
class DeviceReadBuffer;

typedef QMap<uint,int> FilterMap;

typedef enum ASIClockSource
{
    kASIInternalClock         = 0,
    kASIExternalClock         = 1,
    kASIRecoveredReceiveClock = 2,
    kASIExternalClock2        = 1,
} ASIClockSource;

typedef enum ASIRXMode
{
    kASIRXRawMode                  = 0,
    kASIRXSyncOn188                = 1,
    kASIRXSyncOn204                = 2,
    kASIRXSyncOnActualSize         = 3,
    kASIRXSyncOnActualConvertTo188 = 4,
    kASIRXSyncOn204ConvertTo188    = 5,
} ASIRXMode;


//#define RETUNE_TIMEOUT 5000

// Note : This class always uses a DRB && a TS reader.

class ASIStreamHandler : public StreamHandler
{
  public:
    static ASIStreamHandler *Get(const QString &devicename);
    static void Return(ASIStreamHandler * & ref);

    virtual void AddListener(MPEGStreamData *data,
                             bool allow_section_reader = false,
                             bool needs_drb            = false)
    {
        StreamHandler::AddListener(data, false, true);
    } // StreamHandler

    void SetClockSource(ASIClockSource cs);
    void SetRXMode(ASIRXMode m);

  private:
    ASIStreamHandler(const QString &);
    ~ASIStreamHandler();

    bool Open(void);
    void Close(void);

    virtual void run(void); // QThread

    virtual void PriorityEvent(int fd); // DeviceReaderCB

  private:
    int                                     _device_num;
    int                                     _buf_size;
    int                                     _fd;
    uint                                    _packet_size;
    ASIClockSource                          _clock_source;
    ASIRXMode                               _rx_mode;

    // for implementing Get & Return
    static QMutex                           _handlers_lock;
    static QMap<QString, ASIStreamHandler*> _handlers;
    static QMap<QString, uint>              _handlers_refcnt;
};

#endif // _ASISTREAMHANDLER_H_
