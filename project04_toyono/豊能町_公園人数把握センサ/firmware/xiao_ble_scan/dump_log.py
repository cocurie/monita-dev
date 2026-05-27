#!/usr/bin/env python3
"""
dump_log.py — XIAO nRF52840 フラッシュログをCSVに保存
使い方: python3 dump_log.py [ポート名]
        ポート省略時は /dev/cu.usbmodem* を自動検索

注意: 実行前に VS Code のシリアルモニタを閉じてください。
      （同じポートには1つしか接続できません）
"""

import serial
import serial.tools.list_ports
import sys
import os
from datetime import datetime

BAUD     = 115200
TIMEOUT  = 5       # 秒（応答待ち）
OUT_DIR  = os.path.expanduser("~/Desktop")


def find_port():
    """/dev/cu.usbmodem* を自動検索して返す。見つからなければ None。"""
    ports = [p.device for p in serial.tools.list_ports.comports()
             if "usbmodem" in p.device]
    return ports[0] if ports else None


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not port:
        print("[ERROR] シリアルポートが見つかりません。")
        print("        pio device list でポート名を確認して引数に渡してください。")
        print("        例: python3 dump_log.py /dev/cu.usbmodem101")
        sys.exit(1)

    print(f"[INFO] 接続中: {port} ({BAUD} bps)")

    try:
        with serial.Serial(port, BAUD, timeout=TIMEOUT) as ser:
            # 'd' コマンド送信
            ser.write(b"d\n")
            print("[INFO] 'd' を送信しました。データ受信中...")

            lines = []
            capturing = False

            while True:
                raw = ser.readline()
                if not raw:
                    if capturing:
                        print("[WARN] タイムアウト（=== END === が受信できませんでした）。")
                    else:
                        print("[WARN] タイムアウト。デバイスが応答していません。")
                    break
                line = raw.decode("utf-8", errors="replace").rstrip()

                if "=== LOG DUMP ===" in line:
                    capturing = True
                    continue
                if "=== END ===" in line:
                    break
                if capturing:
                    lines.append(line)

    except serial.SerialException as e:
        print(f"[ERROR] シリアル接続に失敗しました: {e}")
        sys.exit(1)

    if not lines:
        print("[WARN] データがありませんでした。")
        print("       ・ENABLE_LOGGING=true でビルドされているか確認してください。")
        print("       ・デバイスが起動してシリアルモニタに出力が出ているか確認してください。")
        return

    # ファイル保存
    ts       = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"ble_log_{ts}.csv"
    filepath = os.path.join(OUT_DIR, filename)

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(filepath, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    record_count = max(0, len(lines) - 1)  # ヘッダ行を除いた件数
    print(f"[OK] {record_count} 件のレコードを保存しました。")
    print(f"     {filepath}")


if __name__ == "__main__":
    main()
