#include "applications.h"
#include <furi.h>
#include <storage/storage.h>
#include "loader/loader.h"
#include "loader_i.h"
#include "applications/main/fap_loader/fap_loader_app.h"
#include <flipper_application/application_manifest.h>
#include <gui/icon_i.h>
#include <core/dangerous_defines.h>

#define TAG "LoaderSrv"

#define LOADER_THREAD_FLAG_SHOW_MENU (1 << 0)
#define LOADER_THREAD_FLAG_ALL (LOADER_THREAD_FLAG_SHOW_MENU)

static Loader* loader_instance = NULL;

static bool
    loader_start_application(const FlipperApplication* application, const char* arguments) {
    loader_instance->application = application;

    furi_assert(loader_instance->application_arguments == NULL);
    if(arguments && strlen(arguments) > 0) {
        loader_instance->application_arguments = strdup(arguments);
    }

    FURI_LOG_I(TAG, "Starting: %s", loader_instance->application->name);

    FuriHalRtcHeapTrackMode mode = furi_hal_rtc_get_heap_track_mode();
    if(mode > FuriHalRtcHeapTrackModeNone) {
        furi_thread_enable_heap_trace(loader_instance->application_thread);
    } else {
        furi_thread_disable_heap_trace(loader_instance->application_thread);
    }

    furi_thread_set_name(loader_instance->application_thread, loader_instance->application->name);
    furi_thread_set_appid(
        loader_instance->application_thread, loader_instance->application->appid);
    furi_thread_set_stack_size(
        loader_instance->application_thread, loader_instance->application->stack_size);
    furi_thread_set_context(
        loader_instance->application_thread, loader_instance->application_arguments);
    furi_thread_set_callback(
        loader_instance->application_thread, loader_instance->application->app);

    furi_thread_start(loader_instance->application_thread);

    return true;
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

const FlipperApplication* loader_find_application_by_name(const char* name) {
    const FlipperApplication* application = NULL;
    application = loader_find_application_by_name_in_list(name, FLIPPER_APPS, FLIPPER_APPS_COUNT);
    if(!application) {
        application =
            loader_find_application_by_name_in_list(name, FLIPPER_PLUGINS, FLIPPER_PLUGINS_COUNT);
    }
    if(!application) {
        application = loader_find_application_by_name_in_list(
            name, FLIPPER_SETTINGS_APPS, FLIPPER_SETTINGS_APPS_COUNT);
    }
    if(!application) {
        application = loader_find_application_by_name_in_list(
            name, FLIPPER_SYSTEM_APPS, FLIPPER_SYSTEM_APPS_COUNT);
    }
    if(!application) {
        application = loader_find_application_by_name_in_list(
            name, FLIPPER_DEBUG_APPS, FLIPPER_DEBUG_APPS_COUNT);
    }

    return application;
}

static void loader_menu_callback(void* _ctx, uint32_t index) {
    UNUSED(index);
    const FlipperApplication* application = _ctx;

    furi_assert(application->app);
    furi_assert(application->name);

    if(!loader_lock(loader_instance)) {
        FURI_LOG_E(TAG, "Loader is locked");
        return;
    }

    loader_start_application(application, NULL);
}

static void loader_main_callback(void* _ctx, uint32_t index) {
    UNUSED(index);
    const char* path = _ctx;
    const FlipperApplication* app = loader_find_application_by_name_in_list(FAP_LOADER_APP_NAME, FLIPPER_APPS, FLIPPER_APPS_COUNT);

    furi_assert(path);

    if(!loader_lock(loader_instance)) {
        FURI_LOG_E(TAG, "Loader is locked");
        return;
    }

    loader_start_application(app, path);
}

static void loader_submenu_callback(void* context, uint32_t index) {
    UNUSED(index);
    uint32_t view_id = (uint32_t)context;
    view_dispatcher_switch_to_view(loader_instance->view_dispatcher, view_id);
}

static void loader_cli_print_usage() {
    printf("Usage:\r\n");
    printf("loader <cmd> <args>\r\n");
    printf("Cmd list:\r\n");
    printf("\tlist\t - List available applications\r\n");
    printf("\topen <Application Name:string>\t - Open application by name\r\n");
    printf("\tinfo\t - Show loader state\r\n");
}

static void loader_cli_open(Cli* cli, FuriString* args, Loader* instance) {
    UNUSED(cli);
    if(loader_is_locked(instance)) {
        if(instance->application) {
            furi_assert(instance->application->name);
            printf("Can't start, %s application is running", instance->application->name);
        } else {
            printf("Can't start, furi application is running");
        }
        return;
    }

    FuriString* application_name;
    application_name = furi_string_alloc();

    do {
        if(!args_read_probably_quoted_string_and_trim(args, application_name)) {
            printf("No application provided\r\n");
            break;
        }

        const FlipperApplication* application =
            loader_find_application_by_name(furi_string_get_cstr(application_name));
        if(!application) {
            printf("%s doesn't exists\r\n", furi_string_get_cstr(application_name));
            break;
        }

        furi_string_trim(args);
        if(!loader_start_application(application, furi_string_get_cstr(args))) {
            printf("Can't start, furi application is running");
            return;
        } else {
            // We must to increment lock counter to keep balance
            // TODO: rewrite whole thing, it's complex as hell
            FURI_CRITICAL_ENTER();
            instance->lock_count++;
            FURI_CRITICAL_EXIT();
        }
    } while(false);

    furi_string_free(application_name);
}

static void loader_cli_list(Cli* cli, FuriString* args, Loader* instance) {
    UNUSED(cli);
    UNUSED(args);
    UNUSED(instance);
    printf("Applications:\r\n");
    for(size_t i = 0; i < FLIPPER_APPS_COUNT; i++) {
        printf("\t%s\r\n", FLIPPER_APPS[i].name);
    }

    printf("Plugins:\r\n");
    for(size_t i = 0; i < FLIPPER_PLUGINS_COUNT; i++) {
        printf("\t%s\r\n", FLIPPER_PLUGINS[i].name);
    }

    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug)) {
        printf("Debug:\r\n");
        for(size_t i = 0; i < FLIPPER_DEBUG_APPS_COUNT; i++) {
            printf("\t%s\r\n", FLIPPER_DEBUG_APPS[i].name);
        }
    }
}

