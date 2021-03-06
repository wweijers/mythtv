// POSIX headers
#include <unistd.h>
#include <sys/time.h> // for gettimeofday

// ANSI C headers
#include <cmath>

// C++ headers
#include <algorithm> // for min/max
using namespace std;

// Qt headers
#include <QString>

// MythTV headers
#include "mythcontext.h"
#include "programinfo.h"
#include "mythplayer.h"

// Commercial Flagging headers
#include "ClassicCommDetector.h"
#include "ClassicLogoDetector.h"
#include "ClassicSceneChangeDetector.h"

enum frameAspects {
    COMM_ASPECT_NORMAL = 0,
    COMM_ASPECT_WIDE
} FrameAspects;

enum frameFormats {
    COMM_FORMAT_NORMAL = 0,
    COMM_FORMAT_LETTERBOX,
    COMM_FORMAT_PILLARBOX,
    COMM_FORMAT_MAX
} FrameFormats;

static QString toStringFrameMaskValues(int mask, bool verbose)
{
    QString msg;

    if (verbose)
    {
        if (COMM_FRAME_SKIPPED & mask)
            msg += "skipped,";
        if (COMM_FRAME_BLANK & mask)
            msg += "blank,";
        if (COMM_FRAME_SCENE_CHANGE & mask)
            msg += "scene,";
        if (COMM_FRAME_LOGO_PRESENT & mask)
            msg += "logo,";
        if (COMM_FRAME_ASPECT_CHANGE & mask)
            msg += "aspect,";
        if (COMM_FRAME_RATING_SYMBOL & mask)
            msg += "rating,";

        if (msg.length())
            msg = msg.left(msg.length() - 1);
        else
            msg = "noflags";
    }
    else
    {
        msg += (COMM_FRAME_SKIPPED       & mask) ? "s" : " ";
        msg += (COMM_FRAME_BLANK         & mask) ? "B" : " ";
        msg += (COMM_FRAME_SCENE_CHANGE  & mask) ? "S" : " ";
        msg += (COMM_FRAME_LOGO_PRESENT  & mask) ? "L" : " ";
        msg += (COMM_FRAME_ASPECT_CHANGE & mask) ? "A" : " ";
        msg += (COMM_FRAME_RATING_SYMBOL & mask) ? "R" : " ";
    }

    return msg;
}

static QString toStringFrameAspects(int aspect, bool verbose)
{
    if (verbose)
        return (COMM_ASPECT_NORMAL == aspect) ? "normal" : " wide ";
    else
        return (COMM_ASPECT_NORMAL == aspect) ? "n" : "w";
}

static QString toStringFrameFormats(int format, bool verbose)
{
    switch (format)
    {
        case COMM_FORMAT_NORMAL:
            return (verbose) ? "normal" : "N";
        case COMM_FORMAT_LETTERBOX:
            return (verbose) ? "letter" : "L";
        case COMM_FORMAT_PILLARBOX:
            return (verbose) ? "pillar" : "P";
        case COMM_FORMAT_MAX:
            return (verbose) ? " max  " : "M";
    }

    return (verbose) ? " null " : "n";
}

QString FrameInfoEntry::GetHeader(void)
{
    return QString("  frame     min/max/avg scene aspect format flags");
}

QString FrameInfoEntry::toString(uint64_t frame, bool verbose) const
{
    return QString(
        "%1: %2/%3/%4 %5%  %6 %7 %8")
        .arg(frame,10)
        .arg(minBrightness,3)
        .arg(maxBrightness,3)
        .arg(avgBrightness,3)
        .arg(sceneChangePercent,3)
        .arg(toStringFrameAspects(aspect, verbose))
        .arg(toStringFrameFormats(format, verbose))
        .arg(toStringFrameMaskValues(flagMask, verbose));
}

ClassicCommDetector::ClassicCommDetector(SkipType commDetectMethod_in,
                                         bool showProgress_in,
                                         bool fullSpeed_in,
                                         MythPlayer* player_in,
                                         const QDateTime& startedAt_in,
                                         const QDateTime& stopsAt_in,
                                         const QDateTime& recordingStartedAt_in,
                                         const QDateTime& recordingStopsAt_in) :


    commDetectMethod(commDetectMethod_in),
    commBreakMapUpdateRequested(false),        sendCommBreakMapUpdates(false),
    verboseDebugging(false),
    lastFrameNumber(0),                        curFrameNumber(0),
    width(0),                                  height(0),
    horizSpacing(0),                           vertSpacing(0),
    fpm(0.0),                                  blankFramesOnly(false),
    blankFrameCount(0),                        currentAspect(0),
    totalMinBrightness(0),                     detectBlankFrames(false),
    detectSceneChanges(false),                 detectStationLogo(false),
    logoInfoAvailable(false),                  logoDetector(0),
    framePtr(0),                               frameIsBlank(false),
    sceneHasChanged(false),                    stationLogoPresent(false),
    lastFrameWasBlank(false),                  lastFrameWasSceneChange(false),
    decoderFoundAspectChanges(false),          sceneChangeDetector(0),
    player(player_in),
    startedAt(startedAt_in),                   stopsAt(stopsAt_in),
    recordingStartedAt(recordingStartedAt_in),
    recordingStopsAt(recordingStopsAt_in),     aggressiveDetection(false),
    stillRecording(recordingStopsAt > QDateTime::currentDateTime()),
    fullSpeed(fullSpeed_in),                   showProgress(showProgress_in),
    fps(0.0),                                  framesProcessed(0),
    preRoll(0),                                postRoll(0)
{
    commDetectBorder =
        gCoreContext->GetNumSetting("CommDetectBorder", 20);
    commDetectBlankFrameMaxDiff =
        gCoreContext->GetNumSetting("CommDetectBlankFrameMaxDiff", 25);
    commDetectDarkBrightness =
        gCoreContext->GetNumSetting("CommDetectDarkBrightness", 80);
    commDetectDimBrightness =
        gCoreContext->GetNumSetting("CommDetectDimBrightness", 120);
    commDetectBoxBrightness =
        gCoreContext->GetNumSetting("CommDetectBoxBrightness", 30);
    commDetectDimAverage =
        gCoreContext->GetNumSetting("CommDetectDimAverage", 35);
    commDetectMaxCommBreakLength =
        gCoreContext->GetNumSetting("CommDetectMaxCommBreakLength", 395);
    commDetectMinCommBreakLength =
        gCoreContext->GetNumSetting("CommDetectMinCommBreakLength", 60);
    commDetectMinShowLength =
        gCoreContext->GetNumSetting("CommDetectMinShowLength", 65);
    commDetectMaxCommLength =
        gCoreContext->GetNumSetting("CommDetectMaxCommLength", 125);

    commDetectBlankCanHaveLogo =
        !!gCoreContext->GetNumSetting("CommDetectBlankCanHaveLogo", 1);
}

void ClassicCommDetector::Init()
{
    QSize video_disp_dim = player->GetVideoSize();
    width  = video_disp_dim.width();
    height = video_disp_dim.height();
    fps = player->GetFrameRate();

    preRoll  = (long long)(max(0,recordingStartedAt.secsTo(startedAt)) * fps);
    postRoll = (long long)(max(0,stopsAt.secsTo(recordingStopsAt)) * fps);

#ifdef SHOW_DEBUG_WIN
    comm_debug_init(width, height);
#endif

    currentAspect = COMM_ASPECT_WIDE;

    lastFrameNumber = -2;
    curFrameNumber = -1;

    if (getenv("DEBUGCOMMFLAG"))
        verboseDebugging = true;
    else
        verboseDebugging = false;

    VERBOSE(VB_COMMFLAG, "Commercial Detection initialized: width = " <<
            width << ", height = " << height << ", fps = " <<
            player->GetFrameRate() << ", method = " << commDetectMethod);

    if ((width * height) > 1000000)
    {
        horizSpacing = 10;
        vertSpacing = 10;
    }
    else if ((width * height) > 800000)
    {
        horizSpacing = 8;
        vertSpacing = 8;
    }
    else if ((width * height) > 400000)
    {
        horizSpacing = 6;
        vertSpacing = 6;
    }
    else if ((width * height) > 300000)
    {
        horizSpacing = 6;
        vertSpacing = 4;
    }
    else
    {
        horizSpacing = 4;
        vertSpacing = 4;
    }

    VERBOSE(VB_COMMFLAG,
            QString("Using Sample Spacing of %1 horizontal & %2 vertical "
                    "pixels.").arg(horizSpacing).arg(vertSpacing));

    framesProcessed = 0;
    totalMinBrightness = 0;
    blankFrameCount = 0;

    aggressiveDetection = true;
    currentAspect = COMM_ASPECT_WIDE;
    decoderFoundAspectChanges = false;

    lastSentCommBreakMap.clear();

    // Check if close to 4:3
    if (fabs(((width*1.0)/height) - 1.333333) < 0.1)
        currentAspect = COMM_ASPECT_NORMAL;

    sceneChangeDetector = new ClassicSceneChangeDetector(width, height,
        commDetectBorder, horizSpacing, vertSpacing);
    connect(
         sceneChangeDetector, 
         SIGNAL(haveNewInformation(unsigned int,bool,float)), 
         this, 
         SLOT(sceneChangeDetectorHasNewInformation(unsigned int,bool,float))
    );

    frameIsBlank = false;
    stationLogoPresent = false;

    framePtr = NULL;

    logoInfoAvailable = false;

    ClearAllMaps();

    if (verboseDebugging)
    {
        VERBOSE(VB_COMMFLAG, "       Fr #      Min Max Avg Scn F A Mask");
        VERBOSE(VB_COMMFLAG, "       ------    --- --- --- --- - - ----");
    }
}

void ClassicCommDetector::deleteLater(void)
{
    if (sceneChangeDetector)
        sceneChangeDetector->deleteLater();

    if (logoDetector)
        logoDetector->deleteLater();

    CommDetectorBase::deleteLater();
}

