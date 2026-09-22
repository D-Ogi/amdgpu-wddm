#!/usr/bin/env python3
"""Unit tests for tools/wddm_contract_check/check.py.

    python -m unittest discover -s tools/wddm_contract_check

The fixtures are strings, not files: a checker that only ever sees a tree that passes proves nothing, so
each negative case is the good fixture with one line taken out or changed, and the test asserts that the
rule which covers that line - and only that rule - turns into a VIOLATION. That is also how a rule with
a typo in a member name is caught: it would stay OK on the negative fixture.

Nothing here reads driver/kmd, so the tests keep working while the lead edits wddm.c. The last test does
read the real tree, and only asserts that the parse found something for each of the five inventories,
which is the failure mode a regex checker really has: silently extracting nothing and reporting OK.
"""

import json
import os
import unittest

import check


RULES_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rules.json")
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# A synthetic miniport that satisfies every rule: the same shapes wddm.c uses, nothing else.
GOOD_WDDM = r"""
#define DXGKDDI_INTERFACE_VERSION 0x5023
#define BC250_WDDM_SEGMENT_VRAM 1u
#define BC250_WDDM_NODE_3D 0u
#define BC250_WDDM_NODE_COUNT 1u
#define BC250_WDDM_LEVEL_BITS 9u
#define BC250_WDDM_LEVEL_COUNT 4u
#define BC250_WDDM_PAGE_SHIFT 12u
#define BC250_WDDM_VA_BITS (BC250_WDDM_PAGE_SHIFT + BC250_WDDM_LEVEL_BITS * BC250_WDDM_LEVEL_COUNT)
#define BC250_WDDM_PAGE_TABLE_BYTES 4096u
#define BC250_WDDM_PAGING_BUFFER_BYTES 0x10000ul

static NTSTATUS WddmDriverCaps(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_DRIVERCAPS* caps = (DXGK_DRIVERCAPS*)Query->pOutputData;

    if (Query->OutputDataSize < sizeof(*caps) || caps == NULL) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(caps, Query->OutputDataSize);
    caps->WDDMVersion = DXGKDDI_WDDMv2;
    caps->HighestAcceptableAddress.QuadPart = -1;
    caps->SupportNonVGA = TRUE;
    caps->NumberOfSwizzlingRanges = 0;
    caps->SupportSmoothRotation = TRUE;
    caps->SupportPerEngineTDR = TRUE;
    caps->SupportDirectFlip = TRUE;
    caps->SchedulingCaps.MultiEngineAware = 1;
    caps->SchedulingCaps.PreemptionAware = 1;
    caps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
    caps->MemoryManagementCaps.VirtualAddressingSupported = 1;
    caps->MemoryManagementCaps.GpuMmuSupported = 1;
    caps->GpuEngineTopology.NbAsymetricProcessingNodes = BC250_WDDM_NODE_COUNT;
    caps->FlipCaps.FlipOnVSyncMmIo = 1;
    caps->FlipCaps.FlipIndependent = 1;
    caps->MaxQueuedFlipOnVSync = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS WddmQuerySegment4(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_QUERYSEGMENTOUT4* out = (DXGK_QUERYSEGMENTOUT4*)Query->pOutputData;
    DXGK_SEGMENTDESCRIPTOR4* descriptor;

    if (Query->OutputDataSize < sizeof(*out) || out == NULL) return STATUS_BUFFER_TOO_SMALL;
    if (out->NbSegment < count || out->pSegmentDescriptor == NULL) return STATUS_INVALID_PARAMETER;
    descriptor = (DXGK_SEGMENTDESCRIPTOR4*)out->pSegmentDescriptor;
    descriptor->Flags.CpuVisible = 1;
    descriptor->Flags.LocalBudgetGroup = 1;
    descriptor->Flags.DirectFlip = 1;
    descriptor->Size = (SIZE_T)length;
    out->NbSegment = count;
    out->PagingBufferSegmentId = 0;
    out->PagingBufferSize = BC250_WDDM_PAGING_BUFFER_BYTES;
    out->PagingBufferPrivateDataSize = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS WddmGpuMmuCaps(_In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_GPUMMUCAPS* caps = (DXGK_GPUMMUCAPS*)Query->pOutputData;

    if (Query->OutputDataSize < sizeof(*caps) || caps == NULL) return STATUS_BUFFER_TOO_SMALL;
    caps->PageTableUpdateMode = DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
    caps->VirtualAddressBitCount = BC250_WDDM_VA_BITS;
    caps->PageTableLevelCount = BC250_WDDM_LEVEL_COUNT;
    return STATUS_SUCCESS;
}

static NTSTATUS WddmPageTableLevelDesc(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_PAGE_TABLE_LEVEL_DESC* desc = (DXGK_PAGE_TABLE_LEVEL_DESC*)Query->pOutputData;

    if (Query->OutputDataSize < sizeof(*desc) || desc == NULL) return STATUS_BUFFER_TOO_SMALL;
    desc->PageTableIndexBitCount = BC250_WDDM_LEVEL_BITS;
    desc->PageTableSizeInBytes = BC250_WDDM_PAGE_TABLE_BYTES;
    desc->PageTableSegmentId = BC250_WDDM_SEGMENT_VRAM;
    return STATUS_SUCCESS;
}

static NTSTATUS Bc250WddmCreateContext(_In_ const HANDLE hDevice, _Inout_ DXGKARG_CREATECONTEXT* pCreateContext)
{
    BC250_WDDM_OBJECT* parent = WddmObject(hDevice, BC250_WDDM_MAGIC_DEVICE);

    if (parent == NULL) return STATUS_INVALID_PARAMETER;
    if (pCreateContext->NodeOrdinal >= BC250_WDDM_NODE_COUNT) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&pCreateContext->ContextInfo, sizeof(pCreateContext->ContextInfo));
    pCreateContext->ContextInfo.DmaBufferSize = PAGE_SIZE;
    pCreateContext->ContextInfo.DmaBufferSegmentSet = 0;
    pCreateContext->ContextInfo.DmaBufferPrivateDataSize = 0;
    pCreateContext->ContextInfo.AllocationListSize =
        pCreateContext->Flags.GdiContext ? DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT : 0;
    pCreateContext->ContextInfo.PatchLocationListSize = 0;
    pCreateContext->ContextInfo.Caps.NoPatchingRequired = 1;
    pCreateContext->ContextInfo.PagingCompanionNodeId = BC250_WDDM_NODE_3D;
    return STATUS_SUCCESS;
}

static NTSTATUS Bc250WddmQueryAdapterInfo(_In_ const HANDLE hAdapter, _In_ const DXGKARG_QUERYADAPTERINFO* QueryAdapterInfo)
{
    switch (QueryAdapterInfo->Type)
    {
    case DXGKQAITYPE_DRIVERCAPS:
        status = WddmDriverCaps(device, QueryAdapterInfo);
        break;
    case DXGKQAITYPE_QUERYSEGMENT4:
        status = WddmQuerySegment4(device, QueryAdapterInfo);
        break;
    case DXGKQAITYPE_GPUMMUCAPS:
        status = WddmGpuMmuCaps(QueryAdapterInfo);
        break;
    case DXGKQAITYPE_PAGETABLELEVELDESC:
        status = WddmPageTableLevelDesc(device, QueryAdapterInfo);
        break;
    case DXGKQAITYPE_NUMPOWERCOMPONENTS:
        *(UINT*)QueryAdapterInfo->pOutputData = 0;
        status = STATUS_SUCCESS;
        break;
    case DXGKQAITYPE_HISTORYBUFFERPRECISION:
        status = STATUS_SUCCESS;
        break;
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    return status;
}

void WddmBuildTable(_Out_ DRIVER_INITIALIZATION_DATA* Data)
{
    RtlZeroMemory(Data, sizeof(*Data));
    Data->Version = DXGKDDI_INTERFACE_VERSION_WDDM2_0;
    Data->DxgkDdiQueryAdapterInfo = Bc250WddmQueryAdapterInfo;
    Data->DxgkDdiGetNodeMetadata = Bc250WddmGetNodeMetadata;
    Data->DxgkDdiCreateContext = Bc250WddmCreateContext;
    Data->DxgkDdiDestroyContext = Bc250WddmDestroyContext;
    Data->DxgkDdiCreateProcess = Bc250WddmCreateProcess;
    Data->DxgkDdiDestroyProcess = Bc250WddmDestroyProcess;
    Data->DxgkDdiGetRootPageTableSize = Bc250WddmGetRootPageTableSize;
    Data->DxgkDdiSetRootPageTable = Bc250WddmSetRootPageTable;
    Data->DxgkDdiBuildPagingBuffer = Bc250WddmBuildPagingBuffer;
    Data->DxgkDdiSubmitCommand = Bc250WddmSubmitCommand;
    Data->DxgkDdiSubmitCommandVirtual = Bc250WddmSubmitCommandVirtual;
    Data->DxgkDdiPreemptCommand = Bc250WddmPreemptCommand;
    Data->DxgkDdiResetFromTimeout = Bc250WddmResetFromTimeout;
    Data->DxgkDdiRestartFromTimeout = Bc250WddmRestartFromTimeout;
    Data->DxgkDdiQueryDependentEngineGroup = Bc250WddmQueryDependentEngineGroup;
    Data->DxgkDdiQueryEngineStatus = Bc250WddmQueryEngineStatus;
    Data->DxgkDdiResetEngine = Bc250WddmResetEngine;
    Data->DxgkDdiCollectDbgInfo = Bc250WddmCollectDbgInfo;
    Data->DxgkDdiSetStablePowerState = Bc250WddmSetStablePowerState;
    Data->DxgkDdiCalibrateGpuClock = Bc250WddmCalibrateGpuClock;
    Data->DxgkDdiFormatHistoryBuffer = Bc250WddmFormatHistoryBuffer;
    Data->DxgkDdiPresent = Bc250WddmPresent;
    Data->DxgkDdiSetVidPnSourceAddress = Bc250WddmSetVidPnSourceAddress;
    Data->DxgkDdiControlInterrupt = Bc250WddmControlInterrupt;
    Data->DxgkDdiGetScanLine = Bc250WddmGetScanLine;
    Data->DxgkDdiIsSupportedVidPn = Bc250IsSupportedVidPn;
    Data->DxgkDdiRecommendFunctionalVidPn = Bc250RecommendFunctionalVidPn;
    Data->DxgkDdiEnumVidPnCofuncModality = Bc250EnumVidPnCofuncModality;
    Data->DxgkDdiSetVidPnSourceVisibility = Bc250SetVidPnSourceVisibility;
    Data->DxgkDdiCommitVidPn = Bc250CommitVidPn;
    Data->DxgkDdiUpdateActiveVidPnPresentPath = Bc250UpdateActiveVidPnPresentPath;
    Data->DxgkDdiRecommendMonitorModes = Bc250RecommendMonitorModes;
    Data->DxgkDdiQueryVidPnHWCapability = Bc250QueryVidPnHWCapability;
    Data->DxgkDdiStopDeviceAndReleasePostDisplayOwnership = Bc250StopDeviceAndReleasePostDisplayOwnership;
}
"""

