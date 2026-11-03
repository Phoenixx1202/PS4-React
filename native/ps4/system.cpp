#include "system.hpp"

#include <cstddef>

#include <orbis/CommonDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ps4::system {
namespace {

int log_fd = -1;

// Internal module IDs. Kept as literals so the file builds against older
// OpenOrbis headers that do not define every enum member.
constexpr std::uint32_t kSysCore = 0x80000004;
constexpr std::uint32_t kIpmi = 0x8000001D;
constexpr std::uint32_t kSystemService = 0x80000010;
constexpr std::uint32_t kUserService = 0x80000011;
constexpr std::uint32_t kPad = 0x80000024;
constexpr std::uint32_t kAudioOut = 0x80000001;
constexpr std::uint32_t kNet = 0x8000001C;
constexpr std::uint32_t kNetCtl = 0x80000009;
constexpr std::uint32_t kVideoOut = 0x80000022;
constexpr std::uint32_t kSaveData = 0x8000000F;
constexpr std::uint32_t kCommonDialog = 0x80000018;

} // namespace

void start_log(const char* name) {
    if (log_fd >= 0)
        return;

    mkdir("/data/PS4React", 0777);
    char primary[256];
    char fallback[256];
    std::snprintf(primary, sizeof primary, "/data/PS4React/%s", name);
    std::snprintf(fallback, sizeof fallback, "/temp0/%s", name);
    const char* paths[] = {primary, fallback};
    for (const char* path : paths) {
        log_fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            log("log: writing %s", path);
            return;
        }
    }
    log("log: no writable log file; klog only");
}

void log(const char* format, ...) {
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);

    // The kernel log accepts a format string; pass the line as data.
    sceKernelDebugOutText(0, "%s", line);
    std::fputs(line, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);

    if (log_fd >= 0) {
        const std::size_t length = std::strlen(line);
        (void)!::write(log_fd, line, length);
        (void)!::write(log_fd, "\n", 1);
    }
}

bool initialize_services() {
    static bool initialized = false;
    if (initialized)
        return true;

    const struct {
        std::uint32_t id;
        const char* name;
    } modules[] = {
        {kSysCore, "syscore"},
        {kIpmi, "ipmi"},
        {kSystemService, "system"},
        {kUserService, "user"},
        {kPad, "pad"},
        {kAudioOut, "audio"},
        {kNet, "net"},
        {kNetCtl, "netctl"},
        {kVideoOut, "videoout"},
        {kSaveData, "savedata"},
        {kCommonDialog, "common_dialog"},
    };
    // Rootfs OpenOrbis declares the parameter as enum OrbisSysModuleInternal;
    // older headers use uint32_t. Both are the same 32-bit ABI here.
    using LoadInternal = std::uint32_t (*)(std::uint32_t);
    const auto load_internal =
        reinterpret_cast<LoadInternal>(sceSysmoduleLoadModuleInternal);
    for (const auto& module : modules) {
        const int result = static_cast<int>(load_internal(module.id));
        log("service: %s=0x%08x", module.name, static_cast<unsigned>(result));
    }

    OrbisUserServiceInitializeParams parameters{};
    parameters.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    const int user = sceUserServiceInitialize(&parameters);
    log("service: user init=0x%08x", static_cast<unsigned>(user));

    // IME and message dialogs are public system modules, unlike the internal
    // SaveData and CommonDialog modules above. The common dialog must be
    // initialized before sceImeDialogInit.
    const int message = sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG);
    log("service: message_dialog=0x%08x", static_cast<unsigned>(message));
    const int ime = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
    log("service: ime_dialog=0x%08x", static_cast<unsigned>(ime));
    const int common = sceCommonDialogInitialize();
    log("service: common dialog init=0x%08x", static_cast<unsigned>(common));

    initialized = true;
    return true;
}

std::int64_t monotonic_us() {
    return static_cast<std::int64_t>(sceKernelGetProcessTime());
}

void sleep_us(std::uint32_t microseconds) {
    sceKernelUsleep(microseconds);
}

void hide_splash() {
    const int result = sceSystemServiceHideSplashScreen();
    if (result != 0)
        log("system: HideSplashScreen=0x%08x", static_cast<unsigned>(result));
}

void park() {
    for (;;)
        sceKernelUsleep(100000);
}

namespace {

using LncLaunchFn = std::uint32_t (*)(const char*, const char**, LncAppParam*);
using LncInitFn = int (*)();

bool dlsym_in_module(const char* module_name, const char* symbol, void** out) {
    OrbisKernelModule modules[256] = {};
    std::size_t count = 0;
    if (sceKernelGetModuleList(modules, 256, &count) != 0)
        return false;
    for (std::size_t i = 0; i < count && i < 256; ++i) {
        OrbisKernelModuleInfo info{};
        info.size = sizeof info;
        if (sceKernelGetModuleInfo(modules[i], &info) != 0)
            continue;
        if (module_name && !std::strstr(info.name, module_name))
            continue;
        if (sceKernelDlsym(static_cast<std::int32_t>(modules[i]), symbol, out) == 0 && *out)
            return true;
    }
    return false;
}

} // namespace

