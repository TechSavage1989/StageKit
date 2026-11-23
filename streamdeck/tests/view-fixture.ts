import type { SceneController, ViewChoice, ViewGroup } from "../src/views.ts";

/** A native snapshot containing each supported group kind and a combined saved view. */
export function controllerSnapshot(): SceneController {
	const choice = (id: string, name: string, active = false, type = ""): ViewChoice => ({ id, name, type, active, available: true });
	const group = (id: string, kind: ViewGroup["kind"], choices: ViewChoice[]): ViewGroup => ({
		id, name: id, kind, enabled: true, available: true, reason: "", current: choices.find((entry) => entry.active)?.id ?? "",
		previous: "", allowNone: false, choices,
	});
	return {
		controllerId: "controller-a", name: "My controller", scene: "Live", sceneUuid: "scene-a", enabled: true, available: true, reason: "",
		groups: [
			group("mode-group", "modes", [choice("game", "Game", true, "game"), choice("art", "Art", false, "art_app")]),
			group("camera-group", "one", [choice("wide", "Wide", true), choice("close", "Close")]),
			group("overlay-group", "together", [choice("on", "On", true), choice("off", "Off")]),
			group("follow-group", "follow", []),
		],
		views: [{ ...choice("stream-view", "Game Live", true, "game"), reason: "" }, { ...choice("art-view", "Art Live", false, "art_app"), reason: "" }],
	};
}
