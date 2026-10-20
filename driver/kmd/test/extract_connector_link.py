"""Compile unchanged connector transactions against host boundary fakes."""
import argparse
import hashlib
import re
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--omit-generation", action="store_true")
    p.add_argument("--omit-open-cleanup", action="store_true")
    p.add_argument("--omit-cold-recovery", action="store_true")
    p.add_argument("--omit-prepare-cleanup", action="store_true")
    p.add_argument("--omit-first-failure", action="store_true")
    p.add_argument("--nested-hdmi-stop", action="store_true")
    p.add_argument("--omit-diagnostic-replay", action="store_true")
    p.add_argument("--write-requested-value", action="store_true")
    p.add_argument("--wrong-recovery-target", action="store_true")
    p.add_argument("--omit-surface-adopt", action="store_true")
    p.add_argument("--omit-detached-mode", action="store_true")
    p.add_argument("--drop-optc-term", action="store_true")
    args = p.parse_args()
    source = Path(__file__).resolve().parents[1] / "connector_link.c"
    data = source.read_text(encoding="utf-8")
    types = data[data.index("#define LINK_TAG"):data.index("static ULONGLONG LinkNow")]
    names = (
        "LinkNow LinkOwned Begin End ClockSessionEnter ClockSessionLeave LinkStep SaveFailure RetainFailure PreviousTargetRecord RetainRecovery LogTarget LogFailure TargetRecord AtomAllowed AtomRead AtomWrite AtomNow LinkDelay LinkRead LinkWrite LinkStall Protocol SignalFor Plan LogPlanRefusal CloseAux LogOpenDiagnostic ConnectorLinkLogOpen OpenResult OpenContext ConnectorLinkPrepare "
        "TrainRead TrainWrite TrainEnable TrainPattern TrainDrive TrainDisable "
        "DisableOwned ConnectorLinkDisable Apply ConnectorLinkCommit "
        "ConnectorLinkCurrentSignal ConnectorLinkNeedsPowerOn ConnectorLinkSuspend ConnectorLinkResume "
        "HandoffTarget ConnectorLinkHandoff ConnectorLinkRelease"
    ).split()
    chunks = []
    for name in names:
        matches = list(re.finditer(r"(?m)^(?:static )?(?:ULONGLONG|uint64_t|int|long|void|ULONG|LONG|NTSTATUS|BOOLEAN)\s+" + name + r"\([^;]*?\)\s*\{", data))
        if len(matches) != 1:
            raise SystemExit(f"Expected one definition of {name}, found {len(matches)}")
        start = matches[0].start()
        brace = data.index("{", matches[0].start())
        depth = 1
        end = brace + 1
        # These selected bodies contain no braces in string literals/comments.
        while depth:
            depth += (data[end] == "{") - (data[end] == "}")
            end += 1
        body = data[start:end]
        if args.wrong_recovery_target and name == "ConnectorLinkCommit":
            needle = "Apply(c,&c->Previous,c->Caps,&previousSurface)"
            if body.count(needle) != 1:
                raise SystemExit("Expected one previous-plan recovery")
            body = body.replace(needle, "Apply(c,&c->Next,c->Caps,&previousSurface)")
        if args.omit_surface_adopt and name == "ConnectorLinkDisable":
            # BD-195: 31v2's detach, which set RuntimeDisplayChanged over an empty RuntimePost.
            needle = "DcnLinkSurfaceAdopt(Device);"
            if body.count(needle) != 1:
                raise SystemExit("Surface adoption mutation anchor changed")
            body = body.replace(needle, "(void)0;")
        if args.omit_detached_mode and name == "ConnectorLinkCommit":
            # BD-090: an OS detach that forgets the mode it took down, so the release falls back to the first target.
            needle = "if(c->Previous.Valid) {c->Detached=c->Previous;c->DetachedGeneration=Snapshot->Generation;}"
            if body.count(needle) != 1:
                raise SystemExit("Detached mode mutation anchor changed")
            body = body.replace(needle, "(void)0;")
        # BD-177: a refusal line that keeps the check but loses the term of the OPTC gate is the mutant that
        # proves the term is read from the gate and not invented by the log.
        if args.drop_optc_term and name == "Plan":
            needle = "Refusal->Check=LINK_PLAN_OPTC;Refusal->Optc=(ULONG)result;"
            if body.count(needle) != 1:
                raise SystemExit("OPTC term mutation anchor changed")
            body = body.replace(needle, "Refusal->Check=LINK_PLAN_OPTC;")
        if args.write_requested_value and name == "AtomWrite":
            needle = "WRITE_REGISTER_ULONG(c->Device->Mmio+Dword,effective)"
            if body.count(needle) != 1:
                raise SystemExit("Expected one effective-value hardware write")
            body = body.replace(needle, "WRITE_REGISTER_ULONG(c->Device->Mmio+Dword,Value)")
        if args.omit_diagnostic_replay and name == "ConnectorLinkLogOpen":
            body = body[:body.index("{")] + "{ (void)Device; }"
        if args.nested_hdmi_stop and name == "ConnectorLinkCommit":
            needle = "HdmiLinkStopOwned(Device)"
            if body.count(needle) != 2:
                raise SystemExit("Nested HDMI stop mutation anchor changed")
            body = body.replace(needle, "HdmiLinkStop(Device)", 1)
        if args.omit_generation and name == "LinkOwned":
            needle = "c->Cleanup || ConnectorGenerationCurrent(c->Device,c->Generation)"
            if body.count(needle) != 1:
                raise SystemExit("Generation negative-control anchor changed")
            body = body.replace(needle, "1")
        if args.omit_prepare_cleanup and name == "ConnectorLinkPrepare":
            needle = "Device->ConnectorLinkCleanup=ConnectorLinkRelease(Device);"
            if body.count(needle) != 1:
                raise SystemExit("Prepare cleanup propagation anchor changed")
            body = body.replace(needle, "(void)ConnectorLinkRelease(Device);")
        if args.omit_open_cleanup and name == "OpenContext":
            needle = "if(c->AtomWorkspace)ExFreePoolWithTag(c->AtomWorkspace,LINK_WORK_TAG);"
            if body.count(needle) != 1:
                raise SystemExit("Open cleanup mutation anchor changed")
            body = body.replace(needle, "(void)c->AtomWorkspace;")
        if args.omit_cold_recovery and name == "ConnectorLinkCommit":
            needle = "c->Previous.Valid=c->Active && c->Current.Valid;"
            if body.count(needle) != 1:
                raise SystemExit("Cold recovery mutation anchor changed")
            body = body.replace(needle, "c->Previous.Valid=c->Active && c->Current.Valid && !c->Current.Inherited;")
        if args.omit_first_failure and name == "ConnectorLinkCommit":
            needle = "RetainFailure(Device,&c->Failure);"
            if body.count(needle) != 2:
                raise SystemExit("Original failure mutation anchors changed")
            body = body.replace(needle, "(void)c->Failure;")
        chunks.append(f'#line {data.count(chr(10), 0, start)+1} "connector_link.c"\n{body}\n')
    args.out.mkdir(parents=True, exist_ok=True)
    psp_source=source.with_name("psp.c")
    psp=psp_source.read_text(encoding="utf-8")
    session=psp[psp.index("NTSTATUS PspSessionAcquire("):psp.index("static void PspExecutePrepared(")]
    chunks.insert(0,session)
    hdmi_source = source.with_name("hdmi_link.c")
    hdmi = hdmi_source.read_text(encoding="utf-8")
    hdmi_types = hdmi[hdmi.index("typedef struct HDMI_LINK_CONTEXT"):hdmi.index("static int Owned")]
    hdmi_bodies = []
    for name in ("HdmiLinkStopOwned", "HdmiLinkStop", "HdmiLinkGetRestoreFailure"):
        match = list(re.finditer(r"(?m)^(?:BOOLEAN|void) " + name + r"\([^;]*?\)\s*\{", hdmi))
        if len(match) != 1:
            raise SystemExit(f"Expected one {name} definition")
        start = match[0].start()
        end = hdmi.index("{", start) + 1
        depth = 1
        while depth:
            depth += (hdmi[end] == "{") - (hdmi[end] == "}")
            end += 1
        hdmi_bodies.append(hdmi[start:end])
    (args.out / "hdmi_stop_types.inc").write_text(hdmi_types, encoding="utf-8")
    (args.out / "hdmi_stop_bodies.inc").write_text("\n".join(hdmi_bodies), encoding="utf-8")
    print(f"hdmi_link.c SHA256 {hashlib.sha256(hdmi_source.read_bytes()).hexdigest()}; 3 unchanged stop/detail bodies")
    # BD-169 review: the decision a refused start makes about the hardware it touched reads the terminal
    # cleanup result that ConnectorLinkPrepare keeps on the device, so the real decision belongs with the
    # real prepare and release. It needs nothing of connector.c but the device field.
    connector_source = source.with_name("connector.c")
    connector = connector_source.read_text(encoding="utf-8")
    decisions = []
    for name in ("ConnectorRefusalPreserved",):
        match = list(re.finditer(r"(?m)^(?:BOOLEAN|NTSTATUS|void) " + name + r"\([^;]*?\)\s*\{", connector))
        if len(match) != 1:
            raise SystemExit(f"Expected one {name} definition")
        start = match[0].start()
        end = connector.index("{", start) + 1
        depth = 1
        while depth:
            depth += (connector[end] == "{") - (connector[end] == "}")
            end += 1
        decisions.append(connector[start:end])
    (args.out / "connector_decision.inc").write_text("\n".join(decisions), encoding="utf-8")
    print(f"connector.c SHA256 {hashlib.sha256(connector_source.read_bytes()).hexdigest()};"
          f" {len(decisions)} unchanged refusal decision body")
    (args.out / "connector_link_types.inc").write_text(types, encoding="utf-8")
    (args.out / "connector_link_bodies.inc").write_text("\n".join(chunks), encoding="utf-8")
    print(f"connector_link.c SHA256 {hashlib.sha256(source.read_bytes()).hexdigest()}; {len(names)} unchanged bodies"
          + ("; generation predicate mutant" if args.omit_generation else ""))


if __name__ == "__main__":
    main()

