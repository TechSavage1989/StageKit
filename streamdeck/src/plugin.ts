import streamDeck from "@elgato/streamdeck";

import { BlurAll } from "./actions/blur-all.ts";
import { Hook } from "./actions/hook.ts";
import { Move } from "./actions/move.ts";
import { FilterSwitch } from "./actions/switch.ts";
import { Tool } from "./actions/tool.ts";
import { type Connection, obs } from "./obs.ts";



streamDeck.logger.setLevel("info");

streamDeck.actions.registerAction(new FilterSwitch());
streamDeck.actions.registerAction(new Move());
streamDeck.actions.registerAction(new Tool());
streamDeck.actions.registerAction(new BlurAll());
streamDeck.actions.registerAction(new Hook());

// Where OBS is: kept once for every key (the "OBS connection" part of each
// key's settings), and followed when it is changed there.
streamDeck.settings.onDidReceiveGlobalSettings<Connection>((ev) => obs.use(ev.settings));

await streamDeck.connect();
obs.use(await streamDeck.settings.getGlobalSettings<Connection>());
