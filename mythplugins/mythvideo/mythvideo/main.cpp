#include <mythcontext.h>
#include <mythversion.h>
#include <lcddevice.h>
#include <mythpluginapi.h>
#include <mythmediamonitor.h>
#include <util.h>

#include <myththemedmenu.h>
#include <mythuihelper.h>
#include <mythmainwindow.h>
#include <mythprogressdialog.h>
#include <mythdialogbox.h>
#include <metadata/cleanup.h>
#include <metadata/globals.h>

#include "videodlg.h"
#include "globalsettings.h"
#include "fileassoc.h"
#include "playersettings.h"
#include "metadatasettings.h"
#include "videolist.h"

#if defined(AEW_VG)
#include <valgrind/memcheck.h>
#endif

namespace
{
    void runScreen(VideoDialog::DialogType type, bool fromJump = false)
    {
        QString message = QObject::tr("Loading videos ...");

        MythScreenStack *popupStack =
                GetMythMainWindow()->GetStack("popup stack");

        MythUIBusyDialog *busyPopup = new MythUIBusyDialog(message,
                popupStack, "mythvideobusydialog");

        if (busyPopup->Create())
            popupStack->AddScreen(busyPopup, false);

        MythScreenStack *mainStack = GetMythMainWindow()->GetMainStack();

        VideoDialog::VideoListPtr video_list;
        if (fromJump)
        {
            VideoDialog::VideoListDeathDelayPtr &saved =
                    VideoDialog::GetSavedVideoList();
            if (!saved.isNull())
            {
                video_list = saved->GetSaved();
            }
        }

        VideoDialog::BrowseType browse = static_cast<VideoDialog::BrowseType>(
                             gCoreContext->GetNumSetting("mythvideo.db_group_type",
                                                     VideoDialog::BRS_FOLDER));

        if (!video_list)
            video_list = new VideoList;

        VideoDialog *mythvideo =
                new VideoDialog(mainStack, "mythvideo", video_list, type, browse);

        if (mythvideo->Create())
        {
            busyPopup->Close();
            mainStack->AddScreen(mythvideo);
        }
        else
            busyPopup->Close();
    }

    void jumpScreenVideoManager() { runScreen(VideoDialog::DLG_MANAGER, true); }
    void jumpScreenVideoBrowser() { runScreen(VideoDialog::DLG_BROWSER, true); }
    void jumpScreenVideoTree()    { runScreen(VideoDialog::DLG_TREE, true);    }
    void jumpScreenVideoGallery() { runScreen(VideoDialog::DLG_GALLERY, true); }
    void jumpScreenVideoDefault() { runScreen(VideoDialog::DLG_DEFAULT, true); }

    ///////////////////////////////////////
    // MythDVD functions
    ///////////////////////////////////////

    // This stores the last MythMediaDevice that was detected:
    QString gDVDdevice;

    void playDisc()
    {
        //
        //  Get the command string to play a DVD
        //

        bool isBD = false;

        QString command_string =
                gCoreContext->GetSetting("mythdvd.DVDPlayerCommand");
        QString bluray_mountpoint =
                gCoreContext->GetSetting("BluRayMountpoint", "/media/cdrom");
        QDir bdtest(bluray_mountpoint + "/BDMV");

        if (bdtest.exists())
            isBD = true;

        if (isBD)
        {
            GetMythUI()->AddCurrentLocation("playdisc");

            QString filename = QString("bd:/%1/").arg(bluray_mountpoint);

            GetMythMainWindow()->HandleMedia("Internal", filename);

            GetMythUI()->RemoveCurrentLocation();
        }
        else
        {
            QString dvd_device = gDVDdevice;

            if (dvd_device.isEmpty())
                dvd_device = MediaMonitor::defaultDVDdevice();

            if (dvd_device.isEmpty())
                return;  // User cancelled in the Popup

            GetMythUI()->AddCurrentLocation("playdisc");

            if ((command_string.indexOf("internal", 0, Qt::CaseInsensitive) > -1) ||
                (command_string.length() < 1))
            {
#ifdef Q_OS_MAC
                // Convert a BSD 'leaf' name into a raw device path
                QString filename = "dvd://dev/r";   // e.g. 'dvd://dev/rdisk2'
#elif USING_MINGW
                QString filename = "dvd:";          // e.g. 'dvd:E\\'
#else
                QString filename = "dvd:/";         // e.g. 'dvd://dev/sda'
#endif
                filename += dvd_device;

                command_string = "Internal";
                GetMythMainWindow()->HandleMedia(command_string, filename);
                GetMythUI()->RemoveCurrentLocation();

                return;
            }
            else
            {
                if (command_string.contains("%d"))
                {
                    //
                    //  Need to do device substitution
                    //
                    command_string =
                            command_string.replace(QRegExp("%d"), dvd_device);
                }
                sendPlaybackStart();
                myth_system(command_string);
                sendPlaybackEnd();
                if (GetMythMainWindow())
                {
                    GetMythMainWindow()->raise();
                    GetMythMainWindow()->activateWindow();
                    if (GetMythMainWindow()->currentWidget())
                        GetMythMainWindow()->currentWidget()->setFocus();
                }
            }
            GetMythUI()->RemoveCurrentLocation();
        }
    }

