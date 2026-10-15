# airTime

The movie player of air/OS. It plays films and music in a window modelled on
the QuickTime Player of Mac OS 9 and the first versions of Mac OS X —
brushed aluminium, a khaki LCD, round metal transport buttons and the little
pentagon playhead — and it does the decoding on the graphics hardware where
it can.

![airTime icon](resources/branding/airtime-icon.svg)

<!-- airos-ci:latest-builds:start -->
## Latest builds

Built automatically by air/OS CI from commit `e82143e` on 2026-10-15 ([all files](https://github.com/jmgasper/airTime/releases/tag/latest)).

| Architecture | Package |
|---|---|
| arm64 | [airtime-1.0.0-20-arm64.hpkg](https://github.com/jmgasper/airTime/releases/download/latest/airtime-1.0.0-20-arm64.hpkg) |
| x86_64 | [airtime-1.0.0-20-x86_64.hpkg](https://github.com/jmgasper/airTime/releases/download/latest/airtime-1.0.0-20-x86_64.hpkg) |

Install with `pkgman install <file>`, or copy the file into `/boot/system/packages`. Haiku SDK: x86_64 hrev60206-1373-g981d2c0a9d, arm64 hrev60206-1373-g981d2c0a9d.
<!-- airos-ci:latest-builds:end -->

## What it does

* **Everything FFmpeg reads**: MP4/MOV, Matroska/WebM, AVI, MPEG transport
  and program streams, Ogg, FLV, and the audio formats (MP3, AAC, FLAC,
  Opus, Vorbis, WAV, AIFF, AC-3, DTS…). Video: H.264, HEVC (8 and 10 bit),
  AV1, VP9, VP8, MPEG-2/4, ProRes, VC-1 and the rest of libavcodec.
* **Hardware decoding** through the Media Kit's decoder add-ons, which airTime
  loads itself so that it can choose them and fall back to libavcodec when
  one refuses a stream:
  * **AMD Polaris** — UVD through the air/OS `amduvd` add-on: progressive
    8-bit 4:2:0 H.264 Baseline, Main and High. Unsupported streams or an
    unavailable device fall back to libavcodec. With the qualified UVD clock
    setup, WX5100 testing plays a three-minute 1080p30 H.264/AAC clip entirely
    in hardware: all 5,400 frames shown, none dropped, and reported A/V offset
    near zero. Sustained decoder overload still triggers software fallback.
    HEVC Main/Main 10 now selects the addon's explicit decoder index 1,
    retaining P010 for ten-bit scaling and tone mapping. Native WX5100 tests
    pass both 48-picture profiles and a three-minute 1080p30 Main 10/AAC clip:
    all 5,400 pictures shown in hardware, no drops and normal completion.
    Paused seeking also preserves the selected picture and resumes normally.
    The build also passes missing-device software fallback in QEMU
    (48 HEVC pictures, no drops).
    Real WX5100 GPU timeout tests now pass software replay, paused seeking,
    hardware decoder recreation through the existing preference, resumed
    playback and normal completion in the same process. This covers H.264,
    HEVC Main and Main 10 readback faults, plus a Main 10 output-clear fault.
    The latter reported one dropped frame during recovery; the other three
    reported none. The paused software and recreated hardware pictures match
    pixel for pixel. Decoder replacement waits for the demux seek to flush
    old packets, and hardware catch-up stays within known video timestamps
    so it cannot suppress every remaining picture near the end of a film.
    These are private-build tests with a test-only timeout add-on; airTime's
    presentation path still uses CPU scaling and drawing.
  * **GeForce** — NVDEC (the `nvdec` add-on):
    8-bit 4:2:0 progressive H.264, and HEVC Main and Main 10 up to 8K.
    Ten-bit pictures come over as P010, so HDR films keep their depth for
    the tone mapping; a 4K HDR10 film plays at 24 frames a second with about
    14 ms of each frame's time spent decoding.
  * **ROCK 5 ITX** — the RK3588's own decoders through Rockchip MPP (the
    `00_rockchip_mpp` add-on): H.264, and HEVC Main and Main 10, on RKVDEC;
    AV1 Main, eight and ten bits, on VPU981. Pictures come over as NV12, or
    P010 for ten bits, which costs the add-on a copy (an unpacking, for ten
    bits) rather than a conversion; a 4K HDR10 film plays at 24 frames a
    second, full screen, in HEVC or AV1.
  * **Raspberry Pi 4** — H.264 up to 1080p on the VideoCore firmware's
    decoder (the `rpi_mmal` add-on), and HEVC Main and Main 10 up to 4K on
    the SoC's own HEVC block (the `rpi_hevc` add-on, which does the parsing
    the block leaves to software). Pictures come over as planes (I420;
    P010 for ten bits). A 1080p30 film plays at 30 pictures a second in
    either; in software HEVC managed 19 to 25.
  * **Radxa Cubie A7S** — H.264, and HEVC Main and Main 10, up to 4096 wide
    on the Allwinner A733's Cedar engine (the `sunxi_cedar` add-on, which
    does the parsing the engine leaves to software), as I420 (P010 for ten
    bits). 1080p30 films play at 30 pictures a second with none dropped
    and half a core busy in all; in software H.264 takes 2.4 cores, and
    HEVC 3.8 to 4.3, at times falling behind. A 1080p30 Main 10 film plays
    at 30 with none dropped and three cores busy; in software it manages
    11. A 4K HDR film (HEVC Main 10, 80 Mbit/s, over SMB) comes over at
    half size on the board's 1080p screen, averaged by the add-on as it
    copies the engine's picture out, and plays at 24 pictures a second with
    a handful dropped for the first 90 seconds from a cool board (with the
    add-on and thermal driver of air/OS from 7966d15352); the board has no
    fan, and once its throttling reaches the lowest speeds (level 8) the
    film comes off the network too slowly and plays at about 21. Before,
    the same film showed about 8 a second.

  The Movie Inspector (⌘I) says which decoder is in use and, when it is the
  processor, why the hardware was not used.
* **HDR10 and HLG** films are tone mapped for an ordinary display. On ARM,
  ten-bit 4:2:0 pictures are scaled to the window and tone mapped in one
  pass with NEON, on several threads; on a processor whose cores differ
  (the RK3588's Cortex-A76 and A55) only the fast ones do it, unless there
  are just one or two of them (the A733's two A76 beside six A55), which
  are throttled first. A 4K film to a 1080p screen takes about 11 ms a
  picture on the ROCK 5, where swscale and a separate tone mapping took 33.
  A hardware decoder that can hand over pictures at half size (the
  Cubie's) is asked to when the screens show no more than that. On x86 swscale's SIMD is quicker,
  and stays (`AIRTIME_HDR_SCALER=1` and `AIRTIME_HDR_SWSCALE=1` choose).
* **Eight-bit pictures** are scaled to the window and made into pixels in
  one pass as well on ARM, between their two nearest rows and samples (what
  swscale's fast bilinear mode does): about as fast as swscale on an idle
  Raspberry Pi 4, and half its time while a film is being decoded next to
  it (`AIRTIME_SDR_SWSCALE=1` goes back to swscale). When nothing is to be
  drawn over the picture and all of it is visible, it is made straight in
  the frame buffer rather than in a bitmap that is then copied there
  (`AIRTIME_NO_DIRECT_RENDER=1` turns that off): moving memory is what a
  small board is slowest at.
* **Interlaced video** (1080i broadcasts, DVDs) is deinterlaced with
  libavfilter's bwdif, a picture a field, so motion stays smooth.
* **Keeping up**: when the processor cannot decode a film as fast as it
  plays, airTime leaves out deblocking and then unreferenced pictures until
  it can, and sound and picture stay together. A hardware decoder that is
  behind (after a seek far into a group of pictures, where the sound has
  gone on) is asked for no pictures until it is level again, which it gets
  to sooner. One hold-up of a picture or two is not answered by leaving
  pictures out: the film is back in step within a few of them
  (`AIRTIME_STRICT_DROPS=1` for the old way, `AIRTIME_TRACE_DROPS=1` to
  see what holds pictures up). A hardware decoder that remains slower than
  the film despite buffered input is replaced by libavcodec, except on the
  ARM boards for pictures larger than 1080p or ten-bit HEVC, which
  libavcodec decodes slower still. Playback resumes at the audio clock,
  and the Inspector explains the switch.
* **High density screens**: on air/OS's app_server the picture is drawn
  straight into the frame buffer at the screen's own density, so a 4K film
  on a 200% desktop keeps its detail (View ▸ Draw at Screen Density, on by
  default; `AIRTIME_NO_DEVICE_PIXELS=1` turns it off for one run).
* **Window resizing** that keeps the film's proportions once the resize
  corner is let go (View ▸ Keep Proportions), plus Half, Actual and Double
  Size and Fit to Screen (⌘0–⌘3), as QuickTime had.
* **Subtitles**: embedded text tracks (SubRip, ASS/SSA with italics, colours
  and placement, WebVTT, MP4 timed text) and picture tracks (Blu-ray PGS,
  DVD VobSub, DVB); subtitle files next to the film are found by name
  (`Film.en.srt`, `Film.srt`); more can be dropped on the window or added
  from the File menu. Files that are not UTF-8 are read as Windows-1252.
  Forced subtitles in the language being heard are shown automatically.
* **Closed captions**: CEA-608 captions carried in H.264, HEVC and MPEG-2
  video (ATSC A/53, as broadcast and most web video carries them) and
  QuickTime `c608` tracks, shown on a black box as a television shows them
  (Subtitles ▸ Closed Captions, or C).
* **Audio tracks** listed by language, title, format and channels; the
  choice is remembered as a preferred language for the next film.
* **Fast forward and rewind**: hold the ◀◀ or ▶▶ button to scan, faster the
  longer it is held; click it to keep scanning (2×, 4×, 8×… up to 64×).
  J, K and L work as in editing programs. Scanning steps through key frames,
  so it is quick whatever the film.
* **Playback speeds** from half to triple speed with the pitch kept
  (Playback ▸ Playback Speed, or [ and ]).
* **Seeking** anywhere by dragging the playhead, to a typed time
  (Playback ▸ Go to Time…, ⌘G), by chapter, or a frame at a time.
* **Full screen** (⌘F, F, or a double click) on the monitor the window is on,
  with a floating controller that hides when the mouse rests.
* Opens what Tracker opens: once installed, double-clicking a film or a song
  plays it in airTime.
* **Quick Look**: on air/OS, the space bar in Tracker plays a film (or a
  song, when Amp is not installed) in the Quick Look panel with this same
  engine, hardware decoding included. The add-on (`src/quicklook/`, built
  from the engine's sources as `add-ons/quick_look/airTime`) ships in the
  package; a double click on the picture, or the full screen button, opens
  the film in airTime itself. It is built when the SDK has
  `add-ons/quick_look/QuickLook.h` (`make QUICKLOOK=1` forces it).

## Keys

| Key | |
| --- | --- |
| Space, or a click on the picture | Play / pause |
| ← → | One frame back / forward (Shift: 10 seconds) |
| Option ← → | Beginning / end |
| ↑ ↓ | Volume |
| J K L | Reverse, pause, forward — again for faster |
| [ ] \ | Slower, faster, normal speed |
| F, Esc | Full screen, leave it |
| S, A, C | Next subtitle track, next audio track, captions |
| M | Mute |
| Page Up / Down | Previous / next chapter |
| I | Movie Inspector |

## How it is built

```
src/engine/   playback: demuxing, decoding, clocks, subtitles, captions
src/ui/       the window, the metal controller, the video view
src/quicklook/ the Quick Look add-on: the engine and the video view in Tracker's panel
src/          the application and its settings
tests/        host-side tests of the engine (make check-host)
tools/        building, packaging, the icon, test material
```

* `Player` runs four threads: a reader (libavformat) that feeds packet
  queues, a video decoder, an audio decoder, and a presentation thread that
  times pictures against the audio clock (or the system clock).
* Video decoders pull their packets: libavcodec with frame threads, or a
  Media Kit decoder add-on driven through a chunk provider.
* Sound goes through libswresample to 48 kHz stereo float, through
  libavfilter's `atempo` when the speed is not normal, into a ring that a
  `BSoundPlayer` empties; the ring records the media time of each stretch, and
  that sets the clock.
* Pictures are scaled and converted by libswscale on several threads, to the
  size they are shown at; subtitles, captions and messages are drawn into
  them, and they go to the screen through the frame buffer (BDirectWindow)
  where the app_server allows it and DrawBitmap otherwise.

## Building

On air/OS or Haiku (x86_64), with `haiku_devel` and FFmpeg 6:

```
make                     # build-haiku/airTime
tools/package-haiku.sh   # artifacts/airtime-<version>-x86_64.hpkg
```

Where `ffmpeg6_devel` cannot be installed (its exact version requirement
would upgrade the system's FFmpeg), `tools/setup-ffmpeg-devel.sh` extracts
its headers instead; pass `FFMPEG_CFLAGS` and `FFMPEG_LDFLAGS` as
`tools/ws.sh` does.

For the ROCK 5 (arm64), cross-built on Linux: `tools/build-arm64.sh`.

## What the hardware decoding needs

airTime finds the decoders by name among the Media Kit's decoder add-ons
(user add-ons first, as the Media Kit does). Both come from the air/OS tree,
branch `airtime-decoders`:

* **X399**: `src/add-ons/media/plugins/nvdec` from commit `a51f2edebf` or
  later — HEVC and 16 reference frames need it. It is not built by Jam (it
  needs NVIDIA's resource manager headers from the NVK checkout on the
  workstation): `docs/x399-workstation/tools/build-nvdec-plugin.sh` builds
  and installs it into the user's non-packaged add-ons. An older `nvdec`
  still works for H.264 with up to 15 reference frames; airTime asks it for
  nothing else.
* **ROCK 5 ITX**: the `rk3588_vpu` kernel driver and the `00_rockchip_mpp`
  add-on from commit `99926997b8` (DMA pool, power domains kept on while
  decoding, cacheable buffers with cache maintenance) for 8-bit films, and
  from commit `2e3a5ca5fb` for HEVC Main 10 and ten-bit AV1: the add-on
  hands over ten-bit pictures as P010 and eight-bit ones as NV12 (and no
  longer has MPP cut ten-bit AV1 to eight), and the driver leaves
  reference pictures out of its cache maintenance, which halves a 4K job
  (17 ms to 8 ms), and keeps a 640 MiB pool, which 4K ten-bit AV1 needs.
  With an older add-on, Main 10 films fall back to libavcodec, about two
  pictures a second at 4K. The add-on in the current `rock5_ffmpeg`
  package stalls on H.264 and is too slow at 1080p;
  `tools/rock5-itx/build-mpp-addon-arm64.sh` builds the fixed one. The
  driver lives in `/boot/system/non-packaged/add-ons/kernel/drivers/video`
  and needs a restart to change.

Without them, everything plays in software.

## Testing

* `make check-host` runs the engine tests on Linux (time and language
  helpers, ASS markup, subtitle files in other encodings, SPS parsing,
  caption extraction and decoding, the ten-bit scaler against swscale) and
  the renderer test (a synthetic HDR10 picture through the scaler and tone
  mapping against the same arithmetic in double precision, timed against
  swscale's path). Both also build for arm64, to try the NEON code on the
  ROCK 5.
* `tools/make-caption-test.py <film> <dir>` makes a clip with CEA-608
  captions in its H.264 SEI messages.
* `tools/ui-test-x399.py [--dev] [film]` clicks, holds and drags the
  controller over VNC on the X399 and checks what the player did.
* The player answers to scripting, which the lab tools use: for example
  `hey airTime get Stats of Window 0`, `set Position of Window 0 to 60000000`,
  `set Rate of Window 0 to 8`, `get SubtitleText of Window 0`,
  `get Decoder of Window 0`.

## Licence

MIT. FFmpeg is LGPL 2.1 and is used as shared libraries.
