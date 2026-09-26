#!/usr/bin/env node
// Copyright CamSim Contributors. All Rights Reserved.
//
// KLV conformance check against misb.js — the decoder that parses CamSim's KLV
// downstream, and therefore the definition of "correct" for this project.
//
//   node check.js packets <packets.jsonl>
//       Decodes packets exported by the CamSim.KlvConformance.ExportPackets
//       automation test and compares every tag with the input telemetry.
//
//   node check.js stream <file.ts | klv.bin | udp://addr:port> [--max-age-sec N]
//                        [--min-packets N] [--duration-sec N]
//                        [--expect-position LAT,LON,ALT]
//       Extracts the KLV data stream (via ffmpeg for .ts and udp://, which is
//       captured for --duration-sec, default 5) and checks every
//       ST 0601 packet: parses, checksum valid, no unknown tags, and the
//       timestamp (tag 2) is UTC within N seconds of now. With
//       --expect-position, the sensor position (tags 13/14/15) must match the
//       pose the CIGI host commanded, which proves the camera follows the host.
//
// Exit code 0 = conformant, 1 = failures, 2 = usage / IO error.

'use strict'

const fs = require('fs')
const { execFileSync } = require('child_process')
const { st0601 } = require('@vidterra/misb.js')

const ST0601_KEY = st0601.key

// Tags CamSim emits. Anything else in a decoded packet is a failure.
const KNOWN_TAGS = new Set([1, 2, 4, 5, 6, 7, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
	23, 24, 25, 43, 44, 47, 48, 56, 65])

// One least-significant-bit of each fixed-point mapping, used as tolerance.
const LSB = {
	heading: 360 / 65535,
	pitch: 40 / 65534,
	roll: 100 / 65534,
	lat: 180 / 4294967294,
	lon: 360 / 4294967294,
	alt: 19900 / 65535,
	fov: 180 / 65535,
	az: 360 / 4294967295,
	elev: 360 / 4294967294,
	range: 5000000 / 4294967295,
}

let failures = 0
const fail = (ctx, msg) => {
	failures++
	console.error(`FAIL [${ctx}] ${msg}`)
}

// misb.js logs checksum problems through console.debug; capture them so they
// are reported against the packet instead of interleaved on stdout.
function parseQuietly(buf) {
	const logs = []
	const original = console.debug
	console.debug = (...args) => logs.push(args.join(' '))
	try {
		return { values: st0601.parse(buf), logs }
	} finally {
		console.debug = original
	}
}

function checkCommon(ctx, buf) {
	let parsed
	try {
		parsed = parseQuietly(buf)
	} catch (e) {
		fail(ctx, `st0601.parse threw ${e.name}: ${e.message}`)
		return null
	}
	const values = parsed.values
	const byKey = new Map(values.map(v => [v.key, v]))

	if (values[0]?.key !== 2) fail(ctx, `first tag is ${values[0]?.key}, expected 2 (timestamp)`)
	if (values[values.length - 1]?.key !== 1) fail(ctx, 'last tag is not 1 (checksum)')
	const checksum = byKey.get(1)
	if (!checksum || checksum.valid === false) {
		fail(ctx, `checksum invalid (${parsed.logs.join('; ') || 'missing tag 1'})`)
	}
	for (const v of values) {
		if (!KNOWN_TAGS.has(v.key) || v.name === 'Unknown' || v.value === 'Not Implemented') {
			fail(ctx, `unexpected tag ${v.key} (${v.name})`)
		}
	}
	return byKey
}

function near(ctx, byKey, key, expected, tol, { wrap = 0 } = {}) {
	const item = byKey.get(key)
	if (!item) return fail(ctx, `tag ${key} missing`)
	let diff = Math.abs(item.value - expected)
	if (wrap) diff = Math.min(diff, Math.abs(diff - wrap))
	if (!(diff <= tol)) {
		fail(ctx, `tag ${key} (${item.name}) decoded ${item.value}, expected ${expected} ±${tol}`)
	}
}

function equal(ctx, byKey, key, expected) {
	const item = byKey.get(key)
	if (!item) return fail(ctx, `tag ${key} missing`)
	if (item.value !== expected) {
		fail(ctx, `tag ${key} (${item.name}) decoded ${JSON.stringify(item.value)}, expected ${JSON.stringify(expected)}`)
	}
}

