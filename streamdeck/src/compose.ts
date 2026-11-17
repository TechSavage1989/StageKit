// A key's image made of two or three pictures: the picture of its type with
// what the key does drawn as a badge in a corner (tools/make-badges.py). The
// owner: "no types selection for manual window/source? all keys must have the
// options". The badges and the type pictures are drawn beforehand; a key's
// combination of them is made here when the key is shown, as a PNG data URL,
// which Stream Deck's setImage takes as it takes a file.
//
// Plain Node (node:fs, node:zlib), no native modules and no Stream Deck code:
// the tests use it as it is. Only 8-bit PNGs without interlacing are read,
// which is what the pack's tools write.

import { existsSync, readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { deflateSync, inflateSync } from "node:zlib";

import badges from "./badges.json" with { type: "json" };

export const SIZE = 144;
export const BADGE_W = badges.width;
export const BADGE_H = badges.height;
export const BADGES: string[] = badges.badges;
/** Where a badge goes on a 144 px key: the function top right, a move's extra option top left (the on/off mark's corner on other keys). */
export const BADGE_AT = { right: { x: SIZE - BADGE_W - 2, y: 2 }, left: { x: 2, y: 2 } } as const;

/** An image as RGBA bytes, row after row. */
export type Raster = { width: number; height: number; data: Uint8Array };

/** The plugin's folder: next to the bundle (bin/plugin.js), or beside src/ when run from the sources (the tests). */
function findRoot(): string {
	const here = path.dirname(fileURLToPath(import.meta.url));
	for (const dir of [path.resolve(here, ".."), path.resolve(here, "../com.techsavage1989.stagekit.sdPlugin")])
		if (existsSync(path.join(dir, "imgs", "pack"))) return dir;
	return path.resolve(here, "..");
}
let root: string | undefined;
export function pluginRoot(): string {
	return (root ??= findRoot());
}

const SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

/** Reads a PNG (8-bit or less per sample, not interlaced) into RGBA. */
export function decodePng(bytes: Uint8Array): Raster {
	const buf = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	if (!buf.subarray(0, 8).equals(SIGNATURE)) throw new Error("not a PNG");
	let at = 8;
	let width = 0,
		height = 0,
		depth = 0,
		type = 0,
		interlace = 0;
	let palette: Buffer | undefined;
	let alpha: Buffer | undefined;
	const idat: Buffer[] = [];
	while (at < buf.length) {
		const length = buf.readUInt32BE(at);
		const name = buf.toString("latin1", at + 4, at + 8);
		const body = buf.subarray(at + 8, at + 8 + length);
		if (name === "IHDR") {
			width = body.readUInt32BE(0);
			height = body.readUInt32BE(4);
			depth = body[8];
			type = body[9];
			interlace = body[12];
		} else if (name === "PLTE") palette = body;
		else if (name === "tRNS") alpha = body;
		else if (name === "IDAT") idat.push(body);
		else if (name === "IEND") break;
		at += 12 + length;
	}
	if (depth > 8 || interlace !== 0) throw new Error(`PNG not read: ${depth}-bit, interlace ${interlace}`);
	const channels = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[type as 0 | 2 | 3 | 4 | 6];
	if (!channels) throw new Error(`PNG colour type ${type}`);
	const bits = channels * depth;
	const stride = Math.ceil((width * bits) / 8);
	const step = Math.max(1, bits >> 3);
	const raw = inflateSync(Buffer.concat(idat));
	const rows = new Uint8Array(stride * height);
	let prev = new Uint8Array(stride);
	for (let y = 0; y < height; y++) {
		const filter = raw[y * (stride + 1)];
		const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
		const out = rows.subarray(y * stride, (y + 1) * stride);
		for (let i = 0; i < stride; i++) {
			const a = i >= step ? out[i - step] : 0;
			const b = prev[i];
			const c = i >= step ? prev[i - step] : 0;
			let v = line[i];
			if (filter === 1) v += a;
			else if (filter === 2) v += b;
			else if (filter === 3) v += (a + b) >> 1;
			else if (filter === 4) {
				const p = a + b - c;
				const pa = Math.abs(p - a),
					pb = Math.abs(p - b),
					pc = Math.abs(p - c);
				v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
			}
			out[i] = v & 0xff;
		}
		prev = out;
	}
	const data = new Uint8Array(width * height * 4);
	const sample = (row: Uint8Array, index: number): number => {
		if (depth === 8) return row[index];
		const bit = index * depth;
		return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1);
	};
	const maxv = (1 << depth) - 1;
	for (let y = 0; y < height; y++) {
		const row = rows.subarray(y * stride, (y + 1) * stride);
		for (let x = 0; x < width; x++) {
			const o = (y * width + x) * 4;
			if (type === 3) {
				const i = sample(row, x);
				data[o] = palette![i * 3];
				data[o + 1] = palette![i * 3 + 1];
				data[o + 2] = palette![i * 3 + 2];
				data[o + 3] = alpha && i < alpha.length ? alpha[i] : 255;
			} else if (type === 0 || type === 4) {
				const g = Math.round((sample(row, x * channels) * 255) / maxv);
				data[o] = data[o + 1] = data[o + 2] = g;
				data[o + 3] = type === 4 ? row[x * 2 + 1] : 255;
			} else {
				data[o] = row[x * channels];
				data[o + 1] = row[x * channels + 1];
				data[o + 2] = row[x * channels + 2];
				data[o + 3] = type === 6 ? row[x * 4 + 3] : 255;
			}
		}
	}
	return { width, height, data };
}