static void loader_cli_info(Cli* cli, FuriString* args, Loader* instance) {
    UNUSED(cli);
    UNUSED(args);
    if(!loader_is_locked(instance)) {
        printf("No application is running\r\n");
    } else {
        printf("Running application: ");
        if(instance->application) {
            furi_assert(instance->application->name);
            printf("%s\r\n", instance->application->name);
        } else {
            printf("unknown\r\n");
        }
    }
}

static void loader_cli(Cli* cli, FuriString* args, void* _ctx) {
    furi_assert(_ctx);
    Loader* instance = _ctx;

    FuriString* cmd;
    cmd = furi_string_alloc();

    do {
        if(!args_read_string_and_trim(args, cmd)) {
            loader_cli_print_usage();
            break;
        }

        if(furi_string_cmp_str(cmd, "list") == 0) {
            loader_cli_list(cli, args, instance);
            break;
        }

        if(furi_string_cmp_str(cmd, "open") == 0) {
            loader_cli_open(cli, args, instance);
            break;
        }

        if(furi_string_cmp_str(cmd, "info") == 0) {
            loader_cli_info(cli, args, instance);
            break;
        }

        loader_cli_print_usage();
    } while(false);

    furi_string_free(cmd);
}

LoaderStatus loader_start(Loader* instance, const char* name, const char* args) {
    UNUSED(instance);
    furi_assert(name);

    const FlipperApplication* application = loader_find_application_by_name(name);

    if(!application) {
        FURI_LOG_E(TAG, "Can't find application with name %s", name);
        return LoaderStatusErrorUnknownApp;
    }

    if(!loader_lock(loader_instance)) {
        FURI_LOG_E(TAG, "Loader is locked");
        return LoaderStatusErrorAppStarted;
    }

    if(!loader_start_application(application, args)) {
        return LoaderStatusErrorInternal;
    }

    return LoaderStatusOk;
}

bool loader_lock(Loader* instance) {
    FURI_CRITICAL_ENTER();
    bool result = false;
    if(instance->lock_count == 0) {
        instance->lock_count++;
        result = true;
    }
    FURI_CRITICAL_EXIT();
    return result;
}

void loader_unlock(Loader* instance) {
    FURI_CRITICAL_ENTER();
    if(instance->lock_count > 0) instance->lock_count--;
    FURI_CRITICAL_EXIT();
}

bool loader_is_locked(const Loader* instance) {
    return instance->lock_count > 0;
}

static void loader_thread_state_callback(FuriThreadState thread_state, void* context) {
    furi_assert(context);

    Loader* instance = context;
    LoaderEvent event;

    if(thread_state == FuriThreadStateRunning) {
        event.type = LoaderEventTypeApplicationStarted;
        furi_pubsub_publish(loader_instance->pubsub, &event);

        if(!(loader_instance->application->flags & FlipperApplicationFlagInsomniaSafe)) {
            furi_hal_power_insomnia_enter();
        }
    } else if(thread_state == FuriThreadStateStopped) {
        FURI_LOG_I(TAG, "Application stopped. Free heap: %zu", memmgr_get_free_heap());

        if(loader_instance->application_arguments) {
            free(loader_instance->application_arguments);
            loader_instance->application_arguments = NULL;
        }

        if(!(loader_instance->application->flags & FlipperApplicationFlagInsomniaSafe)) {
            furi_hal_power_insomnia_exit();
        }
        loader_unlock(instance);

        event.type = LoaderEventTypeApplicationStopped;
        furi_pubsub_publish(loader_instance->pubsub, &event);
    }
}

