.. zephyr:code-sample:: mcumgr-isotp-client
   :name: MCUmgr ISO-TP (CAN) SMP client
   :relevant-api: mcumgr_smp_client mcumgr_transport_smp can_isotp

   Send MCUmgr OS management requests over the ISO-TP (CAN) transport.

Overview
********

This sample is the SMP **client** counterpart to
:zephyr:code-sample:`mcumgr-isotp-server`. It uses the MCUmgr SMP client and the
OS management client to periodically send an ``echo`` request over the ISO-TP
(ISO 15765-2) transport on a CAN bus, and logs whether the round trip succeeded.

The ISO-TP transport is started automatically during MCUmgr handler
initialisation. Because :kconfig:option:`CONFIG_SMP_CLIENT` is enabled, it
registers itself as a client transport of type ``SMP_ISOTP_TRANSPORT``, which
``main()`` looks up with :c:func:`smp_client_object_init`.

CAN identifiers
===============

The client uses the **mirror image** of the server's identifiers, so the IDs are
swapped relative to the transport defaults:

=================== =================================== =========
Direction           Kconfig                             Value
=================== =================================== =========
Client TX           ``MCUMGR_TRANSPORT_ISOTP_TX_ID``    ``0x80``
Client RX           ``MCUMGR_TRANSPORT_ISOTP_RX_ID``    ``0x180``
=================== =================================== =========

Data and flow control share these two ids (ISO 15765-2 normal addressing).

Requirements
************

* A CAN controller. On ``native_sim`` the sample uses the
  :dtcompatible:`zephyr,native-linux-can` driver, which connects to a host
  SocketCAN interface (e.g. ``vcan0``), so the client and server processes share
  the same bus.
* A running :zephyr:code-sample:`mcumgr-isotp-server` on the same CAN interface.

Building and Running
********************

See the top-level ``README.rst`` for the full host setup. Build and run the
client (after the server is already running):

.. zephyr-app-commands::
   :zephyr-app: mcumgr-isotp/client
   :board: native_sim
   :goals: build run

Sample output
=============

.. code-block:: console

   *** Booting Zephyr OS ***
   [00:00:00.000,000] <inf> main: MCUmgr ISO-TP (CAN) SMP client started
   [00:00:00.000,000] <inf> main: Sending to server RX CAN id 0x80, listening on 0x180
   [00:00:00.000,000] <inf> main: [0] Sending echo: "Hello ISO-TP"
   [00:00:00.010,000] <inf> main: [0] Echo round-trip OK
   [00:00:02.010,000] <inf> main: [1] Sending echo: "Hello ISO-TP"
   [00:00:02.020,000] <inf> main: [1] Echo round-trip OK

If the server is not running you will see ``Echo failed`` warnings instead.