const CRC = (() => {
	const table = new Uint32Array(256);
	for (let n = 0; n < 256; n++) {
		let c = n;
		for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
		table[n] = c >>> 0;
	}
	return table;
})();

function chunk(name: string, body: Buffer): Buffer {
	const head = Buffer.alloc(8);
	head.writeUInt32BE(body.length, 0);
	head.write(name, 4, "latin1");
	let c = 0xffffffff;
	for (const b of head.subarray(4)) c = CRC[(c ^ b) & 0xff] ^ (c >>> 8);
	for (const b of body) c = CRC[(c ^ b) & 0xff] ^ (c >>> 8);
	const tail = Buffer.alloc(4);
	tail.writeUInt32BE((c ^ 0xffffffff) >>> 0, 0);
	return Buffer.concat([head, body, tail]);
}

/** Writes RGBA as a PNG. */
export function encodePng(image: Raster): Buffer {
	const ihdr = Buffer.alloc(13);
	ihdr.writeUInt32BE(image.width, 0);
	ihdr.writeUInt32BE(image.height, 4);
	ihdr[8] = 8;
	ihdr[9] = 6;
	const stride = image.width * 4;
	const raw = Buffer.alloc((stride + 1) * image.height);
	for (let y = 0; y < image.height; y++) {
		// (Filter "sub": the badges' flat colours pack well with it.)
		raw[y * (stride + 1)] = 1;
		for (let i = 0; i < stride; i++) {
			const v = image.data[y * stride + i];
			raw[y * (stride + 1) + 1 + i] = (v - (i >= 4 ? image.data[y * stride + i - 4] : 0)) & 0xff;
		}
	}
	return Buffer.concat([SIGNATURE, chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw, { level: 9 })), chunk("IEND", Buffer.alloc(0))]);
}

/** Lays `top` over `under` at x, y ("over", straight alpha). */
export function over(under: Raster, top: Raster, x: number, y: number): void {
	for (let ty = 0; ty < top.height; ty++) {
		const uy = y + ty;
		if (uy < 0 || uy >= under.height) continue;
		for (let tx = 0; tx < top.width; tx++) {
			const ux = x + tx;
			if (ux < 0 || ux >= under.width) continue;
			const t = (ty * top.width + tx) * 4;
			const u = (uy * under.width + ux) * 4;
			const ta = top.data[t + 3] / 255;
			if (ta === 0) continue;
			const ua = under.data[u + 3] / 255;
			const oa = ta + ua * (1 - ta);
			for (let c = 0; c < 3; c++) under.data[u + c] = Math.round((top.data[t + c] * ta + under.data[u + c] * ua * (1 - ta)) / oa);
			under.data[u + 3] = Math.round(oa * 255);
		}
	}
}

/** A file of the plugin's, read once. */
const files = new Map<string, Raster>();
function read(file: string): Raster {
	let image = files.get(file);
	if (!image) {
		image = decodePng(readFileSync(path.join(pluginRoot(), file)));
		files.set(file, image);
	}
	return image;
}

/** A badge's file, inside the plugin's folder, in a palette. */
export function badgeFile(name: string, palette: string): string {
	return `imgs/pack/${palette}/badges/${name}.png`;
}

/** What a key shows: a picture's file, and the badges laid over it. */
export type Layers = {
	base: string;
	palette: string;
	right?: string;
	left?: string;
	/** A picture laid, made smaller, into the base's panel (a key's Layout, src/pictures.ts `laidOut`): its file, the panel and the part of the picture taken. */
	inset?: { file: string; panel: number[]; crop: number[] };
	/** The on/off mark put on the whole (tools/make-state-icons.py's, for a base that has no marked copy). */
	mark?: boolean;
};

/** The two marks by themselves, 144 px, see-through round them (tools/make-state-icons.py). */
export function markFile(on: boolean): string {
	return `imgs/marks/${on ? "on" : "off"}.png`;
}

/** How tools/make-state-icons.py makes a picture "off": most of its brightness, half its colour. */
const OFF_BRIGHTNESS = 0.95;
const OFF_COLOUR = 0.5;

function switchedOff(image: Raster): void {
	const d = image.data;
	for (let i = 0; i < d.length; i += 4) {
		const r = d[i] * OFF_BRIGHTNESS,
			g = d[i + 1] * OFF_BRIGHTNESS,
			b = d[i + 2] * OFF_BRIGHTNESS;
		const grey = r * 0.299 + g * 0.587 + b * 0.114;
		d[i] = Math.round(grey + (r - grey) * OFF_COLOUR);
		d[i + 1] = Math.round(grey + (g - grey) * OFF_COLOUR);
		d[i + 2] = Math.round(grey + (b - grey) * OFF_COLOUR);
	}
}