bool ClassicCommDetector::go()
{
    player->SetNullVideo();

    int secsSince = 0;
    int requiredBuffer = 30;
    int requiredHeadStart = requiredBuffer;
    bool wereRecording = stillRecording;

    emit statusUpdate(QObject::tr("Building Head Start Buffer"));
    secsSince = recordingStartedAt.secsTo(QDateTime::currentDateTime());
    while (stillRecording && (secsSince < requiredHeadStart))
    {
        emit breathe();
        if (m_bStop)
            return false;

        sleep(2);
        secsSince = recordingStartedAt.secsTo(QDateTime::currentDateTime());
    }

    if (player->OpenFile() < 0)
        return false;

    Init();

    if (commDetectMethod & COMM_DETECT_LOGO)
    {
        logoDetector = new ClassicLogoDetector(this, width, height,
            commDetectBorder, horizSpacing, vertSpacing);

        requiredHeadStart += max(0,recordingStartedAt.secsTo(startedAt));
        requiredHeadStart += logoDetector->getRequiredAvailableBufferForSearch();

        emit statusUpdate(QObject::tr("Building Logo Detection Buffer"));
        secsSince = recordingStartedAt.secsTo(QDateTime::currentDateTime());
        while (stillRecording && (secsSince < requiredHeadStart))
        {
            emit breathe();
            if (m_bStop)
                return false;

            sleep(2);
            secsSince = recordingStartedAt.secsTo(QDateTime::currentDateTime());
        }
    }

    // Don't bother flagging short ~realtime recordings
    if ((wereRecording) && (!stillRecording) && (secsSince < requiredHeadStart))
        return false;

    aggressiveDetection = gCoreContext->GetNumSetting("AggressiveCommDetect", 1);

    if (!player->InitVideo())
    {
        VERBOSE(VB_IMPORTANT,
                "NVP: Unable to initialize video for FlagCommercials.");
        return false;
    }
    player->EnableSubtitles(false);

    if (commDetectMethod & COMM_DETECT_LOGO)
    {
        emit statusUpdate(QObject::tr("Searching for Logo"));

        if (showProgress)
        {
            cerr << "Finding Logo";
            cerr.flush();
        }

        logoInfoAvailable = logoDetector->searchForLogo(player);

        if (showProgress)
        {
            cerr << "\b\b\b\b\b\b\b\b\b\b\b\b            "
                    "\b\b\b\b\b\b\b\b\b\b\b\b";
            cerr.flush();
        }
    }

    emit breathe();
    if (m_bStop)
        return false;

    QTime flagTime;
    flagTime.start();
    
    long long myTotalFrames;
    if (recordingStopsAt < QDateTime::currentDateTime() )
        myTotalFrames = player->GetTotalFrameCount();
    else
        myTotalFrames = (long long)(player->GetFrameRate() *
                        (recordingStartedAt.secsTo(recordingStopsAt)));

    if (showProgress)
    {
        if (myTotalFrames)
            cerr << "  0%/      ";
        else
            cerr << "     0/      ";
        cerr.flush();
    }

 
    float flagFPS;
    long long  currentFrameNumber;
    float aspect = player->GetVideoAspect();
    float newAspect = aspect;
    int prevpercent = -1;

    SetVideoParams(aspect);

    emit breathe();

    player->ResetTotalDuration();

    while (!player->GetEof())
    {
        struct timeval startTime;
        if (stillRecording)
            gettimeofday(&startTime, NULL);

        VideoFrame* currentFrame = player->GetRawVideoFrame();
        currentFrameNumber = currentFrame->frameNumber;

        //Lucas: maybe we should make the nuppelvideoplayer send out a signal
        //when the aspect ratio changes.
        //In order to not change too many things at a time, I"m using basic
        //polling for now.
        newAspect = player->GetVideoAspect();
        if (newAspect != aspect)
        {
            SetVideoParams(aspect);
            aspect = newAspect;
        }

        if (((currentFrameNumber % 500) == 0) ||
            (((currentFrameNumber % 100) == 0) &&
             (stillRecording)))
        {
            emit breathe();
            if (m_bStop)
            {
                player->DiscardVideoFrame(currentFrame);
                return false;
            }
        }

        if ((sendCommBreakMapUpdates) &&
            ((commBreakMapUpdateRequested) ||
             ((currentFrameNumber % 500) == 0)))
        {
            frm_dir_map_t commBreakMap;
            frm_dir_map_t::iterator it;
            frm_dir_map_t::iterator lastIt;
            bool mapsAreIdentical = false;

            GetCommercialBreakList(commBreakMap);

            if ((commBreakMap.size() == 0) &&
                (lastSentCommBreakMap.size() == 0))
            {
                mapsAreIdentical = true;
            }
            else if (commBreakMap.size() == lastSentCommBreakMap.size())
            {
                // assume true for now and set false if we find a difference
                mapsAreIdentical = true;
                for (it = commBreakMap.begin();
                     it != commBreakMap.end() && mapsAreIdentical; ++it)
                {
                    lastIt = lastSentCommBreakMap.find(it.key());
                    if ((lastIt == lastSentCommBreakMap.end()) ||
                        (*lastIt != *it))
                        mapsAreIdentical = false;
                }
            }

            if (commBreakMapUpdateRequested || !mapsAreIdentical)
            {
                emit gotNewCommercialBreakList();
                lastSentCommBreakMap = commBreakMap;
            }

            if (commBreakMapUpdateRequested)
                commBreakMapUpdateRequested = false;
        }

        while (m_bPaused)
        {
            emit breathe();
            sleep(1);
        }

        // sleep a little so we don't use all cpu even if we're niced
        if (!fullSpeed && !stillRecording)
            usleep(10000);

        if (((currentFrameNumber % 500) == 0) ||
            ((showProgress || stillRecording) &&
             ((currentFrameNumber % 100) == 0)))
        {
            float elapsed = flagTime.elapsed() / 1000.0;

            if (elapsed)
                flagFPS = currentFrameNumber / elapsed;
            else
                flagFPS = 0.0;

            int percentage;
            if (myTotalFrames)
                percentage = currentFrameNumber * 100 / myTotalFrames;
            else
                percentage = 0;

            if (percentage > 100)
                percentage = 100;

            if (showProgress)
            {
                if (myTotalFrames)
                {
                    QString tmp = QString("\b\b\b\b\b\b\b\b\b\b\b%1%/%2fps")
                        .arg(percentage, 3).arg((int)flagFPS, 3);
                    QByteArray ba = tmp.toAscii();
                    cerr << ba.constData() << flush;
                }
                else
                {
                    QString tmp = QString("\b\b\b\b\b\b\b\b\b\b\b\b\b%1/%2fps")
                        .arg(currentFrameNumber, 6).arg((int)flagFPS, 3);
                    QByteArray ba = tmp.toAscii();
                    cerr << ba.constData() << flush;
                }
                cerr.flush();
            }

            if (myTotalFrames)
                emit statusUpdate(QObject::tr("%1% Completed @ %2 fps.")
                                  .arg(percentage).arg(flagFPS));
            else
                emit statusUpdate(QObject::tr("%1 Frames Completed @ %2 fps.")
                                  .arg(currentFrameNumber).arg(flagFPS));

            if (percentage % 10 == 0 && prevpercent != percentage)
            {
                prevpercent = percentage;
                VERBOSE(VB_GENERAL|VB_EXTRA, QString("%1% Completed @ %2 fps.")
                    .arg(percentage) .arg(flagFPS));
            }
        }

        ProcessFrame(currentFrame, currentFrameNumber);

        if (stillRecording)
        {
            int secondsRecorded =
                recordingStartedAt.secsTo(QDateTime::currentDateTime());
            int secondsFlagged = (int)(framesProcessed / fps);
            int secondsBehind = secondsRecorded - secondsFlagged;
            long usecPerFrame = (long)(1.0 / player->GetFrameRate() * 1000000);

            struct timeval endTime;
            gettimeofday(&endTime, NULL);

            long long usecSleep =
                      usecPerFrame -
                      (((endTime.tv_sec - startTime.tv_sec) * 1000000) +
                       (endTime.tv_usec - startTime.tv_usec));

            if (secondsBehind > requiredBuffer)
            {
                if (fullSpeed)
                    usecSleep = 0;
                else
                    usecSleep = (long)(usecSleep * 0.25);
            }
            else if (secondsBehind < requiredBuffer)
                usecSleep = (long)(usecPerFrame * 1.5);
            
            if (usecSleep > 0)
                usleep(usecSleep);
        }

        player->DiscardVideoFrame(currentFrame);
    }

    if (showProgress)
    {
        float elapsed = flagTime.elapsed() / 1000.0;

        if (elapsed)
            flagFPS = currentFrameNumber / elapsed;
        else
            flagFPS = 0.0;

        if (myTotalFrames)
            cerr << "\b\b\b\b\b\b      \b\b\b\b\b\b";
        else
            cerr << "\b\b\b\b\b\b\b\b\b\b\b\b\b             "
                    "\b\b\b\b\b\b\b\b\b\b\b\b\b";
        cerr.flush();
    }

    return true;
}

void ClassicCommDetector::sceneChangeDetectorHasNewInformation(
    unsigned int framenum,bool isSceneChange,float debugValue)
{
    if (isSceneChange)
    {
        frameInfo[framenum].flagMask |= COMM_FRAME_SCENE_CHANGE;
        sceneMap[framenum] = MARK_SCENE_CHANGE;
    }
    else
    {
        frameInfo[framenum].flagMask &= ~COMM_FRAME_SCENE_CHANGE;
        sceneMap.remove(framenum);
    }

    frameInfo[framenum].sceneChangePercent = (int) (debugValue*100);
}

