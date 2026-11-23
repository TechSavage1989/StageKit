import streamDeck from "@elgato/streamdeck";

import { AppSwitch } from "./actions/app-switch.ts";
import { AutoFrameCommand } from "./actions/auto-frame-command.ts";
import { AUTO_FRAME_COMMANDS } from "./auto-frame.ts";
import { DockOption } from "./actions/option.ts";
import { SceneSwitch } from "./actions/scene.ts";
import { Slide } from "./actions/slide.ts";
import { BlurAll } from "./actions/blur-all.ts";
import { FitOverlap } from "./actions/fit-overlap.ts";
import { SceneModeKey } from "./actions/mode.ts";
import { ViewKey } from "./actions/view.ts";
import { SceneBlurKey } from "./actions/scene-blur.ts";
import { Hook } from "./actions/hook.ts";
import { ObsControl } from "./actions/obs-control.ts";
import { TimerKey } from "./actions/timer.ts";
import { OutputKey } from "./actions/output.ts";
import { ResetStats } from "./actions/reset-stats.ts";
import { StudioTransition } from "./actions/studio-transition.ts";
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
streamDeck.actions.registerAction(new FilterSwitch(base + ".obsfilter", () => true));
for (const key of ["obssource", "obsmute", "obsstudio", "obscamera"])
	streamDeck.actions.registerAction(new ObsControl(key));
for (const feature of features.sceneStarts)
	streamDeck.actions.registerAction(new ObsControl(feature.key, feature.kind));
streamDeck.actions.registerAction(new PictureShape());
for (const feature of AUTO_FRAME_COMMANDS)
	streamDeck.actions.registerAction(new AutoFrameCommand(feature));
streamDeck.actions.registerAction(new Hook());
streamDeck.actions.registerAction(new AppSwitch());
streamDeck.actions.registerAction(new FitOverlap());
streamDeck.actions.registerAction(new SceneSwitch());
streamDeck.actions.registerAction(new SceneModeKey());
streamDeck.actions.registerAction(new ViewKey());
streamDeck.actions.registerAction(new SceneBlurKey());
streamDeck.actions.registerAction(new Tool());
streamDeck.actions.registerAction(new BlurAll());
streamDeck.actions.registerAction(new DockOption());
streamDeck.actions.registerAction(new Slide());
streamDeck.actions.registerAction(new TimerKey());
streamDeck.actions.registerAction(new OutputKey("stream"));
streamDeck.actions.registerAction(new OutputKey("record"));
streamDeck.actions.registerAction(new ResetStats());
streamDeck.actions.registerAction(new StudioTransition());
// The first two keys there were, one for any switch and one for any move:
// no longer offered, but keys made with them keep working.
streamDeck.actions.registerAction(new FilterSwitch(`${base}.switch`, isSwitch));
streamDeck.actions.registerAction(new Move(`${base}.move`, isMove));

// Where OBS is: kept once for every key (the "OBS connection" part of each
// key's settings), and followed when it is changed there.
streamDeck.settings.onDidReceiveGlobalSettings<Connection>((ev) => obs.use(ev.settings));

await streamDeck.connect();
obs.use(await streamDeck.settings.getGlobalSettings<Connection>());