/**
 * Part of an image (`crop`: x0, y0, x1, y1, the end not included) made
 * smaller to w x h, each new pixel the mean of what it covers (with its
 * see-through parts weighed by how see-through they are).
 */
export function shrink(image: Raster, crop: number[], w: number, h: number): Raster {
	const [cx0, cy0, cx1, cy1] = crop;
	const sx = (cx1 - cx0) / w;
	const sy = (cy1 - cy0) / h;
	const out = new Uint8Array(w * h * 4);
	for (let y = 0; y < h; y++) {
		const fy0 = cy0 + y * sy,
			fy1 = fy0 + sy;
		for (let x = 0; x < w; x++) {
			const fx0 = cx0 + x * sx,
				fx1 = fx0 + sx;
			let r = 0,
				g = 0,
				b = 0,
				a = 0,
				area = 0;
			for (let py = Math.floor(fy0); py < Math.ceil(fy1); py++) {
				const wy = Math.min(fy1, py + 1) - Math.max(fy0, py);
				if (wy <= 0 || py < 0 || py >= image.height) continue;
				for (let px = Math.floor(fx0); px < Math.ceil(fx1); px++) {
					const wx = Math.min(fx1, px + 1) - Math.max(fx0, px);
					if (wx <= 0 || px < 0 || px >= image.width) continue;
					const weight = wx * wy;
					const o = (py * image.width + px) * 4;
					const pa = (image.data[o + 3] / 255) * weight;
					r += image.data[o] * pa;
					g += image.data[o + 1] * pa;
					b += image.data[o + 2] * pa;
					a += pa;
					area += weight;
				}
			}
			const o = (y * w + x) * 4;
			if (a > 0) {
				out[o] = Math.round(r / a);
				out[o + 1] = Math.round(g / a);
				out[o + 2] = Math.round(b / a);
			}
			out[o + 3] = area > 0 ? Math.round((a / area) * 255) : 0;
		}
	}
	return { width: w, height: h, data: out };
}

/** Lays `inset`'s picture into the panel of `out` (a 144 px layout), as large as fits with a margin, in the middle. */
function laidIn(out: Raster, inset: NonNullable<Layers["inset"]>): void {
	const picture = read(inset.file);
	const k = out.width / SIZE;
	const pk = picture.width / SIZE;
	const crop = inset.crop.map((v) => v * pk);
	const [x0, y0, x1, y1] = inset.panel.map((v) => v * k);
	const margin = 4 * k;
	const room = { w: x1 - x0 - 2 * margin, h: y1 - y0 - 2 * margin };
	const scale = Math.min(room.w / (crop[2] - crop[0]), room.h / (crop[3] - crop[1]));
	const w = Math.max(1, Math.round((crop[2] - crop[0]) * scale));
	const h = Math.max(1, Math.round((crop[3] - crop[1]) * scale));
	over(out, shrink(picture, crop, w, h), Math.round(x0 + (x1 - x0 - w) / 2), Math.round(y0 + (y1 - y0 - h) / 2));
}

const made = new Map<string, string>();

/**
 * The key's image: the base file as it is (a path, as before) when it has
 * no badge, else the base with its badges as a PNG data URL. Made once for
 * each combination.
 */
export function composed(layers: Layers): string {
	if (!layers.right && !layers.left && !layers.inset && layers.mark === undefined) return layers.base;
	const id = `${layers.base}|${layers.palette}|${layers.right ?? ""}|${layers.left ?? ""}|${layers.inset?.file ?? ""}|${layers.mark ?? ""}`;
	let url = made.get(id);
	if (url) return url;
	const base = read(layers.base);
	// (A picture of another size is drawn at 144 by Stream Deck; badges are placed for 144.)
	const out: Raster = { width: base.width, height: base.height, data: new Uint8Array(base.data) };
	const k = base.width / SIZE;
	if (layers.inset) laidIn(out, layers.inset);
	if (layers.mark !== undefined) {
		// (As tools/make-state-icons.py marks a picture: off is greyed a little first.)
		if (!layers.mark) switchedOff(out);
		const mark = read(markFile(layers.mark));
		over(out, mark.width === out.width ? mark : shrink(mark, [0, 0, mark.width, mark.height], out.width, out.height), 0, 0);
	}
	for (const side of ["right", "left"] as const) {
		const name = layers[side];
		if (!name) continue;
		const at = BADGE_AT[side];
		over(out, read(badgeFile(name, layers.palette)), Math.round(at.x * k), Math.round(at.y * k));
	}
	url = `data:image/png;base64,${encodePng(out).toString("base64")}`;
	if (made.size > 500) made.clear();
	made.set(id, url);
	return url;
}
