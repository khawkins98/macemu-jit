#!/usr/bin/env python3
"""Minimal gdb-remote (RSP) client for QEMU's gdbstub — PPC32 BE.
Just enough to: connect, read/write regs, set sw/hw breakpoints + watchpoints,
continue/step, read memory. No PPC gdb exists on this host (R1)."""
import socket, sys, time

class GDB:
    def __init__(self, host='127.0.0.1', port=1234):
        self.s = socket.create_connection((host, port), timeout=30)
        self.s.settimeout(30)
        self.buf = b''
        self._send_ack_off()
    def _raw_send(self, payload: bytes):
        csum = sum(payload) & 0xff
        pkt = b'$' + payload + b'#' + b'%02x' % csum
        self.s.sendall(pkt)
    def _read_pkt(self):
        # read until we get a full $...#xx, handling +/-
        while True:
            while b'$' not in self.buf or b'#' not in self.buf.split(b'$',1)[1]:
                chunk = self.s.recv(4096)
                if not chunk: raise EOFError("gdbstub closed")
                self.buf += chunk
            pre, rest = self.buf.split(b'$', 1)
            body, after = rest.split(b'#', 1)
            if len(after) < 2:
                continue
            self.buf = after[2:]
            self.s.sendall(b'+')  # ack
            return body
    def _send_ack_off(self):
        # send QStartNoAckMode
        self._raw_send(b'QStartNoAckMode')
        self.s.recv(1)  # the '+'
        r = self._read_pkt()
        self.noack = (r == b'OK')
    def cmd(self, payload: bytes, expect_reply=True):
        if isinstance(payload, str): payload = payload.encode()
        self._raw_send(payload)
        if not self.noack:
            # expect a '+'
            a = self.s.recv(1)
        if expect_reply:
            return self._read_pkt()
        return None
    def read_regs(self):
        r = self.cmd(b'g')
        # PPC32: 32 GPRs (r0-r31) then... layout per QEMU. Each reg 4 bytes BE hex.
        regs = [int(r[i:i+8], 16) for i in range(0, min(len(r), 32*8), 8)]
        return regs  # r0..r31
    def read_reg_raw(self):
        return self.cmd(b'g')
    def read_mem(self, addr, n):
        r = self.cmd(b'm%x,%x' % (addr, n))
        if r.startswith(b'E'): return None
        return bytes.fromhex(r.decode())
    def set_bp(self, addr, kind=0):   # Z0 sw, Z1 hw
        return self.cmd(b'Z%d,%x,4' % (kind, addr))
    def del_bp(self, addr, kind=0):
        return self.cmd(b'z%d,%x,4' % (kind, addr))
    def set_watch(self, addr, length, typ=2):  # Z2 write watch
        return self.cmd(b'Z%d,%x,%x' % (typ, addr, length))
    def cont(self):
        return self.cmd(b'c')   # returns stop reply (T05...)
    def step(self):
        return self.cmd(b's')
    def cont_until(self, timeout=60):
        self.s.settimeout(timeout)
        return self._cont_wait()
    def _cont_wait(self):
        self._raw_send(b'c')
        if not self.noack: self.s.recv(1)
        return self._read_pkt()

def read_pc(g):
    # QEMU ppc32 'g': r0..r31 (32*8 hex), then pc, msr, cr, lr, ctr, xer...
    r = g.cmd(b'g')
    try:
        pc = int(r[32*8:32*8+8], 16); msr = int(r[33*8:33*8+8], 16)
        lr = int(r[35*8:35*8+8], 16)
        return pc, msr, lr
    except Exception:
        return None, None, None

def catch_writes(watch_addrs, timeout=80, length=4):
    import time
    g = GDB()
    print("connected noack=%s stop=%s" % (g.noack, g.cmd(b'?')))
    for a in watch_addrs:
        print("Z2 @ %#x -> %s" % (a, g.set_watch(a, length, typ=2)))
    t0 = time.time(); hits = 0
    while time.time() - t0 < timeout:
        try:
            stop = g.cont_until(timeout=timeout)
        except Exception as e:
            print("cont ended:", e); break
        if not stop or stop.startswith(b'W') or stop.startswith(b'X'):
            print("target exited:", stop); break
        regs = g.read_regs(); pc, msr, lr = read_pc(g)
        print("STOP#%d reply=%s" % (hits, stop.decode(errors='replace')))
        print("  PC=%s MSR=%s LR=%s" % (hex(pc) if pc else '?', hex(msr) if msr else '?', hex(lr) if lr else '?'))
        print("  r0..r12:", [hex(x) for x in regs[:13]])
        hits += 1
        if hits >= 12:
            print("  (12 hits cap)"); break
    print("done hits=%d" % hits)

if __name__ == '__main__':
    import sys
    if len(sys.argv) > 1 and sys.argv[1] == 'catch':
        addrs = [int(x, 16) for x in sys.argv[2].split(',')]
        to = int(sys.argv[3]) if len(sys.argv) > 3 else 80
        catch_writes(addrs, timeout=to)
    else:
        g = GDB()
        print("connected, noack=", g.noack)
        print("stop:", g.cmd(b'?'))
        regs = g.read_regs(); pc, msr, lr = read_pc(g)
        print("PC=%s MSR=%s" % (hex(pc) if pc else '?', hex(msr) if msr else '?'))
        print("r0..r5:", [hex(x) for x in regs[:6]])