function absent(ctx, byKey, key, why) {
	if (byKey.has(key)) fail(ctx, `tag ${key} present but ${why}`)
}

function checkPackets(path) {
	const lines = fs.readFileSync(path, 'utf8').split(/\r?\n/).filter(l => l.trim())
	if (lines.length === 0) {
		console.error(`no packets in ${path}`)
		process.exit(2)
	}
	for (const line of lines) {
		const c = JSON.parse(line.replace(/^﻿/, ''))
		const t = c.telemetry
		const ctx = c.name
		const byKey = checkCommon(ctx, Buffer.from(c.hex, 'hex'))
		if (!byKey) continue

		equal(ctx, byKey, 2, t.timestampUs)
		if (c.tail) equal(ctx, byKey, 4, c.tail)
		else absent(ctx, byKey, 4, 'no tail number configured')
		near(ctx, byKey, 5, ((t.yaw % 360) + 360) % 360, 1.5 * LSB.heading, { wrap: 360 })
		near(ctx, byKey, 6, t.pitch, 1.5 * LSB.pitch)
		near(ctx, byKey, 7, t.roll, 1.5 * LSB.roll)
		equal(ctx, byKey, 11, ['EO Nose', 'LWIR', 'NVG'][t.sensorMode])
		equal(ctx, byKey, 12, 'Geodetic WGS84')
		near(ctx, byKey, 13, t.lat, 1.5 * LSB.lat)
		near(ctx, byKey, 14, t.lon, 1.5 * LSB.lon)
		near(ctx, byKey, 15, t.alt, 1.5 * LSB.alt)
		near(ctx, byKey, 16, t.hfov, 1.5 * LSB.fov)
		near(ctx, byKey, 17, t.vfov, 1.5 * LSB.fov)
		near(ctx, byKey, 18, ((t.gimbalYaw % 360) + 360) % 360, 1.5 * LSB.az, { wrap: 360 })
		near(ctx, byKey, 19, t.gimbalPitch, 1.5 * LSB.elev)
		near(ctx, byKey, 20, ((t.gimbalRoll % 360) + 360) % 360, 1.5 * LSB.az, { wrap: 360 })

		if (t.slantRange > 0) {
			near(ctx, byKey, 21, t.slantRange, 1.5 * LSB.range)
			near(ctx, byKey, 23, t.fcLat, 1.5 * LSB.lat)
			near(ctx, byKey, 24, t.fcLon, 1.5 * LSB.lon)
			near(ctx, byKey, 25, t.fcElev, 1.5 * LSB.alt)
		} else {
			absent(ctx, byKey, 21, 'slant range is 0')
			for (const key of [23, 24, 25]) absent(ctx, byKey, key, 'no ground intersection')
		}

		const [gateW, gateH] = c.gate
		if (gateW > 0) near(ctx, byKey, 43, gateW, 2)
		else absent(ctx, byKey, 43, 'no track gate configured')
		if (gateH > 0) near(ctx, byKey, 44, gateH, 2)
		else absent(ctx, byKey, 44, 'no track gate configured')

		// Bit 3 (0x04) = IR black-hot. Nothing else is set: slant range is
		// calculated (bit 5 = 0) and CamSim never flags icing or invalid images.
		equal(ctx, byKey, 47, t.polarity === 1 ? 0x04 : 0x00)

		const security = byKey.get(48)
		if (security) {
			const inner = new Map(security.value.map(v => [v.key, v.value]))
			if (inner.get(12) !== 'ISO-3166 Two Letter') {
				fail(ctx, `ST 0102 tag 12 decoded ${JSON.stringify(inner.get(12))}, expected "ISO-3166 Two Letter"`)
			}
			if (!inner.has(13) || !/^[A-Z]{2}( [A-Z]{2})*$/.test(inner.get(13))) {
				fail(ctx, `ST 0102 tag 13 decoded ${JSON.stringify(inner.get(13))}, expected country codes`)
			}
		}

		if (t.groundSpeed > 0) near(ctx, byKey, 56, t.groundSpeed, 0.5)
		else absent(ctx, byKey, 56, 'ground speed is 0')
		absent(ctx, byKey, 8, 'tag 8 is true airspeed, which CamSim does not model')

		equal(ctx, byKey, 65, 9)
	}
	return lines.length
}

