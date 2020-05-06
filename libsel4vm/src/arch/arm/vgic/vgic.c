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
/*
 * This component controls and maintains the GIC for the VM.
 * IRQs must be registered at init time with vm_virq_new(...)
 * This function creates and registers an IRQ data structure which will be used for IRQ maintenance
 * b) ENABLING: When the VM enables the IRQ, it checks the pending flag for the VM.
 *   - If the IRQ is not pending, we either
 *        1) have not received an IRQ so it is still enabled in seL4
 *        2) have received an IRQ, but ignored it because the VM had disabled it.
 *     In either case, we simply ACK the IRQ with seL4. In case 1), the IRQ will come straight through,
       in case 2), we have ACKed an IRQ that was not yet pending anyway.
 *   - If the IRQ is already pending, we can assume that the VM has yet to ACK the IRQ and take no further
 *     action.
 *   Transitions: b->c
 * c) PIRQ: When an IRQ is received from seL4, seL4 disables the IRQ and sends an async message. When the VMM
 *    receives the message.
 *   - If the IRQ is enabled, we set the pending flag in the VM and inject the appropriate IRQ
 *     leading to state d)
 *   - If the IRQ is disabled, the VMM takes no further action, leading to state b)
 *   Transitions: (enabled)? c->d :  c->b
 * d) When the VM acknowledges the IRQ, an exception is raised and delivered to the VMM. When the VMM
 *    receives the exception, it clears the pending flag and acks the IRQ with seL4, leading back to state c)
 *    Transition: d->c
 * g) When/if the VM disables the IRQ, we may still have an IRQ resident in the GIC. We allow
 *    this IRQ to be delivered to the VM, but subsequent IRQs will not be delivered as seen by state c)
 *    Transitions g->c
 *
 *   NOTE: There is a big assumption that the VM will not manually manipulate our pending flags and
 *         destroy our state. The affects of this will be an IRQ that is never acknowledged and hence,
 *         will never occur again.
 */

#include "vgic.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <utils/arith.h>
#include <vka/vka.h>
#include <vka/capops.h>

#include <sel4vm/gen_config.h>
#include <sel4vm/guest_vm.h>
#include <sel4vm/boot.h>
#include <sel4vm/guest_memory.h>
#include <sel4vm/guest_irq_controller.h>
#include <sel4vm/guest_vm_util.h>

#include "vm.h"
#include "../fault.h"

//#define DEBUG_IRQ
//#define DEBUG_DIST

#ifdef DEBUG_IRQ
#define DIRQ(...) do{ printf("VDIST: "); printf(__VA_ARGS__); }while(0)
#else
#define DIRQ(...) do{}while(0)
#endif

#ifdef DEBUG_DIST
#define DDIST(...) do{ printf("VDIST: "); printf(__VA_ARGS__); }while(0)
#else
#define DDIST(...) do{}while(0)
#endif

/* GIC Distributor register access utilities */
#define GIC_DIST_REGN(offset, reg) ((offset-reg)/sizeof(uint32_t))
#define RANGE32(a, b) a ... b + (sizeof(uint32_t)-1)

#define IRQ_IDX(irq) ((irq) / 32)
#define IRQ_BIT(irq) (1U << ((irq) % 32))

#define not_pending(...) !is_pending(__VA_ARGS__)
#define not_active(...)  !is_active(__VA_ARGS__)
#define not_enabled(...) !is_enabled(__VA_ARGS__)


/* Inject interrupt into vcpu */
static int vgic_vcpu_inject_irq(struct vgic_device *vgic, vm_vcpu_t *inject_vcpu, struct virq_handle *irq);

static inline void virq_ack(vm_vcpu_t *vcpu, struct virq_handle *irq)
{
    irq->ack(vcpu, irq->virq, irq->token);
}

#define LR_OF_NEXT(_i) (((_i) == MAX_LR_OVERFLOW - 1) ? 0 : ((_i) + 1))

static struct virq_handle *virq_get_sgi_ppi(struct vgic_device *vgic, vm_vcpu_t *vcpu, int virq)
{
    assert(vcpu->vcpu_id < CONFIG_MAX_NUM_NODES);
    return vgic->sgi_ppi_irq[vcpu->vcpu_id][virq];
}

static struct virq_handle *virq_find_spi_irq_data(struct vgic_device *vgic, int virq)
{
    int i;
    for (i = 0; i < MAX_VIRQS; i++) {
        if (vgic->virqs[i] && vgic->virqs[i]->virq == virq) {
            return vgic->virqs[i];
        }
    }
    return NULL;
}

static struct virq_handle *virq_find_irq_data(struct vgic_device *vgic, vm_vcpu_t *vcpu, int virq)
{
    if (virq < GIC_SPI_IRQ_MIN)  {
        return virq_get_sgi_ppi(vgic, vcpu, virq);
    }
    return virq_find_spi_irq_data(vgic, virq);
}

static int virq_spi_add(struct vgic_device *vgic, struct virq_handle *virq_data)
{
    int i;
    for (i = 0; i < MAX_VIRQS; i++) {
        if (vgic->virqs[i] == NULL) {
            vgic->virqs[i] = virq_data;
            return 0;
        }
    }
    return -1;
}

