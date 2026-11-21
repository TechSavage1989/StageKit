export type SceneItemTarget = { collection: string; parent: string; id: number; name: string; group: boolean };

export function decodeItem(raw: string): SceneItemTarget | undefined {
	try {
		const values: unknown = JSON.parse(raw);
		if (!Array.isArray(values) || values.length !== 5) return undefined;
		const [collection, parent, id, name, group] = values as unknown[];
		if (typeof collection !== "string" || !collection || typeof parent !== "string" || !parent ||
			typeof id !== "number" || !Number.isSafeInteger(id) || id < 0 || typeof name !== "string" || !name ||
			typeof group !== "boolean") return undefined;
		return { collection, parent, id, name, group };
	} catch {
		return undefined;
	}
}
