/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_mgr.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>

#define LOG_TAG "DroidVoldCmdListener"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <cutils/fs.h>
#include <cutils/log.h>

#include <sysutils/SocketClient.h>
#include <private/android_filesystem_config.h>

#include "CommandListener.h"
#include "VolumeManager.h"
#include "VolumeBase.h"
#include "ResponseCode.h"
#include "Process.h"

#define DUMP_ARGS 0

CommandListener::CommandListener() :
                 FrameworkListener("droidvold", true) {
    registerCmd(new VolumeCmd());
#ifdef HAS_VIRTUAL_CDROM
    registerCmd(new LoopCmd());
#endif
}

#if DUMP_ARGS
void CommandListener::dumpArgs(int argc, char **argv, int argObscure) {
    char buffer[4096];
    char *p = buffer;

    memset(buffer, 0, sizeof(buffer));
    int i;
    for (i = 0; i < argc; i++) {
        unsigned int len = strlen(argv[i]) + 1; // Account for space
        if (i == argObscure) {
            len += 2; // Account for {}
        }
        if (((p - buffer) + len) < (long)(sizeof(buffer)-1)) {
            if (i == argObscure) {
                *p++ = '{';
                *p++ = '}';
                *p++ = ' ';
                continue;
            }
            strcpy(p, argv[i]);
            p+= strlen(argv[i]);
            if (i != (argc -1)) {
                *p++ = ' ';
            }
        }
    }
    SLOGD("%s", buffer);
}
#else
void CommandListener::dumpArgs(int /*argc*/, char ** /*argv*/, int /*argObscure*/) { }
#endif

int CommandListener::sendGenericOkFail(SocketClient *cli, int cond) {
    if (!cond) {
        return cli->sendMsg(ResponseCode::CommandOkay, "Command succeeded", false);
    } else {
        return cli->sendMsg(ResponseCode::OperationFailed, "Command failed", false);
    }
}

CommandListener::VolumeCmd::VolumeCmd() :
                 VoldCommand("volume") {
}

int CommandListener::VolumeCmd::runCommand(SocketClient *cli,
                                           int argc, char **argv) {
    dumpArgs(argc, argv, -1);

    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing Argument", false);
        return 0;
    }

    VolumeManager *vm = VolumeManager::Instance();
    std::lock_guard<std::mutex> lock(vm->getLock());

    // TODO: tease out methods not directly related to volumes

    std::string cmd(argv[1]);
    if (cmd == "reset") {
        return sendGenericOkFail(cli, vm->reset());

    } else if (cmd == "shutdown") {
        return sendGenericOkFail(cli, vm->shutdown());

    } else if (cmd == "debug") {
        return sendGenericOkFail(cli, vm->setDebug(true));

    } else if (cmd == "mkdirs" && argc > 2) {
        // mkdirs [path]
        return sendGenericOkFail(cli, vm->mkdirs(argv[2]));

    } else if (cmd == "mount" && argc > 2) {
        // mount [volId] [flags] [user]
        std::string id(argv[2]);
        auto vol = vm->findVolume(id);
        if (vol == nullptr) {
            return cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown volume", false);
        }

        int mountFlags = (argc > 3) ? atoi(argv[3]) : 0;
        userid_t mountUserId = (argc > 4) ? atoi(argv[4]) : -1;

        vol->setMountFlags(mountFlags);
        vol->setMountUserId(mountUserId);

        int res = vol->mount();

        return sendGenericOkFail(cli, res);

    } else if (cmd == "unmount" && argc > 2) {
        // unmount [volId]
        std::string id(argv[2]);
        auto vol = vm->findVolume(id);
        if (vol == nullptr) {
            return cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown volume", false);
        }

        return sendGenericOkFail(cli, vol->unmount());

    } else if (cmd == "format" && argc > 3) {
        // format [volId] [fsType|auto]
        std::string id(argv[2]);
        std::string fsType(argv[3]);
        auto vol = vm->findVolume(id);
        if (vol == nullptr) {
            return cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown volume", false);
        }

        return sendGenericOkFail(cli, vol->format(fsType));

    }

    return cli->sendMsg(ResponseCode::CommandSyntaxError, nullptr, false);
}

#ifdef HAS_VIRTUAL_CDROM
CommandListener::LoopCmd::LoopCmd() :
                 VoldCommand("loop") {
}

int CommandListener::LoopCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    dumpArgs(argc, argv, -1);
    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing Argument", false);
        return 0;
    }

    VolumeManager *vm = VolumeManager::Instance();
    int rc = 0;
    if (!strcmp(argv[1], "mount")) {
        if (argc != 3) {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Usage: loop mount <path>", false);
            return 0;
        }
        rc = vm->mountloop(argv[2]);
    } else if (!strcmp(argv[1], "unmount")) {
        if (argc < 2 || argc > 3) {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Usage: loop unmount [force]", false);
            return 0;
        }

        bool force = false;
        if (argc == 3 && !strcmp(argv[2], "force")) {
            force = true;
        }
        rc = vm->unmountloop(force);
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown loop cmd", false);
        return 0;
    }

    if (!rc) {
        cli->sendMsg(ResponseCode::CommandOkay, "loop operation succeeded", false);
    } else {
        int erno = errno;
        rc = ResponseCode::convertFromErrno();
        cli->sendMsg(rc, "loop operation failed", true);
    }

    return 0;
}
#endif