void ClassicCommDetector::GetCommercialBreakList(frm_dir_map_t &marks)
{

    VERBOSE(VB_COMMFLAG, "CommDetect::GetCommBreakMap()");

    marks.clear();

    CleanupFrameInfo();

    bool blank = COMM_DETECT_BLANK & commDetectMethod;
    bool scene = COMM_DETECT_SCENE & commDetectMethod;
    bool logo  = COMM_DETECT_LOGO  & commDetectMethod;

    if (COMM_DETECT_OFF == commDetectMethod)
        return;

    if (!blank && !scene && !logo)
    {
        VERBOSE(VB_COMMFLAG,
                QString("Unexpected commDetectMethod: 0x%1")
                .arg(commDetectMethod,0,16));
        return;
    }

    if (blank && scene && logo)
    {
        BuildAllMethodsCommList();
        marks = commBreakMap;
        VERBOSE(VB_COMMFLAG, "Final Commercial Break Map");
        return;
    }

    if (blank)
    {
        BuildBlankFrameCommList();
        marks = blankCommBreakMap;
    }

    if (scene)
    {
        BuildSceneChangeCommList();
        marks = sceneCommBreakMap;
    }

    if (logo)
    {
        BuildLogoCommList();
        marks = logoCommBreakMap;
    }

    int cnt = ((blank) ? 1 : 0) + ((scene) ? 1 : 0) + ((logo) ? 1 : 0);
    if (cnt == 2)
    {
        if (blank && scene)
        {
            marks = commBreakMap = Combine2Maps(
                blankCommBreakMap, sceneCommBreakMap);
        }
        else if (blank && logo)
        {
            marks = commBreakMap = Combine2Maps(
                blankCommBreakMap, logoCommBreakMap);
        }
        else if (scene && logo)
        {
            marks = commBreakMap = Combine2Maps(
                sceneCommBreakMap, logoCommBreakMap);
        }
    }

    VERBOSE(VB_COMMFLAG, "Final Commercial Break Map");
}

void ClassicCommDetector::recordingFinished(long long totalFileSize)
{
    (void)totalFileSize;

    stillRecording = false;
}

void ClassicCommDetector::requestCommBreakMapUpdate(void)
{
    commBreakMapUpdateRequested = true;
    sendCommBreakMapUpdates = true;
}

void ClassicCommDetector::SetVideoParams(float aspect)
{
    int newAspect = COMM_ASPECT_WIDE;

    VERBOSE(VB_COMMFLAG, QString("CommDetect::SetVideoParams called with "
                                 "aspect = %1").arg(aspect));
    // Default to Widescreen but use the same check as VideoOutput::MoveResize()
    // to determine if is normal 4:3 aspect
    if (fabs(aspect - 1.333333) < 0.1)
        newAspect = COMM_ASPECT_NORMAL;

    if (newAspect != currentAspect)
    {
        VERBOSE(VB_COMMFLAG,
                QString("Aspect Ratio changed from %1 to %2 at frame %3")
                .arg(currentAspect).arg(newAspect)
                .arg(curFrameNumber));

        if (frameInfo.contains(curFrameNumber))
        {
            // pretend that this frame is blank so that we can create test
            // blocks on real aspect ratio change boundaries.
            frameInfo[curFrameNumber].flagMask |= COMM_FRAME_BLANK;
            frameInfo[curFrameNumber].flagMask |= COMM_FRAME_ASPECT_CHANGE;
            decoderFoundAspectChanges = true;
        }
        else if (curFrameNumber != -1)
        {
            VERBOSE(VB_COMMFLAG, QString("Unable to keep track of Aspect ratio "
                                         "change because frameInfo for frame "
                                         "number %1 does not exist.")
                    .arg(curFrameNumber));
        }
        currentAspect = newAspect;
    }
}

void ClassicCommDetector::ProcessFrame(VideoFrame *frame,
                                       long long frame_number)
{
    int max = 0;
    int min = 255;
    int avg = 0;
    unsigned char pixel;
    int blankPixelsChecked = 0;
    long long totBrightness = 0;
    unsigned char *rowMax = new unsigned char[height];
    unsigned char *colMax = new unsigned char[width];
    memset(rowMax, 0, sizeof(*rowMax)*height);
    memset(colMax, 0, sizeof(*colMax)*width);
    int topDarkRow = commDetectBorder;
    int bottomDarkRow = height - commDetectBorder - 1;
    int leftDarkCol = commDetectBorder;
    int rightDarkCol = width - commDetectBorder - 1;
    FrameInfoEntry fInfo;

    if (!frame || frame_number == -1 || frame->codec != FMT_YV12)
    {
        VERBOSE(VB_COMMFLAG, "CommDetect: Invalid video frame or codec, "
                "unable to process frame.");
        return;
    }

    if (!width || !height)
    {
        VERBOSE(VB_COMMFLAG, "CommDetect: Width or Height is 0, "
                "unable to process frame.");
        return;
    }

    curFrameNumber = frame_number;
    framePtr = frame->buf;

    fInfo.minBrightness = -1;
    fInfo.maxBrightness = -1;
    fInfo.avgBrightness = -1;
    fInfo.sceneChangePercent = -1;
    fInfo.aspect = currentAspect;
    fInfo.format = COMM_FORMAT_NORMAL;
    fInfo.flagMask = 0;

    int& flagMask = frameInfo[curFrameNumber].flagMask;

    // Fill in dummy info records for skipped frames.
    if (lastFrameNumber != (curFrameNumber - 1))
    {
        if (lastFrameNumber > 0)
        {
            fInfo.aspect = frameInfo[lastFrameNumber].aspect;
            fInfo.format = frameInfo[lastFrameNumber].format;
        }
        fInfo.flagMask = COMM_FRAME_SKIPPED;

        lastFrameNumber++;
        while(lastFrameNumber < curFrameNumber)
            frameInfo[lastFrameNumber++] = fInfo;

        fInfo.flagMask = 0;
    }
    lastFrameNumber = curFrameNumber;

    frameInfo[curFrameNumber] = fInfo;

    if (commDetectMethod & COMM_DETECT_BLANKS)
        frameIsBlank = false;

    if (commDetectMethod & COMM_DETECT_SCENE)
    {
        sceneChangeDetector->processFrame(framePtr);
    }

    stationLogoPresent = false;

    for(int y = commDetectBorder; y < (height - commDetectBorder);
            y += vertSpacing)
    {
        for(int x = commDetectBorder; x < (width - commDetectBorder);
                x += horizSpacing)
        {
            pixel = framePtr[y * width + x];

            if (commDetectMethod & COMM_DETECT_BLANKS)
            {
                 bool checkPixel = false;
                 if (!commDetectBlankCanHaveLogo)
                     checkPixel = true;

                 if (!logoInfoAvailable)
                     checkPixel = true;
                 else if (!logoDetector->pixelInsideLogo(x,y))
                     checkPixel=true;

                 if (checkPixel)
                 {
                     blankPixelsChecked++;
                     totBrightness += pixel;
  
                     if (pixel < min)
                          min = pixel;

                     if (pixel > max)
                          max = pixel;

                     if (pixel > rowMax[y])
                         rowMax[y] = pixel;

                     if (pixel > colMax[x])
                         colMax[x] = pixel;
                 }
            }
        }
    }

    if (commDetectMethod & COMM_DETECT_BLANKS)
    {
        for(int y = commDetectBorder; y < (height - commDetectBorder);
                y += vertSpacing)
        {
            if (rowMax[y] > commDetectBoxBrightness)
                break;
            else
                topDarkRow = y;
        }

        for(int y = commDetectBorder; y < (height - commDetectBorder);
                y += vertSpacing)
            if (rowMax[y] >= commDetectBoxBrightness)
                bottomDarkRow = y;

        delete[] rowMax;

        for(int x = commDetectBorder; x < (width - commDetectBorder);
                x += horizSpacing)
        {
            if (colMax[x] > commDetectBoxBrightness)
                break;
            else
                leftDarkCol = x;
        }

        for(int x = commDetectBorder; x < (width - commDetectBorder);
                x += horizSpacing)
            if (colMax[x] >= commDetectBoxBrightness)
                rightDarkCol = x;

        delete[] colMax;

        if ((topDarkRow > commDetectBorder) &&
            (topDarkRow < (height * .20)) &&
            (bottomDarkRow < (height - commDetectBorder)) &&
            (bottomDarkRow > (height * .80)))
        {
            frameInfo[curFrameNumber].format = COMM_FORMAT_LETTERBOX;
        }
        else if ((leftDarkCol > commDetectBorder) &&
                 (leftDarkCol < (width * .20)) &&
                 (rightDarkCol < (width - commDetectBorder)) &&
                 (rightDarkCol > (width * .80)))
        {
            frameInfo[curFrameNumber].format = COMM_FORMAT_PILLARBOX;
        }
        else
        {
            frameInfo[curFrameNumber].format = COMM_FORMAT_NORMAL;
        }

        avg = totBrightness / blankPixelsChecked;

        frameInfo[curFrameNumber].minBrightness = min;
        frameInfo[curFrameNumber].maxBrightness = max;
        frameInfo[curFrameNumber].avgBrightness = avg;

        totalMinBrightness += min;
        commDetectDimAverage = min + 10;

        // Is the frame really dark
        if (((max - min) <= commDetectBlankFrameMaxDiff) &&
            (max < commDetectDimBrightness))
            frameIsBlank = true;

        // Are we non-strict and the frame is blank
        if ((!aggressiveDetection) &&
            ((max - min) <= commDetectBlankFrameMaxDiff))
            frameIsBlank = true;

        // Are we non-strict and the frame is dark
        //                   OR the frame is dim and has a low avg brightness
        if ((!aggressiveDetection) &&
            ((max < commDetectDarkBrightness) ||
             ((max < commDetectDimBrightness) && (avg < commDetectDimAverage))))
            frameIsBlank = true;
    }

    if ((logoInfoAvailable) && (commDetectMethod & COMM_DETECT_LOGO))
    {
        stationLogoPresent =
            logoDetector->doesThisFrameContainTheFoundLogo(framePtr);
    }

#if 0
    if ((commDetectMethod == COMM_DETECT_ALL) &&
        (CheckRatingSymbol()))
    {
        flagMask |= COMM_FRAME_RATING_SYMBOL;
    }
#endif

    if (frameIsBlank)
    {
        blankFrameMap[curFrameNumber] = MARK_BLANK_FRAME;
        flagMask |= COMM_FRAME_BLANK;
        blankFrameCount++;
    }

    if (stationLogoPresent)
        flagMask |= COMM_FRAME_LOGO_PRESENT;

    //TODO: move this debugging code out of the perframe loop, and do it after
    // we've processed all frames. this is because a scenechangedetector can
    // now use a few frames to determine wether the frame a few frames ago was
    // a scene change or not.. due to this lookahead possibility the values
    // that are currently in the frameInfo array, might be changed a few frames
    // from now. The ClassicSceneChangeDetector doesn't use this though. future
    // scenechangedetectors might.

    if (verboseDebugging)
        VERBOSE(VB_COMMFLAG,
                QString().sprintf("Frame: %6ld -> %3d %3d %3d %3d %1d %1d %04x",
                                  (long)curFrameNumber,
                                  frameInfo[curFrameNumber].minBrightness,
                                  frameInfo[curFrameNumber].maxBrightness,
                                  frameInfo[curFrameNumber].avgBrightness,
                                  frameInfo[curFrameNumber].sceneChangePercent,
                                  frameInfo[curFrameNumber].format,
                                  frameInfo[curFrameNumber].aspect,
                                  frameInfo[curFrameNumber].flagMask ));

#ifdef SHOW_DEBUG_WIN
    comm_debug_show(frame->buf);
    getchar();
#endif

    framesProcessed++;
}

