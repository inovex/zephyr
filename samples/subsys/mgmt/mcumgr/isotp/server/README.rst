.. zephyr:code-sample:: mcumgr-isotp-server
   :name: MCUmgr ISO-TP (CAN) SMP server
   :relevant-api: mcumgr_transport_smp can_isotp

   Expose an MCUmgr SMP server over the ISO-TP (CAN) transport.

Overview
********

This sample brings up an MCUmgr **SMP server** that communicates over the
ISO-TP (ISO 15765-2) transport on a CAN bus. ISO-TP performs its own
segmentation, reassembly and flow control, so each ISO-TP message carries one
complete SMP packet.

The server registers the OS management group, so a peer can issue commands such
as ``echo`` and ``info``. The transport is started automatically during MCUmgr
handler initialisation — ``main()`` only prints a banner.

The companion :zephyr:code-sample:`mcumgr-isotp-client` sample (a second
``native_sim`` process) acts as the SMP client. Any host MCUmgr tooling that can
speak ISO-TP over SocketCAN works as well.

CAN identifiers
===============

=================== =================================== =========
Direction           Kconfig                             Default
=================== =================================== =========
Server RX           ``MCUMGR_TRANSPORT_ISOTP_RX_ID``    ``0x80``
Server TX           ``MCUMGR_TRANSPORT_ISOTP_TX_ID``    ``0x180``
=================== =================================== =========

The server receives data and sends flow control on ``0x80``. It sends data and
receives flow control on ``0x180`` (ISO 15765-2 normal addressing). The client
mirrors the two ids: it transmits on ``0x80`` and receives on ``0x180``.

Requirements
************

* A CAN controller. On ``native_sim`` the sample uses the
  :dtcompatible:`zephyr,native-linux-can` driver, which connects to a host
  SocketCAN interface, so two ``native_sim`` processes can talk to each other.

Building and Running
********************

See the top-level ``README.rst`` of this directory for the full host setup
(creating a virtual CAN interface and starting both samples). In short:

.. code-block:: console

   # Host: create a virtual CAN interface once
   sudo modprobe vcan
   sudo ip link add dev vcan0 type vcan
   sudo ip link set up vcan0

Build and run the server:

.. zephyr-app-commands::
   :zephyr-app: mcumgr-isotp/server
   :board: native_sim
   :goals: build run

The ``native_sim.overlay`` in ``boards/`` routes ``zephyr,canbus`` to the
native Linux CAN driver and binds it to ``vcan0``.

Sample output
=============

.. code-block:: console

   *** Booting Zephyr OS ***
   [00:00:00.000,000] <inf> main: MCUmgr ISO-TP (CAN) SMP server started
   [00:00:00.000,000] <inf> main: Listening for SMP requests on RX CAN id 0x80, replying on 0x180

When the client sends an ``echo`` request you will see the ISO-TP transport
debug logs for the received and transmitted frames.
