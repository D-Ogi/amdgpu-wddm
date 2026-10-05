# Run010: observation failure before GPU initialization

Boot 03:29:30. Gate observation attempted StartTime.ToString on a null DWM StartTime, aborting before GATE_END and before gart/PSP/GFX initialization or any Vulkan process. Finally restored display-only; runtime gates closed and UnconfirmedStarts0. Candidate ICD remains untested. E19 diagnostic now reports unavailable for null process start times. Run011 repeats without reboot because this boot has not initialized the GPU engines.