GOOD_INF = """
[Bc250_Install]
CopyFiles    = Bc250_Files                      ;@PLAIN-ONLY
;@UMD CopyFiles    = Bc250_Files, Bc250_UmdFiles
;@UMD AddReg       = Bc250_UserModeDriver

;@UMD [Bc250_UserModeDriver]
;@UMD HKR,, UserModeDriverName, 0x00010000, bc250umd.dll, bc250umd.dll, bc250umd.dll
"""

GOOD_BUILD = """
function Write-UmdInf([string]$Source, [string]$Target) {
    foreach ($want in '(?m)^\\s*HKR,,\\s*UserModeDriverName\\b') { }
}
if ($UmdStub) { Write-UmdInf (Join-Path $here 'bc250kmd.inf') (Join-Path $pkgUmd 'bc250kmd.inf') }
"""


def statuses(wddm=GOOD_WDDM, inf=GOOD_INF, build=GOOD_BUILD):
    facts = check.read_facts_text(wddm, "", inf, build)
    with open(RULES_PATH, "r", encoding="utf-8") as handle:
        rules = json.load(handle)
    return {r["id"]: r["status"] for r in check.evaluate(facts, rules)}, facts


def drop(text, needle):
    """The fixture with one line removed. Asserts the line was there, so a renamed member in the
    fixture cannot make a negative test silently pass."""
    lines = [l for l in text.splitlines(True) if needle not in l]
    assert len(lines) < len(text.splitlines(True)), "fixture has no line containing %r" % needle
    return "".join(lines)


