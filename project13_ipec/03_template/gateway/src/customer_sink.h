#pragma once
// ★2026-09-23 FW15: FlexRecord と HTTP 共通処理の定義後に読み込む顧客送信部品。
#if CUSTOMER_SINK == CUSTOMER_SINK_IPEC
// iPEC サーバーへ1台ずつ POST する。
// ★iPEC のサーバーは「1回のHTTP受信＝1つのデータの塊」として扱うため、
//   複数台をまとめず、台数分の POST に分ける（20260829_複数Flex対応_開発方針.md）。
// ★2026-09-23 FW15: 既存 iPEC の JSON と HTTP 成功判定を維持。
static bool postToIpec(const FlexRecord& rec) {
  String t = buildMeasuredAt(rec);
  String json = "{\"device\":\"monita-flex-" + String(rec.deviceId) + "\"";
  // ★2026-09-23 FW19: RTC は日本時間なので UTC の UNIX 秒に直す（FW18 までは9時間未来になっていた）
  json += ",\"time\":" + String(rec.rtcEpoch ? rec.rtcEpoch - RTC_UTC_OFFSET_SEC : 0);
  // ★2026-09-23 FW15(0x06): チャネル数は子機ごと。欠測は null（先方の JSON 仕様が決まったら要確認）。
  for (int ci = 0; ci < rec.nCh; ci++) {
    json += ",\"ch" + String(ci + 1) + "\":";
    json += (rec.ch[ci] == LORA_CH_MISSING) ? String("null") : String(rec.ch[ci]);
  }
  json += ",\"batt\":" + String(rec.battMv);
  json += ",\"rssi\":" + String(rec.rssiDbm) + "}";

  Serial.print(F("  [iPEC] ")); Serial.println(json);
  String hdr = "Content-Type: application/json\r\n";
  hdr += "x-api-key: " + String(IPEC_API_KEY) + "\r\n";
  HttpResult r = httpRequest(IPEC_HOST, IPEC_PATH, "POST", json, hdr, true);
  Serial.print(F("  [iPEC] status=")); Serial.print(r.status);
  Serial.print(F(" ")); Serial.print(r.elapsedMs); Serial.println(F("ms"));
  // ★2026-09-23 FW14: ステータス行だけ届いた応答を成功と誤認しない。
  if (!r.headersComplete) { Serial.println(F("  [iPEC] ヘッダー未完")); return false; }
  return (r.status >= 200 && r.status < 300);
}
#elif CUSTOMER_SINK == CUSTOMER_SINK_INFLUX
// InfluxDB（iPEC の代替）へ、1サイクル分を1回の POST でまとめて送る。
// ★2026-09-23 FW18: 1台ずつ POST していたのを、行プロトコルの複数行で1回にまとめた
//   （Gateway が子機ごとに複数件ためるようになり、件数が増えるとサイクルが長くなるため）。
//   各行に Gateway の受信時刻（秒）を付ける。付けないと同じ子機の複数件が同じ時刻になり、
//   InfluxDB は同じ時刻・同じタグの点を上書きするので1件しか残らない。
static bool postToInflux(const FlexRecord* recs, int n) {
  // ★2026-09-24 FW21: run（起動ID）と seq（新しい計測を取り出したサイクル番号）を埋める。
  //   同じ起動の全点で seq が欠番なら、そのサイクル全体が失われた。一部欠落は件数で見る。
  char runHex[9];
  snprintf(runHex, sizeof(runHex), "%08lx", (unsigned long)s_runId);
  String body = "";
  for (int i = 0; i < n; i++) {
    const FlexRecord& rec = recs[i];
    String line = "monita,device=" + String(rec.deviceId) + ",run=" + String(runHex);
    line += " seq=" + String(rec.seq) + "i";   // ★FW20: 再送分は最初に送ったサイクルの seq のまま
    // ★2026-09-23 FW15(0x06): チャネル数は子機ごと。欠測のチャネルはフィールド自体を送らない（InfluxDB は欠損として扱う）。
    for (int ci = 0; ci < rec.nCh; ci++) {
      if (rec.ch[ci] == LORA_CH_MISSING) continue;
      line += ",ch" + String(ci + 1) + "=" + String(rec.ch[ci]) + "i";
    }
    line += ",batt=" + String(rec.battMv) + "i";
    line += ",rssi=" + String(rec.rssiDbm) + "i";
    // ★2026-09-23 FW19: RTC は日本時間。UTC の UNIX 秒に直して付ける（FW18 は9時間未来の時刻で記録していた）
    if (rec.rtcEpoch != 0) line += " " + String(rec.rtcEpoch - RTC_UTC_OFFSET_SEC);   // 無ければサーバーの受信時刻
    Serial.print(F("  [Influx] ")); Serial.println(line);
    if (i) body += "\n";
    body += line;
  }

  String path = String(INFLUX_PATH) + "?org=" + String(INFLUX_ORG) +
                "&bucket=" + String(INFLUX_BUCKET) + "&precision=s";
  String hdr = "Authorization: Token " + String(INFLUX_TOKEN) + "\r\n";
  hdr += "Content-Type: text/plain; charset=utf-8\r\n";

  HttpResult r = httpRequest(INFLUX_HOST, path, "POST", body, hdr, true);
  Serial.print(F("  [Influx] ")); Serial.print(n); Serial.print(F("件 status=")); Serial.print(r.status);
  Serial.print(F(" ")); Serial.print(r.elapsedMs); Serial.println(F("ms"));
  // ★2026-09-23 FW14: ステータス行だけ届いた応答を成功と誤認しない。
  if (!r.headersComplete) { Serial.println(F("  [Influx] ヘッダー未完")); return false; }
  return (r.status >= 200 && r.status < 300);
}
#endif
// ★2026-09-23 FW15: 呼び出し側は顧客実装を知らず、NONE では送信しない。
// ★2026-09-23 FW18: まとめて渡し、送り方は送信先ごとに決める。成否は POST 1回ごとに数える。
static bool countCustomer(bool ok, bool& anySuccess) {
  if (ok) { s_cloudOk++; s_cloudConsecNg = 0; anySuccess = true; }
  else    { s_cloudNg++; s_cloudConsecNg++; }
  return ok;
}
#if CUSTOMER_SINK == CUSTOMER_SINK_IPEC
static const char* CUSTOMER_SINK_NAME = "IPEC";
// iPEC は「1回の受信＝1つのデータ」なので1件ずつ送る（まとめ方は先方の仕様確認待ち）
// ★FW20: 戻り値は「全件成功したか」。iPEC 側の重複の扱いが決まるまで、iPEC は再送しない（常に true）。
static bool postToCustomerBatch(const FlexRecord* recs, int n, bool& anySuccess) {
  for (int i = 0; i < n; i++) {
    countCustomer(postToIpec(recs[i]), anySuccess);
    if (i + 1 < n) waitWithLora(1000); // ★2026-09-23 FW14: 待機中もLoRa受信を継続。
  }
  return true;
}
#elif CUSTOMER_SINK == CUSTOMER_SINK_INFLUX
static const char* CUSTOMER_SINK_NAME = "INFLUX";
static bool postToCustomerBatch(const FlexRecord* recs, int n, bool& anySuccess) {
  return countCustomer(postToInflux(recs, n), anySuccess);
}
#elif CUSTOMER_SINK == CUSTOMER_SINK_NONE
static const char* CUSTOMER_SINK_NAME = "NONE";
static bool postToCustomerBatch(const FlexRecord*, int, bool&) { return true; }
#else
#error Invalid CUSTOMER_SINK
#endif
