"""Exercise real GStreamer recovery using a loopback station; never changes networking.

Usage: python MusicStreamRetry.py /path/to/retrofe_sdl3_smoke_tests /path/to/Common
The smoke executable must be built after the accompanying C++ test changes.
"""
import collections
import http.server
import io
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave

requests=collections.defaultdict(list)
pcm=b''.join(struct.pack('<hh',int(1000*math.sin(2*math.pi*440*i/48000)),int(1000*math.sin(2*math.pi*440*i/48000))) for i in range(48000))
output=io.BytesIO()
with wave.open(output,'wb') as stream:
    stream.setparams((2,2,48000,0,'NONE','not compressed'));stream.writeframes(pcm*20)
audio=output.getvalue()

class Station(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        path=self.path;requests[path].append(time.monotonic());count=len(requests[path])
        good=path=='/recover' and count>=3 or path=='/pause' and count>=2
        body=audio if good else b'not audio'
        self.send_response(200);self.send_header('Content-Type','audio/wav')
        self.send_header('Content-Length',str(len(body)));self.end_headers()
        try:self.wfile.write(body)
        except (ConnectionError,OSError):pass

server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Station)
threading.Thread(target=server.serve_forever,daemon=True).start()
executable=Path(sys.argv[1]).resolve();assets=Path(sys.argv[2]).resolve()
environment=dict(os.environ,RETROFE_TEST_MUSIC_RETRY_BASE_URL='http://127.0.0.1:'+str(server.server_port))
if os.name=='nt':
    environment.update(GST_PLUGIN_PATH=str(executable.parent/'gst-plugins'),GST_PLUGIN_SYSTEM_PATH=str(executable.parent/'gst-plugins'),GST_PLUGIN_SCANNER=str(executable.parent/'gst-plugin-scanner.exe'))
with tempfile.TemporaryDirectory(prefix='retrofe-retry-test-') as directory:
    log=Path(directory)/'test.log'
    with log.open('w') as stream:
        process=subprocess.Popen([str(executable),str(assets)],cwd=directory,env=environment,stdout=stream,stderr=subprocess.STDOUT)
        deadline=time.monotonic()+180
        try:
            while process.poll() is None:
                if time.monotonic()>deadline:raise TimeoutError('Music retry integration timed out')
                try:process.wait(timeout=15)
                except subprocess.TimeoutExpired:print('Music retry integration running; requests: '+str({k:len(v) for k,v in requests.items()}),flush=True)
            if process.returncode:
                print(log.read_text(errors='replace')[-6000:])
                for detail in Path(directory).glob('*runtime.log'):
                    print(detail.read_text(errors='replace')[-3500:])
                raise SystemExit(process.returncode)
        finally:
            if process.poll() is None:process.kill();process.wait()
    expected={'/recover':3,'/stop':1,'/pause':2,'/exhaust':4,'/next':1}
    actual={key:len(value) for key,value in requests.items()}
    assert actual==expected,(actual,expected)
    delays=[b-a for a,b in zip(requests['/exhaust'],requests['/exhaust'][1:])]
    assert all(actual>=minimum for actual,minimum in zip(delays,(4.8,9.8,19.8))),delays
    print('Real stream recovery, retry limit, stop, pause/resume and playlist replacement passed. '+str(actual),flush=True)
server.shutdown()
