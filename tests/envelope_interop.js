// envelope_interop.js — cross-check the envelope format against the backend's
// actual rules, and against Node's base64, which is what the server sees.
//
//   node envelope_interop.js emit  > envelopes.txt   # generate + validate
//   node envelope_interop.js check < envelopes.txt   # verify a C++ round trip
//
// Run `emit`, then feed envelopes.txt to test_envelope (the C++ side), then run
// `check` on its output. If all three agree, the mod's base64 is byte-identical
// to Node's and every envelope satisfies src/envelope.js.
//
// This exists because the failure it catches is silent. A wrapped or non-
// canonical base64 string comes back as a generic validation error, and a
// plaintext that is 16 bytes too long comes back as "too large" with no hint
// that the tag is what pushed it over.

'use strict';

const crypto = require('node:crypto');

// ---- a faithful copy of the backend's validation rules ---------------------
// Mirrors src/envelope.js. Kept here rather than imported so this test still
// means something if the backend copy drifts: a diff between the two is itself
// the signal.

const ENVELOPE_VERSION = 1;
const IV_BYTES = 12;
const TAG_BYTES = 16;
const MAX_MESSAGE_BYTES = 100000;
const BASE64_RE = /^[A-Za-z0-9+/]+={0,2}$/;

function decodedLength(b64) {
  const len = b64.length;
  if (len % 4 !== 0) return -1;
  let padding = 0;
  if (b64.endsWith('==')) padding = 2;
  else if (b64.endsWith('=')) padding = 1;
  return (len / 4) * 3 - padding;
}

function validateEnvelope(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('not an object');
  if (value.v !== ENVELOPE_VERSION) throw new Error(`bad version ${value.v}`);
  if (typeof value.iv !== 'string' || typeof value.ct !== 'string') throw new Error('iv/ct not strings');
  if (!BASE64_RE.test(value.iv)) throw new Error('iv is not canonical base64');
  if (!BASE64_RE.test(value.ct)) throw new Error('ct is not canonical base64');
  const ivLen = decodedLength(value.iv);
  if (ivLen !== IV_BYTES) throw new Error(`iv decodes to ${ivLen}, want ${IV_BYTES}`);
  const ctLen = decodedLength(value.ct);
  if (ctLen < 0) throw new Error('ct is not valid base64');
  if (ctLen < TAG_BYTES + 1) throw new Error('ct too short for message + tag');
  if (ctLen > MAX_MESSAGE_BYTES) throw new Error(`ct is ${ctLen} bytes, limit ${MAX_MESSAGE_BYTES}`);
  return { bytes: ctLen };
}

// ---- envelope construction, exactly as the mod does it --------------------

const KEY = Buffer.alloc(32, 0x2b); // a fixed test key, not a real one

function seal(plaintext) {
  const iv = crypto.randomBytes(IV_BYTES);
  const cipher = crypto.createCipheriv('aes-256-gcm', KEY, iv);
  const body = Buffer.concat([cipher.update(plaintext, 'utf8'), cipher.final()]);
  const tag = cipher.getAuthTag(); // 16 bytes, APPENDED — Node's convention
  return {
    v: ENVELOPE_VERSION,
    iv: iv.toString('base64'),
    ct: Buffer.concat([body, tag]).toString('base64'),
  };
}

function open(env) {
  const iv = Buffer.from(env.iv, 'base64');
  const raw = Buffer.from(env.ct, 'base64');
  const body = raw.subarray(0, raw.length - TAG_BYTES);
  const tag = raw.subarray(raw.length - TAG_BYTES);
  const decipher = crypto.createDecipheriv('aes-256-gcm', KEY, iv);
  decipher.setAuthTag(tag);
  return Buffer.concat([decipher.update(body), decipher.final()]).toString('utf8');
}

// ---- cases ----------------------------------------------------------------

const maxPlaintext = MAX_MESSAGE_BYTES - TAG_BYTES; // 99984

const cases = [
  ['ascii', 'Hello.'],
  ['one byte', 'x'],
  ['quotes and backslashes', 'She said "no" \\ then left'],
  ['newlines', 'line one\nline two\r\nline three'],
  ['unicode', 'chaosfox26 nods — 「はい」 🦊'],
  ['json-looking', '{"v":1,"iv":"not really","ct":"nope"}'],
  ['exactly at the cap', 'A'.repeat(maxPlaintext)],
  ['one under the cap', 'B'.repeat(maxPlaintext - 1)],
];

// Every length class of base64 padding, so the C++ encoder is exercised on all
// three remainders.
for (let n = 1; n <= 8; n++) cases.push([`len ${n}`, 'z'.repeat(n)]);

