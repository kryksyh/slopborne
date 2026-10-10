#!/usr/bin/env python3
"""Compare a BB_PM4_FAULT_DIR snapshot without changing captured files."""
import argparse
import json
from pathlib import Path
import struct


def decode(words):
    packets = []
    offset = 0
    while offset < len(words):
        header = words[offset]
        kind = header >> 30
        length = 1 if kind == 2 else ((header >> 16) & 0x3FFF) + 2
        packet = {"offset_words": offset, "header": hex(header), "type": kind,
                  "words": length, "remaining_words": len(words) - offset}
        if kind == 3:
            packet["opcode"] = hex((header >> 8) & 0xFF)
        if kind in (0, 1):
            packet["error"] = "unsupported packet type"
        elif length > len(words) - offset:
            packet["error"] = "packet exceeds remaining words"
        packets.append(packet)
        if "error" in packet:
            break
        offset += length
    return packets


def read_words(path):
    data = path.read_bytes()
    if len(data) % 4:
        raise ValueError(f"{path}: incomplete DWORD")
    return struct.unpack(f"<{len(data) // 4}I", data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    initial = read_words(args.directory / "initial.bin")
    current = read_words(args.directory / "current.bin")
    metadata = dict(line.split("=", 1) for line in
                    (args.directory / "metadata.txt").read_text().splitlines() if "=" in line)
    offset = int(metadata["packet_offset_words"])
    if len(initial) != len(current) or not 0 <= offset < len(initial):
        raise ValueError("Inconsistent snapshot sizes or fault offset")
    differences = [i for i, (before, after) in enumerate(zip(initial, current)) if before != after]
    before_packets, after_packets = decode(initial), decode(current)
    def context(packets):
        previous = [p for p in packets if p["offset_words"] <= offset]
        return previous[-4:]
    result = {
        "directory": str(args.directory), "metadata": metadata,
        "words": len(initial), "different_words": len(differences),
        "first_differences": [{"offset_words": i, "initial": hex(initial[i]),
                               "current": hex(current[i])} for i in differences[:32]],
        "initial_context": context(before_packets), "current_context": context(after_packets),
        "initial_static_decode_error": next((p for p in before_packets if "error" in p), None),
        "current_static_decode_error": next((p for p in after_packets if "error" in p), None),
        "caveats": ["Static decoding does not follow indirect buffers or conditional execution.",
                    "A processing_start snapshot cannot detect changes before processing began.",
                    "Changed bytes are evidence of mutation, not proof of which writer caused it."],
    }
    rendered = json.dumps(result, indent=2) + "\n"
    if args.out:
        with args.out.open("x") as output:
            output.write(rendered)
    print(rendered, end="")


if __name__ == "__main__":
    main()
