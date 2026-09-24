from mitmproxy import http
import os

def request(flow: http.HTTPFlow) -> None:
    if "playplay/v1/key" in flow.request.pretty_url:
        with open("C:\\Users\\francisco.herrera\\playplay_dump.bin", "wb") as f:
            f.write(flow.request.content)
        print("DUMPED PLAYPLAY REQUEST!")

