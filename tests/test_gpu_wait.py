"""Check terminal GPU wait failures in a subprocess so an infinite retry fails fast."""
import subprocess
import sys

# Run by ctest with the test binary's path; test discovery only imports it.
if __name__ == '__main__':
    binary = sys.argv[1]
    subprocess.run([binary], check=True, timeout=20)
    result = subprocess.run([binary, '--device-lost'], capture_output=True, text=True, timeout=20)
    output = result.stdout + result.stderr
    if result.returncode == 0 or 'GPU semaphore wait failed at tick 1' not in output or 'ErrorDeviceLost' not in output:
        raise RuntimeError(f'Unexpected device-loss result ({result.returncode}):\n{output}')
    print('PASS: device loss terminates with the failing tick instead of retrying forever')
