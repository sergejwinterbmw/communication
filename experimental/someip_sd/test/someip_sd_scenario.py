# *******************************************************************************
# Copyright (c) 2026 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************
"""SOME/IP-SD prototype scenario shared by the QNX (dual QEMU) and Linux (Docker) integration tests.

The SD side (10.0.3.1) runs someip_event_module, someip_sd_daemon and the test client, the peer side (10.0.3.2) runs the
upstream lg-someip someipd with a provider offering 0x1234.0x5678 and notifying event 0x8778 every second. The test
client receives the notifications as LoLa events from the event module.
"""

import logging

logger = logging.getLogger(__name__)

SD_DIR = "/opt/someip_sd"
PEER_DIR = "/opt/someip_sd_peer"
CLIENT_RUNTIME_SECONDS = 15
# The client unsubscribes after its runtime and checks for late events (grace 0.5 s + check 3 s).
CLIENT_TIMEOUT_SECONDS = CLIENT_RUNTIME_SECONDS + 15


class ConsoleSide:
    """QNX VM driven through its ITF serial console."""

    def __init__(self, console):
        self._console = console

    def sh(self, command, timeout=30):
        return self._console.run_sh_cmd_output(command, timeout=timeout)

    def start(self, command):
        # Subshell, since the console wrapper appends further commands and a trailing '&;' is a syntax error.
        self.sh(f"( {command} & )")


class ContainerSide:
    """Docker container driven through docker exec."""

    def __init__(self, container):
        self._container = container

    def sh(self, command, timeout=30):
        del timeout  # docker exec has no timeout; all commands of the scenario bound themselves.
        rc, out = self._container.exec_run(["/bin/sh", "-c", command])
        return rc, out.decode(errors="replace")

    def start(self, command):
        self._container.exec_run(["/bin/sh", "-c", f"{command}"], detach=True)


def _sh(side, command, timeout=30):
    rc, out = side.sh(command, timeout=timeout)
    if (rc != 0):
        raise RuntimeError(f"Command '{command}' failed with rc={rc}: {out}")
    return out


def run(sd_side, peer_side):
    peer_side.start(f"{PEER_DIR}/bin/someipd {PEER_DIR}/etc/lg_peer.json")
    _sh(peer_side, "sleep 1")
    peer_side.start(f"{PEER_DIR}/bin/someip_sd_peer {PEER_DIR}/etc/lg_peer.json")
    sd_side.start(f"{SD_DIR}/bin/someip_event_module {SD_DIR}/etc/mw_com_config.json")
    _sh(sd_side, "sleep 1")
    sd_side.start(f"{SD_DIR}/bin/someip_sd_daemon {SD_DIR}/etc/sd_daemon.json")
    _sh(sd_side, "sleep 1")

    out = _sh(
        sd_side,
        f"timeout -s TERM {CLIENT_TIMEOUT_SECONDS} {SD_DIR}/bin/someip_sd_test_client"
        " --service 0x1234 --instance 0x5678 --eventgroup 0x4465 --event 0x8778"
        f" --runtime {CLIENT_RUNTIME_SECONDS} --mw-com-config {SD_DIR}/etc/mw_com_config.json",
        timeout=CLIENT_TIMEOUT_SECONDS + 30,
    )

    # expected log outputs from dummy client application
    assert "registered at SD daemon" in out, "test client could not register at the SD daemon"
    assert "0x1234.0x5678 available" in out, "service was not reported available"
    assert "eventgroup 0x4465 acknowledged" in out, "subscription was not acknowledged"
    assert "LoLa: found someip/1234/5678" in out, "event module did not offer the LoLa service"
    assert "event: service 0x1234 event 0x8778" in out, "no event received from the peer via LoLa"
    assert "unsubscribed eventgroup 0x4465" in out, "test client did not unsubscribe"
    assert "result: OK, no events after unsubscribe" in out, "events still received after unsubscribe"
