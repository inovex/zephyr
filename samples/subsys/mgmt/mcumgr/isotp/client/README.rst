.. zephyr:code-sample:: mcumgr-isotp-client
   :name: MCUmgr ISO-TP (CAN) SMP client shell
   :relevant-api: mcumgr_smp_client mcumgr_img_mgmt_client mcumgr_os_mgmt_client mcumgr_transport_smp can_isotp

   Drive MCUmgr firmware updates on remote nodes over ISO-TP (CAN) from an
   interactive shell.

Overview
********

This sample is a "main controller" that performs firmware updates and file
transfers on other nodes over the ISO-TP (ISO 15765-2) transport on a CAN bus,
driven interactively from the console with the ``smpc`` shell commands. It is
the SMP **client** counterpart to :zephyr:code-sample:`mcumgr-isotp-server`.

It uses the in-tree **SMP client shell** subsystem
(:kconfig:option:`CONFIG_MCUMGR_SMP_CLIENT_SHELL`) plus its ISO-TP backend
(:kconfig:option:`CONFIG_MCUMGR_SMP_CLIENT_SHELL_ISOTP`), which provide:

* transport-agnostic ``smpc`` commands built on the **image** and **OS**
  management clients (:c:func:`img_mgmt_client_upload`,
  :c:func:`img_mgmt_client_state_read`, :c:func:`os_mgmt_client_reset`, ...) and
  a small built-in **file management (fs_mgmt) client** (Zephyr ships no fs_mgmt
  client library);
* an ISO-TP-specific ``smpc isotp`` subcommand group that retargets the peer at
  runtime via :c:func:`smp_isotp_set_peer`, so one controller can address many
  nodes in turn over one bus.

The command groups are individually selectable: the base shell provides only
``echo``/``reset``/``transport`` (so a bare connectivity test needs neither a
file system nor the image stack), while
:kconfig:option:`CONFIG_MCUMGR_SMP_CLIENT_SHELL_IMG`,
:kconfig:option:`CONFIG_MCUMGR_SMP_CLIENT_SHELL_IMG_UPLOAD` and
:kconfig:option:`CONFIG_MCUMGR_SMP_CLIENT_SHELL_FILE` (all default ``y``, the
latter two gated on :kconfig:option:`CONFIG_FILE_SYSTEM`) add the ``image``,
``image upload``/``upgrade`` and ``file`` commands.

The shell binds to a transport from the SMP client transport registry. With a
single transport enabled (as in this sample) it is used automatically; with
several (e.g. ISO-TP plus UDP via
:kconfig:option:`CONFIG_MCUMGR_SMP_CLIENT_SHELL_UDP`), pick one at runtime with
``smpc transport <name>`` — or simply use a backend's ``target`` command
(``smpc isotp target ...``, ``smpc udp target <ip> [port]``), which selects its
transport as a side effect.

The sample itself only mounts a **littlefs** at ``/lfs`` (the firmware/file
source, manageable with the built-in ``fs`` shell) and prints startup status.

Shell commands
==============

.. code-block:: console

   smpc echo <text>                     # OS echo round-trip (connectivity check)
   smpc image list                      # read the target's image state
   smpc image upload <file> [slot]      # push /lfs <file> to the node ([slot] = image number, default 0)
   smpc image test <hash>               # mark an image for test (revert on reboot)
   smpc image confirm [hash]            # confirm an image (or the running one)
   smpc image erase <slot>              # erase an image slot
   smpc file upload <local> <remote>    # copy a local file to the target
   smpc file download <remote> <local>  # copy a file from the target
   smpc upgrade <file> [test|confirm]   # upload -> mark -> reset, in one step
   smpc reset                           # reboot the target
   smpc transport [name]                # list registered SMP transports / select one
   smpc isotp target <rx_id> <tx_id>    # (ISO-TP) select the peer node's CAN ids

Addressing several nodes
========================

Each node has its own **data** CAN identifiers, while the **flow-control**
identifiers are shared across all nodes (only one node is addressed at a time).
``smpc isotp target`` swaps the data identifiers at runtime via
:c:func:`smp_isotp_set_peer`; the flow-control identifiers stay fixed. The data
ids default to the mirror image of the server sample:

=================== =================================== =========
Direction           Kconfig                             Value
=================== =================================== =========
Client TX data      ``MCUMGR_TRANSPORT_ISOTP_TX_ID``    ``0x80``
Client RX data      ``MCUMGR_TRANSPORT_ISOTP_RX_ID``    ``0x180``
Client TX flow-ctrl ``MCUMGR_TRANSPORT_ISOTP_TX_FC_ID`` ``0x580``
Client RX flow-ctrl ``MCUMGR_TRANSPORT_ISOTP_RX_FC_ID`` ``0x480``
=================== =================================== =========