void ClassicCommDetector::ClearAllMaps(void)
{
    VERBOSE(VB_COMMFLAG, "CommDetect::ClearAllMaps()");

    frameInfo.clear();
    blankFrameMap.clear();
    blankCommMap.clear();
    blankCommBreakMap.clear();
    sceneMap.clear();
    sceneCommBreakMap.clear();
    commBreakMap.clear();
}

void ClassicCommDetector::GetBlankCommMap(frm_dir_map_t &comms)
{
    VERBOSE(VB_COMMFLAG, "CommDetect::GetBlankCommMap()");

    if (blankCommMap.isEmpty())
        BuildBlankFrameCommList();

    comms = blankCommMap;
}

void ClassicCommDetector::GetBlankCommBreakMap(frm_dir_map_t &comms)
{
    VERBOSE(VB_COMMFLAG, "CommDetect::GetBlankCommBreakMap()");

    if (blankCommBreakMap.isEmpty())
        BuildBlankFrameCommList();

    comms = blankCommBreakMap;
}

void ClassicCommDetector::GetSceneChangeMap(frm_dir_map_t &scenes,
                                            int64_t start_frame)
{
    VERBOSE(VB_COMMFLAG, "CommDetect::GetSceneChangeMap()");

    frm_dir_map_t::iterator it;

    if (start_frame == -1)
        scenes.clear();

    for (it = sceneMap.begin(); it != sceneMap.end(); ++it)
        if ((start_frame == -1) || ((int64_t)it.key() >= start_frame))
            scenes[it.key()] = *it;
}

frm_dir_map_t ClassicCommDetector::Combine2Maps(
    const frm_dir_map_t &a, const frm_dir_map_t &b) const
{
    VERBOSE(VB_COMMFLAG, "CommDetect::BuildMasterCommList()");

    frm_dir_map_t newMap;

    if (a.size())
    {
        frm_dir_map_t::const_iterator it = a.begin();
        for (; it != a.end(); ++it)
            newMap[it.key()] = *it;
    }

    if ((a.size() > 1) &&
        (b.size() > 1))
    {
        // see if beginning of the recording looks like a commercial
        frm_dir_map_t::const_iterator it_a;
        frm_dir_map_t::const_iterator it_b;

        it_a = a.begin();
        it_b = b.begin();

        if ((it_b.key() < 2) && (it_a.key() > 2))
        {
            newMap.remove(it_a.key());
            newMap[0] = MARK_COMM_START;
        }


        // see if ending of recording looks like a commercial
        frm_dir_map_t::const_iterator it;
        uint64_t max_a = 0;
        uint64_t max_b = 0;

        it = a.begin();
        for (; it != a.end(); ++it)
        {
            if ((*it == MARK_COMM_END) && (it.key() > max_a))
                max_a = it.key();
        }

        it = b.begin();
        for (; it != b.end(); ++it)
        {
            if ((*it == MARK_COMM_END) && (it.key() > max_b))
                max_b = it.key();
        }

        if ((max_a < (framesProcessed - 2)) &&
            (max_b > (framesProcessed - 2)))
        {
            newMap.remove(max_a);
            newMap[framesProcessed] = MARK_COMM_END;
        }
    }

    if ((a.size() > 3) &&
        (b.size() > 1))
    {
        frm_dir_map_t::const_iterator it_a;
        frm_dir_map_t::const_iterator it_b;

        it_a = a.begin();
        it_a++;
        it_b = it_a;
        it_b++;
        while (it_b != a.end())
        {
            uint64_t fdiff = it_b.key() - it_a.key();
            bool allTrue = false;

            if (fdiff < (62 * fps))
            {
                uint64_t f = it_a.key() + 1;

                allTrue = true;

                while ((f <= framesProcessed) && (f < it_b.key()) && (allTrue))
                    allTrue = FrameIsInBreakMap(f++, b);
            }

            if (allTrue)
            {
                newMap.remove(it_a.key());
                newMap.remove(it_b.key());
            }

            it_a++; it_a++;
            it_b++;
            if (it_b != a.end())
                it_b++;
        }
    }

    return newMap;
}

void ClassicCommDetector::UpdateFrameBlock(FrameBlock *fbp,
                                           FrameInfoEntry finfo,
                                           int format, int aspect)
{
    int value = 0;

    value = finfo.flagMask;

    if (value & COMM_FRAME_LOGO_PRESENT)
        fbp->logoCount++;

    if (value & COMM_FRAME_RATING_SYMBOL)
        fbp->ratingCount++;

    if (value & COMM_FRAME_SCENE_CHANGE)
        fbp->scCount++;

    if (finfo.format == format)
        fbp->formatMatch++;

    if (finfo.aspect == aspect)
        fbp->aspectMatch++;
}


