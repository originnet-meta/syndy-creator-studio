#include <obs.h>

#include <map>
#include <string>
#include <vector>
#include <stdio.h>

struct module_event {
	std::string module_name;
	enum obs_module_load_progress progress;
	enum obs_module_load_reason reason;
};

struct module_expectation {
	enum obs_module_load_progress progress;
	enum obs_module_load_reason reason;
};

struct module_state {
	bool saw_begin = false;
	bool saw_terminal = false;
	enum obs_module_load_progress terminal_progress = OBS_MODULE_LOAD_PROGRESS_BEGIN;
	enum obs_module_load_reason terminal_reason = OBS_MODULE_LOAD_REASON_NONE;
	size_t event_count = 0;
};

static void module_load_progress_callback(void *param, const char *module_name, enum obs_module_load_progress progress,
					  enum obs_module_load_reason reason)
{
	std::vector<module_event> *events = static_cast<std::vector<module_event> *>(param);

	module_event event = {};
	event.module_name = module_name ? module_name : "";
	event.progress = progress;
	event.reason = reason;
	events->push_back(event);
}

static bool validate_events(const std::vector<module_event> &events)
{
	const std::map<std::string, module_expectation> expected = {
		{"mlp-good", {OBS_MODULE_LOAD_PROGRESS_SUCCESS, OBS_MODULE_LOAD_REASON_NONE}},
		{"mlp-fail-init", {OBS_MODULE_LOAD_PROGRESS_FAILURE, OBS_MODULE_LOAD_REASON_FAILED_TO_INITIALIZE}},
		{"mlp-missing-exports", {OBS_MODULE_LOAD_PROGRESS_SKIP, OBS_MODULE_LOAD_REASON_MISSING_EXPORTS}},
		{"mlp-not-plugin", {OBS_MODULE_LOAD_PROGRESS_SKIP, OBS_MODULE_LOAD_REASON_NOT_OBS_PLUGIN}},
	};
	std::map<std::string, module_state> states;
	bool ok = true;

	for (const module_event &event : events) {
		auto expected_it = expected.find(event.module_name);
		if (expected_it == expected.end()) {
			fprintf(stderr, "Unexpected module callback: %s\n", event.module_name.c_str());
			ok = false;
			continue;
		}

		module_state &state = states[event.module_name];
		state.event_count++;

		if (event.progress == OBS_MODULE_LOAD_PROGRESS_BEGIN) {
			if (state.saw_begin || state.saw_terminal) {
				fprintf(stderr, "Invalid BEGIN ordering for module: %s\n", event.module_name.c_str());
				ok = false;
			}
			if (event.reason != OBS_MODULE_LOAD_REASON_NONE) {
				fprintf(stderr, "BEGIN callback has unexpected reason for module: %s\n",
					event.module_name.c_str());
				ok = false;
			}
			state.saw_begin = true;
			continue;
		}

		if (!state.saw_begin) {
			fprintf(stderr, "Terminal callback before BEGIN for module: %s\n", event.module_name.c_str());
			ok = false;
			continue;
		}
		if (state.saw_terminal) {
			fprintf(stderr, "Multiple terminal callbacks for module: %s\n", event.module_name.c_str());
			ok = false;
			continue;
		}

		state.saw_terminal = true;
		state.terminal_progress = event.progress;
		state.terminal_reason = event.reason;
	}

	for (const auto &entry : expected) {
		const std::string &module_name = entry.first;
		const module_expectation &expectation = entry.second;
		auto state_it = states.find(module_name);
		if (state_it == states.end()) {
			fprintf(stderr, "Missing callbacks for module: %s\n", module_name.c_str());
			ok = false;
			continue;
		}

		const module_state &state = state_it->second;
		if (!state.saw_begin || !state.saw_terminal) {
			fprintf(stderr, "Incomplete callback sequence for module: %s\n", module_name.c_str());
			ok = false;
			continue;
		}
		if (state.event_count != 2) {
			fprintf(stderr, "Unexpected callback count (%zu) for module: %s\n", state.event_count,
				module_name.c_str());
			ok = false;
		}
		if (state.terminal_progress != expectation.progress) {
			fprintf(stderr, "Unexpected terminal progress for module: %s\n", module_name.c_str());
			ok = false;
		}
		if (state.terminal_reason != expectation.reason) {
			fprintf(stderr, "Unexpected terminal reason for module: %s\n", module_name.c_str());
			ok = false;
		}
	}

	if (events.size() != expected.size() * 2) {
		fprintf(stderr, "Unexpected total callback count: %zu\n", events.size());
		ok = false;
	}

	return ok;
}

int main()
{
	std::vector<module_event> events;
	bool success = false;
	const char *module_dir = MODULE_PROGRESS_FIXTURE_DIR;

	if (!obs_startup("en-US", nullptr, nullptr)) {
		fprintf(stderr, "obs_startup failed\n");
		return 1;
	}

	obs_add_module_path(module_dir, module_dir);
	obs_set_module_load_progress_callback(module_load_progress_callback, &events);
	obs_load_all_modules();
	obs_set_module_load_progress_callback(nullptr, nullptr);

	success = validate_events(events);

	obs_shutdown();

	if (!success)
		return 1;

	printf("module-load-progress-test: success\n");
	return 0;
}
