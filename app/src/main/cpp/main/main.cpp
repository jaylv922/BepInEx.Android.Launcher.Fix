#include <jni.h>
#include <dlfcn.h>
#include <string>
#include <cstdlib>
#include <android/log.h>
#ifdef OXIDE_NATIVE_DIAGNOSTIC
#include <link.h>
#include <pthread.h>
#include <cstring>
#include <unistd.h>
#endif

#define LOG_TAG "LibMain"
#define LOGI(fmt, ...) \
do { __android_log_print(ANDROID_LOG_INFO, LOG_TAG, fmt, ##__VA_ARGS__); \
if (logFile) { \
    fprintf(logFile, fmt "\n", ##__VA_ARGS__); \
    fflush(logFile); \
}} while (0);

#define LOGE(fmt, ...) \
do { __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, fmt, ##__VA_ARGS__); \
if (logFile) { \
    fprintf(logFile, fmt "\n", ##__VA_ARGS__); \
    fflush(logFile); \
}} while (0);

using JNI_OnLoad_t = jint (*)(JavaVM *vm, void *reserved);
using JNI_Unload_t = void (*)(JavaVM *vm, void *reserved);
using FusionBootstrapFromLibMain_t = bool (*)(JNIEnv *env);
using FusionJvmInit_t = void (*)(JavaVM *vm);

static FILE *logFile = nullptr;
static std::string log_path;
static std::string override_unity_path;
static std::string override_il2cpp_path;

static void *unityLibHandle = nullptr;
static void *il2cppLibHandle = nullptr;

static JavaVM *g_vm;

#ifdef OXIDE_NATIVE_DIAGNOSTIC
// Observe already-loaded native code only. No hooks, memory writes or
// attempts to resolve any obfuscated function addresses are made.
#define OXDIAG(fmt, ...) do { \
    __android_log_print(ANDROID_LOG_INFO, "OxideNativeDiag", fmt, ##__VA_ARGS__); \
    if (logFile) { fprintf(logFile, "[OxideNativeDiag] " fmt "\n", ##__VA_ARGS__); fflush(logFile); } \
} while (0)

static bool diag_relevant(const char *path) {
    if (!path || !*path) return false;
    const char *names[] = {"libil2cpp.so", "libunity.so", "libloader.so",
                           "libmain.so", "libfusion.so", "libnms.so",
                           "libpglarmor.so"};
    for (const char *name : names) {
        const char *slash = strrchr(path, '/');
        if (strcmp(slash ? slash + 1 : path, name) == 0) return true;
    }
    return false;
}

static int diag_loaded_library(dl_phdr_info *info, size_t, void *data) {
    auto *count = static_cast<int *>(data);
    if (info->dlpi_name && diag_relevant(info->dlpi_name) && *count < 24) {
        OXDIAG("loaded[%d]: %s base=%p", *count, info->dlpi_name,
               reinterpret_cast<void *>(info->dlpi_addr));
        ++*count;
    }
    return 0;
}

static void oxide_diag_snapshot(const char *stage) {
    OXDIAG("=== %s pid=%d ===", stage, getpid());
    int count = 0;
    dl_iterate_phdr(diag_loaded_library, &count);
    OXDIAG("loaded module matches=%d", count);
    if (il2cppLibHandle) {
        const char *names[] = {"il2cpp_init", "il2cpp_domain_get", "il2cpp_thread_attach"};
        for (const char *name : names) {
            dlerror();
            void *sym = dlsym(il2cppLibHandle, name);
            const char *err = dlerror();
            OXDIAG("existing IL2CPP handle dlsym(%s)=%s (%s)",
                   name, sym ? "FOUND" : "MISSING", err ? err : "ok");
        }
    } else {
        OXDIAG("IL2CPP handle not initialized at snapshot");
    }
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        OXDIAG("Cannot read /proc/self/maps");
        return;
    }
    char line[1024];
    int printed = 0;
    while (fgets(line, sizeof(line), maps) && printed < 32) {
        if (strstr(line, ".so") && (strstr(line, "libil2cpp") ||
            strstr(line, "libunity") || strstr(line, "libloader") ||
            strstr(line, "libfusion") || strstr(line, "libnms") ||
            strstr(line, "libpglarmor"))) {
            size_t len = strlen(line);
            if (len && line[len - 1] == '\n') line[len - 1] = 0;
            OXDIAG("maps: %s", line);
            ++printed;
        }
    }
    fclose(maps);
}

static void *oxide_diag_delayed(void *) {
    sleep(2);
    oxide_diag_snapshot("after 2 seconds");
    sleep(3);
    oxide_diag_snapshot("after 5 seconds");
    sleep(5);
    oxide_diag_snapshot("after 10 seconds");
    return nullptr;
}

static void oxide_diag_begin() {
    oxide_diag_snapshot("after native-loader success");
    pthread_t thread{};
    int rc = pthread_create(&thread, nullptr, oxide_diag_delayed, nullptr);
    if (rc == 0) pthread_detach(thread);
    else OXDIAG("Diagnostic pthread_create failed: %d", rc);
}
#endif

static std::string build_sibling_library_path(const char *libraryFileName)
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&build_sibling_library_path), &info) == 0 || !info.dli_fname) {
        return {};
    }

    char *canonicalPath = realpath(info.dli_fname, nullptr);
    std::string selfPath(canonicalPath ? canonicalPath : info.dli_fname);
    free(canonicalPath);
    size_t lastSlash = selfPath.find_last_of('/');
    if (lastSlash == std::string::npos) {
        return {};
    }

    return selfPath.substr(0, lastSlash + 1) + libraryFileName;
}

