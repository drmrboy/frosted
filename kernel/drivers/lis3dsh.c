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
 *      Authors:
 *
 */

#include "frosted.h"
#include "device.h"
#include <stdint.h>
#include "ioctl.h"
#include "gpio.h"
#include "exti.h"
#include "dma.h"
#include "spi.h"
#include <unicore-mx/stm32/spi.h>

/* LIS3DSH registers addresses */
#define ADD_REG_WHO_AM_I				0x0F
#define ADD_REG_CTRL_4					0x20
#define ADD_REG_OUT_X_L					0x28
#define ADD_REG_OUT_X_H					0x29
#define ADD_REG_OUT_Y_L					0x2A
#define ADD_REG_OUT_Y_H					0x2B
#define ADD_REG_OUT_Z_L					0x2C
#define ADD_REG_OUT_Z_H					0x2D

/* WHO AM I register default value */
#define UC_WHO_AM_I_DEFAULT_VALUE		0x3F

/* ADD_REG_CTRL_4 register configuration value:
 * X,Y,Z axis enabled and 400Hz of output data rate */

#define UC_ADD_REG_CTRL_4_CFG_OFF               0x00
#define UC_ADD_REG_CTRL_4_CFG_3_125HZ           ((0x10) | (0x07))
#define UC_ADD_REG_CTRL_4_CFG_6_25HZ            ((0x20) | (0x07))
#define UC_ADD_REG_CTRL_4_CFG_12_5HZ            ((0x30) | (0x07))
#define UC_ADD_REG_CTRL_4_CFG_25HZ              ((0x40) | (0x07))
#define UC_ADD_REG_CTRL_4_CFG_50HZ              ((0x50) | (0x07))
#define UC_ADD_REG_CTRL_4_CFG_3_100HZ           ((0x60) | (0x07))
#define UC_ADD_REG_CTRL_4_CFG_3_400HZ           ((0x70) | (0x07))
#define UC_ADD_REG_CTRL_4_CFG_3_800HZ           ((0x80) | (0x07))
#define UC_ADD_REG_CTRL_4_CFG_3_1600HZ          ((0x90) | (0x07))

#define UC_ADD_REG_CTRL_4_CFG_VALUE_DEFAULT		UC_ADD_REG_CTRL_4_CFG_25HZ


/* Sensitivity for 2G range [mg/digit] */
#define SENS_2G_RANGE_MG_PER_DIGIT		((float)0.06)

/* LED threshold value in mg */
#define LED_TH_MG				(1000)	/* 1000mg (1G) */

/* set read single command. Attention: command must be 0x3F at most */
#define SET_READ_SINGLE_CMD(x)			(x | 0x80)
/* set read multiple command. Attention: command must be 0x3F at most */
#define SET_READ_MULTI_CMD(x)			(x | 0xC0)
/* set write single command. Attention: command must be 0x3F at most */
#define SET_WRITE_SINGLE_CMD(x)			(x & (~(0xC0)))
/* set write multiple command. Attention: command must be 0x3F at most */
#define SET_WRITE_MULTI_CMD(x)			(x & (~(0x80))	\
						 x |= 0x40)

struct dev_lis3dsh {
    struct spi_slave sl; /* First argument, for inheritance */
    struct device * dev;
    const struct gpio_config *pio_cs;
};

struct lis3dsh_ctrl_reg
{
    uint8_t reg;
    uint8_t data;
};

static struct dev_lis3dsh LIS3DSH;

static int devlis3dsh_open(const char *path, int flags);
static int devlis3dsh_read(struct fnode *fno, void *buf, unsigned int len);
static int devlis3dsh_close(struct fnode *fno);

static struct module mod_devlis3dsh = {
    .family = FAMILY_FILE,
    .name = "lis3dsh",
    .ops.open = devlis3dsh_open,
    .ops.read = devlis3dsh_read,
    .ops.close = devlis3dsh_close,
};


