#!/usr/bin/env python3
"""Summarize xctrace Metal XML exports without adding overlapping GPU durations.

Export metal-gpu-intervals and metal-application-encoders-list tables with
xcrun xctrace export --input RUN.trace --xpath
'/trace-toc/run[@number="1"]/data/table[@schema="TABLE"]' --output TABLE.xml
Then pass the two XML files to this script. Output must be a new file.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import xml.etree.ElementTree as ET


def rows(path):
    root = ET.parse(path).getroot()
    refs = {e.attrib['id']: e for e in root.iter() if 'id' in e.attrib}
    columns = [e.text for e in root.findall('.//schema/col/mnemonic')]
    for row in root.findall('.//row'):
        result = {}
        for name, element in zip(columns, row):
            element = refs.get(element.get('ref'), element)
            result[name] = (element.text or '', element.get('fmt', element.text or ''))
        yield result


def union_ms(intervals):
    total, end = 0, -1
    for start, stop in sorted(intervals):
        total += max(0, stop - max(start, end))
        end = max(end, stop)
    return total / 1e6


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('gpu_intervals', type=Path)
    p.add_argument('encoders', type=Path)
    p.add_argument('--process', default='bb-probe')
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args()
    if a.out.exists():
        p.error('Output already exists; choose a new filename to preserve it.')
    channels = defaultdict(list)
    all_intervals = []
    for row in rows(a.gpu_intervals):
        if a.process not in row['process'][1] or row['state'][0] != 'Active':
            continue
        start, duration = int(row['start'][0]), int(row['duration'][0])
        interval = (start, start + duration)
        channels[row['channel-name'][0]].append(interval)
        all_intervals.append(interval)
    frames = defaultdict(Counter)
    for row in rows(a.encoders):
        if a.process in row['process'][1]:
            frames[row['frame-number'][0]][row['encoder-label'][0]] += 1
    span = (max(end for _, end in all_intervals) - min(start for start, _ in all_intervals)) / 1e6 if all_intervals else 0
    result = {
        'process_filter': a.process,
        'gpu_observed_span_ms': span,
        'gpu_active_union_ms': union_ms(all_intervals),
        'channel_active_union_ms': {k: union_ms(v) for k, v in channels.items()},
        'channel_active_percent_of_span': {k: union_ms(v) / span * 100 if span else 0 for k, v in channels.items()},
        'encoders_by_frame': dict(frames),
        'total_encoders_by_frame': {k: sum(v.values()) for k, v in frames.items()},
        'caveats': [
            'Channel intervals overlap; do not add their durations or percentages.',
            'Active time includes stalls within an execution interval; it is not ALU utilization.',
            'First and last frames may be incomplete. Failed submissions may truncate the trace.',
            'Encoder counts identify investigation targets, not proof of redundant work.',
        ],
    }
    with a.out.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'encoders_by_frame'}, indent=2))


if __name__ == '__main__':
    main()