static bool preload_sibling_library(const char *libraryFileName)
{
    const std::string siblingPath = build_sibling_library_path(libraryFileName);
    if (siblingPath.empty()) {
        LOGE("preload_sibling_library: failed to build sibling path for %s", libraryFileName)
        return false;
    }

    dlerror();
    void *handle = dlopen(siblingPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        const char *err = dlerror();
        LOGE("preload_sibling_library: dlopen failed for %s at %s: %s",
             libraryFileName, siblingPath.c_str(), err ? err : "(no dlerror)")
        return false;
    }

    LOGI("preload_sibling_library: loaded %s from %s", libraryFileName, siblingPath.c_str())
    return true;
}

static bool preload_dotnet_runtime_libraries()
{
    const char *required[] = {
            "libcrypto.so",
            "libssl.so",
            "libcoreclr.so",
            "libclrjit.so",
            "libSystem.Native.so",
            "libSystem.Globalization.Native.so",
            "libSystem.IO.Compression.Native.so",
            "libSystem.Security.Cryptography.Native.OpenSsl.so"
    };

    for (const char *libraryName : required) {
        if (!preload_sibling_library(libraryName)) {
            LOGE("preload_dotnet_runtime_libraries: failed to load required runtime library %s", libraryName)
            return false;
        }
    }

    // Optional diagnostics/profiling helpers used by some runtimes.
    preload_sibling_library("libmscordaccore.so");
    preload_sibling_library("libmscordbi.so");
    return true;
}

static void *resolve_or_load_fusion_handle()
{
    dlerror();
    void *fusionHandle = dlopen("libfusion.so", RTLD_NOW | RTLD_NOLOAD);
    if (fusionHandle) {
        return fusionHandle;
    }

    const char *defaultScopeError = dlerror();
    if (defaultScopeError) {
        LOGI("resolve_or_load_fusion_handle: libfusion.so missing from namespace: %s", defaultScopeError)
    }

    preload_sibling_library("libdobby.so");
    if (!preload_dotnet_runtime_libraries()) {
        return nullptr;
    }

    if (!preload_sibling_library("libfusion.so")) {
        return nullptr;
    }

    dlerror();
    fusionHandle = dlopen("libfusion.so", RTLD_NOW | RTLD_GLOBAL);
    if (!fusionHandle) {
        const char *noLoadErr = dlerror();
        LOGE("resolve_or_load_fusion_handle: libfusion.so still not visible after preload: %s",
             noLoadErr ? noLoadErr : "(no dlerror)")
        return nullptr;
    }

    LOGI("resolve_or_load_fusion_handle: loaded libfusion.so into current namespace via sibling preload")
    return fusionHandle;
}

