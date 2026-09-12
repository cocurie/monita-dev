# ★2026-09-12: フィールドユニットは専用RTCが無く、Gateway同期前の暫定時刻として
# 「ビルド・書き込み日時」を使う（main.cppのrtcApplyDefault()参照）。
# 以前は__DATE__/__TIME__（main.cppのソース自体を再コンパイルした瞬間にしか更新されない。
# ソース無変更のままpio run/uploadし直しても値が古いまま＝「時刻埋め込みが壊れている」と
# 誤解される事象があった）を使っていたが、pio run/uploadのたびに必ずこのスクリプトが実行
# されるので、そのたびに新しいFIELD_BUILD_EPOCHを注入しmain.cppの再コンパイルを強制する。
Import("env")

import calendar
import time

# ビルドマシンのローカル時刻（JST）を、TZ変換なしでそのままUTC epoch秒とみなして
# エンコードする（main.cpp側のmktime()/rtcSetTime()と同じ「naive」な流儀に合わせるため）。
build_epoch = calendar.timegm(time.localtime())

env.Append(CPPDEFINES=[("FIELD_BUILD_EPOCH", build_epoch)])
print("[inject_build_epoch] FIELD_BUILD_EPOCH=%d (%s)" %
      (build_epoch, time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())))
