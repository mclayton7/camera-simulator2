// Copyright CamSim Contributors. All Rights Reserved.
//
// Tests for check.js decode. Run: npm test (node --test).

'use strict'

const test = require('node:test')
const assert = require('node:assert')
const { st0601 } = require('@vidterra/misb.js')
const { decodePackets } = require('./check')

function int32(v) { const b = Buffer.alloc(4); b.writeInt32BE(v); return b }
function uint16(v) { const b = Buffer.alloc(2); b.writeUInt16BE(v); return b }

// One ST 0601 packet: Tag 2 (timestamp), 13 / 14 (sensor lat / lon), 75 (sensor HAE), 82 (corner 1 lat), checksum.
function packet(lat, lon, hae, cornerLat) {
	const lat32 = Math.round(lat / 90 * 2147483647)
	const lon32 = Math.round(lon / 180 * 2147483647)
	const hae16 = Math.round((hae + 900) / 19900 * 65535)
	const c32 = Math.round(cornerLat / 90 * 2147483647)
	return Buffer.from(st0601.encode([
		{ key: 2, value: 1760000000000000n },
		{ key: 13, packet: int32(lat32) },
		{ key: 14, packet: int32(lon32) },
		{ key: 75, packet: uint16(hae16) },
		{ key: 82, packet: int32(c32) },
	]), 'hex')
}

test('decodePackets returns every ST 0601 packet with its tag values', () => {
	const buf = Buffer.concat([packet(33.3, -117.45, 412.5, 33.31), Buffer.from('00ff', 'hex'), packet(33.4, -117.4, 100, 33.41)])
	const got = decodePackets(buf)
	assert.strictEqual(got.length, 2)
	assert.ok(Math.abs(got[0].tags[13] - 33.3) < 1e-7)
	assert.ok(Math.abs(got[0].tags[14] + 117.45) < 1e-7)
	assert.ok(Math.abs(got[0].tags[75] - 412.5) < 0.31)
	assert.ok(Math.abs(got[0].tags[82] - 33.31) < 1e-7)
	assert.ok(Math.abs(got[1].tags[13] - 33.4) < 1e-7)
	assert.strictEqual(got[0].tags[2], 1760000000000000)
})