class GoodFixture(unittest.TestCase):
    def test_no_violation(self):
        result, _ = statuses()
        bad = {k: v for k, v in result.items() if v in (check.VIOLATION, check.UNKNOWN)}
        self.assertEqual({}, bad)

    def test_every_rule_reached_a_verdict(self):
        result, _ = statuses()
        self.assertTrue(result)
        for rule_id, status in result.items():
            self.assertIn(status, (check.OK, check.NA, check.WARNING), rule_id)

    def test_extraction(self):
        _, facts = statuses()
        self.assertEqual(0x5023, facts.interface_version)
        self.assertEqual({1, 6, 10, 11, 13, 14}, set(facts.qai_handled))
        self.assertEqual("STATUS_NOT_SUPPORTED", facts.qai_default)
        self.assertEqual(1, facts.caps["GpuEngineTopology.NbAsymetricProcessingNodes"]["value"])
        self.assertEqual(48, facts.gpummu["VirtualAddressBitCount"]["value"])
        self.assertEqual({"CpuVisible", "LocalBudgetGroup", "DirectFlip"}, set(facts.segment_flags))
        self.assertIn("Size", facts.segment)
        self.assertEqual("0", facts.ctx["DmaBufferSegmentSet"]["raw"])
        self.assertEqual("1", facts.ctx["Caps.NoPatchingRequired"]["raw"])
        self.assertIn("GdiContext", facts.ctx_flags_read)
        self.assertTrue(facts.ctx_node_checked)

    def test_a_comparison_is_not_an_assignment(self):
        """out->pSegmentDescriptor == NULL is a guard. Reading it as an answer put a member called
        pSegmentDescriptor into the inventory once; this is the regression."""
        _, facts = statuses()
        self.assertNotIn("pSegmentDescriptor", facts.segment_out)
        self.assertEqual({"NbSegment", "PagingBufferSegmentId", "PagingBufferSize",
                          "PagingBufferPrivateDataSize"}, set(facts.segment_out))


