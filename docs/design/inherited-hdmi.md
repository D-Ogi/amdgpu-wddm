# HDMI on the inherited link

The driver keeps the picture mode that firmware set. It reads the HDMI monitor through DDC and can enable stereo audio.
It does not change the TMDS clock, video packets or PHY settings.
Mode changes and DP/TMDS hot-plug need the ATOM mode-setting path.

## DDC and the monitor

The DDC1 controller reads at most four EDID blocks at 50 kHz.
Each transaction has a 35 ms bound. The session has a 200 ms stall budget.
The driver saves the pin mux and controller settings before it changes them.
It refuses a busy controller and checks completion. Cleanup restores its settings before it releases ownership.
If ownership is lost, cleanup reports failure and leaves the new owner's registers alone.
It does not send AUX packets to the HDMI monitor.

The EDID parser checks the checksum of each block.
It accepts HDMI audio only with a valid HDMI vendor block and LPCM support in CTA.
A missing extension does not supply audio capabilities.
The LG fixture contains only the captured base block. Its serial fields are removed and its checksum is updated.

The adapter query reads address 0x40 after the EDID read.
Type 1 has a 165 MHz limit. Type 2 reports its limit in 2.5 MHz units.
A failed query means that the adapter type is unknown. It does not establish a DVI connection.
GuardLog records the EDID source, parser result, adapter type and reported limits.
These limits do not enable more picture modes in this version.

## Audio

The audio path checks the active DIG back end, its front end and its OTG0 source.
It admits one HDMI stream with an inherited pixel clock from 25 MHz through 300 MHz.
It refuses dual-link, deep colour, scrambling and the divided HDMI clock.
The endpoint advertises two-channel, 16-bit LPCM at the supported rates among 32, 44.1 and 48 kHz.

DTO0 supplies the audio clock. The imported AMD tables and sequence set the ACR N/CTS fields.
AFMT and Azalia form the audio InfoFrame. The inherited video InfoFrames stay unchanged.
The driver reads back each persistent field before it exposes the endpoint to Windows.
GuardLog records the verified N/CTS values and the EDID source of the endpoint data.

Start failure, D3 and stop use the HDMI mute path. Cleanup continues after a failed write.
The HDMI path never writes DP secondary packets or DTO1.
The DP path keeps its existing sequence and settings.
The `EnableDpAudio*` settings control both transports. A DVI sink gets no audio endpoint from this path.

## Contracts and validation

PROVENANCE: Linux AMD display DC (MIT), v6.18 commit `7d0a66e4bb9081d75c82ec4957c50034cb0ea449`.
The source copies and extraction rules are listed in `driver/amdgpu-import/PROVENANCE.md`.
The controller uses `dce_i2c_hw.c`. Adapter decoding follows `query_dp_dual_mode_adaptor` in `link_detection.c`.
The audio path uses `dcn10_stream_encoder.c` and `dce_audio.c`.
The DCN 2.01 headers supply the register offsets and masks.

The HDMI VSDB uses OUI 0x000C03 and the layout described in HDMI 1.4b section 8.3.2.
The local implementation reference is Linux `drm_edid.c`, `drm_parse_hdmi_vsdb_audio` and `drm_parse_hdmi_vsdb_video`.
This work does not claim a separate review of the licensed HDMI specification.
The monitor descriptor follows `DXGKDDI_QUERY_DEVICE_DESCRIPTOR` in WDK 10.0.26100.0.
It returns only bytes that were read and checked.

Host tests cover EDID checksums, adapter identification, busy ownership, timeouts and failure at each controller access.
Audio tests cover both encoders, N/CTS vectors, mute cleanup and register readback mismatches.
They check that HDMI operations leave DP packet registers unchanged.
Hardware validation must check the DDC result, an audible tone and the direct DP regression.