static bool resolve_fusion_symbols(FusionBootstrapFromLibMain_t *bootstrap)
{
    void *fusionHandle = resolve_or_load_fusion_handle();
    if (!fusionHandle) {
        return false;
    }

    dlerror();
    *bootstrap = reinterpret_cast<FusionBootstrapFromLibMain_t>(dlsym(fusionHandle, "fusion_bootstrap_from_libmain"));
    if (!*bootstrap) {
        const char *symErr = dlerror();
        LOGE("resolve_fusion_symbols: dlsym failed for fusion_bootstrap_from_libmain: %s",
             symErr ? symErr : "(no dlerror)")
        return false;
    }

    FusionJvmInit_t loadJni = reinterpret_cast<FusionJvmInit_t>(dlsym(fusionHandle, "init_java"));
    if (loadJni) {
        loadJni(g_vm);
        LOGI("initialized fusion java")
    } else {
        LOGE("failed to init fusion java")
    }

    return true;
}

jboolean internal_load(JNIEnv *env, const char *libraryPath, void **libHandle)
{
    if (!libraryPath) {
        LOGE("internal_load: libraryPath is null")
        return JNI_FALSE;
    }

    LOGI("internal_load: attempting to load '%s'", libraryPath)

    void *handle = dlopen(libraryPath, RTLD_LAZY | RTLD_LOCAL);

    if (!handle)
    {
        const char *err = dlerror();
        LOGE("dlopen failed for '%s': %s", libraryPath, err ? err : "(no dlerror)")
        return JNI_FALSE; // Failed to load Unity library
    }

    JNI_OnLoad_t jniOnLoad = reinterpret_cast<JNI_OnLoad_t>(dlsym(handle, "JNI_OnLoad"));
    if (!jniOnLoad)
    {
        const char *err = dlerror();
        LOGE("dlsym JNI_OnLoad not found in '%s': %s", libraryPath, err ? err : "(no dlerror)")
        // some libil2cpp.so do not have JNI_OnLoad, so we will not treat this as an error. Just log and continue.
    }
    else
    {
        JavaVM *vm = nullptr;
        if (env->GetJavaVM(&vm) != JNI_OK)
        {
            LOGE("internal_load: GetJavaVM failed for '%s'", libraryPath)
            dlclose(handle);
            handle = nullptr;
            return JNI_FALSE; // Failed to obtain Java VM
        }

        jint result = jniOnLoad(vm, nullptr);
        if (result < JNI_VERSION_1_6)
        {
            LOGE("JNI_OnLoad in '%s' returned version %d (expected >= %d)", libraryPath, result, JNI_VERSION_1_6)
            dlclose(handle);
            handle = nullptr;
            return JNI_FALSE; // JNI version mismatch
        }
    }

    *libHandle = handle;
    LOGI("internal_load: successfully loaded '%s'", libraryPath)
    return JNI_TRUE; // Successfully loaded Unity library
}

jboolean internal_unload(JNIEnv *env, void **libHandle)
{
    if (!*libHandle)
    {
        LOGI("internal_unload: libHandle is null, nothing to unload")
        return JNI_FALSE; // Library not loaded
    }

    JavaVM *vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK)
    {
        LOGE("internal_unload: GetJavaVM failed")
        return JNI_FALSE; // Failed to obtain Java VM
    }

    void *handle = *libHandle;
    *libHandle = nullptr;

    void *unload = dlsym(handle, "JNI_Unload");
    if (unload)
    {
        JNI_Unload_t jniUnload = reinterpret_cast<JNI_Unload_t>(unload);
        LOGI("internal_unload: calling JNI_Unload")
        jniUnload(vm, nullptr);
    }
    else
    {
        const char *err = dlerror();
        LOGI("internal_unload: JNI_Unload not found: %s", err ? err : "(no dlerror)")
    }

    dlclose(handle);
    LOGI("internal_unload: successfully unloaded library")
    return JNI_TRUE; // Successfully unloaded library
}