static int virq_sgi_ppi_add(vm_vcpu_t *vcpu, struct vgic_device *vgic, struct virq_handle *virq_data)
{
    if (vgic->sgi_ppi_irq[vcpu->vcpu_id][virq_data->virq] != NULL) {
        ZF_LOGE("VIRQ %d already registered for VCPU %u\n", virq_data->virq, vcpu->vcpu_id);
        return -1;
    }
    vgic->sgi_ppi_irq[vcpu->vcpu_id][virq_data->virq] = virq_data;
    return 0;
}

static int virq_add(vm_vcpu_t *vcpu, struct vgic_device *vgic, struct virq_handle *virq_data)
{
    int virq = virq_data->virq;
    if (virq < GIC_SPI_IRQ_MIN) {
        return virq_sgi_ppi_add(vcpu, vgic, virq_data);
    }
    return virq_spi_add(vgic, virq_data);
}

static int vgic_virq_init(struct vgic_device *vgic)
{
    memset(vgic->virqs, 0, sizeof(vgic->virqs));
    for (int i = 0; i < CONFIG_MAX_NUM_NODES; i++) {
        memset(vgic->irq[i], 0, sizeof(vgic->irq[i]));
        vgic->lr_overflow[i].head = 0;
        vgic->lr_overflow[i].tail = 0;
        vgic->lr_overflow[i].full = false;
        memset(vgic->lr_overflow[i].irqs, 0, sizeof(vgic->lr_overflow[i].irqs));
    }
    return 0;
}


static inline void virq_init(struct virq_handle *virq, int irq, irq_ack_fn_t ack_fn, void *token)
{
    virq->virq = irq;
    virq->token = token;
    virq->ack = ack_fn;
}


static inline void set_sgi_ppi_pending(struct vgic_v2_dist_map *gic_dist, int irq, int value, int vcpu_id)
{
    if (value) {
        gic_dist->pending_set0[vcpu_id] |= IRQ_BIT(irq);
        gic_dist->pending_clr0[vcpu_id] |= IRQ_BIT(irq);
    } else {
        gic_dist->pending_set0[vcpu_id] &= ~IRQ_BIT(irq);
        gic_dist->pending_clr0[vcpu_id] &= ~IRQ_BIT(irq);
    }
}

static inline void set_spi_pending(struct vgic_v2_dist_map *gic_dist, int irq, int value, int vcpu_id)
{
    if (value) {
        gic_dist->pending_set[IRQ_IDX(irq)] |= IRQ_BIT(irq);
        gic_dist->pending_clr[IRQ_IDX(irq)] |= IRQ_BIT(irq);
    } else {
        gic_dist->pending_set[IRQ_IDX(irq)] &= ~IRQ_BIT(irq);
        gic_dist->pending_clr[IRQ_IDX(irq)] &= ~IRQ_BIT(irq);
    }
}

static inline void set_pending(struct vgic_v2_dist_map *gic_dist, int irq, int value, int vcpu_id)
{
    if (irq < GIC_SPI_IRQ_MIN) {
        set_sgi_ppi_pending(gic_dist, irq, value, vcpu_id);
        return;
    }
    set_spi_pending(gic_dist, irq, value, vcpu_id);
}

static inline int is_sgi_ppi_pending(struct vgic_v2_dist_map *gic_dist, int irq, int vcpu_id)
{
    return !!(gic_dist->pending_set0[vcpu_id] & IRQ_BIT(irq));
}

static inline int is_spi_pending(struct vgic_v2_dist_map *gic_dist, int irq, int vcpu_id)
{
    return !!(gic_dist->pending_set[IRQ_IDX(irq)] & IRQ_BIT(irq));
}

static inline int is_pending(struct vgic_v2_dist_map *gic_dist, int irq, int vcpu_id)
{
    if (irq < GIC_SPI_IRQ_MIN) {
        return is_sgi_ppi_pending(gic_dist, irq, vcpu_id);

    }
    return is_spi_pending(gic_dist, irq, vcpu_id);
}

static inline void set_sgi_ppi_enable(struct vgic_v2_dist_map *gic_dist, int irq, int value, int vcpu_id)
{
    if (value) {
        gic_dist->enable_set0[vcpu_id] |= IRQ_BIT(irq);
        gic_dist->enable_clr0[vcpu_id] |= IRQ_BIT(irq);
    } else {
        gic_dist->enable_set0[vcpu_id] &= ~IRQ_BIT(irq);
        gic_dist->enable_clr0[vcpu_id] &= ~IRQ_BIT(irq);
    }
}


static inline void set_spi_enable(struct vgic_v2_dist_map *gic_dist, int irq, int value, int vcpu_id)
{
    if (value) {
        gic_dist->enable_set[IRQ_IDX(irq)] |= IRQ_BIT(irq);
        gic_dist->enable_clr[IRQ_IDX(irq)] |= IRQ_BIT(irq);
    } else {
        gic_dist->enable_set[IRQ_IDX(irq)] &= ~IRQ_BIT(irq);
        gic_dist->enable_clr[IRQ_IDX(irq)] &= ~IRQ_BIT(irq);
    }
}

static inline void set_enable(struct vgic_v2_dist_map *gic_dist, int irq, int value, int vcpu_id)
{
    if (irq < GIC_SPI_IRQ_MIN) {
        set_sgi_ppi_enable(gic_dist, irq, value, vcpu_id);
        return;
    }
    set_spi_enable(gic_dist, irq, value, vcpu_id);
}