void ClassicCommDetector::BuildAllMethodsCommList(void)
{
    VERBOSE(VB_COMMFLAG, "CommDetect::BuildAllMethodsCommList()");

    FrameBlock *fblock;
    FrameBlock *fbp;
    int value = 0;
    int curBlock = 0;
    int maxBlock = 0;
    int lastScore = 0;
    int thisScore = 0;
    int nextScore = 0;
    uint64_t curFrame = 0;
    int64_t  breakStart = 0;
    uint64_t lastStart = 0;
    uint64_t lastEnd = 0;
    int64_t firstLogoFrame = -1;
    bool nextFrameIsBlank = false;
    bool lastFrameWasBlank = false;
    uint64_t formatFrames = 0;
    int format = COMM_FORMAT_NORMAL;
    uint64_t aspectFrames = 0;
    int aspect = COMM_ASPECT_NORMAL;
    QString msg;
    uint64_t formatCounts[COMM_FORMAT_MAX];
    frm_dir_map_t tmpCommMap;
    frm_dir_map_t::iterator it;

    commBreakMap.clear();

    fblock = new FrameBlock[blankFrameCount + 2];

    curBlock = 0;
    curFrame = 1;

    fbp = &fblock[curBlock];
    fbp->start = 0;
    fbp->bfCount = 0;
    fbp->logoCount = 0;
    fbp->ratingCount = 0;
    fbp->scCount = 0;
    fbp->scRate = 0.0;
    fbp->formatMatch = 0;
    fbp->aspectMatch = 0;
    fbp->score = 0;

    lastFrameWasBlank = true;

    if (decoderFoundAspectChanges)
    {
        for (int64_t i = preRoll;
             i < ((int64_t)framesProcessed - (int64_t)postRoll); i++)
        {
            if ((frameInfo.contains(i)) &&
                (frameInfo[i].aspect == COMM_ASPECT_NORMAL))
                aspectFrames++;
        }

        if (aspectFrames < ((framesProcessed - preRoll - postRoll) / 2))
        {
            aspect = COMM_ASPECT_WIDE;
            aspectFrames = framesProcessed - preRoll - postRoll - aspectFrames;
        }
    }
    else
    {
        memset(&formatCounts, 0, sizeof(formatCounts));

        for(int64_t i = preRoll;
            i < ((int64_t)framesProcessed - (int64_t)postRoll); i++ )
            if ((frameInfo.contains(i)) &&
                (frameInfo[i].format >= 0) &&
                (frameInfo[i].format < COMM_FORMAT_MAX))
                formatCounts[frameInfo[i].format]++;

        for(int i = 0; i < COMM_FORMAT_MAX; i++)
        {
            if (formatCounts[i] > formatFrames)
            {
                format = i;
                formatFrames = formatCounts[i];
            }
        }
    }

    while (curFrame <= framesProcessed)
    {
        value = frameInfo[curFrame].flagMask;

        if (((curFrame + 1) <= framesProcessed) &&
            (frameInfo[curFrame + 1].flagMask & COMM_FRAME_BLANK))
            nextFrameIsBlank = true;
        else
            nextFrameIsBlank = false;

        if (value & COMM_FRAME_BLANK)
        {
            fbp->bfCount++;

            if (!nextFrameIsBlank || !lastFrameWasBlank)
            {
                UpdateFrameBlock(fbp, frameInfo[curFrame], format, aspect);

                fbp->end = curFrame;
                fbp->frames = fbp->end - fbp->start + 1;
                fbp->length = fbp->frames / fps;

                if ((fbp->scCount) && (fbp->length > 1.05))
                    fbp->scRate = fbp->scCount / fbp->length;

                curBlock++;

                fbp = &fblock[curBlock];
                fbp->bfCount = 1;
                fbp->logoCount = 0;
                fbp->ratingCount = 0;
                fbp->scCount = 0;
                fbp->scRate = 0.0;
                fbp->score = 0;
                fbp->formatMatch = 0;
                fbp->aspectMatch = 0;
                fbp->start = curFrame;
            }

            lastFrameWasBlank = true;
        }
        else
        {
            lastFrameWasBlank = false;
        }

        UpdateFrameBlock(fbp, frameInfo[curFrame], format, aspect);

        if ((value & COMM_FRAME_LOGO_PRESENT) &&
            (firstLogoFrame == -1))
            firstLogoFrame = curFrame;

        curFrame++;
    }

    fbp->end = curFrame;
    fbp->frames = fbp->end - fbp->start + 1;
    fbp->length = fbp->frames / fps;

    if ((fbp->scCount) && (fbp->length > 1.05))
        fbp->scRate = fbp->scCount / fbp->length;

    maxBlock = curBlock;
    curBlock = 0;
    lastScore = 0;

    VERBOSE(VB_COMMFLAG, "Initial Block pass");
    VERBOSE(VB_COMMFLAG, "Block StTime StFrm  EndFrm Frames Secs    "
            "Bf  Lg Cnt RT Cnt SC Cnt SC Rt FmtMch AspMch Score");
    VERBOSE(VB_COMMFLAG, "----- ------ ------ ------ ------ ------- "
            "--- ------ ------ ------ ----- ------ ------ -----");
    while (curBlock <= maxBlock)
    {
        fbp = &fblock[curBlock];

        msg.sprintf("%5d %3d:%02d %6ld %6ld %6ld %7.2f %3d %6d %6d %6d "
                    "%5.2f %6d %6d %5d",
                    curBlock, (int)(fbp->start / fps) / 60,
                    (int)((fbp->start / fps )) % 60,
                    fbp->start, fbp->end, fbp->frames, fbp->length,
                    fbp->bfCount, fbp->logoCount, fbp->ratingCount,
                    fbp->scCount, fbp->scRate, fbp->formatMatch,
                    fbp->aspectMatch, fbp->score);
        VERBOSE(VB_COMMFLAG, msg);

        if (fbp->frames > fps)
        {
            if (verboseDebugging)
                VERBOSE(VB_COMMFLAG, QString("      FRAMES > %1").arg(fps));

            if (fbp->length > commDetectMaxCommLength)
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      length > max comm length, +20");
                fbp->score += 20;
            }

            if (fbp->length > commDetectMaxCommBreakLength)
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      length > max comm break length,"
                            " +20");
                fbp->score += 20;
            }

            if ((fbp->length > 4) &&
                (fbp->logoCount > (fbp->frames * 0.60)) &&
                (fbp->bfCount < (fbp->frames * 0.10)))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      length > 4 && logoCount > "
                            "frames * 0.60 && bfCount < frames "
                            "* .10");
                if (fbp->length > commDetectMaxCommBreakLength)
                {
                    if (verboseDebugging)
                        VERBOSE(VB_COMMFLAG, "      length > "
                                "max comm break length, +20");
                    fbp->score += 20;
                }
                else
                {
                    if (verboseDebugging)
                        VERBOSE(VB_COMMFLAG, "      length <= "
                                "max comm break length, +10");
                    fbp->score += 10;
                }
            }

            if ((logoInfoAvailable) &&
                (fbp->logoCount < (fbp->frames * 0.50)))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      logoInfoAvailable && logoCount"
                            " < frames * .50, -10");
                fbp->score -= 10;
            }

            if (fbp->ratingCount > (fbp->frames * 0.05))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      rating symbol present > 5% "
                            "of time, +20");
                fbp->score += 20;
            }

            if ((fbp->scRate > 1.0) &&
                (fbp->logoCount < (fbp->frames * .90)))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      scRate > 1.0, -10");
                fbp->score -= 10;

                if (fbp->scRate > 2.0)
                {
                    if (verboseDebugging)
                        VERBOSE(VB_COMMFLAG, "      scRate > 2.0, -10");
                    fbp->score -= 10;
                }
            }

            if ((!decoderFoundAspectChanges) &&
                (fbp->formatMatch < (fbp->frames * .10)))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      < 10% of frames match show "
                            "letter/pillar-box format, -20");
                fbp->score -= 20;
            }

            if ((abs((int)(fbp->frames - (15 * fps))) < 5 ) ||
                (abs((int)(fbp->frames - (30 * fps))) < 6 ) ||
                (abs((int)(fbp->frames - (60 * fps))) < 8 ))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      block appears to be standard "
                            "comm length, -10");
                fbp->score -= 10;
            }
        }
        else
        {
            if (verboseDebugging)
                VERBOSE(VB_COMMFLAG, QString("      FRAMES <= %1").arg(fps));

            if ((logoInfoAvailable) &&
                (fbp->start >= firstLogoFrame) &&
                (fbp->logoCount == 0))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      logoInfoAvailable && logoCount"
                            " == 0, -10");
                fbp->score -= 10;
            }

            if ((!decoderFoundAspectChanges) &&
                (fbp->formatMatch < (fbp->frames * .10)))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      < 10% of frames match show "
                            "letter/pillar-box format, -10");
                fbp->score -= 10;
            }

            if (fbp->ratingCount > (fbp->frames * 0.25))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      rating symbol present > 25% "
                            "of time, +10");
                fbp->score += 10;
            }
        }

        if ((decoderFoundAspectChanges) &&
            (fbp->aspectMatch < (fbp->frames * .10)))
        {
            if (verboseDebugging)
                VERBOSE(VB_COMMFLAG, "      < 10% of frames match show "
                        "aspect, -20");
            fbp->score -= 20;
        }

        msg.sprintf("  NOW %3d:%02d %6ld %6ld %6ld %7.2f %3d %6d %6d %6d "
                    "%5.2f %6d %6d %5d",
                    (int)(fbp->start / fps) / 60,
                    (int)((fbp->start / fps )) % 60,
                    fbp->start, fbp->end, fbp->frames, fbp->length,
                    fbp->bfCount, fbp->logoCount, fbp->ratingCount,
                    fbp->scCount, fbp->scRate, fbp->formatMatch,
                    fbp->aspectMatch, fbp->score);
        VERBOSE(VB_COMMFLAG, msg);

        lastScore = fbp->score;
        curBlock++;
    }

    curBlock = 0;
    lastScore = 0;

    VERBOSE(VB_COMMFLAG, "============================================");
    VERBOSE(VB_COMMFLAG, "Second Block pass");
    VERBOSE(VB_COMMFLAG, "Block StTime StFrm  EndFrm Frames Secs    "
            "Bf  Lg Cnt RT Cnt SC Cnt SC Rt FmtMch AspMch Score");
    VERBOSE(VB_COMMFLAG, "----- ------ ------ ------ ------ ------- "
            "--- ------ ------ ------ ----- ------ ------ -----");
    while (curBlock <= maxBlock)
    {
        fbp = &fblock[curBlock];

        msg.sprintf("%5d %3d:%02d %6ld %6ld %6ld %7.2f %3d %6d %6d %6d "
                    "%5.2f %6d %6d %5d",
                    curBlock, (int)(fbp->start / fps) / 60,
                    (int)((fbp->start / fps )) % 60,
                    fbp->start, fbp->end, fbp->frames, fbp->length,
                    fbp->bfCount, fbp->logoCount, fbp->ratingCount,
                    fbp->scCount, fbp->scRate, fbp->formatMatch,
                    fbp->aspectMatch, fbp->score);
        VERBOSE(VB_COMMFLAG, msg);

        if ((curBlock > 0) && (curBlock < maxBlock))
        {
            nextScore = fblock[curBlock + 1].score;

            if ((lastScore < 0) && (nextScore < 0) && (fbp->length < 35))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      lastScore < 0 && nextScore < 0 "
                            "&& length < 35, setting -10");
                fbp->score -= 10;
            }

            if ((fbp->bfCount > (fbp->frames * 0.95)) &&
                (fbp->frames < (2*fps)) &&
                (lastScore < 0 && nextScore < 0))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      blanks > frames * 0.95 && "
                            "frames < 2*fps && lastScore < 0 && "
                            "nextScore < 0, setting -10");
                fbp->score -= 10;
            }

            if ((fbp->frames < (120*fps)) &&
                (lastScore < 0) &&
                (fbp->score > 0) &&
                (fbp->score < 20) &&
                (nextScore < 0))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      frames < 120 * fps && (-20 < "
                            "lastScore < 0) && thisScore > 0 && "
                            "nextScore < 0, setting score = -10");
                fbp->score = -10;
            }

            if ((fbp->frames < (30*fps)) &&
                (lastScore > 0) &&
                (fbp->score < 0) &&
                (fbp->score > -20) &&
                (nextScore > 0))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG, "      frames < 30 * fps && (0 < "
                            "lastScore < 20) && thisScore < 0 && "
                            "nextScore > 0, setting score = 10");
                fbp->score = 10;
            }
        }

        if ((fbp->score == 0) && (lastScore > 30))
        {
            int offset = 1;
            while(((curBlock + offset) <= maxBlock) &&
                    (fblock[curBlock + offset].frames < (2 * fps)) &&
                    (fblock[curBlock + offset].score == 0))
                offset++;

            if ((curBlock + offset) <= maxBlock)
            {
                offset--;
                if (fblock[curBlock + offset + 1].score > 0)
                {
                    for (; offset >= 0; offset--)
                    {
                        fblock[curBlock + offset].score += 10;
                        if (verboseDebugging)
                            VERBOSE(VB_COMMFLAG, QString("      Setting block "
                                                         "%1 score +10")
                                    .arg(curBlock+offset));
                    }
                }
                else if (fblock[curBlock + offset + 1].score < 0)
                {
                    for (; offset >= 0; offset--)
                    {
                        fblock[curBlock + offset].score -= 10;
                        if (verboseDebugging)
                            VERBOSE(VB_COMMFLAG, QString("      Setting block "
                                                         "%1 score -10")
                                    .arg(curBlock+offset));
                    }
                }
            }
        }

        msg.sprintf("  NOW %3d:%02d %6ld %6ld %6ld %7.2f %3d %6d %6d %6d "
                    "%5.2f %6d %6d %5d",
                    (int)(fbp->start / fps) / 60,
                    (int)((fbp->start / fps )) % 60,
                    fbp->start, fbp->end, fbp->frames, fbp->length,
                    fbp->bfCount, fbp->logoCount, fbp->ratingCount,
                    fbp->scCount, fbp->scRate, fbp->formatMatch,
                    fbp->aspectMatch, fbp->score);
        VERBOSE(VB_COMMFLAG, msg);

        lastScore = fbp->score;
        curBlock++;
    }

    VERBOSE(VB_COMMFLAG, "============================================");
    VERBOSE(VB_COMMFLAG, "FINAL Block stats");
    VERBOSE(VB_COMMFLAG, "Block StTime StFrm  EndFrm Frames Secs    "
            "Bf  Lg Cnt RT Cnt SC Cnt SC Rt FmtMch AspMch Score");
    VERBOSE(VB_COMMFLAG, "----- ------ ------ ------ ------ ------- "
            "--- ------ ------ ------ ----- ------ ------ -----");
    curBlock = 0;
    lastScore = 0;
    breakStart = -1;
    while (curBlock <= maxBlock)
    {
        fbp = &fblock[curBlock];
        thisScore = fbp->score;

        if ((breakStart >= 0) &&
            ((fbp->end - breakStart) > (commDetectMaxCommBreakLength * fps)))
        {
            if (((fbp->start - breakStart) >
                (commDetectMinCommBreakLength * fps)) ||
                (breakStart == 0))
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG,
                            QString("Closing commercial block at start of "
                                    "frame block %1 with length %2, frame "
                                    "block length of %3 frames would put comm "
                                    "block length over max of %4 seconds.")
                            .arg(curBlock).arg(fbp->start - breakStart)
                            .arg(fbp->frames)
                            .arg(commDetectMaxCommBreakLength));

                commBreakMap[breakStart] = MARK_COMM_START;
                commBreakMap[fbp->start] = MARK_COMM_END;
                lastStart = breakStart;
                lastEnd = fbp->start;
                breakStart = -1;
            }
            else
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG,
                            QString("Ignoring what appears to be commercial"
                                    " block at frame %1 with length %2, "
                                    "length of %3 frames would put comm "
                                    "block length under min of %4 seconds.")
                            .arg(breakStart)
                            .arg(fbp->start - breakStart)
                            .arg(fbp->frames)
                            .arg(commDetectMinCommBreakLength));
                breakStart = -1;
            }
        }
        if (thisScore == 0)
        {
            thisScore = lastScore;
        }
        else if (thisScore < 0)
        {
            if ((lastScore > 0) || (curBlock == 0))
            {
                if ((fbp->start - lastEnd) < (commDetectMinShowLength * fps))
                {
                    commBreakMap.remove(lastStart);
                    commBreakMap.remove(lastEnd);
                    breakStart = lastStart;

                    if (verboseDebugging)
                    {
                        if (breakStart)
                            VERBOSE(VB_COMMFLAG,
                                    QString("ReOpening commercial block at "
                                            "frame %1 because show less than "
                                            "%2 seconds")
                                    .arg(breakStart)
                                    .arg(commDetectMinShowLength));
                        else
                            VERBOSE(VB_COMMFLAG,
                                    QString("Opening initial commercial block "
                                            "at start of recording, block 0."));
                    }
                }
                else
                {
                    breakStart = fbp->start;

                    if (verboseDebugging)
                        VERBOSE(VB_COMMFLAG,
                                QString("Starting new commercial block at "
                                        "frame %1 from start of frame block %2")
                                .arg(fbp->start).arg(curBlock));
                }
            }
            else if (curBlock == maxBlock)
            {
                if ((fbp->end - breakStart) >
                    (commDetectMinCommBreakLength * fps))
                {
                    if (fbp->end <=
                        ((int64_t)framesProcessed - (int64_t)(2 * fps) - 2))
                    {
                        if (verboseDebugging)
                            VERBOSE(VB_COMMFLAG,
                                    QString("Closing final commercial block at "
                                            "frame %1").arg(fbp->end));

                        commBreakMap[breakStart] = MARK_COMM_START;
                        commBreakMap[fbp->end] = MARK_COMM_END;
                        lastStart = breakStart;
                        lastEnd = fbp->end;
                        breakStart = -1;
                    }
                }
                else
                {
                    if (verboseDebugging)
                        VERBOSE(VB_COMMFLAG,
                                QString("Ignoring what appears to be commercial"
                                        " block at frame %1 with length %2, "
                                        "length of %3 frames would put comm "
                                        "block length under min of %4 seconds.")
                                .arg(breakStart)
                                .arg(fbp->start - breakStart)
                                .arg(fbp->frames)
                                .arg(commDetectMinCommBreakLength));
                    breakStart = -1;
                }
            }
        }
        else if ((thisScore > 0) &&
                 (lastScore < 0) &&
                 (breakStart != -1))
        {
            if (((fbp->start - breakStart) >
                (commDetectMinCommBreakLength * fps)) ||
                (breakStart == 0))
            {
                commBreakMap[breakStart] = MARK_COMM_START;
                commBreakMap[fbp->start] = MARK_COMM_END;
                lastStart = breakStart;
                lastEnd = fbp->start;

                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG,
                            QString("Closing commercial block at frame %1")
                            .arg(fbp->start));
            }
            else
            {
                if (verboseDebugging)
                    VERBOSE(VB_COMMFLAG,
                            QString("Ignoring what appears to be commercial "
                                    "block at frame %1 with length %2, "
                                    "length of %3 frames would put comm block "
                                    "length under min of %4 seconds.")
                            .arg(breakStart)
                            .arg(fbp->start - breakStart)
                            .arg(fbp->frames)
                            .arg(commDetectMinCommBreakLength));
            }
            breakStart = -1;
        }

        msg.sprintf("%5d %3d:%02d %6ld %6ld %6ld %7.2f %3d %6d %6d %6d "
                    "%5.2f %6d %6d %5d",
                    curBlock, (int)(fbp->start / fps) / 60,
                    (int)((fbp->start / fps )) % 60,
                    fbp->start, fbp->end, fbp->frames, fbp->length,
                    fbp->bfCount, fbp->logoCount, fbp->ratingCount,
                    fbp->scCount, fbp->scRate, fbp->formatMatch,
                    fbp->aspectMatch, thisScore);
        VERBOSE(VB_COMMFLAG, msg);

        lastScore = thisScore;
        curBlock++;
    }

    if ((breakStart != -1) &&
        (breakStart <= ((int64_t)framesProcessed - (int64_t)(2 * fps) - 2)))
    {
        if (verboseDebugging)
            VERBOSE(VB_COMMFLAG,
                    QString("Closing final commercial block started at "
                            "block %1 and going to end of program. length "
                            "is %2 frames")
                    .arg(curBlock)
                    .arg((framesProcessed - breakStart - 1)));

        commBreakMap[breakStart] = MARK_COMM_START;
        commBreakMap[framesProcessed - (int)(2 * fps) - 2] = MARK_COMM_END;
    }

    // include/exclude blanks from comm breaks
    tmpCommMap = commBreakMap;
    commBreakMap.clear();

    if (verboseDebugging)
        VERBOSE(VB_COMMFLAG, "Adjusting start/end marks according to blanks.");
    for (it = tmpCommMap.begin(); it != tmpCommMap.end(); ++it)
    {
        if (*it == MARK_COMM_START)
        {
            uint64_t lastStartLower = it.key();
            uint64_t lastStartUpper = it.key();
            while ((lastStartLower > 0) &&
                   (frameInfo[lastStartLower - 1].flagMask & COMM_FRAME_BLANK))
                lastStartLower--;
            while ((lastStartUpper < (framesProcessed - (2 * fps))) &&
                   (frameInfo[lastStartUpper + 1].flagMask & COMM_FRAME_BLANK))
                lastStartUpper++;
            uint64_t adj = (lastStartUpper - lastStartLower) / 2;
            if (adj > MAX_BLANK_FRAMES)
                adj = MAX_BLANK_FRAMES;
            lastStart = lastStartLower + adj;

            if (verboseDebugging)
                VERBOSE(VB_COMMFLAG, QString("Start Mark: %1 -> %2")
                        .arg(it.key())
                        .arg(lastStart));

            commBreakMap[lastStart] = MARK_COMM_START;
        }
        else
        {
            uint64_t lastEndLower = it.key();
            uint64_t lastEndUpper = it.key();
            while ((lastEndUpper < (framesProcessed - (2 * fps))) &&
                   (frameInfo[lastEndUpper + 1].flagMask & COMM_FRAME_BLANK))
                lastEndUpper++;
            while ((lastEndLower > 0) &&
                   (frameInfo[lastEndLower - 1].flagMask & COMM_FRAME_BLANK))
                lastEndLower--;
            uint64_t adj = (lastEndUpper - lastEndLower) / 2;
            if (adj > MAX_BLANK_FRAMES)
                adj = MAX_BLANK_FRAMES;
            lastEnd = lastEndUpper - adj;

            if (verboseDebugging)
                VERBOSE(VB_COMMFLAG, QString("End Mark  : %1 -> %2")
                        .arg(it.key()).arg(lastEnd));

            commBreakMap[lastEnd] = MARK_COMM_END;
        }
    }

    delete [] fblock;
}


