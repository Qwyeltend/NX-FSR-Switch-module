#pragma once
// Minimal client for SaltyNX's "SaltySD" service: only what's needed to map SaltyNX's shared memory.
// Based on FPSLocker's include/SaltyNX.h by masagrator (GPLv2). Uses the old libnx IPC helpers (ipc.h)
// from the same project.
#include <switch.h>
#include "ipc.h"

namespace saltynx {
    inline Handle port = 0;

    inline Result Connect() {
        for (int i = 0; i < 200; i++) {
            if (!svcConnectToNamedPort(&port, "SaltySD"))
                return 0;
            svcSleepThread(1000 * 1000);
        }
        return 1;
    }

    inline Result Term() {
        IpcCommand c;
        ipcInitialize(&c);
        ipcSendPid(&c);
        struct input {
            u64 magic;
            u64 cmd_id;
            u64 zero;
            u64 reserved[2];
        } *raw = (input*)ipcPrepareHeader(&c, sizeof(input));
        raw->magic = SFCI_MAGIC;
        raw->cmd_id = 0;
        raw->zero = 0;
        Result ret = ipcDispatch(port);
        if (R_SUCCEEDED(ret)) {
            IpcParsedCommand r;
            ipcParse(&r);
            struct output {
                u64 magic;
                u64 result;
            } *resp = (output*)r.Raw;
            ret = resp->result;
        }
        svcCloseHandle(port);
        if (ret == 0xf601) return 0; // session terminated works too
        return ret;
    }

    inline Result GetSharedMemoryHandle(Handle* retrieve) {
        IpcCommand c;
        ipcInitialize(&c);
        ipcSendPid(&c);
        struct input {
            u64 magic;
            u64 cmd_id;
            u32 reserved[4];
        } *raw = (input*)ipcPrepareHeader(&c, sizeof(input));
        raw->magic = SFCI_MAGIC;
        raw->cmd_id = 7;
        Result ret = ipcDispatch(port);
        if (R_SUCCEEDED(ret)) {
            IpcParsedCommand r;
            ipcParse(&r);
            struct output {
                u64 magic;
                u64 result;
                u64 reserved[2];
            } *resp = (output*)r.Raw;
            ret = resp->result;
            if (!ret) *retrieve = r.Handles[0];
        }
        return ret;
    }
}