static inline int is_sgi_ppi_enabled(struct vgic_v2_dist_map *gic_dist, int irq, int vcpu_id)
{
    return !!(gic_dist->enable_set0[vcpu_id] & IRQ_BIT(irq));
}

static inline int is_spi_enabled(struct vgic_v2_dist_map *gic_dist, int irq, int vcpu_id)
{
    return !!(gic_dist->enable_set[IRQ_IDX(irq)] & IRQ_BIT(irq));
}

static inline int is_enabled(struct vgic_v2_dist_map *gic_dist, int irq, int vcpu_id)
{
    if (irq < GIC_SPI_IRQ_MIN) {
        return is_sgi_ppi_enabled(gic_dist, irq, vcpu_id);
    }
    return is_spi_enabled(gic_dist, irq, vcpu_id);
}

static inline void set_sgi_ppi_active(struct vgic_v2_dist_map *gic_dist, int irq, int value, int vcpu_id)
{
    if (value) {
        gic_dist->active0[vcpu_id] |= IRQ_BIT(irq);
    } else {
        gic_dist->active0[vcpu_id] &= ~IRQ_BIT(irq);
    }
}

static inline void set_spi_active(struct vgic_v2_dist_map *gic_dist, int irq, int value, int vcpu_id)
{
    if (value) {
        gic_dist->active[IRQ_IDX(irq)] |= IRQ_BIT(irq);
    } else {
        gic_dist->active[IRQ_IDX(irq)] &= ~IRQ_BIT(irq);
    }
}

static inline void set_active(struct vgic_v2_dist_map *gic_dist, int irq, int value, int vcpu_id)
{
    if (irq < GIC_SPI_IRQ_MIN) {
        set_sgi_ppi_active(gic_dist, irq, value, vcpu_id);
        return;
    }
    set_spi_active(gic_dist, irq, value, vcpu_id);
}

static inline int is_sgi_ppi_active(struct vgic_v2_dist_map *gic_dist, int irq, int vcpu_id)
{
    return !!(gic_dist->active0[vcpu_id] & IRQ_BIT(irq));
}

static inline int is_spi_active(struct vgic_v2_dist_map *gic_dist, int irq, int vcpu_id)
{
    return !!(gic_dist->active[IRQ_IDX(irq)] & IRQ_BIT(irq));
}

static inline int is_active(struct vgic_v2_dist_map *gic_dist, int irq, int vcpu_id)
{
    if (irq < GIC_SPI_IRQ_MIN) {
        return is_sgi_ppi_active(gic_dist, irq, vcpu_id);
    }
    return is_spi_active(gic_dist, irq, vcpu_id);
}

static inline int vgic_add_overflow_cpu(struct lr_of *lr_overflow, struct virq_handle *irq)
{
    /* Add to overflow list */
    int idx = lr_overflow->tail;
    if (unlikely(lr_overflow->full)) {
        ZF_LOGF("too many overflow irqs");
        return -1;
    }

    lr_overflow->irqs[idx] = *irq;
    lr_overflow->full = (lr_overflow->head == LR_OF_NEXT(lr_overflow->tail));
    if (!lr_overflow->full) {
        lr_overflow->tail = LR_OF_NEXT(idx);
    }
    return 0;
}

static inline int vgic_add_overflow(struct vgic_device *vgic, struct virq_handle *irq, vm_vcpu_t *vcpu)
{
    return vgic_add_overflow_cpu(&vgic->lr_overflow[vcpu->vcpu_id], irq);
}

static inline void vgic_handle_overflow_cpu(struct vgic_device *vgic, struct lr_of *lr_overflow, vm_vcpu_t *vcpu)
{
    /* copy tail, as vgic_vcpu_inject_irq can mutate it, and we do
     * not want to process any new overflow irqs */
    size_t tail = lr_overflow->tail;
    for (size_t i = lr_overflow->head; i != tail; i = LR_OF_NEXT(i)) {
        if (vgic_vcpu_inject_irq(vgic, vcpu, &lr_overflow->irqs[i]) == 0) {
            lr_overflow->head = LR_OF_NEXT(i);
            lr_overflow->full = (lr_overflow->head == LR_OF_NEXT(lr_overflow->tail));
        } else {
            break;
        }
    }
}

static inline void vgic_handle_overflow(struct vgic_device *vgic, vm_vcpu_t *vcpu)
{
    vgic_handle_overflow_cpu(vgic, &vgic->lr_overflow[vcpu->vcpu_id], vcpu);
}

static int vgic_vcpu_inject_irq(struct vgic_device *vgic, vm_vcpu_t *inject_vcpu, struct virq_handle *irq)
{
    int err;
    int i;

    seL4_CPtr vcpu;
    vcpu = inject_vcpu->vcpu.cptr;
    for (i = 0; i < MAX_LR_OVERFLOW; i++) {
        if (vgic->irq[inject_vcpu->vcpu_id][i] == NULL) {
            break;
        }
    }
    err = seL4_ARM_VCPU_InjectIRQ(vcpu, irq->virq, 0, 0, i);
    assert((i < 4) || err);
    if (!err) {
        /* Shadow */
        vgic->irq[inject_vcpu->vcpu_id][i] = irq;
        return err;
    } else {
        /* Add to overflow list */
        return vgic_add_overflow(vgic, irq, inject_vcpu);
    }
}

