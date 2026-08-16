'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const gasPath = path.join(__dirname, '..', 'gas', 'one', 'Code.gs');
const context = vm.createContext({ console });
vm.runInContext(
  fs.readFileSync(gasPath, 'utf8') +
    '\nthis.__gasTestApi = { parseGatewayRecord, getProductProfile_, transformChannels_, profileAlertTriggered_, checkAlertsForProductProfile_ };',
  context,
  { filename: gasPath }
);
const gas = context.__gasTestApi;

function hexBytes(hex) {
  return hex.trim().split(/\s+/).map((s) => parseInt(s, 16));
}

function encodeBatt(mV) {
  if (mV === 0) return 255;
  if (mV <= 3000) return 0;
  const value = Math.floor((mV - 3000 + 2) / 5);
  return value >= 254 ? 254 : value;
}

// main.cpp buildBatchQuery()と同じフィールド選択で、19B子機フレームを再構成する。
function buildGatewayChunk(frameHex, cloudV2) {
  const payload = hexBytes(frameHex);
  assert.strictEqual(payload.length, 19, 'LoRa payload must be exactly 19 bytes');
  const epochLe = [0x34, 0x12, 0xA0, 0x68];
  const bytes = epochLe.concat([payload[1]], payload.slice(3, 11));
  if (cloudV2) {
    const battMv = payload[11] | (payload[12] << 8);
    bytes.push(encodeBatt(battMv));
  }
  return bytes.map((b) => b.toString(16).padStart(2, '0').toUpperCase()).join('');
}

function decodeOne(chunk) {
  const record = gas.parseGatewayRecord(chunk);
  const profile = gas.getProductProfile_(record.deviceId);
  return {
    record,
    profile,
    values: Array.from(gas.transformChannels_(record.ch, profile)),
  };
}

const vectors = [
  {
    name: 'GV-1',
    frame: '04 0F 01 05 00 23 00 0B 00 04 00 8C 0F 00 00 00 00 00 00',
    chunk: '3412A0680F050023000B000400C4',
    values: [5, 3.5, 11, 4],
    battV: 3.98,
  },
  {
    name: 'GV-2',
    frame: '04 0F 01 FF FF FF FF 00 00 00 00 36 10 00 00 00 00 00 00',
    chunk: '3412A0680FFFFFFFFF00000000E6',
    values: ['', '', 0, 0],
    battV: 4.15,
  },
  {
    name: 'GV-3',
    frame: '04 0F 01 FF FF FF FF 09 00 00 00 74 0E 00 00 00 00 00 00',
    chunk: '3412A0680FFFFFFFFF090000008C',
    values: ['', '', 9, 0],
    battV: 3.7,
  },
  {
    name: 'GV-4',
    frame: '04 0F 01 01 00 08 00 04 00 04 00 BD 0B 00 00 00 00 00 00',
    chunk: '3412A0680F010008000400040001',
    values: [1, 0.8, 4, 4],
    battV: 3.005,
  },
  {
    name: 'GV-5',
    frame: '04 0F 01 FF 7F FF 7F FF 7F 06 00 94 11 00 00 00 00 00 00',
    chunk: '3412A0680FFF7FFF7FFF7F0600FE',
    values: [32767, 3276.7, 32767, 6],
    battV: 4.27,
    alert: true,
  },
];

vectors.forEach((v) => {
  const v2 = buildGatewayChunk(v.frame, true);
  assert.strictEqual(v2, v.chunk, v.name + ' V2 chunk');

  const v1 = buildGatewayChunk(v.frame, false);
  assert.strictEqual(v1, v.chunk.slice(0, 26), v.name + ' legacy chunk');
  assert.strictEqual(v1.length, 26, v.name + ' legacy length');
  assert.strictEqual(v2.length, 28, v.name + ' V2 length');

  const decoded = decodeOne(v2);
  assert.strictEqual(decoded.record.epoch, 0x68A01234, v.name + ' epoch');
  assert.strictEqual(decoded.profile.productType, 'One-PIR', v.name + ' product');
  assert.deepStrictEqual(decoded.values, v.values, v.name + ' displayed values');
  assert.strictEqual(decoded.record.battV, v.battV, v.name + ' battery');

  const legacy = decodeOne(v1);
  assert.strictEqual(legacy.record.battV, '', v.name + ' legacy battery missing');

  if (v.alert) {
    assert.strictEqual(
      gas.profileAlertTriggered_(decoded.values[2], decoded.profile.channelDefs[2].alert),
      true,
      'GV-5 PIR stuck alert'
    );
  }
});

// GV-6: 電池の下限飽和と不明センチネル。
assert.strictEqual(encodeBatt(2800), 0);
assert.strictEqual(encodeBatt(3000), 0);
assert.strictEqual(encodeBatt(0), 255);
assert.strictEqual(gas.parseGatewayRecord('3412A0680F000000000000000000').battV, 3.0);
assert.strictEqual(gas.parseGatewayRecord('3412A0680F0000000000000000FF').battV, '');

// 0人は欠測ではなく有効な観測値として残す。
const zero = decodeOne('3412A0680F0000000000000100C4');
assert.deepStrictEqual(zero.values, [0, 0, 0, 1]);

assert.throws(() => gas.parseGatewayRecord('00'), /レコード長が不正/);

// Flexは従来のシート内ひずみ閾値とプロファイル固有アラートの両方を通り、
// One-PIRはシート内ひずみ閾値を通らずプロファイル固有アラートだけを通る。
let legacyAlertCalls = 0;
let profileAlertCalls = 0;
context.checkAlertsForDeviceSheet = () => { legacyAlertCalls += 1; };
context.checkProfileAlerts_ = () => { profileAlertCalls += 1; };

gas.checkAlertsForProductProfile_(
  null, null, gas.getProductProfile_(0x01), {}, 'flex', [], new Date(0)
);
assert.strictEqual(legacyAlertCalls, 1, 'Flex legacy strain alert path');
assert.strictEqual(profileAlertCalls, 1, 'Flex profile alert path');

gas.checkAlertsForProductProfile_(
  null, null, gas.getProductProfile_(0x0F), {}, 'one-pir', [], new Date(0)
);
assert.strictEqual(legacyAlertCalls, 1, 'One-PIR skips legacy strain alert path');
assert.strictEqual(profileAlertCalls, 2, 'One-PIR profile alert path');

console.log('GV-1〜GV-6: legacy/V2 chunk, GAS decode, and alert routing OK');
