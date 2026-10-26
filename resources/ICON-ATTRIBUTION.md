# airTime application icon

Redesigned and refined following visual review on 2026-10-26. All artwork is contributor-original and licensed under
this project's [MIT licence](../LICENSE). No external visual assets or fonts
are used by the generator. The current design is a closed movie clapperboard with a play symbol. Paths below are relative to `resources/`.

| File | Author | Source | Licence |
| --- | --- | --- | --- |
| `branding/airtime-icon.hvif` | airTime contributors | Original geometry in `tools/make-icon.py` and `tools/icon_art.py` | MIT (`../LICENSE`) |
| `branding/airtime-icon.svg` | airTime contributors | Original geometry in `tools/make-icon.py` and `tools/icon_art.py` | MIT (`../LICENSE`) |
| `branding/airtime-icon-preview.png` | airTime contributors | Haiku `BIconUtils::GetVectorIcon` render of the HVIF | MIT (`../LICENSE`) |

The generator's HVIF-to-SVG export uses this repository's existing `tools/hvif.py`.
The native renderer in `tools/render-icon.cpp` is reused from Amp/TurboChook (air/OS contributors,
MIT). Generated SVGs contain the same quantized paths as the packaged HVIF.
The native PNG previews were rendered after compiling the `.rdef` and recovering
byte-identical `BEOS:ICON` attributes with Haiku's `rc` and `resattr`.