int handle_vgic_maintenance(vm_vcpu_t *vcpu, int idx)
{
    /* STATE d) */
    struct vgic_device *vgic = vcpu->vm->arch.vgic;
    struct vgic_v2_dist_map *gic_dist;
    struct virq_handle **lr;

    assert(vgic);
    gic_dist = &vgic->reg_v2.distributor;
    assert(gic_dist);
    lr = vgic->irq[vcpu->vcpu_id];
    assert(lr[idx]);

    /* Clear pending */
    DIRQ("Maintenance IRQ %d\n", lr[idx]->virq);
    set_pending(gic_dist, lr[idx]->virq, false, vcpu->vcpu_id);
    virq_ack(vcpu, lr[idx]);

    /* Check the overflow list for pending IRQs */
    lr[idx] = NULL;
    vgic_handle_overflow(vgic, vcpu);
    return 0;
}


static int vgic_dist_enable(struct vgic_device *vgic, vm_t *vm)
{
    struct vgic_v2_dist_map *gic_dist = &vgic->reg_v2.distributor;
    DDIST("enabling gic distributer\n");
    gic_dist->enable = 1;
    return 0;
}

static int vgic_dist_disable(struct vgic_device *vgic, vm_t *vm)
{
    struct vgic_v2_dist_map *gic_dist = &vgic->reg_v2.distributor;
    DDIST("disabling gic distributer\n");
    gic_dist->enable = 0;
    return 0;
}

static int vgic_dist_enable_irq(struct vgic_device *vgic, vm_vcpu_t *vcpu, int irq)
{
    struct vgic_v2_dist_map *gic_dist = &vgic->reg_v2.distributor;
    struct virq_handle *virq_data;
    DDIST("enabling irq %d\n", irq);
    set_enable(gic_dist, irq, true, vcpu->vcpu_id);
    virq_data = virq_find_irq_data(vgic, vcpu, irq);
    if (virq_data) {
        /* STATE b) */
        if (not_pending(gic_dist, virq_data->virq, vcpu->vcpu_id)) {
            virq_ack(vcpu, virq_data);
        }
    } else {
        DDIST("enabled irq %d has no handle", irq);
    }
    return 0;
}

static int vgic_dist_disable_irq(struct vgic_device *vgic, vm_vcpu_t *vcpu, int irq)
{
    /* STATE g) */
    struct vgic_v2_dist_map *gic_dist = &vgic->reg_v2.distributor;
    if (irq >= 16) {
        DDIST("disabling irq %d\n", irq);
        set_enable(gic_dist, irq, false, vcpu->vcpu_id);
    }
    return 0;
}

static int vgic_dist_set_pending_irq(struct vgic_device *vgic, vm_vcpu_t *vcpu, int irq)
{
    /* STATE c) */
    struct vgic_v2_dist_map *gic_dist;
    struct virq_handle *virq_data;

    gic_dist = &vgic->reg_v2.distributor;
    virq_data = virq_find_irq_data(vgic, vcpu, irq);
    /* If it is enables, inject the IRQ */
    if (virq_data && gic_dist->enable && is_enabled(gic_dist, irq, vcpu->vcpu_id)) {
        int err;
        DDIST("Pending set: Inject IRQ from pending set (%d)\n", irq);

        set_pending(gic_dist, virq_data->virq, true, vcpu->vcpu_id);
        err = vgic_vcpu_inject_irq(vgic, vcpu, virq_data);
        assert(!err);

        return err;
    } else {
        /* No further action */
        DDIST("IRQ not enabled (%d) on vcpu %d\n", irq, vcpu->vcpu_id);
        return -1;
    }

    return 0;
}

static int vgic_dist_clr_pending_irq(struct vgic_device *vgic, vm_vcpu_t *vcpu, int irq)
{
    struct vgic_v2_dist_map *gic_dist = &vgic->reg_v2.distributor;
    DDIST("clr pending irq %d\n", irq);
    set_pending(gic_dist, irq, false, vcpu->vcpu_id);
    return 0;
}

