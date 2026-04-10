/* Modifié pour Kronos : Support du plein écran et de l'auto-start 
   via invokeMethod pour contourner les membres protégés.
*/
#include <QApplication>
#include <QTimer>

#include "QtYabause.h"
#include "VolatileSettings.h"
#include "Settings.h"
#include "stv.h"
#include "ui/UIYabause.h"

#ifndef NO_CLI
#include "Arguments.h"
#endif

int main( int argc, char** argv )
{
#ifdef _WIN32
    auto stdout_type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    auto stderr_type = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (stdout_type == FILE_TYPE_UNKNOWN) freopen("CONOUT$", "w", stdout);
        if (stderr_type == FILE_TYPE_UNKNOWN) freopen("CONOUT$", "w", stderr);
    }
#endif

    QApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QApplication app( argc, argv );

    app.setApplicationName( QString( "Kronos v%1" ).arg( VERSION ) );
    Settings::setIniInformations();

#ifdef HAVE_LIBMINI18N
    if ( QtYabause::setTranslationFile() == -1 )
        qWarning( "Can't set translation file" );
#endif

#ifndef NO_CLI
    Arguments::parse();
#endif

    QtYabause::updateTitle();
    UIYabause* window = QtYabause::mainWindow();

    // --- LOGIQUE PLEIN ÉCRAN ET AUTO-START VIA INVOKE ---
    QStringList args = QCoreApplication::arguments();
    bool fullscreen = args.contains("-f") || args.contains("--fullscreen");
    bool autostart = args.contains("-a") || args.contains("--autostart");

    if (fullscreen) {
        window->show();

        QTimer::singleShot(500, [window, autostart]() {
            // On utilise invokeMethod pour appeler le slot protégé 'fullscreenRequested'
            QMetaObject::invokeMethod(window, "fullscreenRequested", Q_ARG(bool, true));

            if (autostart) {
                // On cherche l'action de run et on la déclenche
                QAction* runAction = window->findChild<QAction*>("aEmulationRun");
                if (runAction) {
                    runAction->trigger();
                }
            }
        });
    } else {
        window->show();
        if (autostart) {
            QTimer::singleShot(100, [window]() {
                QAction* runAction = window->findChild<QAction*>("aEmulationRun");
                if (runAction) runAction->trigger();
            });
        }
    }
    // ----------------------------------------------------

    QObject::connect( &app, SIGNAL( lastWindowClosed() ), &app, SLOT( quit() ) );
    
    int i = app.exec();
    QtYabause::closeTranslation();
    return i;
}
