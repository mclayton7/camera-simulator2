// Copyright CamSim Contributors. All Rights Reserved.
//
// Minimal MPEG-TS KLV demuxer and raw UDP capture, so the conformance check
// sees exactly the bytes CamSim sent. ffmpeg is deliberately not used: up to
// at least 6.1 (Ubuntu 24.04's) its demuxer strips 5 bytes from every
// stream_id 0xFC KLV PES as if it carried a metadata AU cell header, even for
// stream_type 0x06, so an ffmpeg capture or extract loses part of the ST 0601
// key.

'use strict'

const dgram = require('dgram')

const TS_PACKET = 188
const SYNC = 0x47
const STREAM_TYPE_PRIVATE_DATA = 0x06
const STREAM_TYPE_METADATA = 0x15
const STREAM_ID_METADATA = 0xfc
const KLVA = Buffer.from('KLVA')

// Offset of the first of several consecutive sync bytes 188 apart, so a
// capture that starts mid-packet (or a stray 0x47) does not misalign.
function findSync(buf, from) {
	for (let i = from; i + TS_PACKET <= buf.length; i++) {
		if (buf[i] !== SYNC) continue
		let ok = true
		for (let k = 1; k < 3 && i + k * TS_PACKET < buf.length; k++) {
			if (buf[i + k * TS_PACKET] !== SYNC) { ok = false; break }
		}
		if (ok) return i
	}
	return -1
}

function* tsPackets(buf) {
	let i = findSync(buf, 0)
	while (i !== -1 && i + TS_PACKET <= buf.length) {
		if (buf[i] !== SYNC) {
			i = findSync(buf, i + 1)
			continue
		}
		const pkt = buf.subarray(i, i + TS_PACKET)
		i += TS_PACKET
		const afc = (pkt[3] >> 4) & 0x3
		if (!(afc & 0x1)) continue
		const start = afc & 0x2 ? 5 + pkt[4] : 4
		if (start > TS_PACKET) continue
		yield {
			pid: ((pkt[1] & 0x1f) << 8) | pkt[2],
			pusi: (pkt[1] & 0x40) !== 0,
			payload: pkt.subarray(start),
		}
	}
}

// The section in a PSI packet (single-packet sections only — PAT and PMT for
// one program always fit).
function psiSection(payload) {
	const pointer = payload[0]
	const section = payload.subarray(1 + pointer)
	const length = ((section[1] & 0x0f) << 8) | section[2]
	return section.subarray(0, Math.min(section.length, 3 + length))
}

function pmtPids(pat) {
	const pids = []
	for (let i = 8; i + 4 <= pat.length - 4; i += 4) {
		const program = (pat[i] << 8) | pat[i + 1]
		if (program !== 0) pids.push(((pat[i + 2] & 0x1f) << 8) | pat[i + 3])
	}
	return pids
}

// The KLV elementary stream in a PMT: stream_type 0x15, or 0x06 carrying a
// "KLVA" registration descriptor (MISB ST 1402).
function klvStream(pmt) {
	const infoLength = ((pmt[10] & 0x0f) << 8) | pmt[11]
	for (let i = 12 + infoLength; i + 5 <= pmt.length - 4;) {
		const streamType = pmt[i]
		const pid = ((pmt[i + 1] & 0x1f) << 8) | pmt[i + 2]
		const esInfoLength = ((pmt[i + 3] & 0x0f) << 8) | pmt[i + 4]
		const descriptors = pmt.subarray(i + 5, i + 5 + esInfoLength)
		if (streamType === STREAM_TYPE_METADATA
			|| (streamType === STREAM_TYPE_PRIVATE_DATA && descriptors.includes(KLVA))) {
			return { pid, streamType }
		}
		i += 5 + esInfoLength
	}
	return null
}

// The KLV elementary stream payloads of a transport stream, concatenated.
// Synchronous KLV (stream_type 0x15 in stream_id 0xFC) loses its 5-byte
// metadata AU cell header; everything else passes through untouched. A PES
// whose start was not captured is dropped.
function extractKlvFromTs(buf) {
	let pmts = null
	let klv = null
	const out = []
	let pes = null
	const flush = () => {
		if (!pes) return
		const data = Buffer.concat(pes)
		pes = null
		if (data.length < 9 || data[0] !== 0 || data[1] !== 0 || data[2] !== 1) return
		const streamId = data[3]
		let body = data.subarray(9 + data[8])
		const pesLength = (data[4] << 8) | data[5]
		if (pesLength) body = body.subarray(0, pesLength - 3 - data[8])
		if (klv.streamType === STREAM_TYPE_METADATA && streamId === STREAM_ID_METADATA) body = body.subarray(5)
		out.push(body)
	}
	for (const { pid, pusi, payload } of tsPackets(buf)) {
		if (pid === 0 && pusi && !pmts) {
			pmts = pmtPids(psiSection(payload))
		} else if (pmts && !klv && pusi && pmts.includes(pid)) {
			klv = klvStream(psiSection(payload))
		} else if (klv && pid === klv.pid) {
			if (pusi) {
				flush()
				pes = [payload]
			} else if (pes) {
				pes.push(payload)
			}
		}
	}
	if (!klv) throw new Error('no KLV stream in the transport stream (no PMT entry with stream_type 0x15 or a KLVA registration)')
	flush()
	return Buffer.concat(out)
}

// CamSim sends each keyframe as one burst of datagrams (NVENC: ~180 KB at
// 4 Mbit/s), which overflows Linux's default 208 KB receive buffer. The kernel
// caps the request at net.core.rmem_max, so raise that too:
//   sudo sysctl -w net.core.rmem_max=26214400
const RECV_BUFFER_BYTES = 16 * 1024 * 1024
const MIN_RECV_BUFFER_BYTES = 4 * 1024 * 1024

// Raw datagrams from udp://addr:port for durationSec seconds, concatenated.
// A multicast address is joined; anything else is bound as given.
function captureUdp(url, durationSec) {
	const m = /^udp:\/\/@?([^:/?]+):(\d+)/i.exec(url)
	if (!m) return Promise.reject(new Error(`bad UDP URL ${url}`))
	const [, addr, port] = m
	const first = Number(addr.split('.')[0])
	const multicast = first >= 224 && first <= 239
	return new Promise((resolve, reject) => {
		const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true })
		const chunks = []
		sock.on('message', msg => chunks.push(msg))
		sock.on('error', reject)
		sock.bind(Number(port), multicast ? undefined : addr, () => {
			if (multicast) sock.addMembership(addr)
			try {
				sock.setRecvBufferSize(RECV_BUFFER_BYTES)
			} catch {
				// Over the OS limit (macOS errors rather than capping); keep the default.
			}
			const granted = sock.getRecvBufferSize()
			if (granted < MIN_RECV_BUFFER_BYTES) {
				console.error(`warning: UDP receive buffer is ${granted} bytes; keyframe bursts may be dropped`
					+ ' (Linux: sudo sysctl -w net.core.rmem_max=26214400)')
			}
			setTimeout(() => {
				sock.close()
				resolve(Buffer.concat(chunks))
			}, durationSec * 1000)
		})
	})
}

module.exports = { extractKlvFromTs, captureUdp }