static uint32_t loader_hide_menu(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t loader_back_to_primary_menu(void* context) {
    furi_assert(context);
    Submenu* submenu = context;
    submenu_set_selected_item(submenu, 0);
    return LoaderMenuViewPrimary;
}

static Loader* loader_alloc() {
    Loader* instance = malloc(sizeof(Loader));

    instance->application_thread = furi_thread_alloc();

    furi_thread_set_state_context(instance->application_thread, instance);
    furi_thread_set_state_callback(instance->application_thread, loader_thread_state_callback);

    instance->pubsub = furi_pubsub_alloc();

#ifdef SRV_CLI
    instance->cli = furi_record_open(RECORD_CLI);
    cli_add_command(
        instance->cli, RECORD_LOADER, CliCommandFlagParallelSafe, loader_cli, instance);
#else
    UNUSED(loader_cli);
#endif

    instance->loader_thread = furi_thread_get_current_id();

    // Gui
    instance->gui = furi_record_open(RECORD_GUI);
    instance->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(
        instance->view_dispatcher, instance->gui, ViewDispatcherTypeFullscreen);
    // Primary menu
    instance->primary_menu = menu_alloc();
    view_set_previous_callback(menu_get_view(instance->primary_menu), loader_hide_menu);
    view_dispatcher_add_view(
        instance->view_dispatcher, LoaderMenuViewPrimary, menu_get_view(instance->primary_menu));
    // Plugins menu
    instance->plugins_menu = submenu_alloc();
    view_set_context(submenu_get_view(instance->plugins_menu), instance->plugins_menu);
    view_set_previous_callback(
        submenu_get_view(instance->plugins_menu), loader_back_to_primary_menu);
    view_dispatcher_add_view(
        instance->view_dispatcher,
        LoaderMenuViewPlugins,
        submenu_get_view(instance->plugins_menu));
    // Debug menu
    instance->debug_menu = submenu_alloc();
    view_set_context(submenu_get_view(instance->debug_menu), instance->debug_menu);
    view_set_previous_callback(
        submenu_get_view(instance->debug_menu), loader_back_to_primary_menu);
    view_dispatcher_add_view(
        instance->view_dispatcher, LoaderMenuViewDebug, submenu_get_view(instance->debug_menu));
    // Settings menu
    instance->settings_menu = submenu_alloc();
    view_set_context(submenu_get_view(instance->settings_menu), instance->settings_menu);
    view_set_previous_callback(
        submenu_get_view(instance->settings_menu), loader_back_to_primary_menu);
    view_dispatcher_add_view(
        instance->view_dispatcher,
        LoaderMenuViewSettings,
        submenu_get_view(instance->settings_menu));

    view_dispatcher_enable_queue(instance->view_dispatcher);

    return instance;
}

static void loader_free(Loader* instance) {
    furi_assert(instance);

    if(instance->cli) {
        furi_record_close(RECORD_CLI);
    }

    furi_pubsub_free(instance->pubsub);

    furi_thread_free(instance->application_thread);

    menu_free(loader_instance->primary_menu);
    view_dispatcher_remove_view(loader_instance->view_dispatcher, LoaderMenuViewPrimary);
    submenu_free(loader_instance->plugins_menu);
    view_dispatcher_remove_view(loader_instance->view_dispatcher, LoaderMenuViewPlugins);
    submenu_free(loader_instance->debug_menu);
    view_dispatcher_remove_view(loader_instance->view_dispatcher, LoaderMenuViewDebug);
    submenu_free(loader_instance->settings_menu);
    view_dispatcher_remove_view(loader_instance->view_dispatcher, LoaderMenuViewSettings);
    view_dispatcher_free(loader_instance->view_dispatcher);

    furi_record_close(RECORD_GUI);

    free(instance);
    instance = NULL;
}

const Icon* loader_get_main_icon(char* name) {
    return NULL;
}

static void loader_build_menu() {
    FURI_LOG_I(TAG, "Building main menu");
    size_t i;
    for(i = 0; i < FLIPPER_APPS_COUNT; i++) {
        menu_add_item(
            loader_instance->primary_menu,
            FLIPPER_APPS[i].name,
            FLIPPER_APPS[i].icon,
            i,
            loader_menu_callback,
            (void*)&FLIPPER_APPS[i]);
    }
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* folder = storage_file_alloc(storage);
    FileInfo info;
    char* name = malloc(100);
    if(storage_dir_open(folder, EXT_PATH("apps/.Main"))) {
        FuriString* path = furi_string_alloc();
        FuriString* appname = furi_string_alloc();
        while(storage_dir_read(folder, &info, name, 100)) {
            if(file_info_is_dir(&info)) continue;
            furi_string_printf(path, EXT_PATH("apps/.Main/%s"), name);
            if(!fap_loader_load_name_and_icon(path, storage, NULL, appname)) continue;
            const Icon* icon = loader_get_main_icon(name);
            menu_add_item(
                loader_instance->primary_menu,
                strdup(furi_string_get_cstr(appname)),
                icon,
                i++,
                loader_main_callback,
                (void*)strdup(furi_string_get_cstr(path)));
        }
        furi_string_free(appname);
        furi_string_free(path);
    }
    free(name);
    storage_file_free(folder);
    furi_record_close(RECORD_STORAGE);
    if(FLIPPER_PLUGINS_COUNT != 0) {
        menu_add_item(
            loader_instance->primary_menu,
            "Plugins",
            &A_Plugins_14,
            i++,
            loader_submenu_callback,
            (void*)LoaderMenuViewPlugins);
    }
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug) && (FLIPPER_DEBUG_APPS_COUNT > 0)) {
        menu_add_item(
            loader_instance->primary_menu,
            "Debug Tools",
            &A_Debug_14,
            i++,
            loader_submenu_callback,
            (void*)LoaderMenuViewDebug);
    }
    menu_add_item(
        loader_instance->primary_menu,
        "Settings",
        &A_Settings_14,
        i++,
        loader_submenu_callback,
        (void*)LoaderMenuViewSettings);
}