bool relaunch(int& result) {
    OrbisAppInfo info{};
    const pid_t pid = getpid();
    const int info_result = sceKernelGetAppInfo(pid, &info);
    if (info_result != 0 || !info.TitleId[0]) {
        result = info_result ? info_result : -1;
        log("system: relaunch GetAppInfo(pid=%d)=0x%08x",
            static_cast<int>(pid), static_cast<unsigned>(result));
        return false;
    }

    int user = -1;
    if (sceUserServiceGetForegroundUser(&user) != 0) user = 0;
    LncAppParam param{};
    param.size = sizeof param;
    param.user_id = static_cast<std::uint32_t>(user);
    param.app_opt = 0;
    param.LaunchAppCheck_flag = LaunchApp_SkipSystemUpdate;

    // sceLncUtilLaunchApp is exported by libSceSystemService on PS4. Calling
    // the linked stub traps because GoldHEN does not load libSceLncUtil, so
    // resolve the real entry point from the loaded module.
    void* launch_symbol = nullptr;
    if (!dlsym_in_module("libSceSystemService", "sceLncUtilLaunchApp", &launch_symbol))
        (void)dlsym_in_module(nullptr, "sceLncUtilLaunchApp", &launch_symbol);

    if (launch_symbol) {
        void* init_symbol = nullptr;
        if (dlsym_in_module(nullptr, "sceLncUtilInitialize", &init_symbol)) {
            const int init_result =
                reinterpret_cast<LncInitFn>(init_symbol)();
            log("system: LncUtilInitialize=0x%08x", static_cast<unsigned>(init_result));
        }
        const auto launch = reinterpret_cast<LncLaunchFn>(launch_symbol);
        const std::uint32_t code = launch(info.TitleId, nullptr, &param);
        result = static_cast<int>(code);
        log("system: LncUtilLaunchApp(%s pid=%d user=0x%08x)=0x%08x",
            info.TitleId, static_cast<int>(pid), static_cast<unsigned>(user), code);
        if ((code & 0x80000000u) == 0 ||
            code == 0x80a40043u || code == 0x80940010u)
            return true;
    } else {
        result = -1;
        log("system: sceLncUtilLaunchApp not exported by the loaded modules");
    }

    // The same title is already running, so LNC refuses the launch. Replace
    // this process image with the app's own eboot instead. libSceSystemService
    // exports both the LncUtil and SystemService entry points.
    void* exec_symbol = nullptr;
    if (dlsym_in_module(nullptr, "sceLncUtilLoadExec", &exec_symbol)) {
        const auto exec = reinterpret_cast<int (*)(const char*, const char**)>(exec_symbol);
        const int exec_result = exec("/app0/eboot.bin", nullptr);
        log("system: LncUtilLoadExec(/app0/eboot.bin)=0x%08x",
            static_cast<unsigned>(exec_result));
        if (exec_result == 0) {
            result = 0;
            return true;
        }
    }
    const int exec_result = sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
    log("system: LoadExec(/app0/eboot.bin)=0x%08x", static_cast<unsigned>(exec_result));
    result = exec_result;
    return exec_result == 0;
}

bool request_close() {
    log("system: requesting title closure");
    std::fflush(nullptr);

    // GetAppIdOfBigApp fails inside a game with LNC 0x8094000f. The kernel app
    // info carries the app id the system service expects.
    OrbisAppInfo info{};
    const pid_t pid = getpid();
    const int info_result = sceKernelGetAppInfo(pid, &info);
    log("system: GetAppInfo(pid=%d)=0x%08x appId=0x%08x",
        static_cast<int>(pid), static_cast<unsigned>(info_result),
        static_cast<unsigned>(info.AppId));
    if (info_result == 0 && info.AppId > 0) {
        // The same call the system uses for Close Application: it closes the
        // title and returns to the home screen without going through the
        // shell's PS-button flow.
        const int killed = sceSystemServiceKillApp(
            static_cast<std::uint32_t>(info.AppId), -1, 0, 0);
        log("system: KillApp(0x%08x)=0x%08x", static_cast<unsigned>(info.AppId),
            static_cast<unsigned>(killed));
        if (killed == 0)
            return true;
    }

    // Older firmware may still take the exit sentinel.
    const int result = sceSystemServiceLoadExec("exit", nullptr);
    log("system: LoadExec(exit)=0x%08x", static_cast<unsigned>(result));
    return result == 0;
}

} // namespace ps4::system