For ``smpc isotp target <rx_id> <tx_id>``, ``rx_id`` is the id the controller receives
on (the node's TX data id) and ``tx_id`` the id it transmits on (the node's RX
data id). Switch peers only between complete request/response exchanges, not
mid-transfer.

.. warning::

   **All four identifiers must be mirrored against the node, including the
   flow-control ids** — not just the data ids. The client's ``RX_FC_ID`` must
   equal the node's ``TX_FC_ID`` and the client's ``TX_FC_ID`` the node's
   ``RX_FC_ID``. If you derive a controller from the *server* sample's config,
   the flow-control ids are in the server orientation and **multi-frame uploads
   silently fail**: the First Frame and the node's Clear-To-Send appear on the
   bus, but the controller, listening for flow control on the wrong id, never
   sees the CTS, sends no consecutive frames, and times out. Single-frame
   commands (``echo``) keep working because they use no flow control.

Receiving updates on the controller itself (second channel)
===========================================================

A controller that pushes updates to nodes is an SMP **client** whose identifiers
*move* per node (``smpc isotp target``). A single ISO-TP channel is one
addressable endpoint, so it cannot simultaneously be a **fixed** SMP **server**
that an upstream host dials to update the controller. To do both, enable a
second, fixed, server-only channel:

.. code-block:: ini

   CONFIG_MCUMGR_TRANSPORT_ISOTP_SECOND_CHANNEL=y
   CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID2=0x90      # the controller's own stable address
   CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID2=0x190
   CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_FC_ID2=0x490
   CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_FC_ID2=0x590
   # plus the server-side image management to apply updates to itself:
   CONFIG_MCUMGR_GRP_IMG=y
   CONFIG_IMG_MANAGER=y

Channel 0 stays the retargetable client (pushes to nodes); channel 1 is a fixed
SMP server, so the controller is reachable at ``RX_ID2`` for its own firmware
updates (applied via MCUboot, exactly like a node) while it updates the nodes.
All eight identifiers across the two channels — and every node's ids — must be
mutually distinct on the bus.

Requirements
************

* A CAN controller. On ``native_sim`` the sample uses the
  :dtcompatible:`zephyr,native-linux-can` driver connected to a host SocketCAN
  interface (e.g. ``vcan0``), shared with the server process.
* A running :zephyr:code-sample:`mcumgr-isotp-server` (or a fleet of nodes) on
  the same CAN bus.
* Storage for ``/lfs``. ``native_sim`` mounts littlefs on the
  ``storage_partition`` of the flash simulator automatically (see
  ``boards/native_sim.overlay``). **On a real board you must provide a
  storage_partition + littlefs fstab overlay** pointing at your storage medium
  (internal flash region, external QSPI, SD, ...); the build succeeds without it
  but ``main()`` then logs that ``/lfs`` is not mounted and uploads have no
  source.

Building and Running
********************

native_sim (against the server sample over ``vcan0``):

.. code-block:: console

   # host: create the virtual CAN bus
   sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0

   west build -b native_sim zephyr/samples/subsys/mgmt/mcumgr/isotp/server -d build/srv
   west build -b native_sim zephyr/samples/subsys/mgmt/mcumgr/isotp/client -d build/cli
   ./build/srv/zephyr/zephyr.exe &     # start the node
   ./build/cli/zephyr/zephyr.exe       # the controller (shell on the console)

frdm_mcxn947 (CAN FD, extended ids, with MCUboot, mirroring the server build):

.. code-block:: console

   west build -p auto --sysbuild -b frdm_mcxn947/mcxn947/cpu0 \
     zephyr/samples/subsys/mgmt/mcumgr/isotp/client -- \
     -DSB_CONFIG_BOOTLOADER_MCUBOOT=y \
     -Dapp_CONFIG_CAN_FD_MODE=y -Dapp_CONFIG_MCUMGR_TRANSPORT_ISOTP_CAN_FD=y \
     -Dapp_CONFIG_MCUMGR_TRANSPORT_ISOTP_EXTENDED_ID=y
   # plus a storage_partition + littlefs fstab overlay for /lfs

Example upgrade workflow
========================

This walks through updating one node end-to-end, from getting the firmware onto
the controller to the ISO-TP commands, using the **default CAN identifiers**.

