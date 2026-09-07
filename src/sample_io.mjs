/*
 * Work — sample I/O shared by every UI that can put audio into the engine.
 *
 * ONE copy, imported by ui_overtake.js and ui_chain.js alike. It used to live
 * only in the overtake UI, which meant the chain builds could not load a
 * sample at all, and the alternative — pasting ~150 lines of WAV parsing and
 * base64 into ui_chain.js — is the "second copy that drifts" this project has
 * been bitten by four times (N_MACHINES, the palette map, the site's machine
 * table, the CC map).
 *
 * Imported RELATIVELY (`./sample_io.mjs`), and that is deliberate: the two
 * chain builds live in different module directories (audio_fx/work and
 * sound_generators/work-in), so an absolute path could only ever serve one.
 * schwung evaluates a UI module under the name "<path>#N" and QuickJS's default
 * normaliser resolves a relative specifier against everything up to the last
 * "/", so the "#N" suffix does not interfere. build.sh ships this file
 * alongside each ui_*.js.
 *
 * Everything here is pure or filesystem-only. Nothing touches UI state, and
 * the one engine write (sendSample) takes its setParam as an argument so it
 * works through whichever UI's shim is installed.
 */

import * as os from 'os';

/* ------------------------------------------------------------ where WAVs live */

/* VERIFIED ON THE DEVICE, 2026-07-29 — these are not guesses. The Move keeps
 * its library under UserLibrary, not at the top of UserData, and Schwung's own
 * resampler and skipback write into DATED SUBDIRECTORIES
 * (Samples/Schwung/Skipback/2026-07-28/...), so a flat scan of the base finds
 * nothing at all. Hence the recursive walk. */
export const SAMPLE_DIRS = [
    '/data/UserData/UserLibrary/Samples',
    '/data/UserData/UserLibrary/Recordings'
];
export const SAMPLE_SCAN_DEPTH = 4;     /* Samples/Schwung/Skipback/<date>/x.wav */
export const SAMPLE_LIMIT = 200;        /* a browser, not an archive */

/* Every .wav under `dir`, walking subdirectories.
 *
 * os.readdir returns a [names, errno] TUPLE and the listing includes "." and
 * ".." — treating it as a flat array of filenames is the bug that broke Mono's
 * preset browser. There is no stat binding to ask "is this a directory", so
 * the walk uses readdir itself: a successful listing means a directory, and
 * ENOTDIR on a file is simply how the recursion terminates. */
export function scanSampleDir(dir, depth, found) {
    if (depth <= 0 || found.length >= SAMPLE_LIMIT) return;
    let result;
    try { result = os.readdir(dir); } catch (e) { return; }
    if (!result || result[1] !== 0) return;

    const subdirs = [];
    for (const f of result[0]) {
        if (f === '.' || f === '..') continue;
        const full = `${dir}/${f}`;
        if (/\.wav$/i.test(f)) {
            if (found.length < SAMPLE_LIMIT) found.push(full);
        } else if (f.indexOf('.') < 0) {
            subdirs.push(full);         /* no extension — probably a directory */
        }
    }
    /* Files first, then descend, so the shallowest samples head the list. */
    for (const sub of subdirs) scanSampleDir(sub, depth - 1, found);
}

export function listSamples() {
    const found = [];
    for (const dir of SAMPLE_DIRS) scanSampleDir(dir, SAMPLE_SCAN_DEPTH, found);
    return found;
}

/* A display name from a path: the file name without its extension. */
export function sampleDisplayName(path) {
    return path.slice(path.lastIndexOf('/') + 1).replace(/\.wav$/i, '');
}

/* -------------------------------------------------------------------- base64 */

/* The transfer is chunked because one set_param carries at most a few KB
 * through the shim's param channel, serviced once per SPI frame. CHUNK_FRAMES
 * keeps each message inside that and spreads the cost over several frames
 * rather than one enormous blocking write. */
export const CHUNK_FRAMES = 8192;              /* 32 KB raw -> ~43 KB of base64 */

const B64_CHARS =
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const B64_LOOKUP = (() => {
    const t = new Array(256).fill(-1);
    for (let i = 0; i < B64_CHARS.length; i++) t[B64_CHARS.charCodeAt(i)] = i;
    return t;
})();

export function b64ToBytes(b64) {
    const out = [];
    let acc = 0, bits = 0;
    for (let i = 0; i < b64.length; i++) {
        const v = B64_LOOKUP[b64.charCodeAt(i)];
        if (v < 0) continue;                        /* '=' padding, newlines */
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push((acc >> bits) & 0xFF);
        }
    }
    return out;
}

