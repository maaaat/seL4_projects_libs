/*
 * Copyright 2019, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(DATA61_BSD)
 */

#pragma once

#include <sel4vm/guest_vm.h>

#include <sel4vmmplatsupport/device.h>
#include <sel4vmmplatsupport/ac_device.h>
#include <sel4vmmplatsupport/vgpio.h>

/*
 * GPIO Access control device
 */
struct gpio_device;

/*
 * Install a GPIO access control
 * @param[in] vm         The VM to install the device into
 * @param[in] default_ac The default access control state to apply
 * @param[in] action     Action to take when access is violated
 * @return               A handle to the GPIO Access control device, NULL on
 *                       failure
 */
struct gpio_device* vm_install_ac_gpio(vm_t* vm, enum vacdev_default default_ac,
                                       enum vacdev_action action);

/*
 * Provide GPIO pin access to the VM
 * @param[in] gpio_device  A handle to the GPIO Access Control device
 * @param[in] gpio_id      The ID of the GPIO to provide access to
 * @return                 0 on success
 */
int vm_gpio_provide(struct gpio_device* gpio_device, gpio_id_t gpio_id);

/*
 * Restrict GPIO pin access to the VM
 * @param[in] gpio_device  A handle to the GPIO Access Control device
 * @param[in] gpio_id      The ID of the GPIO to deny access to
 * @return                 0 on success
 */
int vm_gpio_restrict(struct gpio_device* gpio_device, gpio_id_t gpio_id);
