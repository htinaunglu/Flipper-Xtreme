#include "loader.h"
#include "loader_i.h"
#include "loader_menu.h"
#include <applications.h>
#include <furi_hal.h>
#include <core/dangerous_defines.h>
#include <gui/modules/file_browser.h>
#include <toolbox/stream/file_stream.h>
#include "applications/main/fap_loader/fap_loader_app.h"
#include <xtreme.h>

#define TAG "Loader"
#define LOADER_MAGIC_THREAD_VALUE 0xDEADBEEF
// api

LoaderStatus loader_start(Loader* loader, const char* name, const char* args) {
    LoaderMessage message;
    LoaderMessageLoaderStatusResult result;

    message.type = LoaderMessageTypeStartByName;
    message.start.name = name;
    message.start.args = args;
    message.api_lock = api_lock_alloc_locked();
    message.status_value = &result;
    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
    api_lock_wait_unlock_and_free(message.api_lock);
    return result.value;
}

bool loader_lock(Loader* loader) {
    LoaderMessage message;
    LoaderMessageBoolResult result;
    message.type = LoaderMessageTypeLock;
    message.api_lock = api_lock_alloc_locked();
    message.bool_value = &result;
    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
    api_lock_wait_unlock_and_free(message.api_lock);
    return result.value;
}

void loader_unlock(Loader* loader) {
    LoaderMessage message;
    message.type = LoaderMessageTypeUnlock;
    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
}

bool loader_is_locked(Loader* loader) {
    LoaderMessage message;
    LoaderMessageBoolResult result;
    message.type = LoaderMessageTypeIsLocked;
    message.api_lock = api_lock_alloc_locked();
    message.bool_value = &result;
    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
    api_lock_wait_unlock_and_free(message.api_lock);
    return result.value;
}

void loader_show_menu(Loader* loader) {
    LoaderMessage message;
    message.type = LoaderMessageTypeShowMenu;
    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
}

void loader_show_settings(Loader* loader) {
    LoaderMessage message;
    message.type = LoaderMessageTypeShowSettings;
    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
}

FuriPubSub* loader_get_pubsub(Loader* loader) {
    furi_assert(loader);
    // it's safe to return pubsub without locking
    // because it's never freed and loader is never exited
    // also the loader instance cannot be obtained until the pubsub is created
    return loader->pubsub;
}

// callbacks

static void loader_menu_closed_callback(void* context) {
    Loader* loader = context;
    LoaderMessage message;
    message.type = LoaderMessageTypeMenuClosed;
    furi_message_queue_put(loader->queue, &message, FuriWaitForever);
}

static void loader_menu_click_callback(const char* name, bool external, void* context) {
    Loader* loader = context;
    if(external) {
        loader_start(loader, FAP_LOADER_APP_NAME, name);
    } else {
        loader_start(loader, name, NULL);
    }
}

static void loader_thread_state_callback(FuriThreadState thread_state, void* context) {
    furi_assert(context);

    Loader* loader = context;
    LoaderEvent event;

    if(thread_state == FuriThreadStateRunning) {
        event.type = LoaderEventTypeApplicationStarted;
        furi_pubsub_publish(loader->pubsub, &event);
    } else if(thread_state == FuriThreadStateStopped) {
        LoaderMessage message;
        message.type = LoaderMessageTypeAppClosed;
        furi_message_queue_put(loader->queue, &message, FuriWaitForever);

        event.type = LoaderEventTypeApplicationStopped;
        furi_pubsub_publish(loader->pubsub, &event);
    }
}

bool loader_menu_load_fap_meta(
    Storage* storage,
    FuriString* path,
    FuriString* name,
    const Icon** icon) {
    *icon = NULL;
    uint8_t* icon_buf = malloc(CUSTOM_ICON_MAX_SIZE);
    if(!fap_loader_load_name_and_icon(path, storage, &icon_buf, name)) {
        free(icon_buf);
        icon_buf = NULL;
        return false;
    }
    *icon = malloc(sizeof(Icon));
    FURI_CONST_ASSIGN((*icon)->frame_count, 1);
    FURI_CONST_ASSIGN((*icon)->frame_rate, 0);
    FURI_CONST_ASSIGN((*icon)->width, 10);
    FURI_CONST_ASSIGN((*icon)->height, 10);
    FURI_CONST_ASSIGN_PTR((*icon)->frames, malloc(sizeof(const uint8_t*)));
    FURI_CONST_ASSIGN_PTR((*icon)->frames[0], icon_buf);
    return true;
}

