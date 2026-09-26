# Frame latency profile after rotation correction

Owner reports cursor fluid for about one second, then frozen for about one
second, repeatedly; overlay seconds advance in jumps. M407 visual consistency
is not responsive-desktop acceptance. Instrument actual draw entry-point wall
time and PresentCb time, plus gap between completed presents. Retain rendering,
resource rotation and KMD127/gates. Profile line records <=120 initial frames,
then every60th. Same renderer/draws; no unsupported assertion suppression.
Expected: distinguish expensive CPU draws from presentation waits and idle gaps.
Then choose the next correction from measured timing. Do not treat softpipe
as a final accelerated desktop solution or hide the pauses by slowing overlay.
