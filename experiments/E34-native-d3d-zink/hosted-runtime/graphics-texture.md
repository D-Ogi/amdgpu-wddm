# Textured batch isolation for BD-043

Invoke graphics-state-control with `warp texture`, `baseline texture`, then
`hosted texture` through the bounded offscreen runners. Two8x8 textures carry
distinct blue channels and horizontal/vertical gradients. Each draw switches
texture bindings and independently selects point-clamp or point-wrap sampling.
UVs cross the texture edge; expected texels differ for the sampler choice.

Test both t0/s0 and t1/s0 shader forms; the unused texture slot contains a
different texture as a canary. Dynamic vertex buffers carry position and UV.
Four passes each draw64 tiles. Copy each pass into its own staging resource
before issuing the next pass; wait and map only after all256 draws. This retains
intermediate images, unlike M572's last-image-only checks. Repeat for the other
shader form:512 draws, eight images,32768 exact pixel checks per renderer.

Any mismatch, device loss or deadline failure rejects the control. The prior
eight graphics-state controls run first. A pass narrows the tested resource and
descriptor combinations; it does not establish DWM/Present correctness or fix
the photographic evidence. Keep baseline93B1D1FD/8279AC7F and CPU DWM intact.
