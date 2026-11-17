// What a key keeps of OBS's last answer, so it has its image and name while
// OBS cannot be asked. No Stream Deck code here: the tests use it as it is.

import type { JsonValue } from "@elgato/utils";

/**
 * What a key last learned from OBS about its filter: enough for its image and
 * name. Also, where the image depends on them, the kind of source the key
 * acts on (`inputKind`, "dshow_input" ...) and, for a Scene Mode key, the
 * mode's name and type.
 */
export type Seen = {
	for: string;
	kind: string;
	enabled: boolean;
	settings: Record<string, JsonValue>;
	inputKind?: string;
	name?: string;
	type?: string;
};

export type Fresh = {
	kind: string;
	enabled?: boolean;
	settings?: Record<string, unknown>;
	inputKind?: string;
	name?: string;
	type?: string;
};

/** The filter settings a key's image or name can depend on. */
const LOOK_SETTINGS = ["blur_type", "style", "shadow", "shadow_type", "outline", "backdrop", "destination"];

/**
 * A key keeps what it last learned from OBS, in its own settings: while OBS
 * cannot answer (closed, still loading, the connection lost) the key goes on
 * showing its image and name, where it used to fall back to the plain ones.
 * Give it OBS's answer (or undefined for none) and what the key is for; it
 * returns what to show, or undefined when the key has never been told.
 */
export async function remembered(
	key: { setSettings(settings: Record<string, JsonValue | undefined>): Promise<void> },
	settings: Record<string, JsonValue | undefined>,
	target: string,
	fresh: Fresh | undefined,
): Promise<Seen | undefined> {
	const before = settings.seen as Seen | undefined;
	if (!fresh) return before && before.for === target ? before : undefined;
	const kept: Record<string, JsonValue> = {};
	for (const name of LOOK_SETTINGS) {
		const value = fresh.settings?.[name];
		if (typeof value === "number" || typeof value === "boolean" || typeof value === "string") kept[name] = value;
	}
	const now: Seen = { for: target, kind: fresh.kind, enabled: fresh.enabled === true, settings: kept };
	// (Left out while not known, so a key made before these were kept reads the same.)
	if (fresh.inputKind) now.inputKind = fresh.inputKind;
	if (fresh.name !== undefined) now.name = fresh.name;
	if (fresh.type !== undefined) now.type = fresh.type;
	if (JSON.stringify(now) !== JSON.stringify(before)) await key.setSettings({ ...settings, seen: now });
	return now;
}