    /////////////////////////////////////////////////
    //// Media handlers
    /////////////////////////////////////////////////
    void handleDVDMedia(MythMediaDevice *dvd)
    {
        if (!dvd)
            return;

        QString newDevice = dvd->getDevicePath();

        // Device insertion. Store it for later use
        if (dvd->isUsable())
            if (gDVDdevice.length() && gDVDdevice != newDevice)
            {
                // Multiple DVD devices. Clear the old one so the user has to
                // select a disk to play (in MediaMonitor::defaultDVDdevice())

                VERBOSE(VB_MEDIA, "MythVideo: Multiple DVD drives? Forgetting "
                                    + gDVDdevice);
                gDVDdevice.clear();
            }
            else
            {
                gDVDdevice = newDevice;
                VERBOSE(VB_MEDIA,
                        "MythVideo: Storing DVD device " + gDVDdevice);
            }
        else
        {
            // Ejected/unmounted/error.

            if (gDVDdevice.length() && gDVDdevice == newDevice)
            {
                VERBOSE(VB_MEDIA,
                        "MythVideo: Forgetting existing DVD " + gDVDdevice);
                gDVDdevice.clear();

                // How do I tell the MTD to ignore this device?
            }

            return;
        }

        switch (gCoreContext->GetNumSetting("DVDOnInsertDVD", 1))
        {
            case 0 : // Do nothing
                break;
            case 1 : // Display menu (mythdvd)*/
                GetMythMainWindow()->JumpTo("Main Menu");
                mythplugin_run();
                break;
            case 2 : // play DVD or Blu-ray
                GetMythMainWindow()->JumpTo("Main Menu");
                playDisc();
                break;
            default:
                VERBOSE(VB_IMPORTANT, "mythdvd main.o: handleMedia() does not "
                        "know what to do");
        }
    }

    ///////////////////////////////////////
    // MythVideo functions
    ///////////////////////////////////////

    void setupKeys()
    {
        REG_JUMP(JUMP_VIDEO_DEFAULT, QT_TRANSLATE_NOOP("MythControls",
            "The MythVideo default view"), "", jumpScreenVideoDefault);
        REG_JUMP(JUMP_VIDEO_MANAGER, QT_TRANSLATE_NOOP("MythControls",
            "The MythVideo video manager"), "", jumpScreenVideoManager);
        REG_JUMP(JUMP_VIDEO_BROWSER, QT_TRANSLATE_NOOP("MythControls",
            "The MythVideo video browser"), "", jumpScreenVideoBrowser);
        REG_JUMP(JUMP_VIDEO_TREE, QT_TRANSLATE_NOOP("MythControls",
            "The MythVideo video listings"), "", jumpScreenVideoTree);
        REG_JUMP(JUMP_VIDEO_GALLERY, QT_TRANSLATE_NOOP("MythControls",
            "The MythVideo video gallery"), "", jumpScreenVideoGallery);

        REG_KEY("Video","PLAYALT", QT_TRANSLATE_NOOP("MythControls",
            "Play selected item in alternate player"), "ALT+P");

        REG_KEY("Video","FILTER", QT_TRANSLATE_NOOP("MythControls",
            "Open video filter dialog"), "F");

        REG_KEY("Video","BROWSE", QT_TRANSLATE_NOOP("MythControls",
            "Change browsable in video manager"), "B");
        REG_KEY("Video","INCPARENT", QT_TRANSLATE_NOOP("MythControls",
            "Increase Parental Level"), "],},F11");
        REG_KEY("Video","DECPARENT", QT_TRANSLATE_NOOP("MythControls",
            "Decrease Parental Level"), "[,{,F10");

        REG_KEY("Video","INCSEARCH", QT_TRANSLATE_NOOP("MythControls",
            "Show Incremental Search Dialog"), "Ctrl+S");
        REG_KEY("Video","DOWNLOADDATA", QT_TRANSLATE_NOOP("MythControls",
            "Download metadata for current item"), "W");
        REG_KEY("Video","ITEMDETAIL", QT_TRANSLATE_NOOP("MythControls",
            "Display Item Detail Popup"), "");

        REG_KEY("Video","HOME", QT_TRANSLATE_NOOP("MythControls",
            "Go to the first video"), "Home");
        REG_KEY("Video","END", QT_TRANSLATE_NOOP("MythControls",
            "Go to the last video"), "End");

        // MythDVD
        REG_JUMP("Play Disc", QT_TRANSLATE_NOOP("MythControls",
            "Play an Optical Disc"), "", playDisc);
        REG_MEDIA_HANDLER(QT_TRANSLATE_NOOP("MythControls",
            "MythDVD DVD Media Handler"), "", "", handleDVDMedia,
            MEDIATYPE_DVD, QString::null);

    }