class NegativeFixtures(unittest.TestCase):
    """Each one is the good fixture minus one line. The rule named must go to VIOLATION and the others
    must not move: a rule that fires on every fixture is not testing anything."""

    def assertOnlyViolation(self, result, rule_id):
        self.assertEqual(check.VIOLATION, result[rule_id], "%s did not fire" % rule_id)
        others = {k: v for k, v in result.items() if k != rule_id and v == check.VIOLATION}
        self.assertEqual({}, others, "other rules fired as well")

    def test_cap_without_its_ddis(self):
        """SupportPerEngineTDR = 1 with the three engine DDIs gone: the message the lab's dxgkrnl
        carries at DXGADAPTER::Initialize+0x1505."""
        wddm = GOOD_WDDM
        for member in ("DxgkDdiQueryDependentEngineGroup", "DxgkDdiQueryEngineStatus",
                       "DxgkDdiResetEngine", "DxgkDdiCollectDbgInfo"):
            wddm = drop(wddm, "Data->" + member)
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R01-PERENGINE-TDR")

    def test_directflip_cap_without_the_segment_flag(self):
        wddm = drop(GOOD_WDDM, "descriptor->Flags.DirectFlip")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R02-DIRECTFLIP")

    def test_stable_power_state_null(self):
        """The refusal fact M64 predicts: CalibrateGpuClock filled, SetStablePowerState NULL, at
        interface 0x5023. This is what the 0.7.3 table looked like."""
        wddm = drop(GOOD_WDDM, "Data->DxgkDdiSetStablePowerState")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R06-CALIBRATE-STABLEPOWER")

    def test_missing_umd_name(self):
        """The wall runs 001 and 002 hit (fact M64): a package with no UserModeDriverName."""
        inf = drop(GOOD_INF, "UserModeDriverName")
        result, _ = statuses(inf=inf)
        self.assertOnlyViolation(result, "R05-UMD-NAME")

    def test_umd_block_present_but_no_generator(self):
        """The INF still carries the ;@UMD block but build.ps1 no longer produces that package, so no
        installable package writes the value. The INF alone is not the contract."""
        result, _ = statuses(build="# nothing here generates anything\n")
        self.assertOnlyViolation(result, "R05-UMD-NAME")

    def test_preemption_without_multiengine(self):
        wddm = drop(GOOD_WDDM, "caps->SchedulingCaps.MultiEngineAware")
        result, _ = statuses(wddm=wddm)
        self.assertEqual(check.VIOLATION, result["R07-PREEMPTION-MULTIENGINE"])
        self.assertEqual(check.NA, result["R10-MULTIENGINE-CONTEXTS"])

    def test_gpummu_without_its_query_types(self):
        wddm = GOOD_WDDM.replace("case DXGKQAITYPE_GPUMMUCAPS:", "case DXGKQAITYPE_SEGMENTMEMORYSTATE:")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R11-GPUMMU-SET")

    def test_gpummu_and_iommu_together(self):
        wddm = GOOD_WDDM.replace("caps->MemoryManagementCaps.GpuMmuSupported = 1;",
                                 "caps->MemoryManagementCaps.GpuMmuSupported = 1;\n"
                                 "    caps->MemoryManagementCaps.IoMmuSupported = 1;")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R12-GPUMMU-XOR-IOMMU")

    def test_old_segment_query_answered(self):
        wddm = GOOD_WDDM.replace("case DXGKQAITYPE_QUERYSEGMENT4:",
                                 "case DXGKQAITYPE_QUERYSEGMENT3:\n    case DXGKQAITYPE_QUERYSEGMENT4:")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R15-QAI-OLD-SEGMENTS-REFUSED")

    def test_unhandled_type_not_refused_cleanly(self):
        """The default arm answering something other than STATUS_NOT_SUPPORTED. STATUS_NOT_IMPLEMENTED
        is the trap: harmless here, a bugcheck in BuildPagingBuffer."""
        wddm = GOOD_WDDM.replace("        status = STATUS_NOT_SUPPORTED;\n        break;\n    }",
                                 "        status = STATUS_NOT_IMPLEMENTED;\n        break;\n    }")
        result, _ = statuses(wddm=wddm)
        self.assertEqual(check.VIOLATION, result["R15-QAI-OLD-SEGMENTS-REFUSED"])
        self.assertEqual(check.VIOLATION, result["R16-QAI-15-47-REFUSED"])

    def test_reserved_member_assigned(self):
        wddm = GOOD_WDDM.replace("    Data->DxgkDdiPresent = Bc250WddmPresent;",
                                 "    Data->DxgkDdiUpdatePageTable = Bc250WddmUpdatePageTable;\n"
                                 "    Data->DxgkDdiPresent = Bc250WddmPresent;")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R19-RESERVED-NULL")

    def test_page_table_level_count_out_of_range(self):
        wddm = GOOD_WDDM.replace("#define BC250_WDDM_LEVEL_COUNT 4u", "#define BC250_WDDM_LEVEL_COUNT 1u")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R26-PAGETABLE-LEVELS")

    def test_paging_buffer_in_a_local_segment(self):
        """0.7.4's answer. VIDMM_DMA_POOL::Init calls VerifySegmentSet with required flags = Aperture,
        so naming a segment without that bit is STATUS_INVALID_PARAMETER and the paging process is torn
        down: the refusal that ended E16 run 004 (facts M65, M66)."""
        wddm = GOOD_WDDM.replace("out->PagingBufferSegmentId = 0;",
                                 "out->PagingBufferSegmentId = BC250_WDDM_SEGMENT_VRAM;")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R34-PAGINGBUFFER-SEGMENT")

    def test_paging_buffer_in_an_aperture_segment(self):
        """The same id, but on a segment that carries Flags.Aperture: VerifySegmentSet passes, so the
        rule must not fire. Without this the rule would read as 'never name a segment', which is not
        what dxgmms2 enforces."""
        wddm = GOOD_WDDM.replace("out->PagingBufferSegmentId = 0;",
                                 "out->PagingBufferSegmentId = BC250_WDDM_SEGMENT_VRAM;")
        wddm = wddm.replace("descriptor->Flags.CpuVisible = 1;",
                            "descriptor->Flags.CpuVisible = 1;\n    descriptor->Flags.Aperture = 1;")
        result, _ = statuses(wddm=wddm)
        self.assertEqual(check.OK, result["R34-PAGINGBUFFER-SEGMENT"])

    def test_context_dma_buffer_in_a_segment(self):
        """The same VerifySegmentSet rule, applied to the context's own DMA buffer."""
        wddm = GOOD_WDDM.replace("pCreateContext->ContextInfo.DmaBufferSegmentSet = 0;",
                                 "pCreateContext->ContextInfo.DmaBufferSegmentSet = 1;")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R35-CTX-DMASEGMENTSET")

    def test_gdi_context_allocation_list_ignored(self):
        """AllocationListSize answered without ever looking at Flags.GdiContext. This is 0.7.5's
        answer: a GdiContext would be given 0 where 256 is required."""
        wddm = GOOD_WDDM.replace(
            "pCreateContext->ContextInfo.AllocationListSize =\n"
            "        pCreateContext->Flags.GdiContext ? DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT : 0;",
            "pCreateContext->ContextInfo.AllocationListSize = 0;")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R36-CTX-GDI-ALLOCLIST")

    def test_nopatching_conditional_without_a_patch_ddi(self):
        """0.7.5's answer: NoPatchingRequired follows Flags.VirtualAddressing, so a non-virtual context
        is told dxgkrnl will patch - with DxgkDdiPatch NULL and both list sizes zero."""
        wddm = GOOD_WDDM.replace(
            "pCreateContext->ContextInfo.Caps.NoPatchingRequired = 1;",
            "pCreateContext->ContextInfo.Caps.NoPatchingRequired = "
            "pCreateContext->Flags.VirtualAddressing ? 1u : 0u;")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R37-CTX-NOPATCHING")

    def test_nopatching_conditional_with_a_patch_ddi(self):
        """The same conditional answer, but with a patch path in the table: then it is legal, and the
        rule must not fire. This is what keeps R37 from meaning 'always answer 1'."""
        wddm = GOOD_WDDM.replace(
            "pCreateContext->ContextInfo.Caps.NoPatchingRequired = 1;",
            "pCreateContext->ContextInfo.Caps.NoPatchingRequired = "
            "pCreateContext->Flags.VirtualAddressing ? 1u : 0u;")
        wddm = wddm.replace("    Data->DxgkDdiPresent = Bc250WddmPresent;",
                            "    Data->DxgkDdiPatch = Bc250WddmPatch;\n"
                            "    Data->DxgkDdiPresent = Bc250WddmPresent;")
        result, _ = statuses(wddm=wddm)
        self.assertEqual(check.OK, result["R37-CTX-NOPATCHING"])

    def test_context_node_not_bounds_checked(self):
        wddm = drop(GOOD_WDDM, "pCreateContext->NodeOrdinal >=")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R38-CTX-NODE-RANGE")

    def test_flip_cap_without_the_interrupt_ddis(self):
        wddm = drop(drop(GOOD_WDDM, "Data->DxgkDdiControlInterrupt"), "Data->DxgkDdiGetScanLine")
        result, _ = statuses(wddm=wddm)
        self.assertOnlyViolation(result, "R04-FLIPONVSYNCMMIO")

    def test_empty_source_is_not_a_pass(self):
        """The failure mode of a regex checker: a file it cannot parse must not read as compliant."""
        result, _ = statuses(wddm="/* nothing */\n")
        self.assertIn(check.VIOLATION, result.values())


