# BLE test clients for the UNA Watch

Clients that exercise the watch's Bluetooth from a Linux host's BlueZ, the
way a phone would, with the watch's console logged alongside
([the Bluetooth section](../../../docs/boards/unawatch/index.md) of the board's page). They
were used to bring up the ACI host (decision 261128-081601-02).

They need Python 3 with `bleak`, `pexpect` (pairing) and `bless` (forward
PPoGATT), in a virtual environment rather than installed on the host:

```shell
python3 -m venv ~/.venvs/una-ble && ~/.venvs/una-ble/bin/pip install bleak pexpect bless
```

The watch runs a slot build (`CONFIG_BOARD_UNAWATCH_TETHER_SLOT=y`) and is
reached through `bridge.py`, its tether protocol on port 4500 and its console
on 4501 ([tether.md](../../../docs/boards/unawatch/tether.md)). Everything that touches
the watch goes through `watch.sh`, which takes the tether lock that the
others sharing the watch take too. `$UNA_BLE_ADDR` overrides the watch's
address, `CD:CE:E6:EC:45:CE`.

| Script | |
| --- | --- |
| `watch.sh flash IMAGE [SECONDS]` | flash a slot image, boot it, log the console |
| `watch.sh measure SECONDS LABEL` | `power measure`, then its result (decision 261127-150636-01) |
| `watch.sh run CMD...` | any of the clients below, under the lock |
| `gatt_dump.py [--trigger HEX]` | the services, every readable value, a write of the pairing trigger |
| `pair.py LOG` | pairing by numeric comparison: bluetoothctl as a DisplayYesNo agent, the watch confirmed with Up through its console; leaves the controller not pairable |
| `reconnect.py LOG` | the bonded central again: the link encrypted, the PPS status, an encrypted write |
| `ppog_reversed.py LOG [--count N]` | the Pebble protocol over the watch's PPoGATT service: handshake, version, ping, round trip and rate |
| `ppog_forward.py LOG [--count N]` | the same with BlueZ hosting the phone's PPoGATT service (no advertising), the watch its client |
| `hold.py SECONDS` | a connection held, for measuring connected idle |

For example:

```shell
py=~/.venvs/una-ble/bin/python
tools/una/ble/watch.sh flash build-slot/pebbleos.bin
tools/una/ble/watch.sh run $py tools/una/ble/pair.py /tmp/pair.log
tools/una/ble/watch.sh run $py tools/una/ble/ppog_reversed.py /tmp/ppog.log --count 20
$py tools/una/ble/hold.py 450 & sleep 30; tools/una/ble/watch.sh measure 300 connected
```

Pairing leaves a bond in BlueZ: `bluetoothctl remove CD:CE:E6:EC:45:CE` takes
it away.
