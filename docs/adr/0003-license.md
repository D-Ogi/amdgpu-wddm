# ADR 0003: MIT license for our code

Date: 2026-09-21. Status: accepted (the owner may revisit before the first public release).

Our own code is MIT-licensed, the same terms as the AMD code we import, so that files can be mixed and fixes can flow back towards Mesa/kernel developers without friction. Apache-2.0 code borrowed from the predecessor keeps its license and notice. AMD firmware is never part of the repository; the driver loads it from files the user obtains from `linux-firmware`.
