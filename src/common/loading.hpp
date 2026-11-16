// Whether OBS has finished loading its scenes.
//
// While a scene collection loads, sources and their filters are already
// ticked, but only some scenes have their items yet. Anything that goes
// through every scene's items to add to them or to adjust them must wait:
// stand-in items added then crashed another plugin's load step (beta142), and
// a shadow that gave its room back to the items there were and took it again
// from all of them made the owner's camera 11 px larger at every start in the
// scenes loaded later (beta143).
#pragma once

namespace stagekit {

// From module load to unload.
void loading_watch_start();
void loading_watch_stop();

// True from two seconds after OBS says it has finished loading a collection
// until it starts changing to another or closes. Always true where there is
// no OBS window (the tests).
bool scenes_loaded();

} // namespace stagekit
