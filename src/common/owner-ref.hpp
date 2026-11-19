#pragma once

#include <obs.h>

namespace stagekit {

// A filter's own source, for a registry of filters that is gone through from
// another thread (a "source_rename" handler, a vendor request). Never call
// obs_source_get_ref() on the raw pointer kept there: when the last reference
// goes, obs_source_release frees the source's weak control at once, while the
// filter's destroy callback (which takes it out of the registry) runs later on
// OBS's destruction thread. Reaching the source in between read freed memory
// (valgrind: Fit to Space removed, then a source renamed). The weak reference
// held here keeps the control alive for as long as the filter is, so get()
// is safe from the registry's lock and gives nullptr once the source is going.
class OwnerRef {
public:
	explicit OwnerRef(obs_source_t *source) : weak_(obs_source_get_weak_source(source)) {}
	~OwnerRef() { obs_weak_source_release(weak_); }
	OwnerRef(const OwnerRef &) = delete;
	OwnerRef &operator=(const OwnerRef &) = delete;

	// A strong reference to release, or nullptr when the source is going.
	obs_source_t *get() const { return obs_weak_source_get_source(weak_); }

private:
	obs_weak_source_t *weak_;
};

} // namespace stagekit
