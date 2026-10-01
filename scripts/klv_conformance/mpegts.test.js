// Copyright CamSim Contributors. All Rights Reserved.
//
// Tests for mpegts.js. Run: npm test (node --test).

'use strict'

const test = require('node:test')
const assert = require('node:assert')
const dgram = require('dgram')
const { extractKlvFromTs, captureUdp } = require('./mpegts')

const PMT_PID = 0x1000
const VIDEO_PID = 0x100
const KLV_PID = 0x101

// One 188-byte TS packet. A payload shorter than 184 bytes is padded with an
// adaptation field of 0xFF stuffing, as a muxer does.
function tsPacket(pid, payload, { pusi = false, cc = 0 } = {}) {
	const pkt = Buffer.alloc(188, 0xff)
	pkt[0] = 0x47
	pkt[1] = (pusi ? 0x40 : 0) | (pid >> 8)
	pkt[2] = pid & 0xff
	if (payload.length >= 184) {
		pkt[3] = 0x10 | (cc & 0x0f)
		payload.copy(pkt, 4, 0, 184)
		return pkt
	}
	pkt[3] = 0x30 | (cc & 0x0f)
	const afLen = 183 - payload.length
	pkt[4] = afLen
	if (afLen > 0) pkt[5] = 0x00
	payload.copy(pkt, 5 + afLen)
	return pkt
}

// A PSI section in its own TS packet (pointer_field 0). The CRC is not checked
// by the demuxer, so it is left zero.
function psiPacket(pid, tableId, body) {
	const sectionLength = 5 + body.length + 4
	const section = Buffer.concat([
		Buffer.from([tableId, 0xb0 | (sectionLength >> 8), sectionLength & 0xff, 0x00, 0x01, 0xc1, 0x00, 0x00]),
		body,
		Buffer.alloc(4),
	])
	return tsPacket(pid, Buffer.concat([Buffer.from([0x00]), section]), { pusi: true })
}

function pat() {
	return psiPacket(0x0000, 0x00, Buffer.from([0x00, 0x01, 0xe0 | (PMT_PID >> 8), PMT_PID & 0xff]))
}

function esEntry(streamType, pid, descriptors) {
	return Buffer.concat([
		Buffer.from([streamType, 0xe0 | (pid >> 8), pid & 0xff, 0xf0 | (descriptors.length >> 8), descriptors.length & 0xff]),
		descriptors,
	])
}

const KLVA_REGISTRATION = Buffer.from([0x05, 0x04, 0x4b, 0x4c, 0x56, 0x41])

function pmt(klvStreamType) {
	return psiPacket(PMT_PID, 0x02, Buffer.concat([
		Buffer.from([0xe0 | (VIDEO_PID >> 8), VIDEO_PID & 0xff, 0xf0, 0x00]),
		esEntry(0x1b, VIDEO_PID, Buffer.alloc(0)),
		esEntry(klvStreamType, KLV_PID, KLVA_REGISTRATION),
	]))
}

// PES with a PTS, split over as many TS packets as it needs.
function pesPackets(pid, streamId, payload, cc0 = 0) {
	const pts = Buffer.from([0x21, 0x00, 0x01, 0x00, 0x01])
	const len = 3 + pts.length + payload.length
	const pes = Buffer.concat([
		Buffer.from([0x00, 0x00, 0x01, streamId, len >> 8, len & 0xff, 0x84, 0x80, pts.length]),
		pts,
		payload,
	])
	const out = []
	for (let off = 0, cc = cc0; off < pes.length; off += 184, cc++) {
		out.push(tsPacket(pid, pes.subarray(off, off + 184), { pusi: off === 0, cc }))
	}
	return out
}

// A KLV packet: 16-byte ST 0601 key, BER length, value.
function klvPacket(fill, valueLength) {
	const key = Buffer.from('060e2b34020b01010e01030101000000', 'hex')
	const ber = valueLength < 128 ? Buffer.from([valueLength]) : Buffer.from([0x81, valueLength])
	return Buffer.concat([key, ber, Buffer.alloc(valueLength, fill)])
}

test('async KLV (stream_type 0x06, stream_id 0xFC): payload passes through whole', () => {
	const klv = [klvPacket(0xa1, 151), klvPacket(0xa2, 151)]
	const ts = Buffer.concat([
		pat(), pmt(0x06),
		...pesPackets(VIDEO_PID, 0xe0, Buffer.alloc(300, 0x00)),
		...pesPackets(KLV_PID, 0xfc, klv[0], 0),
		...pesPackets(KLV_PID, 0xfc, klv[1], 1),
	])
	assert.deepStrictEqual(extractKlvFromTs(ts), Buffer.concat(klv))
})

test('sync KLV (stream_type 0x15): the 5-byte metadata AU cell header is removed', () => {
	const klv = klvPacket(0xb1, 100)
	const auHeader = Buffer.from([0x00, 0x00, 0xdf, klv.length >> 8, klv.length & 0xff])
	const ts = Buffer.concat([
		pat(), pmt(0x15),
		...pesPackets(KLV_PID, 0xfc, Buffer.concat([auHeader, klv])),
	])
	assert.deepStrictEqual(extractKlvFromTs(ts), klv)
})

test('a KLV PES spanning several TS packets is reassembled', () => {
	const klv = klvPacket(0xc1, 250)
	const pes = pesPackets(KLV_PID, 0xfc, klv)
	assert.ok(pes.length > 1, 'fixture should span several packets')
	const ts = Buffer.concat([pat(), pmt(0x06), ...pes])
	assert.deepStrictEqual(extractKlvFromTs(ts), klv)
})

test('a capture that starts mid-packet resynchronises', () => {
	const klv = klvPacket(0xd1, 60)
	const ts = Buffer.concat([Buffer.from([0x12, 0x34, 0x47, 0x00]), pat(), pmt(0x06), ...pesPackets(KLV_PID, 0xfc, klv)])
	assert.deepStrictEqual(extractKlvFromTs(ts), klv)
})

test('a PES whose start was not captured is dropped, not emitted partially', () => {
	const klv = [klvPacket(0xe1, 250), klvPacket(0xe2, 40)]
	const first = pesPackets(KLV_PID, 0xfc, klv[0])
	const ts = Buffer.concat([pat(), pmt(0x06), ...first.slice(1), ...pesPackets(KLV_PID, 0xfc, klv[1], first.length)])
	assert.deepStrictEqual(extractKlvFromTs(ts), klv[1])
})

test('a stream without a KLV PID is an error', () => {
	const ts = Buffer.concat([pat(), psiPacket(PMT_PID, 0x02, Buffer.concat([
		Buffer.from([0xe0 | (VIDEO_PID >> 8), VIDEO_PID & 0xff, 0xf0, 0x00]),
		esEntry(0x1b, VIDEO_PID, Buffer.alloc(0)),
	]))])
	assert.throws(() => extractKlvFromTs(ts), /no KLV stream/)
})

test('captureUdp returns the datagrams received, in order', async () => {
	const sender = dgram.createSocket('udp4')
	const port = 20000 + Math.floor(Math.random() * 20000)
	const capture = captureUdp(`udp://127.0.0.1:${port}`, 1)
	const datagrams = [Buffer.alloc(1316, 1), Buffer.alloc(1316, 2), Buffer.alloc(188, 3)]
	setTimeout(() => {
		for (const d of datagrams) sender.send(d, port, '127.0.0.1')
	}, 200)
	const got = await capture
	sender.close()
	assert.deepStrictEqual(got, Buffer.concat(datagrams))
})