The transport binds to the default CAN identifiers **at boot**: the controller
transmits SMP data on ``0x80`` and receives on ``0x180`` (the mirror of the
node's ``RX 0x80`` / ``TX 0x180``). The default node is therefore already
addressable, so for it you can skip straight to the ``image``/``upgrade``
commands — **no ``target`` is required**. You only run
``smpc isotp target <rx_id> <tx_id>`` to point the transport at a *different*
node; for a fleet that derives ids as ``base + node_index``, node *n* is
``smpc isotp target 0x18n 0x8n``.

``smpc image upload`` / ``smpc upgrade`` take a path to a file **on the
controller's** ``/lfs`` as the source. They do not address a location on the
node: the node's image-management handler writes the bytes into the *secondary
(upload) slot* of the selected image, and MCUboot owns the physical address. The
optional ``[slot]`` argument is the MCUmgr **image number** (default ``0``, the
only image on a single-image node), not a node-side path.

1. Build and sign the node's firmware
-------------------------------------

The image pushed to a node is the node application's **signed** binary (the
:zephyr:code-sample:`mcumgr-isotp-server` built with MCUboot), e.g.
``build/server/zephyr/zephyr.signed.bin``.

2. Stage the image on the controller's ``/lfs``
-----------------------------------------------

``smpc image upload`` reads the image from the controller's local file system,
so the signed binary must first be placed on ``/lfs``. Pick whichever fits your
setup:

* **Removable / pre-loaded storage (production):** copy ``zephyr.signed.bin`` to
  the littlefs medium (SD card, external QSPI) offline, then mount it at ``/lfs``.
* **native_sim:** the littlefs lives in the flash-simulator image; populate it on
  the host, or for a quick test write a small payload from the console with the
  built-in ``fs`` shell (``fs write`` is limited by ``CONFIG_SHELL_ARGC_MAX`` /
  ``CONFIG_SHELL_CMD_BUFF_SIZE``, so it is only practical for tiny files).
* **Real board, no removable storage:** stream the image into ``/lfs`` over your
  own channel (UART/USB/extra transport) before invoking the shell.

Confirm it is present:

.. code-block:: console

   uart:~$ fs ls /lfs
        86016 zephyr.signed.bin

3. Check connectivity (target only to switch nodes)
---------------------------------------------------

The default node is selected at boot, so you can talk to it immediately. Run
``smpc isotp target`` **only** when addressing a non-default node:

.. code-block:: console

   uart:~$ smpc echo ping                  # default node; no target needed
   echo OK
   uart:~$ smpc image list
   images: 1
    [0] image=0 slot=0 version=1.0.0 active confirmed bootable
        hash=9f1c... (running image)

   # To work on a different node instead, point the transport at its ids first:
   uart:~$ smpc isotp target 0x185 0x85    # e.g. node 5
   peer set: rx=0x185 tx=0x85

4a. One-shot upgrade
--------------------

``smpc upgrade`` uploads the image, marks it for test using the node-computed
hash, and reboots the node in one step:

.. code-block:: console

   uart:~$ smpc upgrade /lfs/zephyr.signed.bin test
   uploading /lfs/zephyr.signed.bin (86016 bytes) to slot 0
     8192/86016 (9%)
     ...
     86016/86016 (100%)
   upload complete
   image marked for test; resetting target
   upgrade complete

The node boots the new image in *test* mode and reverts on the next reboot
unless confirmed. After verifying it runs, make it permanent:

.. code-block:: console

   uart:~$ smpc image confirm            # confirm the node's running image
   image confirmed

Use ``smpc upgrade <file> confirm`` instead of ``test`` to confirm immediately
(no rollback safety net).

4b. Manual upgrade (full lifecycle)
-----------------------------------

The same steps explicitly, which is useful for scripting or inspecting each
stage:

.. code-block:: console

   uart:~$ smpc image upload /lfs/zephyr.signed.bin
   uploading /lfs/zephyr.signed.bin (86016 bytes) to slot 0
     ...
   upload complete
   uart:~$ smpc image list                # the uploaded image is now in slot 1
   images: 2
    [0] image=0 slot=0 version=1.0.0 active confirmed bootable
        hash=9f1c...
    [1] image=0 slot=1 version=1.1.0 bootable
        hash=4be2...
   uart:~$ smpc image test 4be2...        # arm the new image's hash for test
   image marked for test
   uart:~$ smpc reset                     # reboot into the test image
   reset request sent
   # ... verify the node came up on 1.1.0 ...
   uart:~$ smpc image confirm 4be2...     # make it permanent
   image confirmed

To roll back instead of confirming, just ``smpc reset`` again before confirming:
MCUboot reverts to the previous image.