// implementation

static Loader* loader_alloc() {
    Loader* loader = malloc(sizeof(Loader));
    loader->pubsub = furi_pubsub_alloc();
    loader->queue = furi_message_queue_alloc(1, sizeof(LoaderMessage));
    loader->loader_menu = NULL;
    loader->app.args = NULL;
    loader->app.name = NULL;
    loader->app.thread = NULL;
    loader->app.insomniac = false;
    ExtMainAppList_init(loader->ext_main_apps);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc();
    FuriString* name = furi_string_alloc();
    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, XTREME_APPS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(stream_read_line(stream, path)) {
            furi_string_replace_all(path, "\r", "");
            furi_string_replace_all(path, "\n", "");
            const Icon* icon;
            if(!loader_menu_load_fap_meta(storage, path, name, &icon)) continue;
            ExtMainAppList_push_back(
                loader->ext_main_apps,
                (ExtMainApp){
                    .name = strdup(furi_string_get_cstr(name)),
                    .path = strdup(furi_string_get_cstr(path)),
                    .icon = icon});
        }
    }
    file_stream_close(stream);
    stream_free(stream);
    furi_string_free(name);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    return loader;
}

static FlipperApplication const* loader_find_application_by_name_in_list(
    const char* name,
    const FlipperApplication* list,
    const uint32_t n_apps) {
    for(size_t i = 0; i < n_apps; i++) {
        if(strcmp(name, list[i].name) == 0) {
            return &list[i];
        }
    }
    return NULL;
}

static const FlipperApplication* loader_find_application_by_name(const char* name) {
    if(!strncmp(name, "Bad USB", strlen("Bad USB")))
        name = "Bad KB";
    else if(!strncmp(name, "Applications", strlen("Applications")))
        name = "Apps";
    else if(!strncmp(name, "125 kHz RFID", strlen("125 kHz RFID")))
        name = "RFID";
    else if(!strncmp(name, "Sub-GHz", strlen("Sub-GHz")))
        name = "SubGHz";

    const FlipperApplication* application = NULL;
    application = loader_find_application_by_name_in_list(name, FLIPPER_APPS, FLIPPER_APPS_COUNT);
    if(!application) {
        application = loader_find_application_by_name_in_list(
            name, FLIPPER_SETTINGS_APPS, FLIPPER_SETTINGS_APPS_COUNT);
    }
    if(!application) {
        application = loader_find_application_by_name_in_list(
            name, FLIPPER_SYSTEM_APPS, FLIPPER_SYSTEM_APPS_COUNT);
    }

    return application;
}

static void
    loader_start_internal_app(Loader* loader, const FlipperApplication* app, const char* args) {
    FURI_LOG_I(TAG, "Starting %s", app->name);

    if(app->app == NULL) {
        args = app->appid;
        app = loader_find_application_by_name_in_list(
            FAP_LOADER_APP_NAME, FLIPPER_APPS, FLIPPER_APPS_COUNT);
    }

    // store args
    furi_assert(loader->app.args == NULL);
    if(args && strlen(args) > 0) {
        loader->app.args = strdup(args);
    }

    // store name
    furi_assert(loader->app.name == NULL);
    loader->app.name = strdup(app->name);

    // setup app thread
    loader->app.thread =
        furi_thread_alloc_ex(app->name, app->stack_size, app->app, loader->app.args);
    furi_thread_set_appid(loader->app.thread, app->appid);

    // setup heap trace
    FuriHalRtcHeapTrackMode mode = furi_hal_rtc_get_heap_track_mode();
    if(mode > FuriHalRtcHeapTrackModeNone) {
        furi_thread_enable_heap_trace(loader->app.thread);
    } else {
        furi_thread_disable_heap_trace(loader->app.thread);
    }

    // setup insomnia
    if(!(app->flags & FlipperApplicationFlagInsomniaSafe)) {
        furi_hal_power_insomnia_enter();
        loader->app.insomniac = true;
    } else {
        loader->app.insomniac = false;
    }

    // setup app thread callbacks
    furi_thread_set_state_context(loader->app.thread, loader);
    furi_thread_set_state_callback(loader->app.thread, loader_thread_state_callback);

    // start app thread
    furi_thread_start(loader->app.thread);
}

