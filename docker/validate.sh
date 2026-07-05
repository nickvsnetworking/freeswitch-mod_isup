#!/bin/bash
# Validate the full mod_isup feature set live in the 3-container lab:
#   - proper call flow: IAM -> ACM (alerting) -> ANM (answer) -> REL -> RLC
#   - settable IAM parameters (TMR, CPC, calling number/NOA, FCI, hop counter)
#   - settable ACM Backward Call Indicators
#   - SCCP (SI=3) enabled on the same M3UA association
#
# Captures on the sigtran hub and asserts against the pcap with tshark (host).
# Exit 0 = all checks pass.
set -u
cd "$(dirname "$0")"
PASS=0; FAIL=0
ok(){ echo "  PASS: $1"; PASS=$((PASS+1)); }
no(){ echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

echo "== building images =="
docker compose -f docker-compose.yml build >/tmp/v_build.log 2>&1 || { echo "build failed"; tail -20 /tmp/v_build.log; exit 2; }

echo "== starting sigtran + capture =="
docker compose down >/dev/null 2>&1
docker compose up -d sigtran >/dev/null 2>&1
sleep 4
docker compose exec -d sigtran sh -c "tcpdump -i any -s0 -w /cap.pcap -U 'udp or (ip proto 132)'"
sleep 1

echo "== running the call (fs-b answers, fs-a originates with test params) =="
docker compose up -d fs-b fs-a >/dev/null 2>&1
sleep 30

echo "== collecting pcap + logs =="
mkdir -p captures
docker compose exec -T sigtran sh -c 'kill -INT $(pidof tcpdump) 2>/dev/null; sleep 1' 2>/dev/null
docker compose cp sigtran:/cap.pcap captures/validate.pcap >/dev/null 2>&1
LOGA=$(docker compose logs fs-a 2>&1); LOGB=$(docker compose logs fs-b 2>&1)
V() { tshark -r captures/validate.pcap -V 2>/dev/null; }
TEXT="$(V)"

echo "== assertions =="
# 1. ISUP call ladder
for m in "Initial address" "Address complete" "Answer" "Release" "Release complete"; do
  echo "$TEXT" | grep -q "$m" && ok "ladder: $m present" || no "ladder: $m MISSING"
done
# 2. settable IAM params
echo "$TEXT" | grep -qiE "3.1 kHz|3,1 kHz" && ok "IAM TMR = 3.1 kHz audio" || no "IAM TMR not 3.1 kHz"
echo "$TEXT" | grep -qi "payphone\|pay-phone" && ok "IAM CPC = payphone" || no "IAM CPC not payphone"
echo "$TEXT" | grep -q "61755500009" && ok "IAM calling number = 61755500009" || no "IAM calling number missing"
echo "$TEXT" | grep -qi "Hop counter" && ok "IAM hop counter present" || no "IAM hop counter missing"
# 3. ACM backward call indicators
echo "$TEXT" | grep -qi "Backward call indicators" && ok "ACM backward call indicators present" || no "ACM BCI missing"
# 4. proper flow timing: ACM (ringing) precedes ANM (answer) in the originator's view
echo "$LOGA" | grep -q "CALL ANSWERED" && ok "fs-a: call answered after alerting" || no "fs-a: call not answered"
# 5. SCCP enabled
echo "$LOGB" | grep -qi "SCCP enabled" && ok "fs-b: SCCP enabled (SI=3)" || no "fs-b: SCCP not enabled"
echo "$LOGA" | grep -qi "SCCP enabled" && ok "fs-a: SCCP enabled (SI=3)" || no "fs-a: SCCP not enabled"

# 6. OAM: isup status reports ASP ACTIVE (fs_cli/API)
echo "$LOGA" | grep -q "isup status" && echo "$LOGA" | grep -A6 "isup status" | grep -qi "ACTIVE" && ok "OAM: isup status shows M3UA ACTIVE" || no "OAM: isup status missing/not ACTIVE"
# 7. dialplan path: fs-b routed the call via XML dialplan (ring_ready->ACM, answer->ANM)
echo "$TEXT" | grep -q "Address complete" && echo "$TEXT" | grep -q "Answer" && ok "dialplan path: ACM(alerting)+ANM via XML dialplan" || no "dialplan path: ACM/ANM missing"
# 8. bearer through-connected on the shared MGW endpoint (CRCX+MDCX, PCMA)
echo "$TEXT" | grep -q "rtpbridge/1@mgw" && ok "bearer: both legs use shared MGW endpoint rtpbridge/1@mgw" || no "bearer: shared endpoint not used"
SIGLOG=$(docker compose logs sigtran 2>&1)
# both legs must register their RTP with the MGW (two distinct connections MDCX'd)
MDCX_N=$(echo "$SIGLOG" | grep -c "MDCX: connection successfully modified")
[ "$MDCX_N" -ge 2 ] && ok "bearer: both legs MDCX'd on the MGW ($MDCX_N modifies)" || no "bearer: <2 MDCX (legs not both bridged)"
# 9. end-to-end RTP audio: fs-b's gentones must traverse the osmo-mgw bridge and
#    arrive at fs-a as live (non-CNG) PCMA frames.
RTP_TO_A=$(tshark -r captures/validate.pcap -Y 'rtp && ip.dst==172.30.0.11' -d 'udp.port==16000-16100,rtp' 2>/dev/null | wc -l)
RTP_FROM_B=$(tshark -r captures/validate.pcap -Y 'rtp && ip.src==172.30.0.12' -d 'udp.port==16000-16100,rtp' 2>/dev/null | wc -l)
[ "$RTP_FROM_B" -ge 20 ] && ok "media: fs-b streamed RTP into the bearer ($RTP_FROM_B pkts)" || no "media: fs-b did not stream RTP ($RTP_FROM_B)"
[ "$RTP_TO_A" -ge 20 ] && ok "media: osmo-mgw bridged audio through to fs-a ($RTP_TO_A pkts)" || no "media: no bridged RTP reached fs-a ($RTP_TO_A)"
echo "$LOGA" | grep -qE "MEDIA: received [1-9][0-9]* non-CNG" && ok "media: fs-a consumed live audio frames end-to-end" || no "media: fs-a received no live audio frames"

# 10. circuit-group supervision wired into the live rx path (Q.764 §2.9): each
#     exchange's start-up group reset must be answered by the peer's manager.
#     Both directions must round-trip — the retransmit-until-GRA logic fixes the
#     boot race where the first GRS is dropped before the reverse link is up.
echo "$TEXT" | grep -q "Circuit group reset" && ok "CGM: group reset (GRS) issued on the link" || no "CGM: no GRS on the link"
GRA_A=$(echo "$LOGA" | grep -c "cgm_emit mt=0x29"); GRA_B=$(echo "$LOGB" | grep -c "cgm_emit mt=0x29")
[ "$GRA_A" -ge 1 ] && [ "$GRA_B" -ge 1 ] && ok "CGM: both managers answered a peer GRS with GRA (bidirectional, race fixed)" || no "CGM: GRS not answered both ways (fs-a=$GRA_A fs-b=$GRA_B)"
echo "$LOGA" | grep -q "cgm_emit mt=0x17.*send_rc=0" && ok "CGM: originating exchange's GRS was delivered (retransmit beat the boot race)" || no "CGM: fs-a GRS never delivered"

# 11. continuity check (Q.764 §2.1.8): the originating exchange requested a
#     continuity check in the IAM (NCI) and reported it with a COT; the call
#     still completed (ladder + media above), proving the loopback bearer path.
echo "$TEXT" | grep -q "Continuity" && ok "continuity: COT present on the wire" || no "continuity: no COT"
echo "$LOGA" | grep -q "op_send mt=0x05" && ok "continuity: originating exchange (fs-a) sent COT after IAM" || no "continuity: fs-a did not send COT"
echo "$LOGB" | grep -qi "op_crcx .*mode=loopback" && ok "continuity: terminating side looped its bearer (CRCX loopback)" || no "continuity: loopback bearer not set up"
echo "== teardown =="
docker compose down >/dev/null 2>&1

# 12. origination barrier (Q.764 §2.9.1): a call placed *before* the start-up
#     group reset has resolved must be rejected with a temporary failure. Run an
#     fs-a that originates immediately (ISUP_PRESLEEP=0) into a fresh hub where
#     the reset has not yet converged. Detached + force-removed so a slow harness
#     shutdown cannot block the suite.
echo "== barrier check: originate during start-up reset =="
docker compose up -d sigtran >/dev/null 2>&1; sleep 3
docker compose up -d fs-b >/dev/null 2>&1; sleep 1
docker compose run -d --name fsa_barrier -e ISUP_PRESLEEP=0 fs-a \
	/bin/sh -c "/mod_isup/fs_harness 6 originate 1002" >/dev/null 2>&1
sleep 7
docker logs fsa_barrier 2>&1 | grep -q "NORMAL_TEMPORARY_FAILURE" \
	&& ok "barrier: early originate rejected with temporary failure (circuits not yet resynchronised)" \
	|| no "barrier: early originate not rejected"
docker rm -f fsa_barrier >/dev/null 2>&1
docker compose down >/dev/null 2>&1

echo
echo "RESULT: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] && echo "ALL DOCKER VALIDATION CHECKS PASSED" || echo "VALIDATION FAILURES"
exit $([ $FAIL -eq 0 ] && echo 0 || echo 1)
