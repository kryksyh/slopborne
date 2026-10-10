#!/usr/bin/env python3
"""CPU-only PM4 fault-capture regression; preserve every artifact for inspection."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path, help="new artifact directory")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    executable = str(args.executable.resolve())
    analyzer = Path(__file__).resolve().parents[1] / "packaging/macos/analyze-pm4-fault.py"
    results = []
    for kind, reason in (("truncated", "truncated_type3"),
                         ("type0", "unimplemented_type0"),
                         ("type1", "unsupported_packet_type")):
        for mutated in (False, True):
            case = ("mutated-" if mutated else "") + kind
            directory = args.output / case
            directory.mkdir()
            env = dict(os.environ, BB_PM4_FAULT_DIR=str(directory.resolve()))
            with (directory / "process.log").open("x") as output:
                run = subprocess.run([executable, "--" + case], env=env, stdout=output,
                                     stderr=subprocess.STDOUT, timeout=30)
            assert run.returncode not in (0, 2), (case, "malformed command accepted")
            snapshots = list(directory.glob("pm4_*/metadata.txt"))
            assert len(snapshots) == 1, (case, "missing or duplicate snapshot")
            analysis = subprocess.check_output([sys.executable, str(analyzer),
                                               str(snapshots[0].parent)], text=True)
            with (directory / "analysis.json").open("x") as output:
                output.write(analysis)
            data = json.loads(analysis)
            meta = data["metadata"]
            assert meta["fault_reason"] == reason, (case, meta)
            assert meta["complete_binary_writes"] == "true", (case, meta)
            assert int(meta["packet_offset_words"]) == (5 if mutated else 0), case
            assert data["different_words"] == int(mutated), case
            assert int(meta["different_words"]) == int(mutated), case
            assert data["current_static_decode_error"] is not None, case
            assert (data["initial_static_decode_error"] is None) == mutated, case
            assert meta["observed_header"] == meta["current_header"], case
            if mutated:
                assert data["first_differences"][0]["offset_words"] == 5, case
            results.append({"case": case, "exit_code": run.returncode, "passed": True})
    with (args.output / "results.json").open("x") as output:
        json.dump(results, output, indent=2)
    print("PASS: six malformed PM4 cases distinguish initial corruption from later mutation")


if __name__ == "__main__":
    main()
