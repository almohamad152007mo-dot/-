#include <jni.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>

extern char **environ;

static std::string jstr(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return result;
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_flutteride_codweva_NativePty_nativeStart(
        JNIEnv* env, jclass, jstring executable, jobjectArray arguments,
        jobjectArray environment, jstring workingDirectory) {
    std::string exe = jstr(env, executable);
    std::string cwd = jstr(env, workingDirectory);

    std::vector<std::string> argStrings;
    argStrings.push_back(exe);
    if (arguments) {
        jsize count = env->GetArrayLength(arguments);
        for (jsize i = 0; i < count; ++i) {
            auto item = (jstring) env->GetObjectArrayElement(arguments, i);
            argStrings.push_back(jstr(env, item));
            env->DeleteLocalRef(item);
        }
    }
    std::vector<char*> argv;
    for (auto& item : argStrings) argv.push_back(item.data());
    argv.push_back(nullptr);

    std::vector<std::string> envStrings;
    if (environment) {
        jsize count = env->GetArrayLength(environment);
        for (jsize i = 0; i < count; ++i) {
            auto item = (jstring) env->GetObjectArrayElement(environment, i);
            envStrings.push_back(jstr(env, item));
            env->DeleteLocalRef(item);
        }
    }

    // Preserve Android's linker/runtime variables, then override only the
    // environment keys explicitly supplied by the Kotlin caller.
    std::vector<std::string> finalEnvironment;
    for (char** current = environ; current && *current; ++current) {
        std::string existing(*current);
        auto equal = existing.find('=');
        std::string key = existing.substr(0, equal);
        bool replaced = false;
        for (const auto& replacement : envStrings) {
            auto split = replacement.find('=');
            if (split != std::string::npos &&
                replacement.substr(0, split) == key) {
                replaced = true;
                break;
            }
        }
        if (!replaced) finalEnvironment.push_back(existing);
    }
    for (const auto& replacement : envStrings) finalEnvironment.push_back(replacement);
    std::vector<char*> envp;
    for (auto& item : finalEnvironment) envp.push_back(item.data());
    envp.push_back(nullptr);

    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        jlong values[3] = {-1, -1, errno};
        jlongArray result = env->NewLongArray(3);
        env->SetLongArrayRegion(result, 0, 3, values);
        return result;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        int error = errno;
        close(master);
        jlong values[3] = {-1, -1, error};
        jlongArray result = env->NewLongArray(3);
        env->SetLongArrayRegion(result, 0, 3, values);
        return result;
    }

    char slaveName[256] = {};
    if (ptsname_r(master, slaveName, sizeof(slaveName)) != 0) {
        int error = errno ? errno : EIO;
        close(master);
        jlong values[3] = {-1, -1, error};
        jlongArray result = env->NewLongArray(3);
        env->SetLongArrayRegion(result, 0, 3, values);
        return result;
    }

    struct winsize size {};
    size.ws_row = 24;
    size.ws_col = 80;
    ioctl(master, TIOCSWINSZ, &size);

    pid_t pid = fork();
    if (pid < 0) {
        int error = errno;
        close(master);
        jlong values[3] = {-1, -1, error};
        jlongArray result = env->NewLongArray(3);
        env->SetLongArrayRegion(result, 0, 3, values);
        return result;
    }

    if (pid == 0) {
        close(master);
        if (setsid() < 0) _exit(120);
        int slave = open(slaveName, O_RDWR);
        if (slave < 0) _exit(121);
        if (ioctl(slave, TIOCSCTTY, 0) != 0) _exit(122);
        if (dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) _exit(123);
        if (slave > STDERR_FILENO) close(slave);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(124);
        execve(exe.c_str(), argv.data(), envp.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    jlong values[3] = {master, pid, 0};
    jlongArray result = env->NewLongArray(3);
    env->SetLongArrayRegion(result, 0, 3, values);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_flutteride_codweva_NativePty_nativeRead(
        JNIEnv* env, jclass, jint fd, jbyteArray buffer, jint offset, jint length) {
    if (length <= 0) return 0;
    std::vector<jbyte> temp((size_t) length);
    ssize_t count;
    do { count = read(fd, temp.data(), (size_t) length); } while (count < 0 && errno == EINTR);
    if (count > 0) env->SetByteArrayRegion(buffer, offset, (jsize) count, temp.data());
    return (jint) count;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_flutteride_codweva_NativePty_nativeWrite(
        JNIEnv* env, jclass, jint fd, jbyteArray data, jint offset, jint length) {
    if (length <= 0) return 0;
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) return -1;
    ssize_t count;
    do { count = write(fd, bytes + offset, (size_t) length); } while (count < 0 && errno == EINTR);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    return (jint) count;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_flutteride_codweva_NativePty_nativeWait(
        JNIEnv*, jclass, jint childPid) {
    int status = 0;
    pid_t result;
    do { result = waitpid((pid_t) childPid, &status, 0); } while (result < 0 && errno == EINTR);
    if (result < 0) return -errno;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_flutteride_codweva_NativePty_nativeSignal(
        JNIEnv*, jclass, jint childPid, jint signalNumber) {
    if (kill(-((pid_t) childPid), signalNumber) == 0) return 0;
    if (errno == ESRCH && kill((pid_t) childPid, signalNumber) == 0) return 0;
    return -errno;
}

extern "C" JNIEXPORT void JNICALL
Java_com_flutteride_codweva_NativePty_nativeClose(
        JNIEnv*, jclass, jint fd) {
    if (fd >= 0) close(fd);
}
