/*
 * Copyright (c) 2014, 2015, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "jni.h"
#include "jni_util.h"

#include "ProcessHandleImpl_unix.h"

#include <procfs.h>
#include <sys/types.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Implementation of native ProcessHandleImpl functions for Solaris.
 * See ProcessHandleImpl_unix.c for more details.
 */

void os_initNative(JNIEnv *env, jclass clazz) {}

/*
 * Return pids of active processes, and optionally parent pids and
 * start times for each process.
 * For a specific non-zero pid, only the direct children are returned.
 * If the pid is zero, all active processes are returned.
 * Reads /proc and accumulates any process following the rules above.
 * The resulting pids are stored into an array of longs named jarray.
 * The number of pids is returned if they all fit.
 * If the parentArray is non-null, store also the parent pid.
 * In this case the parentArray must have the same length as the result pid array.
 * Of course in the case of a given non-zero pid all entries in the parentArray
 * will contain this pid, so this array does only make sense in the case of a given
 * zero pid.
 * If the jstimesArray is non-null, store also the start time of the pid.
 * In this case the jstimesArray must have the same length as the result pid array.
 * If the array(s) (is|are) too short, excess pids are not stored and
 * the desired length is returned.
 */
jint os_getChildren(JNIEnv *env, jlong jpid, jlongArray jarray,
                    jlongArray jparentArray, jlongArray jstimesArray) {
    DIR* dir;
    struct dirent* ptr;
    pid_t pid = (pid_t) jpid;
    jlong* pids = NULL;
    jlong* ppids = NULL;
    jlong* stimes = NULL;
    jsize parentArraySize = 0;
    jsize arraySize = 0;
    jsize stimesSize = 0;
    jsize count = 0;

    arraySize = (*env)->GetArrayLength(env, jarray);
    JNU_CHECK_EXCEPTION_RETURN(env, -1);
    if (jparentArray != NULL) {
        parentArraySize = (*env)->GetArrayLength(env, jparentArray);
        JNU_CHECK_EXCEPTION_RETURN(env, -1);

        if (arraySize != parentArraySize) {
            JNU_ThrowIllegalArgumentException(env, "array sizes not equal");
            return 0;
        }
    }
    if (jstimesArray != NULL) {
        stimesSize = (*env)->GetArrayLength(env, jstimesArray);
        JNU_CHECK_EXCEPTION_RETURN(env, -1);

        if (arraySize != stimesSize) {
            JNU_ThrowIllegalArgumentException(env, "array sizes not equal");
            return 0;
        }
    }

    /*
     * To locate the children we scan /proc looking for files that have a
     * position integer as a filename.
     */
    if ((dir = opendir("/proc")) == NULL) {
        JNU_ThrowByNameWithMessageAndLastError(env,
            "java/lang/RuntimeException", "Unable to open /proc");
        return -1;
    }

    do { // Block to break out of on Exception
        pids = (*env)->GetLongArrayElements(env, jarray, NULL);
        if (pids == NULL) {
            break;
        }
        if (jparentArray != NULL) {
            ppids  = (*env)->GetLongArrayElements(env, jparentArray, NULL);
            if (ppids == NULL) {
                break;
            }
        }
        if (jstimesArray != NULL) {
            stimes  = (*env)->GetLongArrayElements(env, jstimesArray, NULL);
            if (stimes == NULL) {
                break;
            }
        }

        while ((ptr = readdir(dir)) != NULL) {
            pid_t ppid = 0;
            jlong totalTime = 0L;
            jlong startTime = 0L;

            /* skip files that aren't numbers */
            pid_t childpid = (pid_t) atoi(ptr->d_name);
            if ((int) childpid <= 0) {
                continue;
            }

            // Get the parent pid, and start time
            ppid = os_getParentPidAndTimings(env, childpid, &totalTime, &startTime);
            if (ppid >= 0 && (pid == 0 || ppid == pid)) {
                if (count < arraySize) {
                    // Only store if it fits
                    pids[count] = (jlong) childpid;

                    if (ppids != NULL) {
                        // Store the parentPid
                        ppids[count] = (jlong) ppid;
                    }
                    if (stimes != NULL) {
                        // Store the process start time
                        stimes[count] = startTime;
                    }
                }
                count++; // Count to tabulate size needed
            }
        }
    } while (0);

    if (pids != NULL) {
        (*env)->ReleaseLongArrayElements(env, jarray, pids, 0);
    }
    if (ppids != NULL) {
        (*env)->ReleaseLongArrayElements(env, jparentArray, ppids, 0);
    }
    if (stimes != NULL) {
        (*env)->ReleaseLongArrayElements(env, jstimesArray, stimes, 0);
    }

    closedir(dir);
    // If more pids than array had size for; count will be greater than array size
    return count;
}