static memory_fault_result_t handle_vgic_dist_read_fault(vm_t *vm, vm_vcpu_t *vcpu, uintptr_t fault_addr,
                                                         size_t fault_length,
                                                         void *cookie)
{
    int err = 0;
    fault_t *fault = vcpu->vcpu_arch.fault;
    struct vgic_device *vgic = cookie;
    struct vgic_v2_registers *r = &vgic->reg_v2;
    struct vgic_v2_dist_map *gic_dist = &r->distributor;
    int offset = fault_get_address(fault) - r->dist_paddr;
    int vcpu_id = vcpu->vcpu_id;
    uint32_t reg = 0;
    int reg_offset = 0;
    uintptr_t base_reg;
    uint32_t *reg_ptr;
    switch (offset) {
    case RANGE32(GIC_DIST_CTLR, GIC_DIST_CTLR):
        reg = gic_dist->enable;
        break;
    case RANGE32(GIC_DIST_TYPER, GIC_DIST_TYPER):
        reg = gic_dist->ic_type;
        break;
    case RANGE32(GIC_DIST_IIDR, GIC_DIST_IIDR):
        reg = gic_dist->dist_ident;
        break;
    case RANGE32(0x00C, 0x01C):
        /* Reserved */
        break;
    case RANGE32(0x020, 0x03C):
        /* Implementation defined */
        break;
    case RANGE32(0x040, 0x07C):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_IGROUPR0, GIC_DIST_IGROUPR0):
        reg = gic_dist->irq_group0[vcpu->vcpu_id];
        break;
    case RANGE32(GIC_DIST_IGROUPR1, GIC_DIST_IGROUPRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_IGROUPR1);
        reg = gic_dist->irq_group[reg_offset];
        break;
    case RANGE32(GIC_DIST_ISENABLER0, GIC_DIST_ISENABLER0):
        reg = gic_dist->enable_set0[vcpu->vcpu_id];
        break;
    case RANGE32(GIC_DIST_ISENABLER1, GIC_DIST_ISENABLERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ISENABLER1);
        reg = gic_dist->enable_set[reg_offset];
        break;
    case RANGE32(GIC_DIST_ICENABLER0, GIC_DIST_ICENABLER0):
        reg = gic_dist->enable_clr0[vcpu->vcpu_id];
        break;
    case RANGE32(GIC_DIST_ICENABLER1, GIC_DIST_ICENABLERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ICENABLER1);
        reg = gic_dist->enable_clr[reg_offset];
        break;
    case RANGE32(GIC_DIST_ISPENDR0, GIC_DIST_ISPENDR0):
        reg = gic_dist->pending_set0[vcpu->vcpu_id];
        break;
    case RANGE32(GIC_DIST_ISPENDR1, GIC_DIST_ISPENDRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ISPENDR1);
        reg = gic_dist->pending_set[reg_offset];
        break;
    case RANGE32(GIC_DIST_ICPENDR0, GIC_DIST_ICPENDR0):
        reg = gic_dist->pending_clr0[vcpu->vcpu_id];
        break;
    case RANGE32(GIC_DIST_ICPENDR1, GIC_DIST_ICPENDRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ICPENDR1);
        reg = gic_dist->pending_clr[reg_offset];
        break;
    case RANGE32(GIC_DIST_ISACTIVER0, GIC_DIST_ISACTIVER0):
        reg = gic_dist->active0[vcpu->vcpu_id];
        break;
    case RANGE32(GIC_DIST_ISACTIVER1, GIC_DIST_ISACTIVERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ISACTIVER1);
        reg = gic_dist->active[reg_offset];
        break;
    case RANGE32(GIC_DIST_ICACTIVER0, GIC_DIST_ICACTIVER0):
        reg = gic_dist->active_clr0[vcpu->vcpu_id];
        break;
    case RANGE32(GIC_DIST_ICACTIVER1, GIC_DIST_ICACTIVERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ICACTIVER1);
        reg = gic_dist->active_clr[reg_offset];
        break;
    case RANGE32(GIC_DIST_IPRIORITYR0, GIC_DIST_IPRIORITYR7):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_IPRIORITYR0);
        reg = gic_dist->priority0[vcpu->vcpu_id][reg_offset];
        break;
    case RANGE32(GIC_DIST_IPRIORITYR8, GIC_DIST_IPRIORITYRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_IPRIORITYR8);
        reg = gic_dist->priority[reg_offset];
        break;
    case RANGE32(0x7FC, 0x7FC):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_ITARGETSR0, GIC_DIST_ITARGETSR7):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ITARGETSR0);
        reg = gic_dist->targets0[vcpu->vcpu_id][reg_offset];
        break;
    case RANGE32(GIC_DIST_ITARGETSR8, GIC_DIST_ITARGETSRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ITARGETSR8);
        reg = gic_dist->targets[reg_offset];
        break;
    case RANGE32(0xBFC, 0xBFC):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_ICFGR0, GIC_DIST_ICFGRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ICFGR0);
        reg = gic_dist->config[reg_offset];
        break;
    case RANGE32(0xD00, 0xDE4):
        base_reg = (uintptr_t) & (gic_dist->spi[0]);
        reg_ptr = (uint32_t *)(base_reg + (offset - 0xD00));
        reg = *reg_ptr;
        break;
    case RANGE32(0xDE8, 0xEFC):
        /* Reserved [0xDE8 - 0xE00) */
        /* GIC_DIST_NSACR [0xE00 - 0xF00) - Not supported */
        break;
    case RANGE32(GIC_DIST_SGIR, GIC_DIST_SGIR):
        reg = gic_dist->sgi_control;
        break;
    case RANGE32(0xF04, 0xF0C):
        /* Implementation defined */
        break;
    case RANGE32(GIC_DIST_CPENDSGIR0, GIC_DIST_CPENDSGIRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_CPENDSGIR0);
        reg = gic_dist->sgi_pending_clr[vcpu->vcpu_id][reg_offset];
        break;
    case RANGE32(GIC_DIST_SPENDSGIR0, GIC_DIST_SPENDSGIRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_SPENDSGIR0);
        reg = gic_dist->sgi_pending_set[vcpu->vcpu_id][reg_offset];
        break;
    case RANGE32(0xF30, 0xFBC):
        /* Reserved */
        break;
    case RANGE32(0xFC0, 0xFFB):
        base_reg = (uintptr_t) & (gic_dist->periph_id[0]);
        reg_ptr = (uint32_t *)(base_reg + (offset - 0xFC0));
        reg = *reg_ptr;
        break;
    default:
        ZF_LOGE("Unknown register offset 0x%x\n", offset);
        err = ignore_fault(fault);
        goto fault_return;
    }
    uint32_t mask = fault_get_data_mask(fault);
    fault_set_data(fault, reg & mask);
    err = advance_fault(fault);

fault_return:
    if (err) {
        return FAULT_ERROR;
    }
    return FAULT_HANDLED;
}

