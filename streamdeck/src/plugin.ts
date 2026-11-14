import streamDeck from "@elgato/streamdeck";

import { AppSwitch } from "./actions/app-switch.ts";
import { DockOption } from "./actions/option.ts";
import { Slide } from "./actions/slide.ts";
import { BlurAll } from "./actions/blur-all.ts";
import { Hook } from "./actions/hook.ts";
import { Move } from "./actions/move.ts";
import { PictureShape } from "./actions/shape.ts";
import { FilterSwitch } from "./actions/switch.ts";
import { Tool } from "./actions/tool.ts";
import features from "./features.json" with { type: "json" };
import { isMove, isSwitch } from "./kinds.ts";
import { type Connection, obs } from "./obs.ts";



streamDeck.logger.setLevel("info");

const base = "com.techsavage1989.stagekit";

// One key for each feature, each offering only that feature's filters.
for (const feature of features.switches)
	streamDeck.actions.registerAction(new FilterSwitch(`${base}.${feature.key}`, (kind) => kind === feature.kind));
for (const feature of features.moves)
	streamDeck.actions.registerAction(new Move(`${base}.${feature.key}`, (kind) => kind === feature.kind));
streamDeck.actions.registerAction(new PictureShape());
streamDeck.actions.registerAction(new Hook());
streamDeck.actions.registerAction(new AppSwitch());
streamDeck.actions.registerAction(new Tool());
streamDeck.actions.registerAction(new BlurAll());
streamDeck.actions.registerAction(new DockOption());
streamDeck.actions.registerAction(new Slide());
// The first two keys there were, one for any switch and one for any move:
// no longer offered, but keys made with them keep working.
streamDeck.actions.registerAction(new FilterSwitch(`${base}.switch`, isSwitch));
streamDeck.actions.registerAction(new Move(`${base}.move`, isMove));

// Where OBS is: kept once for every key (the "OBS connection" part of each
// key's settings), and followed when it is changed there.
streamDeck.settings.onDidReceiveGlobalSettings<Connection>((ev) => obs.use(ev.settings));

await streamDeck.connect();
obs.use(await streamDeck.settings.getGlobalSettings<Connection>());
