# labcam - one cropped frame of the lab unit

A USB camera on the development PC looks at unit A's monitor and case. This script takes one frame from it and
crops the frame hard. It answers the questions that no software on the unit can answer: is there a picture on
the screen, is the machine dark, and is the case on fire.

```
bash tools\win\labcam\snap.sh                                  one frame, prints the file it wrote
CROP=0.23:0.39:0.395:0.61 bash tools\win\labcam\snap.sh        another crop, if the camera moved
```

One frame, one `ffmpeg` process, no window and nothing resident. The camera needs about a second for its auto
exposure, so the script keeps the 30th frame. It needs `ffmpeg` on the path and the camera attached.

| Variable | Default | Meaning |
|---|---|---|
| `CROP` | `0.23:0.39:0.395:0.61` | `w:h:x:y` as fractions of the frame, measured on the 2026-10-01 view |
| `LABCAM_OUT` | `<BC250_ROOT>/scratch/labcam` | Where the frame is written |
| `CAMERA` | `HD 1080P  PC-Camera` | The DirectShow device name (two spaces, as the device reports it) |

`BC250_ROOT` is the workspace root, by default the parent directory of this repository.

## The crop is a rule, not a setting

The camera sees the owner's room with the lab in it. The owner asked for the image to be cut down hard
("Nalezy znaczaco przyciac obraz z kamery" - crop the camera image significantly), and the fractions above
keep unit A's monitor and its case alone.

**Never keep an uncropped frame and never look at one.** If the camera moves, measure a new crop and change
the default. Do not take a full frame "just to see where the camera points now".

## The frames never enter a repository

Frames are written under `<BC250_ROOT>/scratch/labcam` and stay there. They are pictures of a private room.
This repository holds the instrument and nothing it produced.

## What the camera is for

- A picture or a black screen, when the unit answers nothing.
- A hung or dark machine, told apart from a machine that only lost the network.
- The fire watch. After a hot or high-power run, a game session or a clock ceiling test, and after every hang,
  look at the case for smoke, flame or a glow. On any sign, cut the mains with the smart plug at once
  (`tools/win/smartplug`) and tell the owner.

A frame proves what the screen shows. It proves nothing about the operating system: ask ssh, or the emergency
channel in `tools/win/lab-emerg`, before you call the unit alive.

## The operator copy

The copy at `<BC250_ROOT>\scratch\labcam` is the one the workspace rules name, and it writes its frames into
its own directory. This directory holds the source of record. Change the file here first, then copy it over.
