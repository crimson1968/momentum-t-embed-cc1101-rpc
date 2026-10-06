#pragma once
#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_LOADER "loader"
#define LOADER_APPLICATIONS_NAME "Apps"

typedef struct Loader Loader;

typedef enum {
    LoaderStatusOk,
    LoaderStatusErrorAppStarted,
    LoaderStatusErrorUnknownApp,
    LoaderStatusErrorInternal,
} LoaderStatus;

typedef enum {
    LoaderEventTypeApplicationBeforeLoad,
    LoaderEventTypeApplicationLoadFailed,
    LoaderEventTypeApplicationStopped,
    LoaderEventTypeNoMoreAppsInQueue,
    LoaderEventTypeApplicationsBrowserOpened,
    LoaderEventTypeApplicationsBrowserClosed,
    LoaderEventTypeMenuOpened,
    LoaderEventTypeMenuClosed,
} LoaderEventType;

typedef struct {
    LoaderEventType type;
} LoaderEvent;

LoaderStatus loader_start(
    Loader* instance,
    const char* name,
    const char* args,
    FuriString* error_message);
LoaderStatus
    loader_start_with_gui_error(Loader* loader, const char* name, const char* args);
void loader_start_detached_with_gui_error(
    Loader* loader,
    const char* name,
    const char* args);
bool loader_lock(Loader* instance);
void loader_unlock(Loader* instance);
bool loader_is_locked(Loader* instance);

/** Send a signal (e.g. FuriSignalExit) to the currently running application's
 * thread. Returns false if no application is running. Apps handle FuriSignalExit
 * by default and exit, so this is the way to ask the foreground app to close. */
bool loader_signal(Loader* instance, uint32_t signal, void* arg);
void loader_show_menu(Loader* instance);
/** Open the native loader menu directly on its Settings page. */
void loader_show_settings(Loader* instance);
FuriPubSub* loader_get_pubsub(Loader* instance);

typedef enum {
    LoaderDeferredLaunchFlagGui = (1 << 0),
} LoaderDeferredLaunchFlag;

/** Launch after the current app exits, or immediately if the loader is idle. */
void loader_enqueue_launch(
    Loader* instance,
    const char* name,
    const char* args,
    LoaderDeferredLaunchFlag flags);

/** Get the path of the currently running application (stub on ESP32) */
bool loader_get_application_launch_path(Loader* instance, FuriString* path);

int32_t loader_srv(void* p);

#ifdef __cplusplus
}
#endif
