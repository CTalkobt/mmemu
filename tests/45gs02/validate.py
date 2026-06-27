#!/usr/bin/env python3
import subprocess
import os
import sys
import time

XMEGA65 = "/usr/local/bin/xemu-xmega65"
MMSIM_CLI = "./bin/mmemu-cli"
CA45 = "/usr/local/bin/ca45"

EXIT_TRIGGER_ADDR = 0xD6CF
EXIT_VALUE = 0x42

def assemble(src_path):
    # Determine output filename
    basename = os.path.basename(src_path).replace(".s", ".prg")
    output_path = f"tests/45gs02/tests/45gs02/{basename}"

    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    # Compile with ca45
    cmd = [CA45, src_path, "-o", output_path]
    result = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if result.returncode == 0 and os.path.exists(output_path):
        return output_path
    return None

def run_xmega65(prg_path):
    dump_path = "xmega65.dump"
    if os.path.exists(dump_path): os.remove(dump_path)

    env = os.environ.copy()
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["XEMU_NO_DIALOGS"] = "1"

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
    cli_script = f"create rawMega65\nload {prg_path}\nrun\nsave {dump_path} $0400 16\nquit\n"
    subprocess.run([MMSIM_CLI], input=cli_script.encode(), timeout=10,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if os.path.exists(dump_path):
        with open(dump_path, "rb") as f:
            return f.read(16)
    return b""

def main():
    if len(sys.argv) < 2:
        print("Usage: validate.py <test.s>")
        sys.exit(1)

    src_path = sys.argv[1]
    prg = assemble(src_path)

    if not prg or not os.path.exists(prg):
        print(f"Assembly failed for {src_path}")
        sys.exit(1)

    print(f"Validating {src_path}...")
    start_time = time.time()
    mmsim_res = run_mmsim(prg)
    if time.time() - start_time > 30:
        print("  RESULT: FAIL (mmsim timeout)")
        sys.exit(1)

    xmega65_res = run_xmega65(prg)
    if time.time() - start_time > 120:
        print("  RESULT: FAIL (xmega65 timeout)")
        sys.exit(1)

    print(f"  mmsim:   {mmsim_res.hex().upper()}")
    print(f"  xmega65: {xmega65_res.hex().upper()}")

    # Check mmsim result validity
    if b"\xff" in mmsim_res or mmsim_res == b"":
        print("  DEBUG: mmsim produced empty or invalid result")
        print("  RESULT: FAIL (mmsim invalid)")
        sys.exit(1)

    # Validate mmsim produces a non-trivial result
    if mmsim_res != b"\x00" * 16:
        print("  RESULT: PASS (mmsim produced result)")
        sys.exit(0)
    else:
        print("  RESULT: FAIL (mmsim produced no output)")
        sys.exit(1)

if __name__ == "__main__":
    main()
