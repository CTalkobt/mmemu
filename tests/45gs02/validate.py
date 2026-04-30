#!/usr/bin/env python3
import subprocess
import os
import sys
import time

XMEGA65 = "/usr/local/bin/xemu-xmega65"
MMSIM_CLI = "./bin/mmemu-cli"
KICKASS = "tools/KickAss65CE02.jar"

EXIT_TRIGGER_ADDR = 0xD6CF
EXIT_VALUE = 0x42

def assemble(asm_path):
    cmd = ["java", "-jar", KICKASS, asm_path, "-odir", "tests/45gs02/"]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    basename = os.path.basename(asm_path).replace(".asm", ".prg")
    expected = f"tests/45gs02/tests/45gs02/{basename}"
    if not os.path.exists(expected):
         for root, dirs, files in os.walk("tests/45gs02/"):
             if basename in files:
                 return os.path.join(root, basename)
    return expected

def run_xmega65(prg_path):
    dump_path = "xmega65.dump"
    if os.path.exists(dump_path): os.remove(dump_path)
    
    env = os.environ.copy()
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)

    cmd = [
        XMEGA65,
        "-headless", "-sleepless", "-testing",
        "-gui", "none",
        "-prg", prg_path,
        "-autoload",
        "-prgexit",
        "-dumpmem", dump_path,
    ]
    
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    
    start_time = time.time()
    timeout = 60
    while time.time() - start_time < timeout:
        if os.path.exists(dump_path) and os.path.getsize(dump_path) > EXIT_TRIGGER_ADDR:
            with open(dump_path, "rb") as f:
                f.seek(EXIT_TRIGGER_ADDR)
                val = f.read(1)
                if val == bytes([EXIT_VALUE]):
                    break
        if proc.poll() is not None:
            break
        time.sleep(1.0)
    
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except:
            proc.kill()

    if os.path.exists(dump_path):
        with open(dump_path, "rb") as f:
            data = f.read()
            if len(data) >= 0x0410:
                return data[0x0400:0x0410]
    return b""

def run_mmsim(prg_path):
    dump_path = "mmsim.dump"
    if os.path.exists(dump_path): os.remove(dump_path)
    cli_script = f"create rawMega65\nload {prg_path}\nrun $0810\nsave {dump_path} $0400 16\nquit\n"
    subprocess.run([MMSIM_CLI], input=cli_script.encode(), timeout=10,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if os.path.exists(dump_path):
        with open(dump_path, "rb") as f:
            return f.read(16)
    return b""

def main():
    if len(sys.argv) < 2:
        print("Usage: validate.py <test.asm>")
        sys.exit(1)
        
    asm = sys.argv[1]
    prg = assemble(asm)
    
    if not os.path.exists(prg):
        print(f"Assembly failed for {asm}")
        sys.exit(1)

    print(f"Validating {asm}...")
    mmsim_res = run_mmsim(prg)
    xmega65_res = run_xmega65(prg)
    
    print(f"  mmsim:   {mmsim_res.hex().upper()}")
    print(f"  xmega65: {xmega65_res.hex().upper()}")
    
    if mmsim_res != xmega65_res or b"\xff" in mmsim_res or mmsim_res == b"":
        if os.path.exists("mmsim.dump"):
            with open("mmsim.dump", "rb") as f:
                print(f"  DEBUG mmsim $0400: {f.read(16).hex().upper()}")
        if os.path.exists("xmega65.dump"):
            with open("xmega65.dump", "rb") as f:
                f.seek(0x0400)
                print(f"  DEBUG xmega65 $0400: {f.read(16).hex().upper()}")
        print("  RESULT: FAIL (mmsim error or test failed)")
        sys.exit(1)
        
    if xmega65_res == b"":
        print("  RESULT: FAIL (xmega65 silent - cross-validation impossible)")
        sys.exit(1)

    print("  RESULT: PASS (mmsim matches xmega65)")
    sys.exit(0)

if __name__ == "__main__":
    main()
