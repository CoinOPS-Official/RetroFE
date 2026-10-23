# Internet music recovery

The retry state lives in `MusicPlayer`. A failed HTTP(S) audio stream is retried
three times, after waits of 5, 10 and 20 seconds. These are waits between failed
attempts, not deadlines for individual network requests. The player then stops;
manually starting or selecting a station starts a fresh budget. Local-file errors
keep their existing stop behavior. Thirty seconds of sustained decoded audio
restores the budget for a later independent outage. Brief successful playback
does not let a flapping station retry indefinitely.

Timers are advanced by the existing player pump; there are no sleeping UI calls
or added threads. Reconnection rebuilds the GStreamer source and bus for the
same station and retains music volume. Stop, pause, playlist replacement, source
selection, audio release and shutdown cancel pending retries. A paused pending
stream can be explicitly resumed.

Run the existing local-file/lifecycle smoke checks, then the optional real
transport regression with the freshly built smoke executable:

```sh
python Tests/MusicStreamRetry.py /path/to/retrofe_sdl3_smoke_tests /path/to/Package/Environment/Common
```

This launches a temporary localhost HTTP audio server and exercises recovery,
volume preservation, permanent failure, stop, pause/resume and playlist changes.
The server checks request counts and retry intervals. It makes no changes to
the machine's network configuration and does not use an external station.

Pi testing is deferred to the next image. The changed C++ source must first be
rebuilt for ARM and installed into the development build before image capture.
The image builder packages the captured executable; updating its Python/shell
templates alone does not rebuild RetroFE.