class RuleFile(unittest.TestCase):
    def test_every_rule_has_a_source_and_unique_id(self):
        with open(RULES_PATH, "r", encoding="utf-8") as handle:
            rules = json.load(handle)["rules"]
        seen = set()
        for rule in rules:
            self.assertNotIn(rule["id"], seen)
            seen.add(rule["id"])
            for field in ("declaration", "requires_text", "source", "require"):
                self.assertTrue(rule.get(field), "%s has no %s" % (rule["id"], field))
            self.assertGreater(len(rule["source"]), 40, "%s: a source has to say where" % rule["id"])
            self.assertTrue(any(tag in rule["source"] for tag in ("DXGKRNL", "LEARN", "SAMPLE")),
                            "%s: the source names no kind" % rule["id"])

    def test_every_predicate_is_understood(self):
        """A typo in a predicate name would otherwise read as 'cannot tell' forever."""
        facts = check.read_facts_text(GOOD_WDDM, "", GOOD_INF, GOOD_BUILD)
        with open(RULES_PATH, "r", encoding="utf-8") as handle:
            rules = json.load(handle)["rules"]
        for rule in rules:
            for pred in rule.get("when", []) + rule["require"]:
                result, why = check.predicate(facts, pred)
                self.assertFalse(why.startswith("unknown predicate"), "%s: %s" % (rule["id"], why))


