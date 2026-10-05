#!/usr/bin/env python3
"""Convert an explicit vulkaninfo text capture into cached overlay inventory.

This does not launch Vulkan. A timeout retains observed properties, but never
turns a truncated format list into a complete count. Capabilities are advertised
by the captured ICD, not validated by rendering tests. UUIDs are not exported.
"""
import argparse
import json
import re
from pathlib import Path


def parse_report(report, metadata, loader, observed_hash=None):
    result = dict(metadata)
    result["SchemaVersion"] = 1
    result["DetailsPath"] = "vulkan-inventory.txt"
    requested = metadata.get("IcdLibraryPath")
    result["RequestedIcdLibraryPath"] = requested
    result["RequestedIcdLibrarySha256"] = metadata.get("IcdLibrarySha256")
    observed = re.findall(r'Using "[^"\r\n]+" with driver: "([^"\r\n]+)"', loader)
    libraries = list(dict.fromkeys(observed))
    result["IcdVerified"] = len(libraries) == 1
    if len(libraries) == 1:
        result["IcdLibraryPath"] = libraries[0]
        result["IcdLibrarySha256"] = observed_hash
        result["LoaderEvidence"] = "Vulkan loader: Using device with driver (vulkan-loader.txt)"
    else:
        result["LoaderEvidence"] = "No unambiguous loaded ICD witness"
    mismatch = bool(libraries and requested and libraries[0].lower() != requested.lower())

    devices = list(re.finditer(r"^GPU\d+:\s*$", report, re.M))
    result["DeviceCount"] = len(devices)
    device_text = report[devices[0].end():devices[1].start() if len(devices) > 1 else len(report)] if devices else ""

    def field(name):
        match = re.search(r"^\s*" + re.escape(name) + r"\s*=\s*(.+)$", device_text, re.M)
        return match.group(1).strip() if match else None

    result["Device"] = {key: field(name) for key, name in (
        ("Name", "deviceName"), ("ApiVersion", "apiVersion"),
        ("DriverName", "driverName"), ("DriverInfo", "driverInfo"),
        ("DriverVersion", "driverVersion"), ("DeviceType", "deviceType"))}

    def count(name, source):
        match = re.search(r"^" + re.escape(name) + r": count = (\d+)\s*$", source, re.M)
        return int(match.group(1)) if match else None

    result["InstanceExtensionCount"] = count("Instance Extensions", report)
    result["DeviceExtensionCount"] = count("Device Extensions", device_text)
    layer_section = re.search(r"^Layers:\s*\n=+\s*\n(.*?)^(?:Presentable Surfaces:|Device Properties and Extensions:)", report, re.M | re.S)
    layers = re.findall(r"^(VK_LAYER_\S+)", layer_section.group(1), re.M) if layer_section else None
    result["Layers"] = layers
    result["LayerCount"] = len(layers) if layers is not None else None
    features = []
    for name in ("timelineSemaphore", "bufferDeviceAddress", "shaderFloat16", "sparseBinding"):
        values = set(re.findall(r"^\s*" + name + r"\s*=\s*(true|false)\s*$", device_text, re.M))
        features.append({"Name": name, "Supported": values.pop() == "true" if len(values) == 1 else None})
    result["Features"] = features
    formats = dict.fromkeys(("EnumeratedCount", "SupportedCount", "StorageImageCount", "ColorAttachmentCount"))
    complete = metadata.get("Status") == "captured" and not metadata.get("TimedOut") and metadata.get("ExitCode") == 0
    if complete and "Format Properties:" in device_text:
        groups = re.split(r"^Common Format Group\[\d+\]:\s*$", device_text.split("Format Properties:", 1)[1], flags=re.M)[1:]
        totals = dict.fromkeys(formats, 0)
        for group in groups:
            names = re.findall(r"^\t(FORMAT_\w+)\s*$", group, re.M)
            expected = count("Formats", group.strip())
            if expected != len(names) or "Properties:" not in group:
                break
            properties = group.split("Properties:", 1)[1]
            totals["EnumeratedCount"] += len(names)
            totals["SupportedCount"] += len(names) if "FORMAT_FEATURE" in properties else 0
            totals["StorageImageCount"] += len(names) if re.search(r"FORMAT_FEATURE(?:_2)?_STORAGE_IMAGE_BIT\b", properties) else 0
            totals["ColorAttachmentCount"] += len(names) if re.search(r"FORMAT_FEATURE(?:_2)?_COLOR_ATTACHMENT_BIT\b", properties) else 0
        else:
            if groups:
                formats = totals
    result["Formats"] = formats
    result["Status"] = "ok" if complete else ("partial" if result["Device"]["Name"] else "error")
    errors = []
    if not complete:
        errors.append(metadata.get("Error") or "Incomplete capture")
    if mismatch:
        errors.append("System ICD observed; requested override ignored")
    result["Error"] = "; ".join(errors)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--observed-sha256")
    parser.add_argument("--details-path", default="vulkan-inventory.txt")
    args = parser.parse_args()
    if args.observed_sha256 and not re.fullmatch(r"[0-9a-fA-F]{64}", args.observed_sha256):
        parser.error("expected SHA256 of the loader-observed DLL")
    read = lambda name: (args.capture_dir / name).read_text(encoding="utf-8-sig", errors="replace")
    result = parse_report(read("vulkan-inventory.txt"), json.loads(read("capture.json")), read("vulkan-loader.txt"), args.observed_sha256)
    result["DetailsPath"] = args.details_path
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