void ClassicCommDetector::BuildBlankFrameCommList(void)
{
    VERBOSE(VB_COMMFLAG, "CommDetect::BuildBlankFrameCommList()");

    long long *bframes = new long long[blankFrameMap.count()*2];
    long long *c_start = new long long[blankFrameMap.count()];
    long long *c_end   = new long long[blankFrameMap.count()];
    int frames = 0;
    int commercials = 0;
    int i, x;
    frm_dir_map_t::iterator it;

    blankCommMap.clear();

    for (it = blankFrameMap.begin(); it != blankFrameMap.end(); ++it)
        bframes[frames++] = it.key();

    if (frames == 0)
    {
        delete[] c_start;
        delete[] c_end;
        delete[] bframes;
        return;
    }

    // detect individual commercials from blank frames
    // commercial end is set to frame right before ending blank frame to
    //    account for instances with only a single blank frame between comms.
    for(i = 0; i < frames; i++ )
    {
        for(x=i+1; x < frames; x++ )
        {
            // check for various length spots since some channels don't
            // have blanks inbetween commercials just at the beginning and
            // end of breaks
            int gap_length = bframes[x] - bframes[i];
            if (((aggressiveDetection) &&
                ((abs((int)(gap_length - (5 * fps))) < 5 ) ||
                 (abs((int)(gap_length - (10 * fps))) < 7 ) ||
                 (abs((int)(gap_length - (15 * fps))) < 10 ) ||
                 (abs((int)(gap_length - (20 * fps))) < 11 ) ||
                 (abs((int)(gap_length - (30 * fps))) < 12 ) ||
                 (abs((int)(gap_length - (40 * fps))) < 1 ) ||
                 (abs((int)(gap_length - (45 * fps))) < 1 ) ||
                 (abs((int)(gap_length - (60 * fps))) < 15 ) ||
                 (abs((int)(gap_length - (90 * fps))) < 10 ) ||
                 (abs((int)(gap_length - (120 * fps))) < 10 ))) ||
                ((!aggressiveDetection) &&
                 ((abs((int)(gap_length - (5 * fps))) < 11 ) ||
                  (abs((int)(gap_length - (10 * fps))) < 13 ) ||
                  (abs((int)(gap_length - (15 * fps))) < 16 ) ||
                  (abs((int)(gap_length - (20 * fps))) < 17 ) ||
                  (abs((int)(gap_length - (30 * fps))) < 18 ) ||
                  (abs((int)(gap_length - (40 * fps))) < 3 ) ||
                  (abs((int)(gap_length - (45 * fps))) < 3 ) ||
                  (abs((int)(gap_length - (60 * fps))) < 20 ) ||
                  (abs((int)(gap_length - (90 * fps))) < 20 ) ||
                  (abs((int)(gap_length - (120 * fps))) < 20 ))))
            {
                c_start[commercials] = bframes[i];
                c_end[commercials] = bframes[x] - 1;
                commercials++;
                i = x-1;
                x = frames;
            }

            if ((!aggressiveDetection) &&
                ((abs((int)(gap_length - (30 * fps))) < (int)(fps * 0.85)) ||
                 (abs((int)(gap_length - (60 * fps))) < (int)(fps * 0.95)) ||
                 (abs((int)(gap_length - (90 * fps))) < (int)(fps * 1.05)) ||
                 (abs((int)(gap_length - (120 * fps))) < (int)(fps * 1.15))) &&
                ((x + 2) < frames) &&
                ((i + 2) < frames) &&
                ((bframes[i] + 1) == bframes[i+1]) &&
                ((bframes[x] + 1) == bframes[x+1]))
            {
                c_start[commercials] = bframes[i];
                c_end[commercials] = bframes[x];
                commercials++;
                i = x;
                x = frames;
            }
        }
    }

    i = 0;

    // don't allow single commercial at head
    // of show unless followed by another
    if ((commercials > 1) &&
        (c_end[0] < (33 * fps)) &&
        (c_start[1] > (c_end[0] + 40 * fps)))
        i = 1;

    // eliminate any blank frames at end of commercials
    bool first_comm = true;
    for(; i < (commercials-1); i++)
    {
        long long r = c_start[i];
        long long adjustment = 0;

        if ((r < (30 * fps)) &&
            (first_comm))
            r = 1;

        blankCommMap[r] = MARK_COMM_START;

        r = c_end[i];
        if ( i < (commercials-1))
        {
            for(x = 0; x < (frames-1); x++)
                if (bframes[x] == r)
                    break;
            while((x < (frames-1)) &&
                    ((bframes[x] + 1 ) == bframes[x+1]) &&
                    (bframes[x+1] < c_start[i+1]))
            {
                r++;
                x++;
            }

            while((blankFrameMap.contains(r+1)) &&
                  (c_start[i+1] != (r+1)))
                {
                    r++;
                    adjustment++;
                }
        }
        else
        {
            while(blankFrameMap.contains(r+1))
            {
                r++;
                adjustment++;
            }
        }

        adjustment /= 2;
        if (adjustment > MAX_BLANK_FRAMES)
            adjustment = MAX_BLANK_FRAMES;
        r -= adjustment;
        blankCommMap[r] = MARK_COMM_END;
        first_comm = false;
    }

    blankCommMap[c_start[i]] = MARK_COMM_START;
    blankCommMap[c_end[i]] = MARK_COMM_END;

    delete[] c_start;
    delete[] c_end;
    delete[] bframes;

    VERBOSE(VB_COMMFLAG, "Blank-Frame Commercial Map" );
    for(it = blankCommMap.begin(); it != blankCommMap.end(); ++it)
        VERBOSE(VB_COMMFLAG, QString("    %1:%2")
                .arg(it.key()).arg(*it));

    MergeBlankCommList();

    VERBOSE(VB_COMMFLAG, "Merged Blank-Frame Commercial Break Map" );
    for(it = blankCommBreakMap.begin(); it != blankCommBreakMap.end(); ++it)
        VERBOSE(VB_COMMFLAG, QString("    %1:%2")
                .arg(it.key()).arg(*it));
}


