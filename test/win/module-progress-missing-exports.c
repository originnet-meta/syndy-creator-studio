#include <stdbool.h>

#ifdef _WIN32
#define MODULE_EXPORT __declspec(dllexport)
#else
#define MODULE_EXPORT __attribute__((visibility("default")))
#endif

MODULE_EXPORT bool obs_module_load(void)
{
	return true;
}