static memory_fault_result_t handle_vgic_dist_write_fault(vm_t *vm, vm_vcpu_t *vcpu, uintptr_t fault_addr,
                                                          size_t fault_length,
                                                          void *cookie)
{
    int err = 0;
    fault_t *fault = vcpu->vcpu_arch.fault;
    struct vgic_device *vgic = cookie;
    struct vgic_v2_registers *r = &vgic->reg_v2;
    struct vgic_v2_dist_map *gic_dist = &r->distributor;
    int offset = fault_get_address(fault) - r->dist_paddr;
    int vcpu_id = vcpu->vcpu_id;
    uint32_t reg = 0;
    uint32_t mask = fault_get_data_mask(fault);
    uint32_t reg_offset = 0;
    uint32_t data;
    switch (offset) {
    case RANGE32(GIC_DIST_CTLR, GIC_DIST_CTLR):
        data = fault_get_data(fault);
        if (data == 1) {
            vgic_dist_enable(vgic, vm);
        } else if (data == 0) {
            vgic_dist_disable(vgic, vm);
        } else {
            ZF_LOGE("Unknown enable register encoding");
        }
        break;
    case RANGE32(GIC_DIST_TYPER, GIC_DIST_TYPER):
        break;
    case RANGE32(GIC_DIST_IIDR, GIC_DIST_IIDR):
        break;
    case RANGE32(0x00C, 0x01C):
        /* Reserved */
        break;
    case RANGE32(0x020, 0x03C):
        /* Implementation defined */
        break;
    case RANGE32(0x040, 0x07C):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_IGROUPR0, GIC_DIST_IGROUPR0):
        gic_dist->irq_group0[vcpu->vcpu_id] = fault_emulate(fault, gic_dist->irq_group0[vcpu->vcpu_id]);
        break;
    case RANGE32(GIC_DIST_IGROUPR1, GIC_DIST_IGROUPRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_IGROUPR1);
        gic_dist->irq_group[reg_offset] = fault_emulate(fault, gic_dist->irq_group[reg_offset]);
        break;
    case RANGE32(GIC_DIST_ISENABLER0, GIC_DIST_ISENABLERN):
        data = fault_get_data(fault);
        /* Mask the data to write */
        data &= mask;
        while (data) {
            int irq;
            irq = CTZ(data);
            data &= ~(1U << irq);
            irq += (offset - GIC_DIST_ISENABLER0) * 8;
            vgic_dist_enable_irq(vgic, vcpu, irq);
        }
        break;
    case RANGE32(GIC_DIST_ICENABLER0, GIC_DIST_ICENABLERN):
        data = fault_get_data(fault);
        /* Mask the data to write */
        data &= mask;
        while (data) {
            int irq;
            irq = CTZ(data);
            data &= ~(1U << irq);
            irq += (offset - GIC_DIST_ICENABLER0) * 8;
            vgic_dist_disable_irq(vgic, vcpu, irq);
        }
        break;
    case RANGE32(GIC_DIST_ISPENDR0, GIC_DIST_ISPENDRN):
        data = fault_get_data(fault);
        /* Mask the data to write */
        data &= mask;
        while (data) {
            int irq;
            irq = CTZ(data);
            data &= ~(1U << irq);
            irq += (offset - GIC_DIST_ISPENDR0) * 8;
            vgic_dist_set_pending_irq(vgic, vcpu, irq);
        }
        break;
    case RANGE32(GIC_DIST_ICPENDR0, GIC_DIST_ICPENDRN):
        data = fault_get_data(fault);
        /* Mask the data to write */
        data &= mask;
        while (data) {
            int irq;
            irq = CTZ(data);
            data &= ~(1U << irq);
            irq += (offset - 0x280) * 8;
            vgic_dist_clr_pending_irq(vgic, vcpu, irq);
        }
        break;
    case RANGE32(GIC_DIST_ISACTIVER0, GIC_DIST_ISACTIVER0):
        gic_dist->active0[vcpu->vcpu_id] = fault_emulate(fault, gic_dist->active0[vcpu->vcpu_id]);
        break;
    case RANGE32(GIC_DIST_ISACTIVER1, GIC_DIST_ISACTIVERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ISACTIVER1);
        gic_dist->active[reg_offset] = fault_emulate(fault, gic_dist->active[reg_offset]);
        break;
    case RANGE32(GIC_DIST_ICACTIVER0, GIC_DIST_ICACTIVER0):
        gic_dist->active_clr0[vcpu->vcpu_id] = fault_emulate(fault, gic_dist->active0[vcpu->vcpu_id]);
        break;
    case RANGE32(GIC_DIST_ICACTIVER1, GIC_DIST_ICACTIVERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ICACTIVER1);
        gic_dist->active_clr[reg_offset] = fault_emulate(fault, gic_dist->active_clr[reg_offset]);
        break;
    case RANGE32(GIC_DIST_IPRIORITYR0, GIC_DIST_IPRIORITYRN):
        break;
    case RANGE32(0x7FC, 0x7FC):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_ITARGETSR0, GIC_DIST_ITARGETSRN):
        break;
    case RANGE32(0xBFC, 0xBFC):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_ICFGR0, GIC_DIST_ICFGRN):
        /* Not supported */
        break;
    case RANGE32(0xD00, 0xDE4):
        break;
    case RANGE32(0xDE8, 0xEFC):
        /* Reserved [0xDE8 - 0xE00) */
        /* GIC_DIST_NSACR [0xE00 - 0xF00) - Not supported */
        break;
    case RANGE32(GIC_DIST_SGIR, GIC_DIST_SGIR):
        data = fault_get_data(fault);
        int mode = (data & GIC_DIST_SGI_TARGET_LIST_FILTER_MASK) >> GIC_DIST_SGI_TARGET_LIST_FILTER_SHIFT;
        int virq = (data & GIC_DIST_SGI_INTID_MASK);
        uint16_t target_list = 0;
        switch (mode) {
        case GIC_DIST_SGI_TARGET_LIST_SPEC:
            /* Forward virq to vcpus specified in CPUTargetList */
            target_list = (data & GIC_DIST_SGI_CPU_TARGET_LIST_MASK) >> GIC_DIST_SGI_CPU_TARGET_LIST_SHIFT;
            break;
        case GIC_DIST_SGI_TARGET_LIST_OTHERS:
            /* Forward virq to all vcpus but the requesting vcpu */
            target_list = (1 << vcpu->vm->num_vcpus) - 1;
            target_list = target_list & ~(1 << vcpu->vcpu_id);
            break;
        case GIC_DIST_SGI_TARGET_SELF:
            /* Forward to virq to only the requesting vcpu */
            target_list = (1 << vcpu->vcpu_id);
            break;
        default:
            ZF_LOGE("Unknow SGIR Target List Filter mode");
            goto ignore_fault;
        }
        for (int i = 0; i < vcpu->vm->num_vcpus; i++) {
            vm_vcpu_t *target_vcpu = vcpu->vm->vcpus[i];
            if (!(target_list & (1 << i)) || !is_vcpu_online(target_vcpu)) {
                continue;
            }
            vm_inject_irq(target_vcpu, virq);
        }
        break;
    case RANGE32(0xF04, 0xF0C):
        break;
    case RANGE32(GIC_DIST_CPENDSGIR0, GIC_DIST_SPENDSGIRN):
        assert(!"vgic SGI reg not implemented!\n");
        break;
    case RANGE32(0xF30, 0xFBC):
        /* Reserved */
        break;
    case RANGE32(0xFC0, 0xFFB):
        break;
    default:
        ZF_LOGE("Unknown register offset 0x%x\n", offset);
    }
