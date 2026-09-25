# Asteroids Vulkan stops during sixth Present

Unit A, KMD151, candidate Mesa4D027149, unchanged device generation56484062069/epoch5.
Package003/26 hashed files, DiligentEngineee29581 with pinned dependencies and E33
benchmark patch. Run001 requested660 frames, capture, four threads and fresh cache.
The application window appeared, but the screenshot showed no rendered scene.
The180-second runner deadline expired; child terminated and baseline ICD restored.

Run002 uses package004 with optional BC250_BENCHMARK_TRACE stage output and a30-second
diagnostic deadline. Initialization completes. Six render-begin/before-present markers,
five after-present markers: the sixth IDeviceContext render completes through the
subsets stage, but mSwapChain->Present does not return. That method includes queue
submission, presentation and next-image acquisition; the exact blocking call remains
unidentified. This is not evidence that the GPU executed the frame correctly.

Neither run completed660 frames or produced the required image/timing acceptance.
No Windows/device restart performed. The runner's hardware-health checks remained
successful. Both Vulkan registry restorations are preserved. No further performance
runs until the Present stall is explained. Full M12 and Linux parity remain open.

Raw application logs are unmodified. No dumps or secrets are included.
