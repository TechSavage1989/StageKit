import streamDeck from "@elgato/streamdeck";

import { Move } from "./actions/move.ts";
import { FilterSwitch } from "./actions/switch.ts";
import { type Connection, obs } from "./obs.ts";

streamDeck.logger.setLevel("info");

streamDeck.actions.registerAction(new FilterSwitch());
streamDeck.actions.registerAction(new Move());

// Where OBS is: kept once for every key (the "OBS connection" part of each
// key's settings), and followed when it is changed there.
streamDeck.settings.onDidReceiveGlobalSettings<Connection>((ev) => obs.use(ev.settings));

await streamDeck.connect();
obs.use(await streamDeck.settings.getGlobalSettings<Connection>());
