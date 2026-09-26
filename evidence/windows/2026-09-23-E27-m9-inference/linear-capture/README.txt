M305 - Retained unequal-offset identities and slice traversal
Base revision bed764da5192be7132d646be0e6331c1edeb30fd plus working tree changes, snapshots included.

GfxPagingCaptureVirtualGraph now resolves each endpoint independently, retains different source/destination page counts, pads the shorter normalization input with its final page and excludes padding from actual traversal/classification. Equal-offset graph behavior remains. Unequal offsets use PagingPageAliasDirection on captured identities; unsupported dependency shapes return internal NOT_SUPPORTED before output. Normalization workspace still reused after physical flags are fixed.

GfxPagingCapturedLinearSlice selects forward/backward page-bounded physical slices from immutable captured identities. Proposed progress counts accepted traversal bytes, not VA offset, and does not change Capture.Progress. This helper does not publish or execute commands.

Host routing31026checks PASS. Additional disjoint and shifted-alias capture fixtures use2source/3destination pages and disable VA translation after capture. Staged application of selected slices matches the initial snapshot; original actual-DDI M303 regression still open. These are selector/capture tests, not new packet-emitter or OS acceptance. Existing equal-offset actual-packet tests pass. WDK26100build PASS, SYS1F21557D881EA1737A95979360FEA7C17AE0D1A3225BA539925D3E0E85759411, not deployed.

Next wire captured physical slices into packet emission and exact logical publication; advance only accepted progress and free at completion through context owner. General cyclic unequal-offset interval dependencies remain open. No lab access/mutation this turn; sshd restoration pending.
