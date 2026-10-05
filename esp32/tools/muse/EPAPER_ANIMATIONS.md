# One-time animations on the 1.54-inch e-paper

The Waveshare ESP32-S3-ePaper-1.54 backend shows three procedural key poses
on entry to boot or listening. These are frames within existing modes, not
new device states. Boot wakes from a squash, stretches and raises a hand;
listening raises one hand to the ear and leans toward the user. Existing
shading, paper dithering and background particles remain.

The display task owns the sequence. It waits 100 ms after each completed
panel refresh before drawing the next pose. Actual frame time includes the
panel refresh; this is not a fixed frame rate. After the third pose, the
screen stays still. Boot completes through ordinary connection updates;
voice actions, errors, pairing confirmation, reply text and external images
can interrupt it. Other entry sequences yield to newer states. An already-running physical refresh must
finish, but obsolete frames are not queued. A quick release/repress starts
a new listening sequence even if the display never showed the intermediate
state. Status debounce is bypassed while animation frames are pending, so
connection notifications do not stall boot or delay a voice interruption.

The first update and the existing periodic ghost-clearing updates use full
refresh. Animation frames count toward the same full-refresh policy as
other screen updates. Refresh failures stop the sequence rather than
repeatedly retrying animation frames.

Configuration, under the device SDK menu:

- `CONFIG_HOMEHUB_EPAPER_154_ANIMATIONS`: enabled by default; disable for
  still poses.
- `CONFIG_HOMEHUB_EPAPER_154_ANIMATION_HOLD_MS`: 40–2000 ms, default 100.
  Tune after checking the logged `e-paper fast refresh in ... ms` on hardware.

## Local validation and preview

Run from `esp32/`:

```sh
python3 -m unittest discover -s tests -p 'test_link_epaper*.py'
mkdir -p /tmp/epaper-frames
cc -std=c11 -Wall -Wextra -Werror -I main -I components/muse \
  tests/link_epaper_animation_harness.c avatar/muse_pixel.c -lm \
  -o /tmp/epaper-preview
/tmp/epaper-preview /tmp/epaper-frames
```

The six PGM images use the actual 112-pixel renderer and monochrome dithering.
They show character output, not physical refresh behavior or ghosting. The
test also checks deadlines after slow refreshes, one-shot completion,
interruption, rapid state reentry, hidden content, failure and still fallback.

## Device check, after user confirmation

1. Identify the connected board and flash the matching build.
2. Check boot logs for successful startup, refresh durations and no resets.
   Routine connection progress should not interrupt the three boot poses.
3. Hold the talk button long enough to see the listening entry poses settle.
4. Release during the sequence. Thinking/replying must replace the remaining
   listening frames after any in-flight refresh finishes.
5. Repeat a quick release/repress. Verify the newest action wins.
6. Verify reply cards and external images stay visible, and that the avatar
   does not keep refreshing once its entry sequence ends.
7. Judge ghosting and response delay before adjusting the hold interval.