/**
 * Helper function to get the 'psinfo_t' data from "/proc/%d/psinfo".
 * Returns 0 on success and -1 on error.
 */
static int getPsinfo(pid_t pid, psinfo_t *psinfo) {
    FILE* fp;
    char fn[32];
    size_t ret;

    /*
     * Try to open /proc/%d/psinfo
     */
    snprintf(fn, sizeof fn, "/proc/%d/psinfo", pid);
    fp = fopen(fn, "r");
    if (fp == NULL) {
        return -1;
    }

    ret = fread(psinfo, 1, sizeof(psinfo_t), fp);
    fclose(fp);
    if (ret < sizeof(psinfo_t)) {
        return -1;
    }
    return 0;
}

pid_t os_getParentPidAndTimings(JNIEnv *env, pid_t pid, jlong *totalTime, jlong *startTime) {
    psinfo_t psinfo;

    if (getPsinfo(pid, &psinfo) < 0) {
        return -1;
    }

    // Validate the pid before returning the info
    if (kill(pid, 0) < 0) {
        return -1;
    }

    *totalTime = psinfo.pr_time.tv_sec * 1000000000L + psinfo.pr_time.tv_nsec;

    *startTime = psinfo.pr_start.tv_sec * (jlong)1000 +
                 psinfo.pr_start.tv_nsec / 1000000;

    return (pid_t) psinfo.pr_ppid;
}

void os_getCmdlineAndUserInfo(JNIEnv *env, jobject jinfo, pid_t pid) {
    psinfo_t psinfo;
    char fn[32];
    char exePath[PATH_MAX];
    char prargs[PRARGSZ + 1];
    jstring cmdexe = NULL;
    int ret;

    /*
     * The full path to the executable command is the link in
     * /proc/<pid>/paths/a.out. But it is only readable for processes we own.
     */
    snprintf(fn, sizeof fn, "/proc/%d/path/a.out", pid);
    if ((ret = readlink(fn, exePath, PATH_MAX - 1)) > 0) {
        // null terminate and create String to store for command
        exePath[ret] = '\0';
        CHECK_NULL(cmdexe = JNU_NewStringPlatform(env, exePath));
    }

    /*
     * Now try to open /proc/%d/psinfo
     */
    if (getPsinfo(pid, &psinfo) < 0) {
        unix_fillArgArray(env, jinfo, 0, NULL, NULL, cmdexe, NULL);
        return;
    }

    unix_getUserInfo(env, jinfo, psinfo.pr_uid);

    /*
     * Now read psinfo.pr_psargs which contains the first PRARGSZ characters of the
     * argument list (i.e. arg[0] arg[1] ...). Unfortunately, PRARGSZ is usually set
     * to 80 characters only. Nevertheless it's better than nothing :)
     */
    strncpy(prargs, psinfo.pr_psargs, PRARGSZ);
    prargs[PRARGSZ] = '\0';
    if (prargs[0] == '\0') {
        /* If psinfo.pr_psargs didn't contain any strings, use psinfo.pr_fname
         * (which only contains the last component of exec()ed pathname) as a
         * last resort.
         */
        strncpy(prargs, psinfo.pr_fname, PRARGSZ);
        prargs[PRARGSZ] = '\0';
    }
    unix_fillArgArray(env, jinfo, 0, NULL, NULL, cmdexe,
                      prargs[0] == '\0' ? NULL : prargs);
}

