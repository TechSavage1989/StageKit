import streamDeck from "@elgato/streamdeck";

import { Move } from "./actions/move.ts";
import { look } from "./actions/shared.ts";
import { FilterSwitch } from "./actions/switch.ts";
import { type Connection, obs } from "./obs.ts";

type Global = Connection & { colour?: string };

streamDeck.logger.setLevel("info");

streamDeck.actions.registerAction(new FilterSwitch());
streamDeck.actions.registerAction(new Move());

// Where OBS is: kept once for every key (the "OBS connection" part of each
// key's settings), and followed when it is changed there.
streamDeck.settings.onDidReceiveGlobalSettings<Global>((ev) => {
	look.use(ev.settings.colour);
	obs.use(ev.settings);
});

await streamDeck.connect();
const saved = await streamDeck.settings.getGlobalSettings<Global>();
look.use(saved.colour);
obs.use(saved);
