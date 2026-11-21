#include "component.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("StageKit contributors")

MODULE_EXPORT const char *obs_module_name(void)
{
	return stagekit::native::text("Ports.Module." STAGEKIT_COMPONENT_KEY);
}

bool obs_module_load(void)
{
	stagekit::native::component.load();
	return true;
}

void obs_module_unload(void)
{
	if (stagekit::native::component.unload)
		stagekit::native::component.unload();
}

void obs_module_post_load(void)
{
	if (stagekit::native::component.post_load)
		stagekit::native::component.post_load();
}
