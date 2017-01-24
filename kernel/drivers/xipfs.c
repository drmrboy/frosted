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
#include <string.h>
#include "bflt.h"
#include "kprintf.h"
#include "sys/fs/xipfs.h"
#include "vfs.h"
#define GDB_PATH "frosted-userland/gdb/"

static struct fnode *xipfs;
static struct module mod_xipfs;

struct xipfs_fnode {
    struct fnode *fnode;
    void (*init)(void *);
};



#define SECTOR_SIZE (512)

static int xipfs_read(struct fnode *fno, void *buf, unsigned int len)
{
    struct xipfs_fnode *xfno;
    if (len <= 0)
        return len;

    xfno = FNO_MOD_PRIV(fno, &mod_xipfs);
    if (!xfno)
        return -1;

    if (fno->size <= (fno->off))
        return -1;

    if (len > (fno->size - fno->off))
        len = fno->size - fno->off;

    memcpy(buf, ((char *)xfno->init) + fno->off, len);
    fno->off += len;
    return len;
}

static int xipfs_block_read(struct fnode *fno, void *buf, uint32_t sector, int offset, int count)
{
    fno->off = sector * SECTOR_SIZE + offset;
    if (fno->off > fno->size) {
        fno->off = 0;
        return -1;
    }
    if (xipfs_read(fno, buf, count) == count)
        return 0;
    return -1;
}



static int xipfs_write(struct fnode *fno, const void *buf, unsigned int len)
{
    return -1; /* Cannot write! */
}

static int xipfs_poll(struct fnode *fno, uint16_t events, uint16_t *revents)
{
    return -1;
}

static int xipfs_seek(struct fnode *fno, int off, int whence)
{
    return -1;
}

static int xipfs_close(struct fnode *fno)
{
    return 0;
}

static int xipfs_creat(struct fnode *fno)
{
    return -1;

}

static void *xipfs_exe(struct fnode *fno, void *arg)
{
    struct xipfs_fnode *xip = (struct xipfs_fnode *)fno->priv;
    void *reloc_text, *reloc_data, *reloc_bss;
    size_t stack_size;
    void *init = NULL;
    struct vfs_info *vfsi = NULL;

    if (!xip)
        return NULL;

    vfsi = f_calloc(MEM_KERNEL, 1u, sizeof(struct vfs_info));
    if (!vfsi)
        return NULL;

    /* note: xip->init is bFLT load address! */
    if (bflt_load((uint8_t*)xip->init, &reloc_text, &reloc_data, &reloc_bss, &init, &stack_size, (uint32_t *)&vfsi->pic, &vfsi->text_size, &vfsi->data_size))
    {
        kprintf("xipfs: bFLT loading failed.\n");
        return NULL;
    }

    kprintf("xipfs: GDB: add-symbol-file %s%s.gdb 0x%p -s .data 0x%p -s .bss 0x%p\n", GDB_PATH, fno->fname, reloc_text, reloc_data, reloc_bss);

    vfsi->type = VFS_TYPE_BFLT;
    vfsi->allocated = reloc_data;
    vfsi->init = init;

    return (void*)vfsi;
}

static int xipfs_unlink(struct fnode *fno)
{
    return -1; /* Cannot unlink */
}

static int xip_add(const char *name, const void (*init), uint32_t size)
{
    struct xipfs_fnode *xip = kalloc(sizeof(struct xipfs_fnode));
    if (!xip)
        return -1;
    xip->fnode = fno_create(&mod_xipfs, name, fno_search("/bin"));
    if (!xip->fnode) {
        kfree(xip);
        return -1;
    }
    xip->fnode->priv = xip;

    /* Make executable */
    xip->fnode->flags |= FL_EXEC;
    xip->fnode->size = size;
    xip->init = init;
    return 0;
}

static int xipfs_parse_blob(const uint8_t *blob)
{
    const struct xipfs_fat *fat = (const struct xipfs_fat *)blob;
    const struct xipfs_fhdr *f;
    int i, offset;
    if (!fat || fat->fs_magic != XIPFS_MAGIC)
        return -1;

    offset = sizeof(struct xipfs_fat);
    for (i = 0; i < fat->fs_files; i++) {
        f = (const struct xipfs_fhdr *) (blob + offset);
        if (f->magic != XIPFS_MAGIC)
            return -1;
        xip_add(f->name, f->payload, f->len);
        offset += f->len + sizeof(struct xipfs_fhdr);
    }
    return 0;
}

static int xipfs_mount(char *source, char *tgt, uint32_t flags, void *arg)
{
    struct fnode *tgt_dir = NULL;
    /* Source must NOT be NULL */
    if (!source)
        return -1;

    /* Target must be a valid dir */
    if (!tgt)
        return -1;

    tgt_dir = fno_search(tgt);

    if (!tgt_dir || ((tgt_dir->flags & FL_DIR) == 0)) {
        /* Not a valid mountpoint. */
        return -1;
    }

    tgt_dir->owner = &mod_xipfs;
    if (xipfs_parse_blob((uint8_t *)source) < 0)
        return -1;

    return 0;
}


void xipfs_init(void)
{
    mod_xipfs.family = FAMILY_FILE;
    mod_xipfs.mount = xipfs_mount;
    strcpy(mod_xipfs.name,"xipfs");
    mod_xipfs.ops.read = xipfs_read;
    mod_xipfs.ops.poll = xipfs_poll;
    mod_xipfs.ops.write = xipfs_write;
    mod_xipfs.ops.seek = xipfs_seek;
    mod_xipfs.ops.creat = xipfs_creat;
    mod_xipfs.ops.unlink = xipfs_unlink;
    mod_xipfs.ops.close = xipfs_close;
    mod_xipfs.ops.exe = xipfs_exe;

    mod_xipfs.ops.block_read = xipfs_block_read;
    register_module(&mod_xipfs);
}
