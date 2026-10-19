#!/usr/bin/env python3
"""Export one contiguous C5VRX IQ40 snapshot. No truth or range score implied."""
import argparse
import datetime
import json
from pathlib import Path
import re
import time


def fnv1a(data):
    value=2166136261
    for byte in data:value=((value^byte)*16777619)&0xffffffff
    return value


def decode(lines):
    metadata=None;data=bytearray()
    for line in lines:
        line=line.strip()
        if line.startswith('IQSNAP_BEGIN '):
            if metadata is not None:raise ValueError('second capture header')
            metadata=dict(re.findall(r'(\w+)=(\w+)',line))
            if metadata.get('format')!='Ihigh_Qlow' or metadata.get('bytes')!='8190' or metadata.get('rate_hz')!='40000000':
                raise ValueError('unsupported source format')
        elif line.startswith('IQSNAP_DATA '):
            if metadata is None:raise ValueError('data before header')
            match=re.fullmatch(r'IQSNAP_DATA offset=(\d+) ([0-9a-f]+)',line)
            if not match or int(match[1])!=len(data):raise ValueError('missing/reordered IQ bytes')
            data.extend(bytes.fromhex(match[2]))
            if len(data)>8190:raise ValueError('capture overflow')
        elif line.startswith('IQSNAP_END '):
            if metadata is None or len(data)!=8190:raise ValueError('truncated capture')
            checksum=re.fullmatch(r'IQSNAP_END fnv1a=([0-9a-f]{8})',line)
            if not checksum or int(checksum[1],16)!=fnv1a(data) or metadata['fnv1a']!=checksum[1]:
                raise ValueError('IQ checksum mismatch')
            return bytes(data),metadata
        elif line.startswith('IQSNAP_REFUSED'):raise ValueError(line)
    raise ValueError('incomplete IQ snapshot')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('port');ap.add_argument('output',type=Path);a=ap.parse_args()
    if a.output.exists() or a.output.with_suffix('.json').exists():ap.error('use a fresh output path')
    import serial
    s=serial.Serial();s.port=a.port;s.baudrate=115200;s.timeout=.1;s.write_timeout=1;s.dtr=False;s.rts=False
    lines=[]
    try:
        s.open();s.reset_input_buffer();s.write(b'I');end=time.monotonic()+10;buffer=b''
        while time.monotonic()<end:
            buffer+=s.read(4096)
            while b'\n' in buffer:
                line,buffer=buffer.split(b'\n',1);line=line.decode('ascii',errors='replace').strip();lines.append(line)
                if line.startswith(('IQSNAP_END','IQSNAP_REFUSED')):
                    raw,metadata=decode(lines)
                    metadata.update(timestamp_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                                    scope='204.75us contiguous snapshot; capture-start state unknown; no full vertical/range validation')
                    a.output.write_bytes(raw);a.output.with_suffix('.json').write_text(json.dumps(metadata,indent=2))
                    print(f'Saved {len(raw)} IQ samples to {a.output}');return
    finally:s.close()
    raise RuntimeError('IQ snapshot timeout; requires firmware with uppercase I capture support')


if __name__=='__main__':main()
