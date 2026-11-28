# Connecting the watch to an Android phone with Gadgetbridge

PebbleOS on the UNA Watch talks to a phone over Bluetooth Low Energy with the
Pebble protocol, like any Pebble. [Gadgetbridge](https://gadgetbridge.org), a
free Android app that supports Pebbles, works with it today. This page was
written with Gadgetbridge 2026.11.1 from F-Droid on a Pixel 6a running
Android 17; the screenshots are from that test.

## What works, and what does not yet

| | |
| --- | --- |
| Pairing | Yes: numeric comparison, confirmed on the phone and on the watch; the bond survives restarts of both |
| Reconnecting | Yes: after a restart of the watch, within a few seconds; after the phone's Bluetooth was turned off and on, once Gadgetbridge is opened (see Troubleshooting) |
| Time | Yes: the watch takes the phone's time and time zone when it connects |
| Notifications | Yes: the phone's notifications, as Gadgetbridge forwards them |
| Incoming calls | Yes: the caller is shown, with accept and decline |
| Battery level | Yes: shown in Gadgetbridge |
| Screenshots of the watch | Yes: Gadgetbridge's *Take Screenshot* (66 kB in about 5 s) |
| Gadgetbridge's app manager | Opens, but lists only what Gadgetbridge installed itself |
| Installing apps and watchfaces | Not tried yet |
| App messages (PebbleKit) | Not tried yet: no installed watchapp uses them |
| The Core Devices Pebble app | Not tried yet |
| iPhone (ANCS, AMS) | Not tried yet |

The watch has Bluetooth while PebbleOS's Bluetooth is on (*Settings >
Bluetooth*), on the radio of its BLE module (see [](#unawatch-bluetooth)).

## Install Gadgetbridge

Install Gadgetbridge from [F-Droid](https://f-droid.org/packages/nodomain.freeyourgadget.gadgetbridge/).
On its first start it walks through its permissions: it needs at least
*Bluetooth scan*, *Bluetooth connect*, *Notifications* and, for forwarding
the phone's notifications, *Notification access*.

## Pair

1. Make the watch pairable: a watch that has never paired is; otherwise open
   *Settings > Bluetooth* on the watch. The watch keeps one phone: pairing a
   new one replaces the old.
2. In Gadgetbridge, tap **+** (*Connect new device*). The watch shows up as
   *Pebble* followed by the last four digits of its address, e.g.
   *Pebble 45CE*. Tap it.

   ```{thumbnail} images/phone-discovery.webp
   :title: Gadgetbridge finds the watch.
   ```

3. Android asks whether to pair, then shows a six-digit code. The watch
   asks *Pair?*, with the phone's name and the same code.

   ```{thumbnail} images/phone-pairing-code.webp
   :title: The pairing code on the phone.
   ```

   ```{thumbnail} images/watch-pairing.webp
   :title: The same code on the watch.
   ```

4. If the codes match, tap **Pair** on the phone and press **Up** (the upper
   button on the left) on the watch: it is the tick at the top of the
   watch's action bar. **Down** declines.
5. Gadgetbridge shows the watch as *Connected*, with its battery level.

   ```{thumbnail} images/phone-connected.webp
   :title: The watch in Gadgetbridge.
   ```

From then on the phone and the watch reconnect by themselves.

## Notifications

Gadgetbridge forwards the notifications Android shows. By default it forwards
none while the phone's screen is on: turn on *Settings > Notifications >
…also when screen is on* in Gadgetbridge to have them on the watch at once.

```{thumbnail} images/watch-notification.webp
:title: A notification on the watch.
```

```{thumbnail} images/watch-call.webp
:title: An incoming call (Gadgetbridge's test call).
```

```{thumbnail} images/watch-face-via-gadgetbridge.webp
:title: The watchface, taken with Gadgetbridge's Take Screenshot.
```

## Troubleshooting

- **The watch is not found.** It advertises its name only while it is
  pairable; paired, it advertises without it, for its phone to reconnect.
  Open *Settings > Bluetooth* on the watch.
- **A watch paired before 28 November 2026 is not found, or does not
  connect.** Earlier builds read the module's address wrongly: the watch was
  `C1:CE:E6:EC:45:CE` and is now `CD:CE:E6:EC:45:CE` (decision
  261128-100132-02; the last two bytes are this watch's, yours differ). Delete
  the old device in Gadgetbridge (*⋮ > Delete Device*) and in Android's
  Bluetooth settings, and pair again.
- **Pairing fails at once.** The watch keeps the bond of the last phone;
  delete the watch on the phone's side too before pairing again.
- **No notifications.** Gadgetbridge needs *Notification access* (Android's
  *Settings > Notifications > Device & app notifications*), and forwards
  nothing while the screen is on unless asked (above).
- **Gadgetbridge does not reconnect after Bluetooth was turned off and on.**
  Open Gadgetbridge, or accept its offer to pair the watch as a companion
  device, which lets it reconnect in the background.

The screenshots of the watch on this page were taken with Gadgetbridge or,
for the pairing prompt, which shows before Gadgetbridge is connected, read
from the watch's framebuffer over the tether (`tools/una/ble/watchshot.py`).
The phone's name on it is blanked out.