export function bytesToB64(bytes, from, count) {
    let out = '';
    const end = from + count;
    for (let i = from; i < end; i += 3) {
        const b0 = bytes[i];
        const b1 = i + 1 < end ? bytes[i + 1] : 0;
        const b2 = i + 2 < end ? bytes[i + 2] : 0;
        const v = (b0 << 16) | (b1 << 8) | b2;
        out += B64_CHARS[(v >> 18) & 63];
        out += B64_CHARS[(v >> 12) & 63];
        out += (i + 1 < end) ? B64_CHARS[(v >> 6) & 63] : '=';
        out += (i + 2 < end) ? B64_CHARS[v & 63] : '=';
    }
    return out;
}

/* ----------------------------------------------------------------------- WAV */

function u32(b, at) {
    return (b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0;
}
function u16(b, at) { return b[at] | (b[at + 1] << 8); }

/* Walk the RIFF chunk list rather than assuming fmt/data sit at fixed offsets —
 * plenty of WAVs carry LIST/fact/cue chunks first, and a fixed offset reads
 * metadata as audio. Returns interleaved stereo Int16 bytes, or null. */
export function parseWav(bytes) {
    if (bytes.length < 44) return null;
    if (String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]) !== 'RIFF') return null;
    if (String.fromCharCode(bytes[8], bytes[9], bytes[10], bytes[11]) !== 'WAVE') return null;

    let at = 12, fmt = null, dataAt = -1, dataLen = 0;
    while (at + 8 <= bytes.length) {
        const id = String.fromCharCode(bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]);
        const size = u32(bytes, at + 4);
        const body = at + 8;
        if (id === 'fmt ' && size >= 16) {
            fmt = {
                format: u16(bytes, body),
                channels: u16(bytes, body + 2),
                rate: u32(bytes, body + 4),
                bits: u16(bytes, body + 14)
            };
            /* WAVE_FORMAT_EXTENSIBLE hides the real format in the GUID's
             * first two bytes. */
            if (fmt.format === 0xFFFE && size >= 40) fmt.format = u16(bytes, body + 24);
        } else if (id === 'data') {
            dataAt = body;
            dataLen = Math.min(size, bytes.length - body);
        }
        at = body + size + (size & 1);              /* chunks are word-aligned */
    }
    if (!fmt || dataAt < 0 || !fmt.channels) return null;

    const bytesPerSample = fmt.bits >> 3;
    if (!bytesPerSample) return null;
    const frames = Math.floor(dataLen / (bytesPerSample * fmt.channels));
    if (frames <= 0) return null;

    const read = (at2) => {
        if (fmt.format === 3) {                     /* IEEE float */
            if (fmt.bits === 32) {
                const v = u32(bytes, at2);
                const sign = (v >>> 31) ? -1 : 1;
                const exp = (v >>> 23) & 0xFF;
                const man = v & 0x7FFFFF;
                if (exp === 0) return 0;
                const f = sign * (1 + man / 0x800000) * Math.pow(2, exp - 127);
                return Math.max(-32768, Math.min(32767, Math.round(f * 32767)));
            }
            return 0;
        }
        if (fmt.bits === 8) return (bytes[at2] - 128) << 8;      /* 8-bit is unsigned */
        if (fmt.bits === 16) {
            const v = u16(bytes, at2);
            return v >= 0x8000 ? v - 0x10000 : v;
        }
        if (fmt.bits === 24) {
            let v = bytes[at2] | (bytes[at2 + 1] << 8) | (bytes[at2 + 2] << 16);
            if (v >= 0x800000) v -= 0x1000000;
            return v >> 8;
        }
        if (fmt.bits === 32) {
            const v = u32(bytes, at2) | 0;
            return v >> 16;
        }
        return 0;
    };

    const out = new Array(frames * 4);              /* 2 channels x int16 LE */
    for (let f = 0; f < frames; f++) {
        const base = dataAt + f * bytesPerSample * fmt.channels;
        const l = read(base);
        const r = fmt.channels > 1 ? read(base + bytesPerSample) : l;
        const o = f * 4;
        out[o]     = l & 0xFF;
        out[o + 1] = (l >> 8) & 0xFF;
        out[o + 2] = r & 0xFF;
        out[o + 3] = (r >> 8) & 0xFF;
    }
    return { bytes: out, frames, rate: fmt.rate, channels: fmt.channels };
}

/* ------------------------------------------------------------------ transfer */

/* Push a parsed WAV into the engine through `setParam`, which is whichever
 * UI's write function is calling — the overtake shim and the chain shim
 * prefix keys differently and this must not know which. Returns the number
 * of frames sent. */
export function sendSample(setParam, name, wav, maxFrames) {
    let frames = wav.frames;
    if (maxFrames > 0 && frames > maxFrames) frames = maxFrames;

    setParam('sample_begin', `${frames}:${name.slice(0, 24)}`);
    for (let at = 0; at < frames; at += CHUNK_FRAMES) {
        const n = Math.min(CHUNK_FRAMES, frames - at);
        setParam('sample_chunk', bytesToB64(wav.bytes, at * 4, n * 4));
    }
    setParam('sample_end', '1');
    return frames;
}
