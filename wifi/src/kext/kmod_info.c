#include <mach/mach_types.h>
#include <mach/kmod.h>

extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);

KMOD_EXPLICIT_DECL(br.com.jonasw8.driver.ath10kmac, "0.0.98", _start, _stop)

static kern_return_t ath10kmac_module_start(kmod_info_t *ki, void *data)
{
    (void)ki;
    (void)data;
    return KERN_SUCCESS;
}

static kern_return_t ath10kmac_module_stop(kmod_info_t *ki, void *data)
{
    (void)ki;
    (void)data;
    return KERN_SUCCESS;
}

kmod_start_func_t *_realmain = ath10kmac_module_start;
kmod_stop_func_t  *_antimain = ath10kmac_module_stop;
