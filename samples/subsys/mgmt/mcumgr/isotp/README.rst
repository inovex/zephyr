.. _mcumgr_isotp_samples:

MCUmgr SMP over ISO-TP (CAN) — server & client samples
######################################################

These two samples demonstrate the MCUmgr **SMP transport over ISO-TP**
(ISO 15765-2) on a CAN bus, using ``CONFIG_MCUMGR_TRANSPORT_ISOTP``:

``server/``
   An MCUmgr SMP **server** that answers OS management commands (``echo``,
   ``info``) received over ISO-TP.

``client/``
   An MCUmgr SMP **client** ("main controller") driven interactively with the
   ``smpc`` shell: echo, image upload/upgrade, file transfer and runtime peer
   retargeting over ISO-TP.

ISO-TP handles segmentation, reassembly and flow control, so a single ISO-TP
message carries one complete SMP packet — the MCUmgr reassembly layer is not
used.

How it fits together
********************

Each side is its own ``native_sim`` process. They communicate over a shared host
CAN interface using the :dtcompatible:`zephyr,native-linux-can` driver. The CAN
identifiers are mirrored so that each side's transmit id is the other side's
receive id:

.. code-block::

   client  --- SMP request  (CAN id 0x80)  -->  server
   client  <-- SMP response (CAN id 0x180) ---  server

The transport uses **four** CAN identifiers: a data id and a flow-control id
per direction. Flow control must use identifiers distinct from the data ids.
Zephyr's ISO-TP API installs a separate CAN RX filter for the receive context
(on the data id) and for the send context (on the id it expects flow control
on); if those shared an id, every frame would be delivered to both state
machines and transfers would abort with ``ISOTP_N_UNEXP_PDU``. The four ids are
mirrored between the two sides:

=============== ========= =========
Identifier      Server    Client
=============== ========= =========
RX data         ``0x80``  ``0x180``
TX data         ``0x180`` ``0x80``
TX flow-control ``0x480`` ``0x580``
RX flow-control ``0x580`` ``0x480``
=============== ========= =========

The swap is configured in the client's ``prj.conf`` via
``CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID`` / ``..._TX_ID`` and
``..._TX_FC_ID`` / ``..._RX_FC_ID``.

Host setup: a virtual CAN interface
***********************************

On ``native_sim`` the samples bind to a host SocketCAN interface named
``vcan0``. Create it once (Linux host):

.. code-block:: console

   sudo modprobe vcan          # skip if vcan is built into the kernel
   sudo ip link add dev vcan0 type vcan
   sudo ip link set up vcan0

You can watch the traffic with ``candump vcan0`` (from ``can-utils``).

.. note::

   The ``boards/native_sim.overlay`` in each sample re-routes
   ``chosen { zephyr,canbus }`` to the native Linux CAN node and sets
   ``host-interface = "vcan0"``. Change that property if you use a different
   interface name.

Building and running
********************

Set up your Zephyr environment, then build each sample into its own directory.
Run the **server first**, then the client.

Terminal 1 — server:

.. code-block:: console

   west build -b native_sim -d build/server mcumgr-isotp/server
   ./build/server/zephyr/zephyr.exe

Terminal 2 — client:

.. code-block:: console

   west build -b native_sim -d build/client mcumgr-isotp/client
   ./build/client/zephyr/zephyr.exe

Expected output
===============

Server:

.. code-block:: console

   [00:00:00.000,000] <inf> main: MCUmgr ISO-TP (CAN) SMP server started
   [00:00:00.000,000] <inf> main: Listening for SMP requests on RX CAN id 0x80, replying on 0x180

Client:

.. code-block:: console

   [00:00:00.000,000] <inf> main: [0] Sending echo: "Hello ISO-TP"
   [00:00:00.010,000] <inf> main: [0] Echo round-trip OK

Using a host MCUmgr tool instead of the client
**********************************************

Because the server speaks standard SMP, you can also drive it from a host tool
that supports ISO-TP over SocketCAN (instead of the ``client`` sample). Point the
tool at ``vcan0`` with TX id ``0x80`` and RX id ``0x180``.

Configuration reference
***********************

Key options (see ``zephyr/subsys/mgmt/mcumgr/transport/Kconfig.isotp``):

==================================================== =====================================
Option                                               Purpose
==================================================== =====================================
``CONFIG_MCUMGR_TRANSPORT_ISOTP``                    Enable the ISO-TP SMP transport
``CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID`` / ``_TX_ID`` Data CAN identifiers (mirror on each side)
``..._TX_FC_ID`` / ``..._RX_FC_ID``                  Flow-control CAN ids (distinct from data ids)
``CONFIG_MCUMGR_TRANSPORT_ISOTP_EXTENDED_ID``        Use 29-bit CAN identifiers
``CONFIG_MCUMGR_TRANSPORT_ISOTP_CAN_FD``             Use CAN FD framing
``CONFIG_MCUMGR_TRANSPORT_ISOTP_FC_BS`` / ``_STMIN`` ISO-TP flow-control tuning
``CONFIG_MCUMGR_TRANSPORT_ISOTP_AUTO_START``         Let the transport start the CAN device
==================================================== =====================================