void ClassicCommDetector::BuildSceneChangeCommList(void)
{
    int section_start = -1;
    int seconds = (int)(framesProcessed / fps);
    int *sc_histogram = new int[seconds+1];

    sceneCommBreakMap.clear();

    memset(sc_histogram, 0, sizeof(sc_histogram));
    for (uint64_t f = 1; f <= framesProcessed; f++)
    {
        if (sceneMap.contains(f))
            sc_histogram[(uint64_t)(f / fps)]++;
    }

    for(long long s = 0; s < (seconds + 1); s++)
    {
        if (sc_histogram[s] > 2)
        {
            if (section_start == -1)
            {
                long long f = (long long)(s * fps);
                for(int i = 0; i < fps; i++, f++)
                {
                    if (sceneMap.contains(f))
                    {
                        sceneCommBreakMap[f] = MARK_COMM_START;
                        i = (int)(fps) + 1;
                    }
                }
            }

            section_start = s;
        }

        if ((section_start >= 0) &&
            (s > (section_start + 32)))
        {
            long long f = (long long)(section_start * fps);
            bool found_end = false;

            for(int i = 0; i < fps; i++, f++)
            {
                if (sceneMap.contains(f))
                {
                    frm_dir_map_t::iterator dit =  sceneCommBreakMap.find(f);
                    if (dit != sceneCommBreakMap.end())
                        sceneCommBreakMap.erase(dit);
                    else
                        sceneCommBreakMap[f] = MARK_COMM_END;
                    i = (int)(fps) + 1;
                    found_end = true;
                }
            }
            section_start = -1;

            if (!found_end)
            {
                f = (long long)(section_start * fps);
                sceneCommBreakMap[f] = MARK_COMM_END;
            }
        }
    }
    delete[] sc_histogram;

    if (section_start >= 0)
        sceneCommBreakMap[framesProcessed] = MARK_COMM_END;

    frm_dir_map_t::iterator it;
    frm_dir_map_t::iterator prev;
    frm_dir_map_t deleteMap;

    it = sceneCommBreakMap.begin();
    prev = it;
    if (it != sceneCommBreakMap.end())
    {
        it++;
        while (it != sceneCommBreakMap.end())
        {
            if ((*it == MARK_COMM_END) &&
                (it.key() - prev.key()) < (30 * fps))
            {
                deleteMap[it.key()] = MARK_CUT_START;
                deleteMap[prev.key()] = MARK_CUT_START;
            }
            prev++;
            if (it != sceneCommBreakMap.end())
                it++;
        }

        for (it = deleteMap.begin(); it != deleteMap.end(); ++it)
            sceneCommBreakMap.remove(it.key());
    }

    VERBOSE(VB_COMMFLAG, "Scene-Change Commercial Break Map" );
    for(it = sceneCommBreakMap.begin(); it != sceneCommBreakMap.end(); ++it)
        VERBOSE(VB_COMMFLAG, QString("    %1:%2")
                .arg(it.key()).arg(*it));
}


void ClassicCommDetector::BuildLogoCommList()
{
    show_map_t showmap;
    GetLogoCommBreakMap(showmap);
    CondenseMarkMap(showmap, (int)(25 * fps), (int)(30 * fps));
    ConvertShowMapToCommMap(logoCommBreakMap, showmap);

    frm_dir_map_t::iterator it;
    VERBOSE(VB_COMMFLAG, "Logo Commercial Break Map" );
    for(it = logoCommBreakMap.begin(); it != logoCommBreakMap.end(); ++it)
        VERBOSE(VB_COMMFLAG, QString("    %1:%2")
                .arg(it.key()).arg(*it));
}

void ClassicCommDetector::MergeBlankCommList(void)
{
    frm_dir_map_t::iterator it;
    frm_dir_map_t::iterator prev;
    QMap<long long, long long> tmpMap;
    QMap<long long, long long>::Iterator tmpMap_it;
    QMap<long long, long long>::Iterator tmpMap_prev;

    blankCommBreakMap.clear();

    if (blankCommMap.isEmpty())
        return;

    for (it = blankCommMap.begin(); it != blankCommMap.end(); ++it)
        blankCommBreakMap[it.key()] = *it;

    if (blankCommBreakMap.isEmpty())
        return;

    it = blankCommMap.begin();
    prev = it;
    it++;
    for(; it != blankCommMap.end(); ++it, ++prev)
    {
        // if next commercial starts less than 15*fps frames away then merge
        if ((((prev.key() + 1) == it.key()) ||
            ((prev.key() + (15 * fps)) > it.key())) &&
            (*prev == MARK_COMM_END) &&
            (*it == MARK_COMM_START))
        {
            blankCommBreakMap.remove(prev.key());
            blankCommBreakMap.remove(it.key());
        }
    }


    // make temp copy of commercial break list
    it = blankCommBreakMap.begin();
    prev = it;
    it++;
    tmpMap[prev.key()] = it.key();
    for(; it != blankCommBreakMap.end(); ++it, ++prev)
    {
        if ((*prev == MARK_COMM_START) &&
            (*it == MARK_COMM_END))
            tmpMap[prev.key()] = it.key();
    }

    tmpMap_it = tmpMap.begin();
    tmpMap_prev = tmpMap_it;
    tmpMap_it++;
    for(; tmpMap_it != tmpMap.end(); ++tmpMap_it, ++tmpMap_prev)
    {
        // if we find any segments less than 35 seconds between commercial
        // breaks include those segments in the commercial break.
        if (((*tmpMap_prev + (35 * fps)) > tmpMap_it.key()) &&
            ((*tmpMap_prev - tmpMap_prev.key()) > (35 * fps)) &&
            ((*tmpMap_it - tmpMap_it.key()) > (35 * fps)))
        {
            blankCommBreakMap.remove(*tmpMap_prev);
            blankCommBreakMap.remove(tmpMap_it.key());
        }
    }
}

