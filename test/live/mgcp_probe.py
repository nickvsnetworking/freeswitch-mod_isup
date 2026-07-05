#!/usr/bin/env python3
# Live bearer test: drive a real CRCX/MDCX/DLCX against osmo-mgw, mirroring
# what mod_isup's bearer_mgcp driver does for one ISUP call's bearer.
import socket, re, sys

MGW = ("127.0.0.1", 2427)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", 0)); s.settimeout(3)

def tx(msg, label):
    s.sendto(msg.replace("\n","\r\n").encode(), MGW)
    data,_ = s.recvfrom(4096)
    txt = data.decode(errors="replace")
    code = txt.split()[0]
    print(f"--- {label} ---\n{txt.strip()}\n")
    return code, txt

fail = 0

# 1) CRCX on the wildcard endpoint, recvonly (inbound IAM: allocate bearer)
code, r = tx("CRCX 1001 rtpbridge/*@mgw MGCP 1.0\nC: isup-cic-5\nM: recvonly\n\n", "CRCX recvonly")
if not code.startswith("200"):
    print("CRCX failed"); sys.exit(1)
conn = re.search(r"^I:\s*(\S+)", r, re.M)
ep   = re.search(r"^Z:\s*(\S+)", r, re.M)
port = re.search(r"^m=audio\s+(\d+)", r, re.M)
conn = conn.group(1) if conn else None
ep   = ep.group(1) if ep else "rtpbridge/1@mgw"
print(f"==> MGW allocated endpoint={ep} conn={conn} rtp_port={port.group(1) if port else '?'}")
if not (conn and port): fail += 1

# 2) MDCX sendrecv, pointing the MGW at FreeSWITCH's RTP (here 127.0.0.1:9000)
sdp = "v=0\no=- 0 0 IN IP4 127.0.0.1\ns=-\nc=IN IP4 127.0.0.1\nt=0 0\nm=audio 9000 RTP/AVP 8\na=rtpmap:8 PCMA/8000\n"
code, r = tx(f"MDCX 1002 {ep} MGCP 1.0\nC: isup-cic-5\nI: {conn}\nM: sendrecv\n\n{sdp}", "MDCX sendrecv + remote SDP")
if not code.startswith("200"): fail += 1

# 3) DLCX (REL/RLC: tear the bearer down)
code, r = tx(f"DLCX 1003 {ep} MGCP 1.0\nC: isup-cic-5\nI: {conn}\n\n", "DLCX")
if not code.startswith("250"): fail += 1

print("RESULT:", "PASS — live MGW bearer control works" if fail==0 else f"FAIL ({fail})")
sys.exit(1 if fail else 0)