// process messages

static void loader_do_menu_show(Loader* loader, bool settings) {
    if(!loader->loader_menu) {
        loader->loader_menu = loader_menu_alloc();
        loader_menu_set_closed_callback(loader->loader_menu, loader_menu_closed_callback, loader);
        loader_menu_set_click_callback(loader->loader_menu, loader_menu_click_callback, loader);
        loader_menu_start(loader->loader_menu, settings, &loader->ext_main_apps);
    }
}

static void loader_do_menu_closed(Loader* loader) {
    if(loader->loader_menu) {
        loader_menu_stop(loader->loader_menu);
        loader_menu_free(loader->loader_menu);
        loader->loader_menu = NULL;
    }
}

static bool loader_do_is_locked(Loader* loader) {
    return loader->app.thread != NULL;
}

static LoaderStatus loader_do_start_by_name(Loader* loader, const char* name, const char* args) {
    if(loader_do_is_locked(loader)) {
        return LoaderStatusErrorAppStarted;
    }

    const FlipperApplication* app = loader_find_application_by_name(name);

    if(!app) {
        return LoaderStatusErrorUnknownApp;
    }

    loader_start_internal_app(loader, app, args);
    return LoaderStatusOk;
}

static bool loader_do_lock(Loader* loader) {
    if(loader->app.thread) {
        return false;
    }

    loader->app.thread = (FuriThread*)LOADER_MAGIC_THREAD_VALUE;
    return true;
}

static void loader_do_unlock(Loader* loader) {
    furi_assert(loader->app.thread == (FuriThread*)LOADER_MAGIC_THREAD_VALUE);
    loader->app.thread = NULL;
}

static void loader_do_app_closed(Loader* loader) {
    furi_assert(loader->app.thread);
    FURI_LOG_I(TAG, "Application stopped. Free heap: %zu", memmgr_get_free_heap());
    if(loader->app.args) {
        free(loader->app.args);
        loader->app.args = NULL;
    }

    if(loader->app.insomniac) {
        furi_hal_power_insomnia_exit();
    }

    free(loader->app.name);
    loader->app.name = NULL;

    furi_thread_join(loader->app.thread);
    furi_thread_free(loader->app.thread);
    loader->app.thread = NULL;
}

// app

int32_t loader_srv(void* p) {
    UNUSED(p);
    Loader* loader = loader_alloc();
    furi_record_create(RECORD_LOADER, loader);

    FURI_LOG_I(TAG, "Executing system start hooks");
    for(size_t i = 0; i < FLIPPER_ON_SYSTEM_START_COUNT; i++) {
        FLIPPER_ON_SYSTEM_START[i]();
    }

    if(FLIPPER_AUTORUN_APP_NAME && strlen(FLIPPER_AUTORUN_APP_NAME)) {
        loader_do_start_by_name(loader, FLIPPER_AUTORUN_APP_NAME, NULL);
    }

    LoaderMessage message;
    while(true) {
        if(furi_message_queue_get(loader->queue, &message, FuriWaitForever) == FuriStatusOk) {
            switch(message.type) {
            case LoaderMessageTypeStartByName:
                message.status_value->value =
                    loader_do_start_by_name(loader, message.start.name, message.start.args);
                api_lock_unlock(message.api_lock);
                break;
            case LoaderMessageTypeShowMenu:
                loader_do_menu_show(loader, false);
                break;
            case LoaderMessageTypeShowSettings:
                loader_do_menu_show(loader, true);
                break;
            case LoaderMessageTypeMenuClosed:
                loader_do_menu_closed(loader);
                break;
            case LoaderMessageTypeIsLocked:
                message.bool_value->value = loader_do_is_locked(loader);
                api_lock_unlock(message.api_lock);
                break;
            case LoaderMessageTypeAppClosed:
                loader_do_app_closed(loader);
                break;
            case LoaderMessageTypeLock:
                message.bool_value->value = loader_do_lock(loader);
                api_lock_unlock(message.api_lock);
                break;
            case LoaderMessageTypeUnlock:
                loader_do_unlock(loader);
            }
        }
    }

    return 0;
}