function extractKlv(path, durationSec) {
	const ffmpegArgs = ['-map', '0:d:0', '-c', 'copy', '-f', 'data', '-']
	if (/^udp:\/\//i.test(path)) {
		return execFileSync('ffmpeg', ['-v', 'error', '-i', path, '-t', String(durationSec), ...ffmpegArgs],
			{ maxBuffer: 1 << 30, timeout: (durationSec + 30) * 1000 })
	}
	if (!/\.(ts|m2ts|mts)$/i.test(path)) return fs.readFileSync(path)
	return execFileSync('ffmpeg', ['-v', 'error', '-i', path, ...ffmpegArgs], { maxBuffer: 1 << 30 })
}

function berLength(buf, pos) {
	const first = buf[pos]
	if (first === undefined) return null
	if (!(first & 0x80)) return { header: 1, length: first }
	const n = first & 0x7f
	if (n === 0 || n > 4 || pos + 1 + n > buf.length) return null
	let length = 0
	for (let i = 0; i < n; i++) length = length * 256 + buf[pos + 1 + i]
	return { header: 1 + n, length }
}

function checkStream(path, maxAgeSec, minPackets, durationSec, expectPosition) {
	const data = extractKlv(path, durationSec)
	const nowUs = Date.now() * 1000
	let count = 0
	let prevTs = null
	let pos = data.indexOf(ST0601_KEY)
	while (pos !== -1) {
		const ber = berLength(data, pos + 16)
		const end = ber ? pos + 16 + ber.header + ber.length : -1
		const ctx = `packet ${count} @${pos}`
		if (!ber || end > data.length) {
			fail(ctx, 'truncated packet')
			break
		}
		const byKey = checkCommon(ctx, data.subarray(pos, end))
		const ts = byKey?.get(2)?.value
		if (ts !== undefined) {
			const ageSec = (nowUs - ts) / 1e6
			if (!(ageSec >= -5 && ageSec <= maxAgeSec)) {
				fail(ctx, `timestamp ${new Date(ts / 1000).toISOString()} is not UTC within ${maxAgeSec}s of now`)
			}
			if (prevTs !== null && ts < prevTs) fail(ctx, 'timestamp went backwards')
			prevTs = ts
		}
		if (byKey && expectPosition) {
			const [lat, lon, alt] = expectPosition
			near(ctx, byKey, 13, lat, 1e-4)
			near(ctx, byKey, 14, lon, 1e-4)
			near(ctx, byKey, 15, alt, 5)
		}
		count++
		pos = data.indexOf(ST0601_KEY, end)
	}
	if (count < minPackets) fail(path, `found ${count} ST 0601 packets, expected at least ${minPackets}`)
	return count
}

function main() {
	const [mode, path, ...rest] = process.argv.slice(2)
	const optStr = name => {
		const i = rest.indexOf(name)
		return i === -1 ? undefined : rest[i + 1]
	}
	const opt = (name, dflt) => optStr(name) === undefined ? dflt : Number(optStr(name))
	const position = optStr('--expect-position')?.split(',').map(Number)
	if (position && (position.length !== 3 || position.some(Number.isNaN))) {
		console.error('--expect-position takes LAT,LON,ALT')
		process.exit(2)
	}
	if (!path || !['packets', 'stream'].includes(mode)) {
		console.error('usage: check.js packets <packets.jsonl>')
		console.error('       check.js stream <file.ts|klv.bin|udp://addr:port> [--max-age-sec N] [--min-packets N] [--duration-sec N] [--expect-position LAT,LON,ALT]')
		process.exit(2)
	}
	let count
	try {
		count = mode === 'packets'
			? checkPackets(path)
			: checkStream(path, opt('--max-age-sec', 3600), opt('--min-packets', 1), opt('--duration-sec', 5), position)
	} catch (e) {
		console.error(`error: ${e.message}`)
		process.exit(2)
	}
	const version = require('@vidterra/misb.js/package.json').version
	if (failures) {
		console.error(`${failures} failure(s) across ${count} packet(s) (misb.js ${version})`)
		process.exit(1)
	}
	console.log(`OK: ${count} packet(s) conform to misb.js ${version}`)
}

main()
