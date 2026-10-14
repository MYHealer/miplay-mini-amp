# 抓完整启动日志：先持有串口，再靠 RTS/DTR 触发 S3 USB-Serial-JTAG 复位。
# 顺序不能反——先复位再开串口会丢掉 app_main 之前的全部输出。
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM18"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

s = None
for attempt in range(20):
    try:
        s = serial.Serial(PORT, 115200, timeout=0.2)
        break
    except Exception as exc:  # 端口正在 USB 重枚举，等它稳定
        if attempt == 19:
            raise
        print(f"open {PORT} retry {attempt}: {exc}", file=sys.stderr)
        time.sleep(0.5)

time.sleep(0.3)

# S3 内置 USB-Serial-JTAG：DTR/RTS 直接映射到 GPIO0 / EN。
# RTS=1 拉低 EN 进复位；释放后芯片从 flash 重新启动。
s.setDTR(False)
s.setRTS(True)
time.sleep(0.2)
s.setRTS(False)
time.sleep(0.1)

buf = bytearray()
t0 = time.time()
while time.time() - t0 < SECONDS:
    chunk = s.read(8192)
    if chunk:
        buf += chunk

s.close()
sys.stdout.buffer.write(bytes(buf))