extern "C" {

void libmain_set_override_unity_path(const char *path)
{
    override_unity_path = path;
    LOGI("set_override_unity_path: %s", path ? path : "(null)")
}

void libmain_set_override_il2cpp_path(const char *path)
{
    override_il2cpp_path = path;
    LOGI("set_override_il2cpp_path: %s", path ? path : "(null)")
}

const char *libmain_get_override_unity_path()
{
    return override_unity_path.c_str();
}

const char *libmain_get_override_il2cpp_path()
{
    return override_il2cpp_path.c_str();
}

void libmain_set_log_path(const char *path)
{
    log_path = path;
    if (logFile)
    {
        fclose(logFile);
        logFile = nullptr;
    }

    if (!log_path.empty())
    {
        logFile = fopen(log_path.c_str(), "w");
        if (logFile)
        {
            LOGI("Logging initialized at %s", log_path.c_str())
        }
        else
        {
            LOGE("Failed to open log file at %s", log_path.c_str())
        }
    }
}

JNIEXPORT jboolean JNICALL
load(JNIEnv *env, jobject activityObject, jstring path)
{
    (void) path;

    FusionBootstrapFromLibMain_t bootstrap = nullptr;
    if (!resolve_fusion_symbols(&bootstrap)) {
        return JNI_FALSE;
    }

    if (!bootstrap(env))
    {
        LOGE("load: fusion_bootstrap_from_libmain failed")
        return JNI_FALSE;
    }

    const char *unityPath = override_unity_path.c_str();
    const char *il2cppPath = override_il2cpp_path.c_str();
    LOGI("load: unityPath=%s, il2cppPath=%s", unityPath ? unityPath : "(null)", il2cppPath ? il2cppPath : "(null)")

    if (override_unity_path.empty() || override_il2cpp_path.empty())
    {
        LOGE("load: paths not set")
        return JNI_FALSE; // Paths not set
    }

    if (!internal_load(env, unityPath, &unityLibHandle))
    {
        LOGE("load: failed to load Unity library from %s", unityPath)
        return JNI_FALSE; // Failed to load Unity library
    }

    if (!internal_load(env, il2cppPath, &il2cppLibHandle))
    {
        // Unload previously loaded Unity library
        if (unityLibHandle)
        {
            LOGI("load: unloading previously loaded Unity library due to IL2CPP load failure")
            internal_unload(env, &unityLibHandle);
        }

        LOGE("load: failed to load IL2CPP library from %s", il2cppPath)
        return JNI_FALSE; // Failed to load IL2CPP library
    }

    LOGI("load: successfully loaded Unity and IL2CPP libraries")
#ifdef OXIDE_NATIVE_DIAGNOSTIC
    if (override_il2cpp_path.find("com.catsbit.oxidesurvivalisland") != std::string::npos) {
        oxide_diag_begin();
    }
#endif
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
unload(JNIEnv *env, jclass activityObject)
{
    JavaVM *vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK)
    {
        LOGE("unload: GetJavaVM failed")
        return JNI_FALSE; // Failed to obtain Java VM
    }

    if (unityLibHandle && !internal_unload(env, &unityLibHandle))
    {
        LOGE("unload: failed to unload Unity library")
        return JNI_FALSE; // Failed to unload Unity library
    }

    if (il2cppLibHandle && !internal_unload(env, &il2cppLibHandle))
    {
        LOGE("unload: failed to unload IL2CPP library")
        return JNI_FALSE; // Failed to unload IL2CPP library
    }

    if (logFile)
    {
        fclose(logFile);
        logFile = nullptr;
    }

    LOGI("unload: successfully unloaded all libraries")
    return JNI_TRUE;
}

JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved)
{
    g_vm = vm;

    JNIEnv *globalEnv;
    if (vm->GetEnv(reinterpret_cast<void **>(&globalEnv), JNI_VERSION_1_6) != JNI_OK)
    {
        LOGE("JNI_OnLoad: GetEnv failed")
        return JNI_ERR; // Failed to obtain JNIEnv
    }

    jclass clazz = globalEnv->FindClass("com/unity3d/player/NativeLoader");
    if (!clazz)
    {
        LOGE("JNI_OnLoad: FindClass com/unity3d/player/NativeLoader failed")
        return JNI_ERR; // Class not found
    }

    static const JNINativeMethod methods[] = {
            {"load",   "(Ljava/lang/String;)Z", reinterpret_cast<void *>(load)},
            {"unload", "()Z",                   reinterpret_cast<void *>(unload)}
    };

    jint ret = globalEnv->RegisterNatives(clazz, methods,
                                          sizeof(methods) / sizeof(JNINativeMethod));
    if (ret != JNI_OK)
    {
        LOGE("JNI_OnLoad: RegisterNatives failed with code %d", ret)
        return ret; // Failed to register natives
    }

    LOGI("JNI_OnLoad: successfully registered natives and initialized")
    return JNI_VERSION_1_6; // Successful initialization
}
} // extern "C"