bool ClassicCommDetector::FrameIsInBreakMap(
    uint64_t f, const frm_dir_map_t &breakMap) const
{
    for (uint64_t i = f; i < framesProcessed; i++)
    {
        if (breakMap.contains(i))
        {
            int type = breakMap[i];
            if ((type == MARK_COMM_END) || (i == f))
                return true;
            if (type == MARK_COMM_START)
                return false;
        }
    }

    for (uint64_t i = f; i >= 0; i--)
    {
        if (breakMap.contains(i))
        {
            int type = breakMap[i];
            if ((type == MARK_COMM_START) || (i == f))
                return true;
            if (type == MARK_COMM_END)
                return false;
        }
    }

    return false;
}

void ClassicCommDetector::DumpMap(frm_dir_map_t &map)
{
    frm_dir_map_t::iterator it;
    QString msg;

    VERBOSE(VB_COMMFLAG, "---------------------------------------------------");
    for (it = map.begin(); it != map.end(); ++it)
    {
        long long frame = it.key();
        int flag = *it;
        int my_fps = (int)ceil(fps);
        int hour = (frame / my_fps) / 60 / 60;
        int min = (frame / my_fps) / 60 - (hour * 60);
        int sec = (frame / my_fps) - (min * 60) - (hour * 60 * 60);
        int frm = frame - ((sec * my_fps) + (min * 60 * my_fps) +
                           (hour * 60 * 60 * my_fps));
        int my_sec = (int)(frame / my_fps);
        msg.sprintf("%7ld : %d (%02d:%02d:%02d.%02d) (%d)",
                    (long)frame, flag, hour, min, sec, frm, my_sec);
        VERBOSE(VB_COMMFLAG, msg);
    }
    VERBOSE(VB_COMMFLAG, "---------------------------------------------------");
}

void ClassicCommDetector::CondenseMarkMap(show_map_t &map, int spacing,
                                          int length)
{
    show_map_t::iterator it;
    show_map_t::iterator prev;
    show_map_t tmpMap;

    if (map.size() <= 2)
        return;

    // merge any segments less than 'spacing' frames apart from each other
    VERBOSE(VB_COMMFLAG, "Commercial Map Before condense:" );
    for (it = map.begin(); it != map.end(); it++)
    {
        VERBOSE(VB_COMMFLAG, QString("    %1:%2")
                .arg(it.key()).arg(*it));
        tmpMap[it.key()] = *it;
    }

    prev = tmpMap.begin();
    it = prev;
    it++;
    while (it != tmpMap.end())
    {
        if ((*it == MARK_START) &&
            (*prev == MARK_END) &&
            ((it.key() - prev.key()) < (uint64_t)spacing))
        {
            map.remove(prev.key());
            map.remove(it.key());
        }
        prev++;
        it++;
    }

    if (map.size() == 0)
        return;

    // delete any segments less than 'length' frames in length
    tmpMap.clear();
    for (it = map.begin(); it != map.end(); it++)
        tmpMap[it.key()] = *it;

    prev = tmpMap.begin();
    it = prev;
    it++;
    while (it != tmpMap.end())
    {
        if ((*prev == MARK_START) &&
            (*it == MARK_END) &&
            ((it.key() - prev.key()) < (uint64_t)length))
        {
            map.remove(prev.key());
            map.remove(it.key());
        }
        prev++;
        it++;
    }

    VERBOSE(VB_COMMFLAG, "Commercial Map After condense:" );
    for (it = map.begin(); it != map.end(); it++)
        VERBOSE(VB_COMMFLAG, QString("    %1:%2")
                .arg(it.key()).arg(*it));
}

void ClassicCommDetector::ConvertShowMapToCommMap(
    frm_dir_map_t &out, const show_map_t &in)
{
    out.clear();
    if (in.empty())
        return;

    show_map_t::const_iterator sit;
    for (sit = in.begin(); sit != in.end(); sit++)
    {
        if (*sit == MARK_START)
            out[sit.key()] = MARK_COMM_END;
        else
            out[sit.key()] = MARK_COMM_START;
    }

    frm_dir_map_t::iterator it = out.begin();
    if (it == out.end())
        return;

    switch (out[it.key()])
    {
        case MARK_COMM_END:
            if (it.key() == 0)
                out.remove(0);
            else
                out[0] = MARK_COMM_START;
            break;
        case MARK_COMM_START:
            break;
        default:
            out.remove(0);
            break;
    }
}


/* ideas for this method ported back from comskip.c mods by Jere Jones
 * which are partially mods based on Myth's original commercial skip
 * code written by Chris Pinkham. */

void ClassicCommDetector::CleanupFrameInfo(void)
{
    VERBOSE(VB_COMMFLAG, "CommDetect::CleanupFrameInfo()");

    int value;
    int before, after;

    // try to account for noisy signal causing blank frames to be undetected
    if ((framesProcessed > (fps * 60)) &&
        (blankFrameCount < (framesProcessed * 0.0004)))
    {
        int avgHistogram[256];
        int minAvg = -1;
        int newThreshold = -1;

        VERBOSE(VB_COMMFLAG,
                QString("ClassicCommDetect: Only found %1 blank frames but "
                        "wanted at least %2, rechecking data using higher "
                        "threshold.")
                        .arg(blankFrameCount)
                        .arg((int)(framesProcessed * 0.0004)));
        blankFrameMap.clear();
        blankFrameCount = 0;

        memset(avgHistogram, 0, sizeof(avgHistogram));

        for (uint64_t i = 1; i <= framesProcessed; i++)
            avgHistogram[clamp(frameInfo[i].avgBrightness, 0, 255)] += 1;

        for (int i = 1; i <= 255 && minAvg == -1; i++)
            if (avgHistogram[i] > (framesProcessed * 0.0004))
                minAvg = i;

        newThreshold = minAvg + 3;
        VERBOSE(VB_COMMFLAG, QString("Minimum Average Brightness on a frame "
                                     "was %1, will use %2 as new threshold")
                .arg(minAvg).arg(newThreshold));

        for (uint64_t i = 1; i <= framesProcessed; i++)
        {
            value = frameInfo[i].flagMask;
            frameInfo[i].flagMask = value & ~COMM_FRAME_BLANK;

            if (( !(frameInfo[i].flagMask & COMM_FRAME_BLANK)) &&
                (frameInfo[i].avgBrightness < newThreshold))
            {
                frameInfo[i].flagMask = value | COMM_FRAME_BLANK;
                blankFrameMap[i] = MARK_BLANK_FRAME;
                blankFrameCount++;
            }
        }

        VERBOSE(VB_COMMFLAG, QString("Found %1 blank frames using new value")
                .arg(blankFrameCount));
    }

    // try to account for fuzzy logo detection
    for (uint64_t i = 1; i <= framesProcessed; i++)
    {
        if ((i < 10) || ((i+10) > framesProcessed))
            continue;

        before = 0;
        for (int offset = 1; offset <= 10; offset++)
            if (frameInfo[i - offset].flagMask & COMM_FRAME_LOGO_PRESENT)
                before++;

        after = 0;
        for (int offset = 1; offset <= 10; offset++)
            if (frameInfo[i + offset].flagMask & COMM_FRAME_LOGO_PRESENT)
                after++;

        value = frameInfo[i].flagMask;
        if (value == -1)
            frameInfo[i].flagMask = 0;

        if (value & COMM_FRAME_LOGO_PRESENT)
        {
            if ((before < 4) && (after < 4))
                frameInfo[i].flagMask = value & ~COMM_FRAME_LOGO_PRESENT;
        }
        else
        {
            if ((before > 6) && (after > 6))
                frameInfo[i].flagMask = value | COMM_FRAME_LOGO_PRESENT;
        }
    }
}

void ClassicCommDetector::GetLogoCommBreakMap(show_map_t &map)
{
    VERBOSE(VB_COMMFLAG, "CommDetect::GetLogoCommBreakMap()");

    map.clear();

    bool PrevFrameLogo = false;

    for (uint64_t curFrame = 1 ; curFrame <= framesProcessed; curFrame++)
    {
        bool CurrentFrameLogo =
            (frameInfo[curFrame].flagMask & COMM_FRAME_LOGO_PRESENT);

        if (!PrevFrameLogo && CurrentFrameLogo)
            map[curFrame] = MARK_START;
        else if (PrevFrameLogo && !CurrentFrameLogo)
            map[curFrame] = MARK_END;

        PrevFrameLogo = CurrentFrameLogo;
    }
}

void ClassicCommDetector::logoDetectorBreathe()
{
    emit breathe();
}

void ClassicCommDetector::PrintFullMap(
    ostream &out, const frm_dir_map_t *comm_breaks, bool verbose) const
{
    if (verbose)
    {
        QByteArray tmp = FrameInfoEntry::GetHeader().toAscii();
        out << tmp.constData() << " mark" << endl;
    }

    for (long long i = 1; i < curFrameNumber; i++)
    {
        QMap<long long, FrameInfoEntry>::const_iterator it = frameInfo.find(i);
        if (it == frameInfo.end())
            continue;

        QByteArray atmp = (*it).toString(i, verbose).toAscii();
        out << atmp.constData() << " ";
        if (comm_breaks)
        {
            frm_dir_map_t::const_iterator mit = comm_breaks->find(i);
            if (mit != comm_breaks->end())
            {
                QString tmp = (verbose) ?
                    toString((MarkTypes)*mit) : QString::number(*mit);
                atmp = tmp.toAscii();

                out << atmp.constData();
            }
        }
        out << "\n";
    }

    out << flush;
}

/* vim: set expandtab tabstop=4 shiftwidth=4: */
