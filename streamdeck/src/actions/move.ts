import {
	action,
	type KeyDownEvent,
	type SendToPluginEvent,
	SingletonAction,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { isMove } from "../kinds.ts";
import { obs } from "../obs.ts";
import { answerLists, type KeySettings } from "./shared.ts";

/**
 * A key that runs one StageKit move: a Send To key on the camera, a Place
 * Item, a Layout or a Picture Shape key. Those run when they are switched on
 * and switch themselves off again.
 */
@action({ UUID: "com.techsavage1989.stagekit.move" })
export class Move extends SingletonAction<KeySettings> {
	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const { source, filter } = ev.payload.settings;
		if (!obs.isConnected || !source || !filter) {
			await ev.action.showAlert();
			return;
		}
		try {
			await obs.setFilter(source, filter, true);
		} catch {
			await ev.action.showAlert();
		}
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, isMove));
	}
}
