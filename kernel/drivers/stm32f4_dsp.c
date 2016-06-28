/*
 *      This file is part of frosted.
 *
 *      frosted is free software: you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License version 2, as
 *      published by the Free Software Foundation.
 *
 *
 *      frosted is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with frosted.  If not, see <http://www.gnu.org/licenses/>.
 *
 *      Authors: Daniele Lacamera <root@danielinux.net>
 *
 */
 
#include "frosted.h"
#include "device.h"
#include <stdint.h>
#include "cirbuf.h"
#include "stm32f4_dma.h"
#include "cirbuf.h"
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/dac.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/timer.h>

#define DSP_BUFSIZ 256

#define DSP_IDLE 0
#define DSP_BUSY 1

/* Single device supported in this driver. */
struct dev_dsp {
    struct device *dev;
    uint8_t outb[DSP_BUFSIZ];
    int written;
    int transfer_size;
    int state;
} Dsp;


/* Module description */

int dsp_read(struct fnode *fno, void *buf, unsigned int len);
int dsp_write(struct fnode *fno, const void *buf, unsigned int len);

static struct module mod_devdsp = {
    .family = FAMILY_FILE,
    .name = "dsp",
    .ops.open = device_open,
    .ops.write = dsp_write,
    .ops.read = dsp_read
};

int dsp_read(struct fnode *fno, void *buf, unsigned int len)
{
    /* ADC not attached yet */
    return -EBUSY;
}

int dsp_write(struct fnode *fno, const void *buf, unsigned int len)
{
    struct dev_dsp *dsp;
    int space;


    dsp = (struct dev_dsp *)FNO_MOD_PRIV(fno, &mod_devdsp);
    if (dsp == NULL)
        return -1;

    mutex_lock(dsp->dev->mutex);
    while (dsp->written < len) {
        if (dsp->state == DSP_BUSY) {
            dsp->dev->pid = scheduler_get_cur_pid();
            mutex_unlock(dsp->dev->mutex);
            task_suspend();
            return SYS_CALL_AGAIN;
        }

        space = DSP_BUFSIZ;
        if (space > (len - dsp->written)) {
            space = (len - dsp->written);
        }
        memcpy(dsp->outb, buf + dsp->written, space);
        dsp->state = DSP_BUSY;
        dsp->transfer_size = space;

        /* Start DMA transfer of waveform */
        dac_trigger_enable(CHANNEL_1);
        dac_set_trigger_source(DAC_CR_TSEL1_T2);
        dac_dma_enable(CHANNEL_1);
        dma_set_memory_address(DMA1, DMA_STREAM5, (uint32_t) dsp->outb);
        dma_set_number_of_data(DMA1, DMA_STREAM5, space);
        dma_enable_transfer_complete_interrupt(DMA1, DMA_STREAM5);
        dma_channel_select(DMA1, DMA_STREAM5, DMA_SxCR_CHSEL_7);
        dma_enable_stream(DMA1, DMA_STREAM5);
    }
    dsp->written = 0;
    mutex_unlock(dsp->dev->mutex);
    return len;
}


/* IRQ Handler */
void dma1_stream5_isr(void)
{
    if (dma_get_interrupt_flag(DMA1, DMA_STREAM5, DMA_TCIF)) {
        Dsp.written += Dsp.transfer_size;
        Dsp.state = DSP_IDLE;
        if (Dsp.dev->pid)
            task_resume(Dsp.dev->pid);
        dma_clear_interrupt_flags(DMA1, DMA_STREAM5, DMA_TCIF);
        dac_trigger_disable(CHANNEL_1);
        dac_dma_disable(CHANNEL_1);
    }
}


/* Initialization functions */

#define PERIOD 1152
static void timer_setup(void)
{
    /* Enable TIM2 clock. */
    rcc_periph_clock_enable(RCC_TIM2);
    timer_reset(TIM2);
    /* Timer global mode: - No divider, Alignment edge, Direction up */
    timer_set_mode(TIM2, TIM_CR1_CKD_CK_INT,
            TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);
    timer_continuous_mode(TIM2);
    timer_set_period(TIM2, PERIOD);
    timer_disable_oc_output(TIM2, TIM_OC2 | TIM_OC3 | TIM_OC4);
    timer_enable_oc_output(TIM2, TIM_OC1);
    timer_disable_oc_clear(TIM2, TIM_OC1);
    timer_disable_oc_preload(TIM2, TIM_OC1);
    timer_set_oc_slow_mode(TIM2, TIM_OC1);
    timer_set_oc_mode(TIM2, TIM_OC1, TIM_OCM_TOGGLE);
    timer_set_oc_value(TIM2, TIM_OC1, 500);
    timer_disable_preload(TIM2);
    /* Set the timer trigger output (for the DAC) to the channel 1 output
     *     compare */
    timer_set_master_mode(TIM2, TIM_CR2_MMS_COMPARE_OC1REF);
    timer_enable_counter(TIM2);
}


static void dsp_dma_setup(void)
{
    /* DAC channel 1 uses DMA controller 1 Stream 5 Channel 7. */
    /* Enable DMA1 clock and IRQ */
    rcc_periph_clock_enable(RCC_DMA1);
    nvic_set_priority(NVIC_DMA1_STREAM5_IRQ, 1);
    nvic_enable_irq(NVIC_DMA1_STREAM5_IRQ);
    dma_stream_reset(DMA1, DMA_STREAM5);
    dma_set_priority(DMA1, DMA_STREAM5, DMA_SxCR_PL_LOW);
    dma_set_memory_size(DMA1, DMA_STREAM5, DMA_SxCR_MSIZE_8BIT);
    dma_set_peripheral_size(DMA1, DMA_STREAM5, DMA_SxCR_PSIZE_8BIT);
    dma_enable_memory_increment_mode(DMA1, DMA_STREAM5);
    dma_enable_circular_mode(DMA1, DMA_STREAM5);
    dma_set_transfer_mode(DMA1, DMA_STREAM5,
            DMA_SxCR_DIR_MEM_TO_PERIPHERAL);
    /* The register to target is the DAC1 8-bit right justified data
        register */
    dma_set_peripheral_address(DMA1, DMA_STREAM5, (uint32_t) &DAC_DHR8R1);
}

static void dsp_hw_init(data_channel c)
{
    /* Set DAC GPIO pin to analog mode */
    rcc_periph_clock_enable(RCC_GPIOC);
    gpio_mode_setup(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, GPIO4);
    timer_setup();

    /* Set up DAC */
    rcc_periph_clock_enable(RCC_DAC);
    dac_enable(c);
}

void dsp_init(struct fnode *devdir)
{
    int i;
    memset(&Dsp, 0, sizeof(struct dev_dsp));
    Dsp.dev = device_fno_init(&mod_devdsp, "dsp", devdir, 0, &Dsp);
    dsp_hw_init(CHANNEL_1);
    dsp_dma_setup();
}