ignore_fault:
    err = ignore_fault(fault);
    if (err) {
        return FAULT_ERROR;
    }
    return FAULT_HANDLED;
}

static memory_fault_result_t handle_vgic_dist_fault(vm_t *vm, vm_vcpu_t *vcpu, uintptr_t fault_addr,
                                                    size_t fault_length,
                                                    void *cookie)
{
    if (fault_is_read(vcpu->vcpu_arch.fault)) {
        return handle_vgic_dist_read_fault(vm, vcpu, fault_addr, fault_length, cookie);
    }
    return handle_vgic_dist_write_fault(vm, vcpu, fault_addr, fault_length, cookie);
}

static void vgic_dist_reset(struct vgic_v2_dist_map *gic_dist)
{
    memset(gic_dist, 0, sizeof(*gic_dist));
    gic_dist->ic_type         = 0x0000fce7; /* RO */
    gic_dist->dist_ident      = 0x0200043b; /* RO */

    for (int i = 0; i < CONFIG_MAX_NUM_NODES; i++) {
        gic_dist->enable_set0[i]   = 0x0000ffff; /* 16bit RO */
        gic_dist->enable_clr0[i]   = 0x0000ffff; /* 16bit RO */
    }

    /* Reset value depends on GIC configuration */
    gic_dist->config[0]       = 0xaaaaaaaa; /* RO */
    gic_dist->config[1]       = 0x55540000;
    gic_dist->config[2]       = 0x55555555;
    gic_dist->config[3]       = 0x55555555;
    gic_dist->config[4]       = 0x55555555;
    gic_dist->config[5]       = 0x55555555;
    gic_dist->config[6]       = 0x55555555;
    gic_dist->config[7]       = 0x55555555;
    gic_dist->config[8]       = 0x55555555;
    gic_dist->config[9]       = 0x55555555;
    gic_dist->config[10]      = 0x55555555;
    gic_dist->config[11]      = 0x55555555;
    gic_dist->config[12]      = 0x55555555;
    gic_dist->config[13]      = 0x55555555;
    gic_dist->config[14]      = 0x55555555;
    gic_dist->config[15]      = 0x55555555;

    /* Configure per-processor SGI/PPI target registers */
    for (int i = 0; i < CONFIG_MAX_NUM_NODES; i++) {
        for (int j = 0; j < ARRAY_SIZE(gic_dist->targets0[i]); j++) {
            for (int irq = 0; irq < sizeof(uint32_t); irq++) {
                gic_dist->targets0[i][j] |= ((1 << i) << (irq * 8));
            }
        }
    }
    /* Deliver the SPI interrupts to the first CPU interface */
    for (int i = 0; i < ARRAY_SIZE(gic_dist->targets); i++) {
        gic_dist->targets[i] = 0x1010101;
    }

    /* identification */
    gic_dist->periph_id[4]    = 0x00000004; /* RO */
    gic_dist->periph_id[8]    = 0x00000090; /* RO */
    gic_dist->periph_id[9]    = 0x000000b4; /* RO */
    gic_dist->periph_id[10]   = 0x0000002b; /* RO */
    gic_dist->component_id[0] = 0x0000000d; /* RO */
    gic_dist->component_id[1] = 0x000000f0; /* RO */
    gic_dist->component_id[2] = 0x00000005; /* RO */
    gic_dist->component_id[3] = 0x000000b1; /* RO */
}

