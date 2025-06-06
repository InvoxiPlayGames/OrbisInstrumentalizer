/*
    main.c - OrbisInstrumentalizer
    Initialisation code for the OrbisInstrumentalizer plugin.
    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define attr_module_hidden __attribute__((weak)) __attribute__((visibility("hidden")))
#define attr_public __attribute__((visibility("default")))

#define PLUGIN_NAME "OrbisInstrumentalizer"
#define final_printf(a, args...) klog("[" PLUGIN_NAME "] " a, ##args)

#include <GoldHEN/Common.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>

attr_public const char *g_pluginName = PLUGIN_NAME;
attr_public const char *g_pluginDesc = "Use other platform's plastic instruments on a PS4.";
attr_public const char *g_pluginAuth = "InvoxiPlayGames";
attr_public uint32_t g_pluginVersion = 0x00000100; // 1.00

static struct proc_info procInfo;

void InitPadHooks();
void DestroyPadHooks();
static bool UsingPadHooks = false;

void InitUsbdHooks();
void DestroyUsbdHooks();
static bool UsingUsbdHooks = false;

void set_xbox_360_player_led();

// Guitar Hero Live
static const char *PadHookTitleIDs[] = { "CUSA02410", "CUSA02188" };
static const int num_PadHookTitleIDs = sizeof(PadHookTitleIDs) / sizeof(PadHookTitleIDs[0]);

// Rock Band 4
static const char *UsbdHookTitleIDs[] = { "CUSA02901", "CUSA02084" };
static const int num_UsbdHookTitleIDs = sizeof(UsbdHookTitleIDs) / sizeof(UsbdHookTitleIDs[0]);

void DoNotification(const char* text) {
    OrbisNotificationRequest Buffer = { 0 };
    Buffer.useIconImageUri = 1;
    Buffer.targetId = -1;
    strcpy(Buffer.message, text);
    strcpy(Buffer.iconUri, "cxml://psnotification/tex_icon_system");
    sceKernelSendNotificationRequest(0, &Buffer, sizeof(Buffer), 0);
}

int32_t attr_module_hidden module_start(size_t argc, const void *args)
{
    if (sys_sdk_proc_info(&procInfo) != 0) {
        final_printf("Failed to get process info!\n");
        return 0;
    }

    final_printf("Started plugin! Title ID: %s\n", procInfo.titleid);

    for (int i = 0; i < num_PadHookTitleIDs; i++) {
        if (strcmp(procInfo.titleid, PadHookTitleIDs[i]) == 0) {
            final_printf("Applying scePad hooks...\n");
            DoNotification("OrbisInstrumentalizer active!");
            InitPadHooks();
            UsingPadHooks = true;
        }
    }

    for (int i = 0; i < num_UsbdHookTitleIDs; i++) {
        if (strcmp(procInfo.titleid, UsbdHookTitleIDs[i]) == 0) {
            final_printf("Applying sceUsbd hooks...\n");
            DoNotification("OrbisInstrumentalizer active!");
            InitUsbdHooks();
            UsingUsbdHooks = true;
        }
    }
    return 0;
}

int32_t attr_module_hidden module_stop(size_t argc, const void *args)
{
    final_printf("Stopping plugin...\n");

    if (UsingPadHooks)
        DestroyPadHooks();

    if (UsingUsbdHooks)
        DestroyUsbdHooks();

    return 0;
}