function emit() {
  let failures = 0;
  const out = [];

  for (const [label, plaintext] of cases) {
    const env = seal(plaintext);
    try {
      const { bytes } = validateEnvelope(env);
      if (bytes !== Buffer.byteLength(plaintext, 'utf8') + TAG_BYTES) {
        console.error(`FAIL ${label}: ct length ${bytes} != plaintext + tag`);
        failures++;
      }
      if (open(env) !== plaintext) {
        console.error(`FAIL ${label}: round trip mismatch`);
        failures++;
      }
      // iv is always 12 bytes, so always exactly 16 base64 characters.
      if (env.iv.length !== 16) {
        console.error(`FAIL ${label}: iv is ${env.iv.length} chars, want 16`);
        failures++;
      }
      out.push(JSON.stringify({
        label,
        iv: env.iv,
        ct: env.ct,
        plainLen: Buffer.byteLength(plaintext),
        // base64 rather than the raw text so newlines and unicode cannot be
        // mangled by whatever reads this next.
        plainB64: Buffer.from(plaintext, 'utf8').toString('base64'),
      }));
    } catch (err) {
      console.error(`FAIL ${label}: ${err.message}`);
      failures++;
    }
  }

  // The one over the cap MUST be rejected — this is the 99,984 vs 100,000 trap.
  try {
    validateEnvelope(seal('C'.repeat(maxPlaintext + 1)));
    console.error('FAIL over-cap plaintext was accepted; the limit is on ciphertext');
    failures++;
  } catch (err) {
    if (!/limit/.test(err.message)) {
      console.error(`FAIL over-cap rejected for the wrong reason: ${err.message}`);
      failures++;
    }
  }

  // Malformed envelopes the mod must never produce.
  const rejects = [
    ['wrapped base64', { v: 1, iv: 'q83vASNFZ4mrze8B', ct: 'AAAA\nAAAA' }],
    ['whitespace', { v: 1, iv: 'q83vASNFZ4mrze8B', ct: 'AAAA AAAA' }],
    ['unpadded', { v: 1, iv: 'q83vASNFZ4mrze8B', ct: 'AAAAA' }],
    ['short iv', { v: 1, iv: 'AAAA', ct: 'A'.repeat(24) }],
    ['url-safe alphabet', { v: 1, iv: 'q83vASNFZ4mrze8-', ct: 'A'.repeat(24) }],
    ['tag only', { v: 1, iv: 'q83vASNFZ4mrze8B', ct: Buffer.alloc(16).toString('base64') }],
    ['wrong version', { v: 2, iv: 'q83vASNFZ4mrze8B', ct: 'A'.repeat(24) }],
  ];
  for (const [label, env] of rejects) {
    let rejected = false;
    try { validateEnvelope(env); } catch { rejected = true; }
    if (!rejected) {
      console.error(`FAIL ${label} should have been rejected`);
      failures++;
    }
  }

  console.error(`\nemit: ${cases.length} envelopes, ${rejects.length + 1} rejection cases, ${failures} failures`);
  if (failures) process.exit(1);
  process.stdout.write(out.join('\n') + '\n');
}

// Verify the C++ side's re-encoding matched Node byte for byte.
// Input: lines of {label, iv, ct, cppIv, cppCt}
function check() {
  const text = require('node:fs').readFileSync(0, 'utf8');
  let failures = 0;
  let n = 0;
  for (const line of text.split('\n')) {
    if (!line.trim()) continue;
    const r = JSON.parse(line);
    n++;
    if (r.cppIv !== r.iv) {
      console.error(`FAIL ${r.label}: C++ iv differs from Node's`);
      failures++;
    }
    if (r.cppCt !== r.ct) {
      console.error(`FAIL ${r.label}: C++ ct differs from Node's`);
      failures++;
    }
  }
  console.error(`check: ${n} envelopes compared, ${failures} failures`);
  if (failures) process.exit(1);
}

// Decrypt envelopes the C++ side produced. This is the direction that actually
// matters in game: if Node cannot open what the mod sealed, the server's own
// tests and any future web client cannot either.
// Input: lines of {label, iv, ct, plainB64}
function verify() {
  const text = require('node:fs').readFileSync(0, 'utf8');
  let failures = 0;
  let n = 0;

  for (const line of text.split('\n')) {
    if (!line.trim()) continue;
    const r = JSON.parse(line);
    n++;

    try {
      validateEnvelope({ v: ENVELOPE_VERSION, iv: r.iv, ct: r.ct });
    } catch (err) {
      console.error(`FAIL ${r.label}: the mod produced an envelope the server would reject: ${err.message}`);
      failures++;
      continue;
    }

    let plain;
    try {
      plain = open({ iv: r.iv, ct: r.ct });
    } catch (err) {
      console.error(`FAIL ${r.label}: Node could not decrypt what C++ sealed: ${err.message}`);
      failures++;
      continue;
    }

    const expected = Buffer.from(r.plainB64, 'base64').toString('utf8');
    if (plain !== expected) {
      console.error(`FAIL ${r.label}: decrypted text differs from the original`);
      failures++;
    }
  }

  if (n === 0) {
    console.error('FAIL verify: no envelopes on stdin — did the C++ side run?');
    failures++;
  }

  console.error(`verify: ${n} C++ envelopes decrypted by Node, ${failures} failures`);
  if (failures) process.exit(1);
}

const mode = process.argv[2];
if (mode === 'emit') emit();
else if (mode === 'check') check();
else if (mode === 'verify') verify();
else {
  console.error('usage: node envelope_interop.js emit | check | verify');
  process.exit(2);
}