static void loader_build_submenu() {
    FURI_LOG_I(TAG, "Building plugins menu");
    size_t i;
    for(i = 0; i < FLIPPER_PLUGINS_COUNT; i++) {
        submenu_add_item(
            loader_instance->plugins_menu,
            FLIPPER_PLUGINS[i].name,
            i,
            loader_menu_callback,
            (void*)&FLIPPER_PLUGINS[i]);
    }

    FURI_LOG_I(TAG, "Building debug menu");
    for(i = 0; i < FLIPPER_DEBUG_APPS_COUNT; i++) {
        submenu_add_item(
            loader_instance->debug_menu,
            FLIPPER_DEBUG_APPS[i].name,
            i,
            loader_menu_callback,
            (void*)&FLIPPER_DEBUG_APPS[i]);
    }

    FURI_LOG_I(TAG, "Building settings menu");
    for(i = 0; i < FLIPPER_SETTINGS_APPS_COUNT; i++) {
        submenu_add_item(
            loader_instance->settings_menu,
            FLIPPER_SETTINGS_APPS[i].name,
            i,
            loader_menu_callback,
            (void*)&FLIPPER_SETTINGS_APPS[i]);
    }
}

void loader_show_menu() {
    furi_assert(loader_instance);
    furi_thread_flags_set(loader_instance->loader_thread, LOADER_THREAD_FLAG_SHOW_MENU);
}

void loader_update_menu() {
    menu_reset(loader_instance->primary_menu);
    loader_build_menu();
}

int32_t loader_srv(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "Executing system start hooks");
    for(size_t i = 0; i < FLIPPER_ON_SYSTEM_START_COUNT; i++) {
        FLIPPER_ON_SYSTEM_START[i]();
    }

    FURI_LOG_I(TAG, "Starting");
    loader_instance = loader_alloc();

    loader_build_menu();
    loader_build_submenu();

    FURI_LOG_I(TAG, "Started");

    furi_record_create(RECORD_LOADER, loader_instance);

    if(FLIPPER_AUTORUN_APP_NAME && strlen(FLIPPER_AUTORUN_APP_NAME)) {
        loader_start(loader_instance, FLIPPER_AUTORUN_APP_NAME, NULL);
    }

    while(1) {
        uint32_t flags =
            furi_thread_flags_wait(LOADER_THREAD_FLAG_ALL, FuriFlagWaitAny, FuriWaitForever);
        if(flags & LOADER_THREAD_FLAG_SHOW_MENU) {
            menu_set_selected_item(loader_instance->primary_menu, 0);
            view_dispatcher_switch_to_view(
                loader_instance->view_dispatcher, LoaderMenuViewPrimary);
            view_dispatcher_run(loader_instance->view_dispatcher);
        }
    }

    furi_record_destroy(RECORD_LOADER);
    loader_free(loader_instance);

    return 0;
}

FuriPubSub* loader_get_pubsub(Loader* instance) {
    return instance->pubsub;
}