    class RunSettingsCompletion : public QObject
    {
        Q_OBJECT

      public:
        static void Create(bool check)
        {
            new RunSettingsCompletion(check);
        }

      private:
        RunSettingsCompletion(bool check)
        {
            if (check)
            {
                connect(&m_plcc,
                        SIGNAL(SigResultReady(bool, ParentalLevel::Level)),
                        SLOT(OnPasswordResultReady(bool,
                                        ParentalLevel::Level)));
                m_plcc.Check(ParentalLevel::plMedium, ParentalLevel::plHigh);
            }
            else
            {
                OnPasswordResultReady(true, ParentalLevel::plHigh);
            }
        }

        ~RunSettingsCompletion() {}

      private slots:
        void OnPasswordResultReady(bool passwordValid,
                ParentalLevel::Level newLevel)
        {
            (void) newLevel;

            if (passwordValid)
            {
                VideoGeneralSettings settings;
                settings.exec();
            }
            else
            {
                VERBOSE(VB_IMPORTANT,
                        QObject::tr("Aggressive Parental Controls Warning: "
                                "invalid password. An attempt to enter a "
                                "MythVideo settings screen was prevented."));
            }

            deleteLater();
        }

      public:
        ParentalLevelChangeChecker m_plcc;
    };

    void VideoCallback(void *data, QString &selection)
    {
        (void) data;

        QString sel = selection.toLower();

        if (sel == "manager")
            runScreen(VideoDialog::DLG_MANAGER);
        else if (sel == "browser")
            runScreen(VideoDialog::DLG_BROWSER);
        else if (sel == "listing")
            runScreen(VideoDialog::DLG_TREE);
        else if (sel == "gallery")
            runScreen(VideoDialog::DLG_GALLERY);
        else if (sel == "settings_general")
        {
            RunSettingsCompletion::Create(gCoreContext->
                    GetNumSetting("VideoAggressivePC", 0));
        }
        else if (sel == "settings_player")
        {
            MythScreenStack *mainStack = GetMythMainWindow()->GetMainStack();

            PlayerSettings *ps = new PlayerSettings(mainStack, "player settings");

            if (ps->Create())
                mainStack->AddScreen(ps);
        }
        else if (sel == "settings_metadata")
        {
            MythScreenStack *mainStack = GetMythMainWindow()->GetMainStack();

            MetadataSettings *ms = new MetadataSettings(mainStack, "metadata settings");

            if (ms->Create())
                mainStack->AddScreen(ms);
        }
        else if (sel == "settings_associations")
        {
            MythScreenStack *mainStack = GetMythMainWindow()->GetMainStack();

            FileAssocDialog *fa = new FileAssocDialog(mainStack, "fa dialog");

            if (fa->Create())
                mainStack->AddScreen(fa);
        }
        else if (sel == "disc_play")
        {
            playDisc();
        }
    }

    int runMenu(const QString &menuname)
    {
        QString themedir = GetMythUI()->GetThemeDir();

        MythThemedMenu *diag =
                new MythThemedMenu(themedir, menuname,
                                    GetMythMainWindow()->GetMainStack(),
                                    "video menu");

        diag->setCallback(VideoCallback, NULL);
        diag->setKillable();

        if (diag->foundTheme())
        {
            if (LCD *lcd = LCD::Get())
            {
                lcd->setFunctionLEDs(FUNC_MOVIE, false);
                lcd->switchToTime();
            }
            GetMythMainWindow()->GetMainStack()->AddScreen(diag);
            return 0;
        }
        else
        {
            VERBOSE(VB_IMPORTANT, QString("Couldn't find menu %1 or theme %2")
                                  .arg(menuname).arg(themedir));
            delete diag;
            return -1;
        }
    }
}

int mythplugin_init(const char *libversion)
{
    if (!gContext->TestPopupVersion("mythvideo", libversion,
                                    MYTH_BINARY_VERSION))
        return -1;

    VideoGeneralSettings general;
    general.Load();
    general.Save();

    setupKeys();

    return 0;
}

int mythplugin_run()
{
    return runMenu("videomenu.xml");
}

int mythplugin_config()
{
    return runMenu("video_settings.xml");
}

void mythplugin_destroy()
{
    CleanupHooks::getInstance()->cleanup();
#if defined(AEW_VG)
    // valgrind forgets symbols of unloaded modules
    // I'd rather sort out known non-leaks than piece
    // together a call stack.
    VALGRIND_DO_LEAK_CHECK;
#endif
}

#include "main.moc"