int vm_register_irq(vm_vcpu_t *vcpu, int irq, irq_ack_fn_t ack_fn, void *cookie)
{
    struct virq_handle *virq_data;
    struct vgic_device *vgic;
    int err;

    vgic = vcpu->vm->arch.vgic;
    assert(vgic);

    virq_data = calloc(1, sizeof(*virq_data));
    if (!virq_data) {
        return -1;
    }
    virq_init(virq_data, irq, ack_fn, cookie);
    err = virq_add(vcpu, vgic, virq_data);
    if (err) {
        free(virq_data);
        return -1;
    }
    return 0;
}

int vm_inject_irq(vm_vcpu_t *vcpu, int irq)
{
    struct vgic_device *vgic = vcpu->vm->arch.vgic;
    // vm->lock();

    DIRQ("VM received IRQ %d\n", irq);

    assert(vgic);
    int err = vgic_dist_set_pending_irq(vgic, vcpu, irq);

    if (!fault_handled(vcpu->vcpu_arch.fault) && fault_is_wfi(vcpu->vcpu_arch.fault)) {
        ignore_fault(vcpu->vcpu_arch.fault);
    }

    // vm->unlock();

    return err;
}

static memory_fault_result_t handle_vgic_vcpu_fault(vm_t *vm, vm_vcpu_t *vcpu, uintptr_t fault_addr,
                                                    size_t fault_length,
                                                    void *cookie)
{
    /* We shouldn't fault on the vgic vcpu region as it should be mapped in
     * with all rights */
    return FAULT_ERROR;
}

struct vgic_vcpu_iterator_cookie {
    vm_t *vm;
    struct vm_irq_controller_params *params;
};

static vm_frame_t vgic_vcpu_iterator(uintptr_t addr, void *data)
{
    int err;
    cspacepath_t frame;
    vm_frame_t frame_result = { seL4_CapNull, seL4_NoRights, 0, 0 };
    struct vgic_vcpu_iterator_cookie *cookie = data;
    vm_t *vm = cookie->vm;
    struct vm_irq_controller_params *params = cookie->params;

    err = vka_cspace_alloc_path(vm->vka, &frame);
    if (err) {
        printf("Failed to allocate cslot for vgic vcpu\n");
        return frame_result;
    }
    seL4_Word vka_cookie;
    err = vka_utspace_alloc_at(vm->vka, &frame, kobject_get_type(KOBJECT_FRAME, 12), 12, params->gic_vcpu_paddr, &vka_cookie);
    if (err) {
        err = simple_get_frame_cap(vm->simple, (void *)params->gic_vcpu_paddr, 12, &frame);
        if (err) {
            ZF_LOGE("Failed to find device cap for vgic vcpu\n");
            return frame_result;
        }
    }
    frame_result.cptr = frame.capPtr;
    frame_result.rights = seL4_AllRights;
    frame_result.vaddr = params->gic_cpu_paddr;
    frame_result.size_bits = seL4_PageBits;
    return frame_result;
}

/*
 * 1) completely virtual the distributor
 * 2) remap vcpu to cpu. Full access
 */
int vm_install_vgic_v2(vm_t *vm, struct vm_irq_controller_params *params)
{
    struct vgic_device *vgic = NULL;
    struct vgic_vcpu_iterator_cookie cookie = {vm, params};

    if (!(vgic = malloc(sizeof(*vgic)))) {
        assert(!"Unable to malloc memory for VGIC");
        return -1;
    }
    if (vgic_virq_init(vgic)) {
        free(vgic);
        return -1;
    }
    vgic->version = VM_GIC_V2;

    /* Distributor */
    vgic->reg_v2.dist_paddr = params->gic_dist_paddr;
    vgic->reg_v2.dist_size = params->gic_dist_paddr;

    vm_memory_reservation_t *vgic_dist_res = vm_reserve_memory_at(vm, params->gic_dist_paddr, PAGE_SIZE_4K,
                                                                  handle_vgic_dist_fault, vgic);
    vgic_dist_reset(&vgic->reg_v2.distributor);

    /* Remap VCPU to CPU */
    vm_memory_reservation_t *vgic_vcpu_reservation = vm_reserve_memory_at(vm, params->gic_cpu_paddr,
                                                                          0x1000, handle_vgic_vcpu_fault, NULL);
    if (vm_map_reservation(vm, vgic_vcpu_reservation, vgic_vcpu_iterator, &cookie)) {
        free(vgic);
        return -1;
    }

    vm->arch.vgic = vgic;
    return 0;
}

int vm_vgic_maintenance_handler(vm_vcpu_t *vcpu)
{
    int idx;
    int err;
    idx = seL4_GetMR(seL4_UnknownSyscall_ARG0);
    /* Currently not handling spurious IRQs */
    assert(idx >= 0);

    err = handle_vgic_maintenance(vcpu, idx);
    if (!err) {
        seL4_MessageInfo_t reply;
        reply = seL4_MessageInfo_new(0, 0, 0, 0);
        seL4_Reply(reply);
    }
    return VM_EXIT_HANDLED;
}
