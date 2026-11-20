#include "palm.h"

enum { Resolve = 1, Connecting, Connected };

static Boolean temporary(Err error)
{
    return error == netErrWouldBlock || error == netErrTimeout ||
           error == netErrAlreadyInProgress || error == netErrSocketBusy;
}

Boolean pt_net_start(PtNet *net)
{
    net->socket = -1;
    net->error = SysLibFind("Net.lib", &net->ref);
    if (net->error) return false;
    UInt16 interface_errors = 0;
    net->error = NetLibOpen(net->ref, &interface_errors);
    if (net->error && net->error != netErrAlreadyOpen) return false;
    net->owned = true;
    if (interface_errors) {
        net->error = interface_errors;
        return false;
    }
    net->state = Resolve;
    net->deadline = TimGetTicks() + SysTicksPerSecond() * 30UL;
    return true;
}

UInt16 pt_net_poll(PtNet *net, const Char *host, UInt16 port)
{
    if ((Int32)(TimGetTicks() - net->deadline) >= 0 && net->state != Connected) {
        net->error = netErrTimeout;
        return PT_NET_FAIL;
    }
    Err err = errNone;
    if (net->state == Resolve) {
        NetSocketAddrINType address;
        MemSet(&address, sizeof(address), 0);
        address.family = netSocketAddrINET;
        address.port = NetHToNS(port);
        address.addr = NetLibAddrAToIN(net->ref, host);
        if (address.addr == (NetIPAddr)0xffffffffUL) {
            NetHostInfoBufType dns;
            NetHostInfoPtr result = NetLibGetHostByName(net->ref, host, &dns,
                                     SysTicksPerSecond() / 4, &err);
            if (!result || !result->addrListP || !result->addrListP[0]) {
                net->error = err;
                return temporary(err) ? PT_NET_WORK : PT_NET_FAIL;
            }
            MemMove(&address.addr, result->addrListP[0], sizeof(address.addr));
        }
        net->socket = NetLibSocketOpen(net->ref, netSocketAddrINET,
                                       netSocketTypeStream, 0, 0, &err);
        Int16 enabled = 1;
        if (net->socket < 0 || NetLibSocketOptionSet(net->ref, net->socket,
            netSocketOptLevelSocket, netSocketOptSockNonBlocking,
            &enabled, sizeof(enabled), 0, &err) < 0) {
            net->error = err;
            return PT_NET_FAIL;
        }
        if (NetLibSocketConnect(net->ref, net->socket, (NetSocketAddrType *)&address,
                                sizeof(address), 0, &err) == 0)
            net->state = Connected;
        else if (temporary(err))
            net->state = Connecting;
        else {
            net->error = err;
            return PT_NET_FAIL;
        }
    }
    if (net->state == Connecting) {
        NetFDSetType write_set, error_set;
        netFDZero(&write_set);
        netFDZero(&error_set);
        netFDSet(net->socket, &write_set);
        netFDSet(net->socket, &error_set);
        Int16 selected = NetLibSelect(net->ref, net->socket + 1, NULL,
                                      &write_set, &error_set, 0, &err);
        if (selected <= 0) {
            net->error = err;
            return !selected || temporary(err) ? PT_NET_WORK : PT_NET_FAIL;
        }
        Err socket_error = errNone;
        UInt16 length = sizeof(socket_error);
        if (NetLibSocketOptionGet(net->ref, net->socket, netSocketOptLevelSocket,
            netSocketOptSockErrorStatus, &socket_error, &length, 0, &err) < 0 ||
            socket_error) {
            net->error = err ? err : socket_error;
            return PT_NET_FAIL;
        }
        net->state = Connected;
    }
    return PT_NET_READY;
}

UInt16 pt_net_io(PtNet *net, PtArm *arm)
{
    Err error = errNone;
    if (net->sent == net->pending) {
        Int32 count = pt_call(arm, PT_DRAIN, net->tx, sizeof(net->tx), 0);
        if (count < 0) return PT_NET_FAIL;
        net->pending = (UInt16)count;
        net->sent = 0;
    }
    if (net->sent < net->pending) {
        Int16 count = NetLibSend(net->ref, net->socket, net->tx + net->sent,
                         net->pending - net->sent, 0, NULL, 0, 0, &error);
        if (count > 0) net->sent += count;
        else if (count < 0 && !temporary(error)) {
            net->error = error;
            return PT_NET_FAIL;
        }
    }
    if (net->fed == net->received && !net->eof) {
        Int16 count = NetLibReceive(net->ref, net->socket, net->rx, sizeof(net->rx),
                                    0, NULL, NULL, 0, &error);
        if (count > 0) {
            net->received = count;
            net->fed = 0;
        } else if (!count || error == netErrSocketClosedByRemote ||
                   error == netErrSocketInputShutdown) {
            net->eof = true;
        } else if (!temporary(error)) {
            net->error = error;
            return PT_NET_FAIL;
        }
    }
    if (net->fed < net->received) {
        Int32 fed = pt_call(arm, PT_FEED, net->rx + net->fed, net->received - net->fed, 0);
        if (fed < 0) return PT_NET_FAIL;
        net->fed += (UInt16)fed;
    }
    return net->eof && net->fed == net->received ? PT_NET_END : PT_NET_READY;
}

void pt_net_close(PtNet *net)
{
    if (net->owned) {
        Err error = errNone;
        if (net->socket >= 0) NetLibSocketClose(net->ref, net->socket, 0, &error);
        NetLibClose(net->ref, false);
    }
    MemSet(net, sizeof(*net), 0);
    net->socket = -1;
}
