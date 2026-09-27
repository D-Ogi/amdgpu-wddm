# M629: isolated KMD158 and duplicate-import control build

Isolated source fd8f7d60df515ec9396c7f4e07fa0658b9109982 adds the M628 binding
change to exact157/885922a. The303-file audit against153/c3499f1b retains the
same six changed existing driver files; no unrelated main-tree recovery work
is imported. Full compile/link/stack/catalog/signing and12 quality gates pass.
Source manifest reports clean eligible source. Both packages contain SYS:
6CF6FCBC3DEB1932F6A443B0381B2DED6AEDADF998F83A2BE3388E7BAAC637C9.
Package-UMD version is0.7.158.1. No deployment has occurred in this record.

The cross-process control adds an optional duplicate mode on a third D3D
device. It verifies content through both imports, closes one, writes through
the survivor, reopens and verifies the first, then closes both before owner
release.101 iterations require two duplicate-reopen witnesses across two
allocation generations. CPU and hosted runs remain required; no runtime
result is claimed. The binary compiles with /W4 /WX; its exact source and
binary hashes are in control-build.json. Existing mode remains the default.

Build logs/manifests/audit are copied unchanged. Lab slot is with the other
coordinated experiment on157; this preparation did not access or change it.
GPU Present remains disabled in the current lab state; full G0 remains open.