class RealTree(unittest.TestCase):
    """The one test that reads driver/kmd. It asserts only that the parse found things, never what they
    are: the values are the lead's to change, the parser going blind is the checker's bug."""

    def setUp(self):
        self.wddm = os.path.join(REPO, "driver", "kmd", "wddm.c")
        if not os.path.exists(self.wddm):
            self.skipTest("driver/kmd/wddm.c not in this tree")

    def test_parse_is_not_empty(self):
        kmd = os.path.join(REPO, "driver", "kmd")
        facts = check.read_facts(self.wddm, os.path.join(kmd, "bc250kmd.h"),
                                 os.path.join(kmd, "bc250kmd.inf"), os.path.join(kmd, "build.ps1"))
        self.assertGreaterEqual(len(facts.qai_handled), 4)
        self.assertGreaterEqual(len(facts.caps), 10)
        self.assertGreaterEqual(len(facts.ddis), 40)
        self.assertGreaterEqual(len(facts.segment_flags), 1)
        self.assertGreaterEqual(len(facts.gpummu), 2)
        self.assertGreaterEqual(len(facts.pagetable), 2)
        self.assertGreaterEqual(len(facts.ctx), 5)
        self.assertIsNotNone(facts.interface_version)
        self.assertEqual("STATUS_NOT_SUPPORTED", facts.qai_default)


if __name__ == "__main__":
    unittest.main()