/* Function to write a register to LIS3DSH through SPI  */
static void lis3dsh_write_reg(int reg, int data)
{
	/* set CS low */
    gpio_clear(LIS3DSH.pio_cs->base, LIS3DSH.pio_cs->pin);
	spi_xfer(SPI1, SET_WRITE_SINGLE_CMD(reg));
	spi_xfer(SPI1, data);
	/* set CS high */
    gpio_set(LIS3DSH.pio_cs->base, LIS3DSH.pio_cs->pin);
}


/* Function to read a register from LIS3DSH through SPI */
static int lis3dsh_read_reg(int reg)
{
	int reg_value;
	/* set CS low */
    gpio_clear(LIS3DSH.pio_cs->base, LIS3DSH.pio_cs->pin);
	reg_value = spi_xfer(SPI1, SET_READ_SINGLE_CMD(reg));
	reg_value = spi_xfer(SPI1, 0xFF);
	/* set CS high */
    gpio_set(LIS3DSH.pio_cs->base, LIS3DSH.pio_cs->pin);
	return reg_value;
}

static int devlis3dsh_open(const char *path, int flags)
{
    struct fnode *f = fno_search(path);
    volatile int int_reg_value;
    if (!f)
        return -1;
    /* get WHO AM I value */
    int_reg_value = lis3dsh_read_reg(ADD_REG_WHO_AM_I);

    /* if WHO AM I value is the expected one */
    if (int_reg_value == UC_WHO_AM_I_DEFAULT_VALUE) {
        /* set output data rate to 400 Hz and enable X,Y,Z axis */
        lis3dsh_write_reg(ADD_REG_CTRL_4, UC_ADD_REG_CTRL_4_CFG_VALUE_DEFAULT);
        /* verify written value */
        int_reg_value = lis3dsh_read_reg(ADD_REG_CTRL_4);
        /* if written value is different */
        if (int_reg_value != UC_ADD_REG_CTRL_4_CFG_VALUE_DEFAULT) {
            return -EIO;
        }
    } else {
        return -EIO;
    }
    return task_filedesc_add(f);
}

static int devlis3dsh_read(struct fnode *fno, void *buf, unsigned int len)
{
    struct dev_lis3dsh *lis3dsh = FNO_MOD_PRIV(fno, &mod_devlis3dsh);
    uint8_t reg_base = ADD_REG_OUT_X_L;
    uint8_t i;
    volatile int int_reg_value;
    if (len < 6)
        return len;

    if (!lis3dsh)
        return -1;
    for (i = 0; i < 6; i++) {
        ((uint8_t*)buf)[i] = lis3dsh_read_reg(reg_base + i);
    }
    return 6;
}

static int devlis3dsh_close(struct fnode *fno)
{
    struct dev_lis3dsh *lis3dsh;
    lis3dsh = FNO_MOD_PRIV(fno, &mod_devlis3dsh);
    if (!lis3dsh)
        return -1;

    return 0;
}

static void lis3dsh_isr(struct spi_slave *sl)
{
    struct dev_lis3dsh *lis = (struct dev_lis3dsh *)sl;
    task_resume(lis->dev->task);
}


int lis3dsh_init(uint8_t bus, const struct gpio_config *lis3dsh_cs)
{
    struct fnode *devfs;
    memset(&LIS3DSH, 0, sizeof(struct dev_lis3dsh));

    devfs = fno_search("/dev");
    if (!devfs)
        return -EFAULT;

    LIS3DSH.dev = device_fno_init(&mod_devlis3dsh, "ins", devfs, FL_RDONLY, &LIS3DSH);
    if (!LIS3DSH.dev)
        return -EFAULT;

    /* Populate spi_slave struct */
    LIS3DSH.sl.bus = bus;
    LIS3DSH.sl.isr = lis3dsh_isr;
    LIS3DSH.pio_cs = lis3dsh_cs;
    register_module(&mod_devlis3dsh);
    gpio_create(&mod_devlis3dsh, lis3dsh_cs);
    gpio_set(LIS3DSH.pio_cs->base, LIS3DSH.pio_cs->pin);
    return 0;
}
